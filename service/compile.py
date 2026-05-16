#!/usr/bin/env python3
"""Compile service: receives png2amiga viewer .cpp source, returns an Amiga
binary. Each compile runs inside a bubblewrap (bwrap) sandbox:

- Read-only access to toolchain binaries and headers
- Read-write access to a fresh temp directory only
- No network, no access to host filesystem
- `apt install bubblewrap`

Supported `?format=` values (POST /api/compile):
    exe — AmigaOS .exe (hunk format) via m68k-amiga-elf-gcc + elf2hunk
    adf — Amiga bootable floppy (880 KB) via exe2adf

Toolchain layout on the server (under service/toolchain/ or
/var/www/png2amiga/service/toolchain/):
    amiga/linux/opt/bin/m68k-amiga-elf-gcc — Amiga cross-gcc
    amiga/linux/elf2hunk                    — Amiga hunk linker
    amiga/linux/exe2adf                     — Amiga floppy builder
    amiga/dh0/                              — Amiga ADF startup files
    amiga/template/                         — Amiga C startup (gcc8_*.s/c)

DOS viewers are no longer compiled server-side: the generator now emits
a self-contained .c file (see src/cheader_dos_c.cpp) that the user
compiles locally with ia16-elf-gcc.

Dev fallback: if service/toolchain/ is absent, uses
third_party/vscode-amiga-debug/.
"""

import gzip
import os
import re
import subprocess
import tempfile
import shutil
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import sys

# Matches the per-request scratch directory tempfile creates (random
# 6+ chars after the prefix on Linux). Redacted from compiler stderr
# before the response is sent so a user-facing error doesn't leak the
# server's tempfile-prefix scheme.
_TMPDIR_RE = re.compile(r"/tmp/png2amiga[a-z_]*[A-Za-z0-9]+")

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

# --- DOS toolchain (ia16-elf-gcc, TK Chia's PPA build) ---
# Two-tier lookup:
#   service/toolchain/dos/ia16/  (production deploy, .debs extracted with
#                                  dpkg-deb -x — see deploy notes in README)
#   third_party/ia16-elf/         (optional dev fallback; not currently shipped)
#
# The Noble PPA .debs put binaries under usr/bin and the runtime libs
# (libopcodes / libbfd) under usr/x86_64-linux-gnu/ia16-elf/lib. No rpath
# is embedded, so we set LD_LIBRARY_PATH at invoke time (bwrap --setenv).
_LOCAL_TC_DOS  = os.path.join(SCRIPT_DIR, "toolchain", "dos", "ia16")
_PROJECT_TC_DOS = os.path.join(PROJECT_ROOT, "third_party", "ia16-elf")
if os.path.isdir(_LOCAL_TC_DOS):
    _DOS_ROOT = _LOCAL_TC_DOS
elif os.path.isdir(_PROJECT_TC_DOS):
    _DOS_ROOT = _PROJECT_TC_DOS
else:
    _DOS_ROOT = ""  # DOS path disabled — preflight will warn, requests 503.
IA16_GCC = os.path.join(_DOS_ROOT, "usr", "bin", "ia16-elf-gcc") if _DOS_ROOT else ""
IA16_NM  = os.path.join(_DOS_ROOT, "usr", "bin", "ia16-elf-nm")  if _DOS_ROOT else ""
IA16_LIB = os.path.join(_DOS_ROOT, "usr", "x86_64-linux-gnu", "ia16-elf", "lib") if _DOS_ROOT else ""

# FreeDOS boot files for the bootable .img path. Sourced from a FreeDOS
# 1.3 install (KERNL086.SYS is the 8086-compatible kernel — works on
# every CPU from the original PC up through modern emulators; we rename
# it to KERNEL.SYS on the floppy because that's what boot12.bin searches
# the root dir for). boot12.bin is the standard FreeDOS FAT12 boot sector.
_DOS_BOOT_DIR = os.path.join(SCRIPT_DIR, "toolchain", "dos")
DOS_BOOT12  = os.path.join(_DOS_BOOT_DIR, "boot12.bin")
DOS_KERNEL  = os.path.join(_DOS_BOOT_DIR, "KERNL086.SYS")
DOS_COMMAND = os.path.join(_DOS_BOOT_DIR, "COMMAND.COM")

