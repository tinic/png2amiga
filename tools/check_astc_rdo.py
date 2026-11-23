#!/usr/bin/env python3
"""ASTC --rdo checks. One check per invocation, selected by --check:

  off-identical  --rdo 0 writes the same file as no --rdo option.
  decode         with --rdo on, the encoder's predicted decode equals the
                 astcenc decode (astc_predict_mismatch=0) and ktx2_to_png
                 decodes the file.
  smaller        with --rdo on, the zstd-compressed block data is smaller
                 than with --rdo off (the .ktx2 level data is zstd -19), by
                 at least the factor --max-ratio.

--input tiled encodes a generated image instead of a file: a 64 x 64 tile
of smooth colour noise, repeated 4 x 4 times. Its blocks repeat far apart
in the stream, out of reach of the neighbour donors.

Prints key=value lines; exit status 0 on success.
"""
import argparse
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zlib


def write_tiled_png(path, tile=64, reps=4):
    """A tile of smooth colour noise, repeated reps x reps times (RGB8)."""
    def h(x, y, s):
        n = (x * 374761393 + y * 668265263 + s * 2147483647) & 0xFFFFFFFF
        n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
        return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0

    def px(x, y):
        out = []
        m = tile // 8
        gx, gy, fx, fy = x // 8, y // 8, (x % 8) / 8.0, (y % 8) / 8.0
        for c in range(3):
            v = 0.5 + 0.22 * math.sin(2 * math.pi * (x * (c + 1) + y * 2) / tile + c)
            v += 0.18 * math.sin(2 * math.pi * (x * 3 - y * (c + 2)) / tile + 2 * c)
            # Value noise on an 8-texel lattice, periodic in the tile.
            a, b = h(gx % m, gy % m, c), h((gx + 1) % m, gy % m, c)
            d, e = h(gx % m, (gy + 1) % m, c), h((gx + 1) % m, (gy + 1) % m, c)
            v += 0.35 * ((a * (1 - fx) + b * fx) * (1 - fy) + (d * (1 - fx) + e * fx) * fy - 0.5)
            v += 0.06 * (h(x, y, c + 7) - 0.5)
            out.append(max(0, min(255, int(v * 255 + 0.5))))
        return bytes(out)

    rows = [b"".join(px(x, y) for x in range(tile)) * reps for y in range(tile)]
    raw = b"".join(b"\0" + rows[y % tile] for y in range(tile * reps))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    n = tile * reps
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


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
    # smaller: zstd bytes with --rdo on must be below this share of the
    # bytes with --rdo off.
    ap.add_argument("--max-ratio", type=float, default=1.0)
    a = ap.parse_args()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        if a.input == "tiled":
            a.input = str(td / "tiled.png")
            write_tiled_png(a.input)
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
            print(f"zstd_ratio={n1 / max(n0, 1):.4f}")
            print(f"zstd_ratio_limit={a.max_ratio:.4f}")
            ok = s0 == 2 and s1 == 2 and n1 < n0 * a.max_ratio
    print(f"status={'ok' if ok else 'fail'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
