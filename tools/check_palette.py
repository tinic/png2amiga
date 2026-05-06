#!/usr/bin/env python3
"""
ctest helper: runs png2amiga and asserts specific palette slots in the
emitted IFF's CMAP chunk.

Usage:
    check_palette.py --bin <png2amiga> --in <input.png> --out <output.iff>
        [--expect IDX=RRGGBB] ... [--expect-not-zero IDX] ...
        [--expect-unique-min N] -- <encoder args>...

Each --expect IDX=RRGGBB asserts the slot's exact CMAP bytes. Use
--expect-not-zero to assert a slot is not all-zero (i.e., the fill
loop actually placed a colour there — catches the trailing-black bug
without pinning to a specific quantizer-dependent colour). Use
--expect-unique-min N to assert at least N distinct CMAP entries
(loose check that the palette isn't degenerate).

Exit codes:
    0  all assertions passed
    1  encoder failed
    2  output file missing
    3  no CMAP chunk in output
    4  one or more assertions failed
"""

from __future__ import annotations
import argparse
import struct
import subprocess
import sys
from pathlib import Path


def read_cmap(path: Path) -> list[tuple[int, int, int]] | None:
    """Return list of (r, g, b) tuples from the CMAP chunk, or None if absent."""
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"FORM":
        return None
    i = 12  # skip FORM<size>ILBM
    while i + 8 <= len(data):
        tag = data[i:i + 4]
        size = struct.unpack(">I", data[i + 4:i + 8])[0]
        if i + 8 + size > len(data):
            return None
        if tag == b"CMAP":
            payload = data[i + 8:i + 8 + size]
            entries: list[tuple[int, int, int]] = []
            for k in range(0, len(payload) - 2, 3):
                entries.append((payload[k], payload[k + 1], payload[k + 2]))
            return entries
        i += 8 + size + (size & 1)  # skip + pad to even
    return None


def parse_hex(s: str) -> tuple[int, int, int]:
    s = s.lower().lstrip("#")
    if len(s) != 6:
        raise ValueError(f"expected RRGGBB, got '{s}'")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--in", dest="input_path", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--expect", action="append", default=[],
                    help="IDX=RRGGBB — assert exact CMAP slot colour")
    ap.add_argument("--expect-not-zero", action="append", default=[], type=int,
                    help="IDX — assert slot is not (0,0,0)")
    ap.add_argument("--expect-unique-min", type=int, default=0,
                    help="N — assert at least N distinct CMAP entries")
    ap.add_argument("encoder_args", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    enc_args = args.encoder_args[1:] if args.encoder_args[:1] == ["--"] \
                                     else args.encoder_args
    cmd = [args.bin, *enc_args, args.input_path, args.out]
    print(f"[check_palette] {' '.join(cmd)}", file=sys.stderr)
    rc = subprocess.run(cmd, capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stderr)
        sys.stderr.write(f"[check_palette] encoder exit {rc.returncode}\n")
        return 1

    out_path = Path(args.out)
    if not out_path.exists():
        sys.stderr.write(f"[check_palette] output missing: {out_path}\n")
        return 2

    cmap = read_cmap(out_path)
    if cmap is None:
        sys.stderr.write(f"[check_palette] no CMAP in {out_path}\n")
        return 3

    fails: list[str] = []
    for spec in args.expect:
        if "=" not in spec:
            fails.append(f"--expect needs IDX=RRGGBB, got '{spec}'")
            continue
        idx_s, hx = spec.split("=", 1)
        idx = int(idx_s)
        want = parse_hex(hx)
        if idx >= len(cmap):
            fails.append(f"slot {idx} out of range (CMAP has {len(cmap)})")
            continue
        got = cmap[idx]
        if got != want:
            fails.append(
                f"slot {idx}: want #{want[0]:02x}{want[1]:02x}{want[2]:02x}, "
                f"got #{got[0]:02x}{got[1]:02x}{got[2]:02x}")
    for idx in args.expect_not_zero:
        if idx >= len(cmap):
            fails.append(f"slot {idx} out of range (CMAP has {len(cmap)})")
            continue
        if cmap[idx] == (0, 0, 0):
            fails.append(f"slot {idx}: expected non-zero, got #000000")
    if args.expect_unique_min > 0:
        unique = len(set(cmap))
        if unique < args.expect_unique_min:
            fails.append(
                f"unique colours: want >= {args.expect_unique_min}, "
                f"got {unique}")
    if fails:
        sys.stderr.write(f"[check_palette] {len(fails)} failure(s):\n")
        for f in fails:
            sys.stderr.write(f"  • {f}\n")
        sys.stderr.write("CMAP dump:\n")
        for k, c in enumerate(cmap):
            sys.stderr.write(f"  {k:3}: #{c[0]:02x}{c[1]:02x}{c[2]:02x}\n")
        return 4

    print(f"[check_palette] OK ({len(args.expect)} exact, "
          f"{len(args.expect_not_zero)} not-zero, "
          f"{args.expect_unique_min} unique-min)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