PORT = int(os.environ.get("PORT", "3001"))
MAX_BODY = 5 * 1024 * 1024  # 5MB max source size
# Hard cap on the compiler-produced binary that we'll ship back. The
# Amiga adf is a fixed 901 120 bytes (880 KB), DOS .img is 737 280
# (720 KB), DOS / Amiga .exe is bounded by what ia16-elf-gcc / m68k-
# amiga-elf-gcc can emit in their 30 s timeout under the 512 M cgroup.
# 8 MB clears every legitimate case with margin and bounds the per-
# request response if a compile pathologically produces a giant binary.
MAX_OUTPUT = 8 * 1024 * 1024


def _sandbox(cmd, tmpdir, extra_binds=(), env=None):
    """Wrap a toolchain command in a bubblewrap sandbox.

    `extra_binds` is an iterable of paths to additionally `--ro-bind`
    into the sandbox (used by the DOS path for the ia16-elf tree).
    `env` is a dict of extra environment variables to pass via
    `--setenv` (used to set LD_LIBRARY_PATH for ia16-elf-as, which
    has no embedded rpath).
    """
    bwrap = [
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
    ]
    for p in extra_binds:
        bwrap += ["--ro-bind", p, p]
    for k, v in (env or {}).items():
        bwrap += ["--setenv", k, v]
    bwrap += [
        "--bind", tmpdir, tmpdir,
        "--dev", "/dev",
        "--chdir", tmpdir,
    ]
    return bwrap + cmd


# --- Post-compile sanity checks on DOS viewers --------------------------
# Defence-in-depth on top of the bwrap sandbox. The sandbox stops the
# *compile* from escaping the host; these checks stop a *successful*
# compile that produced something other than a png2amiga viewer from
# being shipped back to a client. The API is still effectively a
# generic "POST C, get .exe" service, but legitimate use is bounded to
# the small allowlist of helper functions that src/cheader_dos_c.cpp's
# kPreamble emits, and the size envelope of a real viewer (a few KB
# to ~200 KB for VGA-13h with 256 colors of palette + 64 KB plane).

# Functions src/cheader_dos_c.cpp may legitimately put in the .o:
# kPreamble helpers + main + the per-mode blit_seg / blit_seg_far.
# Anything else is suspect.
_VIEWER_FUNC_ALLOWLIST = frozenset((
    "main",
    "set_mode", "cga_palette",
    "outb", "inb", "inb_discard", "outw",
    "wait_key",
    "blit_b800", "blit_seg", "blit_seg_far",
))
# Data symbols follow sanitize_symbol's output. Per src/cheader_dos_c
# .cpp, the generator may emit any of:
#   <sym>_data         CGA-320 / cga-text linear frame
#   <sym>_data[01]     cga-composite banked pair
#   <sym>_plane[0-3]   EGA / VGA planar
#   <sym>_plane[0-3]h[01]
#                      VGA-10h / VGA-12h half-bank split for planes
#                      that exceed 64 KB
#   <sym>_planes       pointer table to the four planes
#   <sym>_palette      CGA-320 / EGA palette
#   <sym>_dac          VGA DAC (256×3 bytes)
# `sym` is the alnum+underscore output of sanitize_symbol(), so the
# anchor on a leading [A-Za-z_] is enforced.
_VIEWER_DATA_RE = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_]*_"
    r"(data[01]?|dac|palette|plane[0-3](h[01])?|planes)$"
)
# Literal data symbols the generator hardcodes (not user-derived).
# Adding them by exact name rather than widening the regex with a
# generic `_init` suffix that would also match user-controllable
# `<sym>_init` from a future encoder.
_VIEWER_DATA_LITERALS = frozenset(("crtc_init",))
# Symbols we expect from the C runtime / compiler intrinsics — these
# are emitted by ia16-elf-gcc / newlib's crt0 even for a hello-world
# and aren't viewer-source-controllable.
_RUNTIME_SYMBOL_PREFIXES = ("__", "_GLOBAL_OFFSET_TABLE_")

# MZ header field bounds. A real png2amiga viewer is at least the size
# of kPreamble + main + the smallest image data, and at most a couple
# of MB with a comfortable margin over the largest legitimate output
# (VGA-10h with 4×64KB plane halves ≈ 256KB).
_MZ_MIN_BYTES = 512        # smallest possible MZ (one block)
_MZ_MAX_BYTES = 2 * 1024 * 1024


