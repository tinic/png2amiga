#!/usr/bin/env python3
"""A/B compare: png2amiga (main.cpp inline) vs api_pipeline_smoke (api::encode_state).

Both binaries share the underlying encoders (encode_copper, dither::apply,
strips::*), but main.cpp's amiga modes still build their option sets and
invoke encoders inline rather than going through api::run_pipeline. If
the two paths produce identical PNG output for the same inputs, the
inline pipelines can be safely replaced with api::encode_state calls
during the merge.

Strategy:
  1. Run png2amiga with the given args.
  2. Run api_pipeline_smoke with --apply-tuning + the same args.
  3. Compare native-resolution rendered PNG bytes.

The `--apply-tuning` flag mirrors what `make_api_options` does on the
CLI bridge — fills in dither_tuning::defaults_for(...) when the caller
didn't pass --dither-strength / --error-clamp explicitly.

PNGs from the CLI are NN-upscaled by save_preview's display-aspect
adjustment; the api_pipeline_smoke saves the raw rendered image. We
invert main.cpp's scale_preview_nn forward formula (`out[oy] = src[(oy *
src_h) / dst_h]`) to recover the native rendered image before
comparing — picking the FIRST output row/col that maps to each native
row/col exactly inverts the integer-truncate NN upscale."""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"$ {' '.join(cmd)}\n")
        sys.stderr.write(r.stderr)
        raise SystemExit(2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True, help="png2amiga binary")
    ap.add_argument("--api", required=True, help="api_pipeline_smoke binary")
    ap.add_argument("--in", dest="in_path", required=True)
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
        cli_out = Path(td) / "cli.png"
        api_out = Path(td) / "api.png"
        run([args.cli] + extra + [args.in_path, str(cli_out)])
        run([args.api, "--apply-tuning"] + extra + [args.in_path, str(api_out)])

        api_img = np.array(Image.open(api_out).convert("RGB"))
        cli_img = np.array(Image.open(cli_out).convert("RGB"))

        ah, aw = api_img.shape[:2]
        ch, cw = cli_img.shape[:2]
        if cw == aw and ch == ah:
            cli_native = cli_img
        elif cw % aw == 0 and ch % ah == 0:
            sx = cw // aw
            sy = ch // ah
            cli_native = cli_img[::sy, ::sx]
        else:
            # Non-integer upscale (e.g. PAR-corrected ega-hi 350 → 479).
            # Walk CLI's output rows/cols and pick the FIRST output index
            # that maps to each native index. This is the exact inverse
            # of main.cpp's scale_preview_nn forward formula
            # (out[oy] = src[(oy * src_h) / dst_h]).
            row_pick = [-1] * ah
            for oy in range(ch):
                sy = (oy * ah) // ch
                if 0 <= sy < ah and row_pick[sy] == -1:
                    row_pick[sy] = oy
            col_pick = [-1] * aw
            for ox in range(cw):
                sx = (ox * aw) // cw
                if 0 <= sx < aw and col_pick[sx] == -1:
                    col_pick[sx] = ox
            # Fill any unmatched (shouldn't happen for upsamples) with
            # the previous-seen index, so cli_native is well-defined.
            last = 0
            for sy in range(ah):
                if row_pick[sy] == -1: row_pick[sy] = last
                else: last = row_pick[sy]
            last = 0
            for sx in range(aw):
                if col_pick[sx] == -1: col_pick[sx] = last
                else: last = col_pick[sx]
            row_idx = np.array(row_pick)
            col_idx = np.array(col_pick)
            cli_native = cli_img[row_idx[:, None], col_idx[None, :]]

        if api_img.shape != cli_native.shape:
            print(f"FAIL: shapes differ — api {api_img.shape} cli {cli_native.shape}")
            return 1

        diff = np.any(api_img != cli_native, axis=2)
        n = int(diff.sum())
        if n == 0:
            print(f"PASS — byte-identical ({api_img.shape[1]}×{api_img.shape[0]})")
            return 0
        else:
            total = diff.size
            print(f"FAIL — {n} of {total} pixels differ ({n / total * 100:.2f}%)")
            return 1


if __name__ == "__main__":
    sys.exit(main())
