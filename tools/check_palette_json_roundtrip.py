#!/usr/bin/env python3
"""Round-trip check for --print-palette-json pins[].

Runs png2amiga with --print-palette-json + --pin-index-at, captures
the JSON dump, then re-runs with --palette <captured.json> and
verifies the loaded JSON's pins[] match the user-supplied --pin-index-at
entries.

This pins the JSON writer ↔ JSON loader contract for the pin field, so
a regression in either side (writer drops pins[], loader silently
ignores it) is caught immediately.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile


def run(args, *, capture_stdout=True):
    return subprocess.run(
        args,
        check=True,
        stdout=subprocess.PIPE if capture_stdout else None,
        stderr=subprocess.PIPE,
        text=True,
    )


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--bin", required=True, help="path to png2amiga binary")
    p.add_argument("--in", dest="in_path", required=True,
                   help="input PNG/JPEG to encode")
    args = p.parse_args()

    pin_specs = [(5, 100, 50), (12, 200, 100)]

    cmd = [args.bin, "--quiet"]
    for idx, x, y in pin_specs:
        cmd += ["--pin-index-at", str(idx), str(x), str(y)]
    cmd += ["--print-palette-json", args.in_path]

    res = run(cmd)
    raw_json = res.stdout.strip()
    try:
        doc = json.loads(raw_json)
    except json.JSONDecodeError as e:
        print(f"FAIL: emitted JSON did not parse: {e}", file=sys.stderr)
        print(f"raw stdout:\n{raw_json}", file=sys.stderr)
        return 1

    if "pins" not in doc:
        print("FAIL: emitted JSON missing 'pins' array", file=sys.stderr)
        return 1
    emitted = [(p["idx"], p["x"], p["y"]) for p in doc["pins"]]
    if emitted != pin_specs:
        print(f"FAIL: emitted pins {emitted} != expected {pin_specs}",
              file=sys.stderr)
        return 1

    with tempfile.NamedTemporaryFile(
            "w", suffix=".json", delete=False) as f:
        f.write(raw_json)
        json_path = f.name

    try:
        # Re-encode with the captured JSON as the input palette, then
        # re-dump JSON. The pins[] section must round-trip exactly.
        res2 = run([args.bin, "--quiet",
                    "--palette", json_path,
                    "--print-palette-json", args.in_path])
        doc2 = json.loads(res2.stdout.strip())
        loaded = [(p["idx"], p["x"], p["y"]) for p in doc2.get("pins", [])]
        if loaded != pin_specs:
            print(f"FAIL: round-tripped pins {loaded} != expected {pin_specs}",
                  file=sys.stderr)
            return 1
    finally:
        os.unlink(json_path)

    return 0


if __name__ == "__main__":
    sys.exit(main())