def _verify_viewer_symbols(obj_path):
    """Refuse a compiled viewer whose .o contains any function symbol
    not on the kPreamble allowlist or any data symbol that doesn't
    match the generator's naming pattern. Catches "POST arbitrary C,
    get an .exe" abuse on top of what the sandbox already blocks.
    """
    out = subprocess.run(
        [IA16_NM, "--defined-only", obj_path],
        check=True, capture_output=True, timeout=5,
        env={**os.environ, "LD_LIBRARY_PATH": IA16_LIB},
    ).stdout.decode("utf-8", errors="replace")
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        sym_type, name = parts[1], parts[2]
        if name.startswith(_RUNTIME_SYMBOL_PREFIXES):
            continue
        # Capital = global, lowercase = local — accept either; the
        # source itself is what we're vetting, not visibility.
        t = sym_type.upper()
        if t == "T":  # text (function)
            if name not in _VIEWER_FUNC_ALLOWLIST:
                raise ValueError(
                    f"unexpected function symbol: {name!r} "
                    "(only kPreamble helpers + main are allowed)"
                )
        elif t in ("D", "R", "B"):  # data / rodata / bss
            if name not in _VIEWER_DATA_LITERALS and not _VIEWER_DATA_RE.match(name):
                raise ValueError(
                    f"unexpected data symbol: {name!r} "
                    "(must match <sym>_(data|dac|palette|planeN|planes) "
                    "or be a known generator literal)"
                )
        # ignore U (undefined — pulled in from libc) and N (debug)


def _verify_mz_geometry(exe_path):
    """Sanity-check the MZ header. Refuses anything whose advertised
    file size doesn't match the actual file size (catches truncation
    or post-compile tampering), or anything outside the size envelope
    of a real png2amiga viewer (catches a runaway compile producing a
    pathological binary that still managed to link).
    """
    actual = os.path.getsize(exe_path)
    if actual < _MZ_MIN_BYTES:
        raise ValueError(f"MZ too small: {actual} bytes")
    if actual > _MZ_MAX_BYTES:
        raise ValueError(f"MZ too large: {actual} bytes")
    with open(exe_path, "rb") as f:
        hdr = f.read(28)
    if len(hdr) < 28 or hdr[:2] != b"MZ":
        raise ValueError("not an MZ binary (missing magic)")
    last_block = int.from_bytes(hdr[2:4], "little")
    blocks     = int.from_bytes(hdr[4:6], "little")
    relocs     = int.from_bytes(hdr[6:8], "little")
    hdr_paras  = int.from_bytes(hdr[8:10], "little")
    # MZ size convention: blocks*512 minus (512-last_block) when
    # last_block ≠ 0; full blocks*512 when last_block == 0.
    advertised = blocks * 512 - ((512 - last_block) if last_block else 0)
    # The .exe may be padded to a sector by the linker — the header
    # describes the *useful* size, not the on-disk size. Allow padding
    # up to one sector; refuse mismatches larger than that.
    if not (advertised <= actual <= advertised + 512):
        raise ValueError(
            f"MZ size mismatch: header {advertised}, file {actual}"
        )
    # Header should fit before the first byte of code/data. Standard
    # MZ has 2 ≤ hdr_paras ≤ ~32 (paragraph = 16 bytes). Anything
    # outside that is suspect.
    if not (2 <= hdr_paras <= 32):
        raise ValueError(f"MZ header_paragraphs out of range: {hdr_paras}")
    # Relocation count: a real viewer has tens to a few hundred
    # 16-bit far/near relocs. > 64k is impossible (MZ header field
    # is 16-bit anyway, but be explicit).
    if relocs > 8192:
        raise ValueError(f"MZ relocation count out of range: {relocs}")


