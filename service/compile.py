#!/usr/bin/env python3
"""Compile service: receives png2amiga viewer .cpp source, returns a target
binary (.exe for AmigaOS, .adf bootable Amiga floppy, .exe for DOS, .img
bootable DOS floppy). Each compile runs inside a bubblewrap (bwrap) sandbox:

- Read-only access to toolchain binaries and headers
- Read-write access to a fresh temp directory only
- No network, no access to host filesystem
- `apt install bubblewrap mtools`

Supported `?format=` values (POST /api/compile):
    exe     — AmigaOS .exe (hunk format) via m68k-amiga-elf-gcc + elf2hunk
    adf     — Amiga bootable floppy (880 KB) via exe2adf
    dos-exe — MS-DOS .exe (DPMI) via i586-pc-msdosdjgpp-g++ (DJGPP toolchain)
    img     — Bootable MS-DOS 1.44 MB floppy (FreeDOS kernel + autoexec +
              our viewer.exe), built via mtools from a template image.

Toolchain layout on the server (under service/toolchain/ or
/var/www/png2amiga/service/toolchain/):
    amiga/linux/opt/bin/m68k-amiga-elf-gcc     — Amiga cross-gcc
    amiga/linux/elf2hunk                        — Amiga hunk linker
    amiga/linux/exe2adf                         — Amiga floppy builder
    amiga/dh0/                                  — Amiga ADF startup files
    amiga/template/                             — Amiga C startup (gcc8_*.s/c)
    dos/djgpp/                                  — DJGPP cross-gcc tree
                                                  (bin/i586-pc-msdosdjgpp-g++)
    dos/freedos-bootable-1.44M.img              — FreeDOS boot template with
                                                  kernel.sys + command.com,
                                                  blank autoexec.bat
Dev fallback: if service/toolchain/ is absent, uses
third_party/vscode-amiga-debug/ for the Amiga side. No dev fallback for DOS
— deployer must drop the DJGPP + FreeDOS assets into service/toolchain/dos/.
"""

import gzip
import os
import subprocess
import tempfile
import shutil
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

# --- Amiga toolchain ---
# Three-tier lookup:
#   service/toolchain/amiga/         (preferred new layout, Amiga + DOS coexist)
#   service/toolchain/               (legacy deploy layout — kept to avoid
#                                     breaking existing production servers)
#   third_party/vscode-amiga-debug/  (dev vendored copy)
_LOCAL_TC_AMIGA  = os.path.join(SCRIPT_DIR, "toolchain", "amiga")
_LEGACY_TC       = os.path.join(SCRIPT_DIR, "toolchain")
_PROJECT_TC_AMIGA = os.path.join(PROJECT_ROOT, "third_party", "vscode-amiga-debug")
if os.path.isdir(_LOCAL_TC_AMIGA):
    _TC_ROOT = _LOCAL_TC_AMIGA
elif (os.path.isdir(_LEGACY_TC) and
      os.path.isfile(os.path.join(_LEGACY_TC, "bin", "linux",
                                  "opt", "bin", "m68k-amiga-elf-gcc"))):
    _TC_ROOT = _LEGACY_TC
else:
    _TC_ROOT = _PROJECT_TC_AMIGA
TOOLCHAIN = os.path.join(_TC_ROOT, "bin")
TEMPLATE = os.path.join(_TC_ROOT, "template")
SUPPORT = os.path.join(TEMPLATE, "support")

PLATFORM = "linux"
GCC = os.path.join(TOOLCHAIN, PLATFORM, "opt", "bin", "m68k-amiga-elf-gcc")
GCC_DIR = os.path.join(TOOLCHAIN, PLATFORM, "opt")
SYS_INCLUDE = os.path.join(GCC_DIR, "m68k-amiga-elf", "sys-include")
ELF2HUNK = os.path.join(TOOLCHAIN, PLATFORM, "elf2hunk")
EXE2ADF = os.path.join(TOOLCHAIN, PLATFORM, "exe2adf")
DH0 = os.path.join(TOOLCHAIN, "dh0")  # startup-sequence + system commands for ADF

