#!/usr/bin/env python3
"""ctest helper: runs png2amiga with --print-palette-json and asserts
specific slots in the emitted palette.

Use for modes whose output container doesn't carry the palette
(e.g. strips DPF supports only .png / .h / .cpp), or when you want
to verify the encoder's palette without bothering with file I/O.

Usage:
    check_palette_slot.py --bin <png2amiga> --in <input.png>
        [--expect IDX=RRGGBB] ... -- <encoder args>...

Each --expect IDX=RRGGBB asserts the JSON's palette[IDX].rgb matches.
Always passes --quiet so only the JSON line hits stdout.

Exit codes:
    0  all assertions passed
    1  encoder failed
    2  no JSON parsed from stdout
    3  assertion failed
"""
from __future__ import annotations
import argparse
import json
import subprocess
import sys


def parse_hex(s: str) -> tuple[int, int, int]:
    s = s.lower().lstrip("#")
    if len(s) != 6:
        raise ValueError(f"expected RRGGBB, got '{s}'")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--in", dest="input_path", required=True)
    ap.add_argument("--expect", action="append", default=[],
                    help="IDX=RRGGBB — assert palette[IDX].rgb matches")
    ap.add_argument("encoder_args", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    cmd = [args.bin, "--quiet", "--print-palette-json"]
    cmd += args.encoder_args[1:] if args.encoder_args and args.encoder_args[0] == "--" else args.encoder_args
    cmd += [args.input_path]

    print(f"[check_palette_slot] {' '.join(cmd)}", file=sys.stderr)
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[check_palette_slot] encoder exit {res.returncode}",
              file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        return 1

    try:
        doc = json.loads(res.stdout.strip())
    except json.JSONDecodeError as e:
        print(f"[check_palette_slot] JSON parse error: {e}", file=sys.stderr)
        print(f"stdout: {res.stdout!r}", file=sys.stderr)
        return 2

    palette = doc.get("palette", [])
    failures: list[str] = []
    for spec in args.expect:
        try:
            idx_s, hex_s = spec.split("=", 1)
            idx = int(idx_s)
            want = parse_hex(hex_s)
        except (ValueError, KeyError):
            failures.append(f"bad --expect spec '{spec}'")
            continue
        if idx >= len(palette):
            failures.append(
                f"slot {idx} out of range (palette has {len(palette)} entries)")
            continue
        got_hex = palette[idx].get("rgb", "")
        try:
            got = parse_hex(got_hex)
        except ValueError:
            failures.append(f"slot {idx}: malformed rgb '{got_hex}'")
            continue
        if got != want:
            failures.append(
                f"slot {idx}: got #{got_hex.lower()}, "
                f"expected #{want[0]:02x}{want[1]:02x}{want[2]:02x}")

    for f in failures:
        print(f"[check_palette_slot] FAIL: {f}", file=sys.stderr)
    return 3 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