def compile_dos_viewer(source_code):
    """Compile a generated DOS viewer .c source to a real-mode 8086+ .exe.

    Uses ia16-elf-gcc (TK Chia's PPA build, extracted under
    service/toolchain/dos/ia16/). The generated viewer code (see
    src/cheader_dos_c.cpp) does BIOS calls via inline asm only, so
    libi86 / dos.h aren't needed; the standard newlib crt0 is enough
    to land a `main()` and produce an MZ-format `.exe` directly via
    the binutils MZ writer.

    Same bwrap sandbox shape as the Amiga path: no network, no host
    filesystem, fresh tmpdir per request, host /usr/lib/lib64 read-
    only for the linker's host-side dependencies, the ia16 toolchain
    tree read-only on top.
    """
    if not IA16_GCC or not os.path.isfile(IA16_GCC):
        raise RuntimeError(
            "DOS toolchain missing — install via dpkg-deb -x of TK Chia's "
            "ia16-elf-* .debs into service/toolchain/dos/ia16/."
        )
    with tempfile.TemporaryDirectory(prefix="png2amiga_dos_") as tmpdir:
        src_path = os.path.join(tmpdir, "viewer.c")
        exe_path = os.path.join(tmpdir, "viewer.exe")
        obj_path = os.path.join(tmpdir, "viewer.o")
        with open(src_path, "w") as f:
            f.write(source_code)

        # -save-temps=cwd keeps the .o (ELF) alongside the .exe (MZ)
        # so we can run nm against the source-of-truth symbol table
        # before shipping the .exe. tempdir is per-request so the .o
        # is discarded with the rest at function exit.
        subprocess.run(_sandbox(
            [IA16_GCC, "-march=i80286", "-mcmodel=small", "-Os",
             "-save-temps=cwd",
             src_path, "-o", exe_path],
            tmpdir,
            extra_binds=[_DOS_ROOT],
            env={"LD_LIBRARY_PATH": IA16_LIB},
        ), check=True, capture_output=True, timeout=30)

        _verify_viewer_symbols(obj_path)
        _verify_mz_geometry(exe_path)

        with open(exe_path, "rb") as f:
            return f.read()


