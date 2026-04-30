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

# DOS server-side compile path (DJGPP + CWSDPMI) was removed when the
# generator switched to ia16-elf-gcc (see src/cheader_dos_c.cpp). The
# .c source the API now returns is compiled locally by the user with
# `ia16-elf-gcc -march=i80286 -mcmodel=small -Os -o foo.exe foo.c`.

PORT = int(os.environ.get("PORT", "3001"))
MAX_BODY = 5 * 1024 * 1024  # 5MB max source size


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
        if fmt not in ("exe", "adf"):
            self.send_error(400, "format must be exe | adf")
            return

        content_length = int(self.headers.get("Content-Length", 0))
        if content_length == 0 or content_length > MAX_BODY:
            self.send_error(400, "Invalid content length")
            return

        source = self.rfile.read(content_length).decode("utf-8")

        try:
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

        ext = {"adf": "adf", "exe": "exe"}.get(fmt, "bin")
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

    server = HTTPServer(("127.0.0.1", PORT), CompileHandler)
    print(f"Compile server listening on http://127.0.0.1:{PORT} (sandboxed)",
          file=sys.stderr)
    server.serve_forever()


if __name__ == "__main__":
    main()