# --- DOS toolchain (DJGPP + FreeDOS boot bits + CWSDPMI stub) ---
# Files checked into the repo at service/toolchain/dos/ (small redistributables):
#   KERNEL.SYS    — FreeDOS 2.44 kernel (386 build), GPL
#   COMMAND.COM   — FreeCOM 0.86 shell, GPL
#   boot12.bin    — FAT12 boot sector assembled from FreeDOS boot.asm, GPL
#   CWSDSTUB.EXE  — CWSDPMI-embedded DJGPP stub (csdpmi7b.zip), public domain
# Deploy-time drop-in (too large to commit, ~40 MB):
#   djgpp/        — prebuilt DJGPP cross toolchain tree
#       bin/i586-pc-msdosdjgpp-g++
#       i586-pc-msdosdjgpp/bin/exe2coff
# Runtime built-on-first-request cache (outside the service dir so it can
# live on /var/cache or equivalent if desired):
#   freedos-bootable-1.44M.img  — 1.44 MB FAT12 image with KERNEL.SYS +
#       COMMAND.COM + blank AUTOEXEC.BAT. Built from the checked-in files
#       via mtools on first dos-img request; cached thereafter.
_TC_DOS = os.path.join(SCRIPT_DIR, "toolchain", "dos")
DJGPP_ROOT = os.path.join(_TC_DOS, "djgpp")
DJGPP_GXX  = os.path.join(DJGPP_ROOT, "bin", "i586-pc-msdosdjgpp-g++")
DJGPP_EXE2COFF = os.path.join(DJGPP_ROOT, "i586-pc-msdosdjgpp",
                              "bin", "exe2coff")
CWSDSTUB     = os.path.join(_TC_DOS, "CWSDSTUB.EXE")
FREEDOS_KERNEL  = os.path.join(_TC_DOS, "KERNEL.SYS")
FREEDOS_SHELL   = os.path.join(_TC_DOS, "COMMAND.COM")
FREEDOS_BOOTSEC = os.path.join(_TC_DOS, "boot12.bin")
FREEDOS_IMG_TEMPLATE = os.path.join(_TC_DOS, "freedos-bootable-1.44M.img")
# mtools lives on the host (`apt install mtools`). We bind-mount /usr read-only
# into the sandbox, so /usr/bin/mcopy is reachable from inside.
MCOPY = "/usr/bin/mcopy"
MFORMAT = "/usr/bin/mformat"

PORT = int(os.environ.get("PORT", "3001"))
MAX_BODY = 5 * 1024 * 1024  # 5MB max source size

# Heuristic: DOS viewer sources always include <dpmi.h>. If a client posts a
# source that doesn't include it but requests dos-exe / img, we reject — we
# don't want to hand arbitrary C++ to DJGPP in case a cross-compile leaks
# behavior the Amiga path was supposed to prevent.
_DOS_SOURCE_SIGIL = "#include <dpmi.h>"


def _sandbox(cmd, tmpdir):
    """Wrap an Amiga-toolchain command in a bubblewrap sandbox."""
    return [
        "bwrap",
        "--unshare-all",
        "--die-with-parent",
        "--ro-bind", "/usr", "/usr",
        "--ro-bind", "/lib", "/lib",
        "--ro-bind", "/lib64", "/lib64",
        "--ro-bind", GCC_DIR, GCC_DIR,
        "--ro-bind", ELF2HUNK, ELF2HUNK,
        "--ro-bind", EXE2ADF, EXE2ADF,
        "--ro-bind", DH0, DH0,
        "--ro-bind", TEMPLATE, TEMPLATE,
        "--bind", tmpdir, tmpdir,
        "--dev", "/dev",
        "--chdir", tmpdir,
    ] + cmd


