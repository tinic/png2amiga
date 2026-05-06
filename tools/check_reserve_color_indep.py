#!/usr/bin/env python3
"""Run the encoder twice with two different reserve colours but identical slot
masks, then assert the rendered PNG is byte-identical. Reserved slots are
supposed to be carved out of the dither candidate set AND ignored by the
swap planner's cluster math, so the colour stamped at the reserved slots
must NEVER affect any pixel the encoder writes.

Failure means a code path is reading the reserve colour somewhere it
shouldn't — past bugs were in build_swap_scratch, refine_ehb_base_palette,
strips planner inner loops, etc."""
from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path


def run(bin_path: str, in_path: str, out_path: str,
        reserve_range: str, reserve_color: str,
        extra_args: list[str]) -> None:
    args = [
        bin_path, "--mode", "lores",  # overridden by extra_args
    ]
    # extra_args carries --mode + any other flags; build the real argv.
    cmd = [bin_path]
    cmd += extra_args
    cmd += ["--reserve-range", reserve_range, reserve_color]
    cmd += [in_path, out_path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"encoder failed (rc={r.returncode}):\n")
        sys.stderr.write(r.stderr)
        sys.exit(2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="png2amiga binary")
    ap.add_argument("--in", dest="in_path", required=True, help="input image")
    ap.add_argument("--reserve-range", required=True,
                    help="slot range to reserve (e.g. 16-31)")
    ap.add_argument("--colors", nargs="+", default=["000000", "ff0000", "cacaca"],
                    help="two or more reserve colours to compare")
    ap.add_argument("--", dest="sep", action="store_true",
                    help="separator (ignored)")
    args, extra = ap.parse_known_args()
    # Strip leading "--" from extra args (argparse leaves it).
    if extra and extra[0] == "--":
        extra = extra[1:]

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        hashes: dict[str, str] = {}
        for color in args.colors:
            out = tdp / f"out_{color}.png"
            run(args.bin, args.in_path, str(out),
                args.reserve_range, color, extra)
            h = hashlib.sha256(out.read_bytes()).hexdigest()
            hashes[color] = h

        unique = set(hashes.values())
        if len(unique) == 1:
            print(f"PASS — all {len(hashes)} reserve colours produce identical PNG")
            for color, h in hashes.items():
                print(f"  {color}: {h[:16]}…")
            return 0
        else:
            print("FAIL — reserve colour leaks into encoder output:")
            for color, h in hashes.items():
                print(f"  {color}: {h[:16]}…")
            return 1


if __name__ == "__main__":
    sys.exit(main())
