#!/usr/bin/env python3
"""ASTC --rdo checks. One check per invocation, selected by --check:

  off-identical  --rdo 0 writes the same file as no --rdo option.
  decode         with --rdo on, the encoder's predicted decode equals the
                 astcenc decode (astc_predict_mismatch=0) and ktx2_to_png
                 decodes the file.
  smaller        with --rdo on, the zstd-compressed block data is smaller
                 than with --rdo off (the .ktx2 level data is zstd -19).

Prints key=value lines; exit status 0 on success.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def encode(cli, src, out, mode, width, extra):
    cmd = [cli, "--mode", mode, "--width", str(width), *extra, src, str(out)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(f"encode_exit={p.returncode}")
        sys.exit(1)
    m = re.search(r"astc_predict_mismatch=(\d+)", p.stdout + p.stderr)
    return int(m.group(1)) if m else -1


def level_bytes(path):
    """Stored length of mip level 0 (the zstd frame of the block data)."""
    d = Path(path).read_bytes()
    scheme = struct.unpack_from("<I", d, 44)[0]
    _, length, _ = struct.unpack_from("<QQQ", d, 80)
    return scheme, length


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--ktx2-to-png", default="")
    ap.add_argument("--input", required=True)
    ap.add_argument("--check", required=True, choices=["off-identical", "decode", "smaller"])
    ap.add_argument("--mode", default="astc-6x6")
    ap.add_argument("--width", type=int, default=192)
    ap.add_argument("--rdo", type=int, default=30)
    a = ap.parse_args()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        if a.check == "off-identical":
            encode(a.cli, a.input, td / "a.ktx2", a.mode, a.width, [])
            encode(a.cli, a.input, td / "b.ktx2", a.mode, a.width, ["--rdo", "0"])
            same = (td / "a.ktx2").read_bytes() == (td / "b.ktx2").read_bytes()
            print(f"rdo0_identical={int(same)}")
            ok = same
        elif a.check == "decode":
            mm = encode(a.cli, a.input, td / "r.ktx2", a.mode, a.width, ["--rdo", str(a.rdo)])
            off = encode(a.cli, a.input, td / "o.ktx2", a.mode, a.width, [])
            changed = (td / "r.ktx2").read_bytes() != (td / "o.ktx2").read_bytes()
            p = subprocess.run([a.ktx2_to_png, str(td / "r.ktx2"), str(td / "r.png")],
                               capture_output=True, text=True)
            print(f"astc_predict_mismatch={mm}")
            print(f"astc_predict_mismatch_rdo_off={off}")
            print(f"rdo_changed_output={int(changed)}")
            print(f"thirdparty_decode_exit={p.returncode}")
            ok = mm == 0 and off == 0 and changed and p.returncode == 0 and (td / "r.png").exists()
        else:
            encode(a.cli, a.input, td / "o.ktx2", a.mode, a.width, [])
            encode(a.cli, a.input, td / "r.ktx2", a.mode, a.width, ["--rdo", str(a.rdo)])
            s0, n0 = level_bytes(td / "o.ktx2")
            s1, n1 = level_bytes(td / "r.ktx2")
            print(f"supercompression_scheme={s0}")
            print(f"zstd_bytes_rdo_off={n0}")
            print(f"zstd_bytes_rdo_on={n1}")
            ok = s0 == 2 and s1 == 2 and n1 < n0
    print(f"status={'ok' if ok else 'fail'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