def _sandbox_dos(cmd, tmpdir):
    """Wrap a DJGPP command in a bubblewrap sandbox. Separate from _sandbox()
    because DJGPP needs its own toolchain tree bound and has no business
    seeing the Amiga toolchain paths."""
    return [
        "bwrap",
        "--unshare-all",
        "--die-with-parent",
        "--ro-bind", "/usr", "/usr",
        "--ro-bind", "/lib", "/lib",
        "--ro-bind", "/lib64", "/lib64",
        "--ro-bind", DJGPP_ROOT, DJGPP_ROOT,
        "--bind", tmpdir, tmpdir,
        "--dev", "/dev",
        "--chdir", tmpdir,
    ] + cmd


def _sandbox_mtools(cmd, tmpdir):
    """Sandbox for mtools invocations that modify a FAT12 floppy image in
    the temp dir. No toolchain needed, just /usr (for /usr/bin/mcopy)."""
    return [
        "bwrap",
        "--unshare-all",
        "--die-with-parent",
        "--ro-bind", "/usr", "/usr",
        "--ro-bind", "/lib", "/lib",
        "--ro-bind", "/lib64", "/lib64",
        "--ro-bind", "/etc", "/etc",  # mtools reads /etc/mtools.conf if present
        "--bind", tmpdir, tmpdir,
        "--dev", "/dev",
        "--chdir", tmpdir,
    ] + cmd


def _dos_toolchain_ready():
    """Return (ok, reason) — True iff DJGPP + CWSDPMI + FreeDOS bits are
    all present. The 1.44 MB bootable template is built on first use
    from the checked-in sources, so we don't require it here."""
    if not os.path.isfile(DJGPP_GXX):
        return False, f"DJGPP not installed (expected at {DJGPP_GXX})"
    if not os.path.isfile(DJGPP_EXE2COFF):
        return False, f"DJGPP exe2coff missing (expected at {DJGPP_EXE2COFF})"
    if not os.path.isfile(CWSDSTUB):
        return False, (f"CWSDSTUB.EXE missing (expected at {CWSDSTUB}; "
                        "extract from csdpmi7b.zip)")
    for path, label in [(FREEDOS_KERNEL, "FreeDOS KERNEL.SYS"),
                        (FREEDOS_SHELL,  "FreeDOS COMMAND.COM"),
                        (FREEDOS_BOOTSEC, "FreeDOS boot12.bin")]:
        if not os.path.isfile(path):
            return False, f"{label} missing at {path}"
    if not shutil.which("mcopy"):
        return False, "mtools not installed (apt install mtools)"
    if not shutil.which("mformat"):
        return False, "mformat not installed (apt install mtools)"
    return True, ""


def _ensure_freedos_template():
    """Build the 1.44 MB bootable FreeDOS floppy template from the checked-in
    KERNEL.SYS + COMMAND.COM + boot12.bin on first call. Cached afterwards.
    Thread safety is trivial (single-threaded HTTPServer); if this ever
    becomes multi-threaded, wrap in a lock."""
    if os.path.isfile(FREEDOS_IMG_TEMPLATE):
        return
    # Build outside the sandbox — we own service/toolchain/dos/ and mtools
    # is trusted. The finished template is then bind-mounted read-only into
    # the per-request sandboxes for mcopy to overlay AUTOEXEC + VIEWER onto.
    tmp_img = FREEDOS_IMG_TEMPLATE + ".tmp"
    # 1) Create zero-filled 1.44 MB file.
    with open(tmp_img, "wb") as f:
        f.truncate(1474560)
    # 2) Format FAT12 with our FreeDOS boot sector.
    subprocess.run(
        [MFORMAT, "-i", tmp_img, "-f", "1440", "-B", FREEDOS_BOOTSEC,
         "-v", "PNG2DOS", "::"],
        check=True, capture_output=True, timeout=10)
    # 3) Copy system files. Order matters on strict FreeDOS boot sectors —
    #    KERNEL.SYS should occupy the very first directory entries and
    #    (ideally) the first data clusters. The checked-in boot.asm walks
    #    the root dir by name so this is informational, not critical.
    subprocess.run(
        [MCOPY, "-i", tmp_img, FREEDOS_KERNEL, FREEDOS_SHELL, "::/"],
        check=True, capture_output=True, timeout=10)
    # 4) Blank AUTOEXEC.BAT so the template is usable as-is even without
    #    the per-request overlay (though our flow always overwrites it).
    blank = FREEDOS_IMG_TEMPLATE + ".autoexec"
    with open(blank, "w", newline="\r\n") as f:
        f.write("@ECHO OFF\r\n")
    subprocess.run(
        [MCOPY, "-i", tmp_img, blank, "::AUTOEXEC.BAT"],
        check=True, capture_output=True, timeout=10)
    os.unlink(blank)
    os.replace(tmp_img, FREEDOS_IMG_TEMPLATE)


