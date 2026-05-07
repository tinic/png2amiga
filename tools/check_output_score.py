#!/usr/bin/env python3
"""Run png2amiga; assert SSIMULACRA2 score on the encoded preview is at
least the supplied threshold.

Replaces check_output_hash.py for tests where byte-equality of the
encoded output drifts across platforms (median-cut k-means thread-order
nondeterminism, FP rounding in HAM beam search, etc.). The encoder
prints "S2: <float>" on its final summary line — that's the SSIMULACRA2
score of the displayed preview vs. the source image, computed with the
same in-process port the --best ranker uses, so it is the actual
per-mode quality number we want to gate on.

We assert score >= threshold (one-sided): regressions fail; tools that
get smarter and score higher don't trip the test.

Usage:
    check_output_score.py --cli ./png2amiga --in src.png --out /tmp/x.iff \\
        --score-min 70.0 -- --mode ham6
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile


_S2_RE = re.compile(r"S2:\s*(-?\d+(?:\.\d+)?)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True, help="path to png2amiga binary")
    ap.add_argument("--in", dest="src", required=True, help="input image")
    ap.add_argument("--out", dest="out", required=True,
                    help="output path (extension picks the format)")
    ap.add_argument("--score-min", type=float, default=None,
                    help="minimum SSIMULACRA2 score for PASS")
    ap.add_argument("--print-score", action="store_true",
                    help="don't compare; just print the parsed score")
    ap.add_argument("forward", nargs=argparse.REMAINDER,
                    help="args to forward to png2amiga (after --)")
    args = ap.parse_args()

    forward = args.forward or []
    if forward and forward[0] == "--":
        forward = forward[1:]

    # Use a temp-dir so concurrent ctests don't fight over the path.
    with tempfile.TemporaryDirectory() as td:
        out_path = os.path.join(td, os.path.basename(args.out))
        cmd = [args.cli, *forward, args.src, out_path]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        except FileNotFoundError as e:
            print(f"FAIL - cli not found: {e}", file=sys.stderr)
            return 2
        if r.returncode != 0:
            print(f"FAIL - png2amiga exited {r.returncode}", file=sys.stderr)
            print(r.stderr, file=sys.stderr)
            return 1
        if not os.path.exists(out_path):
            print(f"FAIL - output file not produced: {out_path}",
                  file=sys.stderr)
            return 1
        size = os.path.getsize(out_path)
        if size == 0:
            print(f"FAIL - output file empty: {out_path}", file=sys.stderr)
            return 1

    matches = _S2_RE.findall(r.stdout)
    if not matches:
        print("FAIL - no 'S2:' line in stdout", file=sys.stderr)
        print(r.stdout, file=sys.stderr)
        return 1
    score = float(matches[-1])

    if args.print_score:
        print(f"{score:.4f}")
        return 0

    if args.score_min is None:
        print("FAIL - --score-min required (or pass --print-score to capture)",
              file=sys.stderr)
        return 2
    if score < args.score_min:
        print(f"FAIL - S2 {score:.4f} < threshold {args.score_min:.4f}",
              file=sys.stderr)
        print(f"  cmd: {' '.join(cmd)}", file=sys.stderr)
        return 1
    print(f"PASS - S2 {score:.4f} >= {args.score_min:.4f}  "
          f"({os.path.basename(args.out)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
