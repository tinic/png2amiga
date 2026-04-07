#!/usr/bin/env python3
"""Compile service: receives png2amiga viewer .cpp source, returns .exe or .adf."""

import os
import subprocess
import tempfile
import shutil
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import sys
import platform

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

# Toolchain: service/toolchain/ (deployed) or third_party/vscode-amiga-debug/ (dev)
_LOCAL_TC = os.path.join(SCRIPT_DIR, "toolchain")
_PROJECT_TC = os.path.join(PROJECT_ROOT, "third_party", "vscode-amiga-debug")
_TC_ROOT = _LOCAL_TC if os.path.isdir(_LOCAL_TC) else _PROJECT_TC
TOOLCHAIN = os.path.join(_TC_ROOT, "bin")
TEMPLATE = os.path.join(_TC_ROOT, "template")
SUPPORT = os.path.join(TEMPLATE, "support")

# Detect platform
SYSTEM = platform.system()
if SYSTEM == "Darwin":
    PLATFORM = "darwin"
elif SYSTEM == "Linux":
    PLATFORM = "linux"
else:
    PLATFORM = "win32"

GCC = os.path.join(TOOLCHAIN, PLATFORM, "opt", "bin", "m68k-amiga-elf-gcc")
ELF2HUNK = os.path.join(TOOLCHAIN, PLATFORM, "elf2hunk")
EXE2ADF = os.path.join(TOOLCHAIN, PLATFORM, "exe2adf")

PORT = int(os.environ.get("PORT", "3001"))


def compile_viewer(source_code, output_format="exe"):
    """Compile viewer source to .exe or .adf. Returns binary bytes."""
    with tempfile.TemporaryDirectory(prefix="png2amiga_") as tmpdir:
        src_path = os.path.join(tmpdir, "viewer.cpp")
        elf_path = os.path.join(tmpdir, "viewer.elf")
        exe_path = os.path.join(tmpdir, "viewer.exe")

        # Write source
        with open(src_path, "w") as f:
            f.write(source_code)

        # Compile
        subprocess.run([
            GCC, "-m68000", "-Ofast", "-nostdlib",
            "-fomit-frame-pointer", "-fno-exceptions",
            "-I", TEMPLATE,
            "-Wa,--register-prefix-optional",
            "-Wl,--emit-relocs,-Ttext=0",
            "-o", elf_path,
            src_path,
            os.path.join(SUPPORT, "gcc8_a_support.s"),
            os.path.join(SUPPORT, "gcc8_c_support.c"),
        ], check=True, capture_output=True, timeout=30)

        # elf2hunk
        subprocess.run([
            ELF2HUNK, elf_path, exe_path,
        ], check=True, capture_output=True, timeout=10)

        if output_format == "adf":
            adf_path = os.path.join(tmpdir, "viewer.adf")
            subprocess.run([
                EXE2ADF, "-i", exe_path, "-a", adf_path, "-l", "png2amiga",
            ], check=True, capture_output=True, timeout=10)
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
            self.send_error(400, "format must be exe or adf")
            return

        content_length = int(self.headers.get("Content-Length", 0))
        if content_length == 0 or content_length > 50 * 1024 * 1024:
            self.send_error(400, "Invalid content length")
            return

        source = self.rfile.read(content_length).decode("utf-8")

        try:
            result = compile_viewer(source, fmt)
        except subprocess.CalledProcessError as e:
            self.send_response(500)
            self._cors_headers()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            stderr = e.stderr.decode("utf-8", errors="replace") if e.stderr else str(e)
            self.wfile.write(f"Compile error:\n{stderr}".encode())
            return
        except Exception as e:
            self.send_response(500)
            self._cors_headers()
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(str(e).encode())
            return

        content_type = "application/octet-stream"
        ext = "adf" if fmt == "adf" else "exe"

        self.send_response(200)
        self._cors_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Disposition", f'attachment; filename="viewer.{ext}"')
        self.send_header("Content-Length", str(len(result)))
        self.end_headers()
        self.wfile.write(result)

    def _cors_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def log_message(self, format, *args):
        print(f"[compile] {args[0]}", file=sys.stderr)


def main():
    # Verify toolchain
    for tool, path in [("gcc", GCC), ("elf2hunk", ELF2HUNK), ("exe2adf", EXE2ADF)]:
        if not os.path.isfile(path):
            print(f"Error: {tool} not found at {path}", file=sys.stderr)
            sys.exit(1)

    server = HTTPServer(("127.0.0.1", PORT), CompileHandler)
    print(f"Compile server listening on http://127.0.0.1:{PORT}", file=sys.stderr)
    server.serve_forever()


if __name__ == "__main__":
    main()