def _embed_cwsdpmi(exe_path, tmpdir):
    """Replace the DJGPP plain stub on `exe_path` with CWSDSTUB.EXE, which
    embeds CWSDPMI and auto-loads it when no DPMI host is present. This is
    how we avoid the `Load error: no DPMI — Get csdpmi*b.zip` message on
    stock DOS environments (pre-Windows 3.1, FreeDOS without autoexec DPMI
    server, bootable-floppy environments, etc).

    Strategy:
      1. `exe2coff viewer.exe`      → writes `viewer` (COFF, no stub)
      2. cat CWSDSTUB.EXE viewer    → replacement stub + COFF = standalone .exe

    `exe_path` is modified in place."""
    coff_path = os.path.splitext(exe_path)[0]  # exe2coff strips .exe
    subprocess.run(_sandbox_dos([DJGPP_EXE2COFF, exe_path], tmpdir),
                   check=True, capture_output=True, timeout=10)
    with open(CWSDSTUB, "rb") as stub, \
         open(coff_path, "rb") as coff, \
         open(exe_path, "wb") as out:
        out.write(stub.read())
        out.write(coff.read())
    os.unlink(coff_path)


def compile_viewer_dos(source_code, output_format="dos-exe"):
    """Compile DJGPP DOS viewer source. Returns:
      output_format='dos-exe' → .exe bytes (DOS DPMI executable)
      output_format='img'     → 1.44 MB bootable floppy (.img) that boots
                                FreeDOS and autoruns the viewer.
    Requires DJGPP toolchain + FreeDOS template + mtools on the host."""
    ok, reason = _dos_toolchain_ready()
    if not ok:
        raise RuntimeError(reason)

    if _DOS_SOURCE_SIGIL not in source_code:
        # Sanity check: refuse to hand arbitrary C++ to DJGPP. Every
        # png2amiga-generated DOS viewer includes <dpmi.h>.
        raise ValueError("source does not look like a png2amiga DOS viewer "
                         "(missing #include <dpmi.h>)")

    with tempfile.TemporaryDirectory(prefix="png2amiga_dos_") as tmpdir:
        src_path = os.path.join(tmpdir, "viewer.cpp")
        exe_path = os.path.join(tmpdir, "viewer.exe")

        with open(src_path, "w") as f:
            f.write(source_code)

        # -O2 is enough; larger -O levels rarely help on data-blit viewers
        # and cost compile time that matters for a web roundtrip.
        subprocess.run(_sandbox_dos([
            DJGPP_GXX, "-O2",
            "-o", exe_path, src_path,
        ], tmpdir), check=True, capture_output=True, timeout=60)

        # Bake CWSDPMI into the stub so the viewer runs on any DOS without
        # needing an external csdpmi*b.zip install. Adds ~20 KB to each exe.
        _embed_cwsdpmi(exe_path, tmpdir)

        if output_format == "img":
            # Build the FAT12 bootable template on first use (cached).
            _ensure_freedos_template()
            img_path = os.path.join(tmpdir, "floppy.img")
            # Copy the bootable template, then drop the viewer + autoexec
            # onto it with mcopy. mtools doesn't need write access to the
            # whole host — just the image file.
            shutil.copy(FREEDOS_IMG_TEMPLATE, img_path)
            autoexec = os.path.join(tmpdir, "autoexec.bat")
            with open(autoexec, "w", newline="\r\n") as f:
                f.write("@ECHO OFF\r\nVIEWER.EXE\r\n")

            # mcopy -o overwrites existing files; ::PATH is the DOS-side
            # location inside the floppy image. We place VIEWER.EXE in /
            # so the root AUTOEXEC.BAT reaches it.
            subprocess.run(_sandbox_mtools(
                [MCOPY, "-o", "-i", img_path, exe_path, "::VIEWER.EXE"],
                tmpdir
            ), check=True, capture_output=True, timeout=10)
            subprocess.run(_sandbox_mtools(
                [MCOPY, "-o", "-i", img_path, autoexec, "::AUTOEXEC.BAT"],
                tmpdir
            ), check=True, capture_output=True, timeout=10)

            with open(img_path, "rb") as f:
                return f.read()
        else:
            with open(exe_path, "rb") as f:
                return f.read()