def compile_dos_image(source_code):
    """Compile a generated DOS viewer source and pack it into a 720 KB
    bootable FAT12 floppy .img — drop into MartyPC, real hardware, etc.

    Disk layout (FreeDOS 1.3 boot chain):
      sector 0:    boot12.bin (FreeDOS FAT12 boot sector → loads KERNEL.SYS)
      KERNEL.SYS   the 8086-compatible FreeDOS kernel (was KERNL086.SYS)
      COMMAND.COM  the FreeDOS shell
      AUTOEXEC.BAT runs VIEWER.EXE on boot
      VIEWER.EXE   the actual image displayer

    DOSBox's CGA emulation isn't accurate enough for some artifact-color
    paths; MartyPC reproduces the real chip behavior, and a self-
    bootable floppy makes the round-trip "convert → drop disk → see it"
    one click on the web side.
    """
    if not IA16_GCC or not os.path.isfile(IA16_GCC):
        raise RuntimeError(
            "DOS toolchain missing — install via dpkg-deb -x of TK Chia's "
            "ia16-elf-* .debs into service/toolchain/dos/ia16/."
        )
    for f in (DOS_BOOT12, DOS_KERNEL, DOS_COMMAND):
        if not os.path.isfile(f):
            raise RuntimeError(f"FreeDOS boot file missing: {f}")
    if not shutil.which("mformat") or not shutil.which("mcopy"):
        raise RuntimeError("mtools missing — apt install mtools")

    with tempfile.TemporaryDirectory(prefix="png2amiga_dosimg_") as tmpdir:
        src_path = os.path.join(tmpdir, "viewer.c")
        exe_path = os.path.join(tmpdir, "viewer.exe")
        img_path = os.path.join(tmpdir, "out.img")
        autoexec_path = os.path.join(tmpdir, "AUTOEXEC.BAT")

        with open(src_path, "w") as f:
            f.write(source_code)
        # AUTOEXEC.BAT uses CRLF + final CR/LF — DOS shell parsers are
        # picky about trailing line terminators. @ECHO OFF + the viewer
        # name. The viewer's main() ends with set_mode(0x03) so control
        # returns to the COMMAND prompt with text mode restored.
        with open(autoexec_path, "wb") as f:
            f.write(b"@ECHO OFF\r\nVIEWER.EXE\r\n")

        # Compile via ia16-elf-gcc (same path as compile_dos_viewer)
        # — including -save-temps so we get the .o for symbol-allowlist
        # verification.
        obj_path = os.path.join(tmpdir, "viewer.o")
        subprocess.run(_sandbox(
            [IA16_GCC, "-march=i80286", "-mcmodel=small", "-Os",
             "-save-temps=cwd",
             src_path, "-o", exe_path],
            tmpdir,
            extra_binds=[_DOS_ROOT],
            env={"LD_LIBRARY_PATH": IA16_LIB},
        ), check=True, capture_output=True, timeout=30)

        _verify_viewer_symbols(obj_path)
        _verify_mz_geometry(exe_path)

        # Create a blank 720 KB image, then mformat with the FreeDOS
        # boot sector. mformat's -B reads the file once at startup
        # before any chroot — read-binding _DOS_BOOT_DIR is enough.
        # Use Python's own truncate so this stays inside the service's
        # systemd-sandboxed process — every other subprocess in this
        # function goes through bwrap, an external `truncate` call
        # would be the lone exception that bypasses it.
        with open(img_path, "wb") as f:
            f.truncate(737280)
        subprocess.run(_sandbox(
            ["mformat", "-i", img_path, "-f", "720", "-B", DOS_BOOT12, "::"],
            tmpdir,
            extra_binds=[_DOS_BOOT_DIR],
        ), check=True, capture_output=True, timeout=10)

        # Copy KERNEL.SYS FIRST — old FreeDOS boot sectors expect it at
        # cluster 2 (start of data area). Modern boot12.bin scans the
        # root dir, so order matters less, but the convention is cheap
        # to honour and bullet-proof. `-D o` overwrites if the file
        # already exists; `-n` skips the "already exists" prompt.
        for src, dst in (
            (DOS_KERNEL,   "::KERNEL.SYS"),
            (DOS_COMMAND,  "::COMMAND.COM"),
            (autoexec_path,"::AUTOEXEC.BAT"),
            (exe_path,     "::VIEWER.EXE"),
        ):
            subprocess.run(_sandbox(
                ["mcopy", "-i", img_path, "-D", "o", src, dst],
                tmpdir,
                extra_binds=[_DOS_BOOT_DIR],
            ), check=True, capture_output=True, timeout=10)

        # KERNEL.SYS is conventionally marked system+hidden+read-only on
        # FreeDOS install floppies. Not strictly required for boot, but
        # matches stock layouts so a `dir` from the prompt looks normal.
        subprocess.run(_sandbox(
            ["mattrib", "-i", img_path, "+s", "+h", "+r", "::KERNEL.SYS"],
            tmpdir,
        ), check=True, capture_output=True, timeout=10)

        with open(img_path, "rb") as f:
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
            "-isystem", os.path.join(GCC_DIR, "lib", "gcc", "m68k-amiga-elf", "15.1.0", "include"),  # gcc builtins (stddef.h etc.)
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
        if fmt not in ("exe", "adf", "dos-exe", "dos-img"):
            self.send_error(400, "format must be exe | adf | dos-exe | dos-img")
            return

        content_length = int(self.headers.get("Content-Length", 0))
        if content_length == 0 or content_length > MAX_BODY:
            self.send_error(400, "Invalid content length")
            return

        source = self.rfile.read(content_length).decode("utf-8")

        try:
            if fmt == "dos-exe":
                result = compile_dos_viewer(source)
            elif fmt == "dos-img":
                result = compile_dos_image(source)
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
            if _DOS_ROOT:
                stderr = stderr.replace(_DOS_ROOT, "<toolchain>")
            stderr = _TMPDIR_RE.sub("<work>", stderr)
            self.wfile.write(f"Compile error:\n{stderr}".encode())
            return
        except Exception as e:
            self.send_response(500)
            self._cors_headers()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"Internal error")
            return

        # Defence-in-depth output cap. Legitimate outputs are bounded
        # by the cross-compiler / linker behaviour and the 30 s + 512 M
        # ceiling on the compile step; this catches pathological cases
        # that slip through (e.g. a future toolchain that produces a
        # giant binary inside the time budget) before they go on the
        # wire. 8 MB > worst legitimate (880 KB adf / 720 KB img).
        if len(result) > MAX_OUTPUT:
            self.send_response(500)
            self._cors_headers()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"Output too large")
            return

        ext = {"adf": "adf", "exe": "exe", "dos-exe": "exe", "dos-img": "img"}.get(fmt, "bin")
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

    # Soft requirement for DOS — service stays up if missing, returns 503
    # to /api/compile?format=dos-exe requests.
    if not IA16_GCC or not os.path.isfile(IA16_GCC):
        print(f"Warning: ia16-elf-gcc not found at {IA16_GCC or '<unset>'} — "
              "DOS .exe compilation disabled", file=sys.stderr)

    if not shutil.which("bwrap"):
        print("Error: bubblewrap (bwrap) not found. apt install bubblewrap",
              file=sys.stderr)
        sys.exit(1)

    server = HTTPServer(("127.0.0.1", PORT), CompileHandler)
    print(f"Compile server listening on http://127.0.0.1:{PORT} (sandboxed)",
          file=sys.stderr)
    server.serve_forever()


if __name__ == "__main__":
    main()
