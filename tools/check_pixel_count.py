#!/usr/bin/env python3
"""Run the encoder, then assert the output PNG meets per-pixel constraints:

    --expect-unique-colors N   exact count of unique RGB pixels
    --expect-all-color RRGGBB  every pixel equals this colour
    --expect-no-color RRGGBB   no pixel equals this colour

Pairs with the existing reserve-* / reserve-indep-* ctest harnesses; this one
covers cases where the invariant lives in the rendered image rather than the
CMAP. Used by `reserve-render-*` ctests."""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_hex(s: str) -> tuple[int, int, int]:
    s = s.lstrip("#")
    if len(s) != 6:
        raise SystemExit(f"--expect-*-color: need RRGGBB hex, got {s!r}")
    return int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--in", dest="in_path", required=True)
    ap.add_argument("--expect-unique-colors", type=int, default=None)
    ap.add_argument("--expect-all-color", type=str, default=None)
    ap.add_argument("--expect-no-color", type=str, default=None)
    args, extra = ap.parse_known_args()
    if extra and extra[0] == "--":
        extra = extra[1:]

    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        sys.stderr.write("missing pillow / numpy\n")
        return 2

    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "out.png"
        cmd = [args.bin] + extra + [args.in_path, str(out)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(f"encoder failed:\n{r.stderr}")
            return 2
        img = np.array(Image.open(out).convert("RGB"))
        flat = img.reshape(-1, 3)

        ok = True

        if args.expect_unique_colors is not None:
            n = len(set(map(tuple, flat.tolist())))
            if n != args.expect_unique_colors:
                print(f"FAIL: unique colors = {n}, expected "
                      f"{args.expect_unique_colors}")
                ok = False
            else:
                print(f"  ✓ unique colors = {n}")

        if args.expect_all_color is not None:
            target = parse_hex(args.expect_all_color)
            if not (img == np.array(target)).all():
                # Find one outlier for the message
                mask = np.any(img != np.array(target), axis=2)
                count = int(mask.sum())
                print(f"FAIL: not all pixels {target} ({count} differ)")
                ok = False
            else:
                print(f"  ✓ all pixels = {target}")

        if args.expect_no_color is not None:
            target = parse_hex(args.expect_no_color)
            mask = np.all(img == np.array(target), axis=2)
            count = int(mask.sum())
            if count > 0:
                print(f"FAIL: {count} pixels are {target} (should be 0)")
                ok = False
            else:
                print(f"  ✓ no pixel is {target}")

        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