def compile_viewer(source_code, output_format="exe"):
    """Compile viewer source to .exe or .adf. Returns binary bytes."""
    with tempfile.TemporaryDirectory(prefix="png2amiga_") as tmpdir:
        src_path = os.path.join(tmpdir, "viewer.cpp")
        elf_path = os.path.join(tmpdir, "viewer.elf")
        exe_path = os.path.join(tmpdir, "viewer.exe")

        with open(src_path, "w") as f:
            f.write(source_code)

        subprocess.run(_sandbox([
            GCC, "-m68000", "-Ofast", "-nostdlib",
            "-fomit-frame-pointer", "-fno-exceptions",
            "-nostdinc",                   # block #include of host system files
            "-isystem", SYS_INCLUDE,       # Amiga SDK headers (proto/, exec/, etc.)
            "-isystem", os.path.join(GCC_DIR, "lib", "gcc", "m68k-amiga-elf", "14.2.0", "include"),  # gcc builtins (stddef.h etc.)
            "-I", TEMPLATE,
            "-Wa,--register-prefix-optional",
            "-Wl,--emit-relocs,-Ttext=0",
            "-o", elf_path,
            src_path,
            os.path.join(SUPPORT, "gcc8_a_support.s"),
            os.path.join(SUPPORT, "gcc8_c_support.c"),
        ], tmpdir), check=True, capture_output=True, timeout=30)

        subprocess.run(_sandbox(
            [ELF2HUNK, elf_path, exe_path], tmpdir
        ), check=True, capture_output=True, timeout=10)

        if output_format == "adf":
            adf_path = os.path.join(tmpdir, "viewer.adf")
            subprocess.run(_sandbox(
                [EXE2ADF, "-i", exe_path, "-a", adf_path, "-l", "png2amiga"], tmpdir
            ), check=True, capture_output=True, timeout=10)
            with open(adf_path, "rb") as f:
                return f.read()
        else:
            with open(exe_path, "rb") as f:
                return f.read()


class CompileHandler(BaseHTTPRequestHandler):
    def do_OPTIONS(self):
        self.send_response(200)
        self._cors_headers()
        self.end_headers()

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/api/compile":
            self.send_error(404)
            return

        params = parse_qs(parsed.query)
        fmt = params.get("format", ["exe"])[0]
        AMIGA_FORMATS = ("exe", "adf")
        DOS_FORMATS = ("dos-exe", "img")
        if fmt not in AMIGA_FORMATS + DOS_FORMATS:
            self.send_error(400, "format must be exe | adf | dos-exe | img")
            return

        content_length = int(self.headers.get("Content-Length", 0))
        if content_length == 0 or content_length > MAX_BODY:
            self.send_error(400, "Invalid content length")
            return

        source = self.rfile.read(content_length).decode("utf-8")

        try:
            if fmt in DOS_FORMATS:
                result = compile_viewer_dos(source, fmt)
            else:
                result = compile_viewer(source, fmt)
        except (RuntimeError, ValueError) as e:
            # Missing DOS toolchain or malformed source — 400/503-ish.
            self.send_response(503 if isinstance(e, RuntimeError) else 400)
            self._cors_headers()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(str(e).encode())
            return
        except subprocess.CalledProcessError as e:
            self.send_response(500)
            self._cors_headers()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            stderr = e.stderr.decode("utf-8", errors="replace") if e.stderr else str(e)
            stderr = stderr.replace(TEMPLATE, "<toolchain>")
            stderr = stderr.replace(GCC_DIR, "<toolchain>")
            self.wfile.write(f"Compile error:\n{stderr}".encode())
            return
        except Exception as e:
            self.send_response(500)
            self._cors_headers()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"Internal error")
            return

        ext = {"adf": "adf", "dos-exe": "exe",
               "img": "img", "exe": "exe"}.get(fmt, "bin")
        # Gzip compress — ADF is 880KB mostly empty, exe has compressible image data
        accept = self.headers.get("Accept-Encoding", "")
        if "gzip" in accept:
            compressed = gzip.compress(result, compresslevel=6)
            self.send_response(200)
            self._cors_headers()
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Disposition", f'attachment; filename="viewer.{ext}"')
            self.send_header("Content-Length", str(len(compressed)))
            self.end_headers()
            self.wfile.write(compressed)
        else:
            self.send_response(200)
            self._cors_headers()
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="viewer.{ext}"')
            self.send_header("Content-Length", str(len(result)))
            self.end_headers()
            self.wfile.write(result)

    def _cors_headers(self):
        origin = self.headers.get("Origin", "")
        # Use the constant from the allow-list, not the raw header value,
        # to avoid reflecting user input into the response (CWE-113).
        allowed = {"https://www.png2amiga.app": "https://www.png2amiga.app",
                   "https://png2amiga.app": "https://png2amiga.app"}
        matched = allowed.get(origin)
        if matched is not None:
            self.send_header("Access-Control-Allow-Origin", matched)
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def log_message(self, format, *args):
        print(f"[compile] {args[0]}", file=sys.stderr)


def main():
    # Hard requirements for the Amiga path (which every deployment supports).
    for tool, path in [("gcc", GCC), ("elf2hunk", ELF2HUNK), ("exe2adf", EXE2ADF)]:
        if not os.path.isfile(path):
            print(f"Error: {tool} not found at {path}", file=sys.stderr)
            sys.exit(1)

    if not shutil.which("bwrap"):
        print("Error: bubblewrap (bwrap) not found. apt install bubblewrap",
              file=sys.stderr)
        sys.exit(1)

    # Optional: DOS toolchain. Service still starts without it; dos-exe /
    # img requests get 503 with a clear reason.
    dos_ok, dos_reason = _dos_toolchain_ready()
    if dos_ok:
        print(f"DOS toolchain OK at {_TC_DOS} (DJGPP + FreeDOS template)",
              file=sys.stderr)
    else:
        print(f"DOS toolchain NOT ready: {dos_reason} — dos-exe/img "
              f"requests will 503 until this is fixed", file=sys.stderr)

    server = HTTPServer(("127.0.0.1", PORT), CompileHandler)
    print(f"Compile server listening on http://127.0.0.1:{PORT} (sandboxed)",
          file=sys.stderr)
    server.serve_forever()


if __name__ == "__main__":
    main()
