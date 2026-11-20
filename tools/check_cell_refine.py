#!/usr/bin/env python3
"""Check opt-in cell refinement gains and its whole-image S2 fallback."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cli', required=True)
p.add_argument('--mode', required=True)
p.add_argument('--input', required=True)
p.add_argument('--min-gain', type=float, default=0)
p.add_argument('--best', action='store_true', help='Compare refined vs best+refined')
a = p.parse_args()
with tempfile.TemporaryDirectory() as folder:
    scores = []
    for refine in (False, True):
        cmd = [a.cli, '--mode', a.mode]
        if a.best:
            cmd.append('--cell-refine')
            if refine:
                cmd.append('--best')
        elif refine:
            cmd.append('--cell-refine')
        cmd += [a.input, str(Path(folder) / f'{refine}.png')]
        r = subprocess.run(cmd, capture_output=True, text=True, check=True)
        scores.append(float(re.findall(r'S2:\s*(-?\d+(?:\.\d+)?)', r.stdout + r.stderr)[-1]))
    delta = scores[1] - scores[0]
    if delta + 0.011 < a.min_gain:
        raise SystemExit(f'S2 gain {delta:.2f} below {a.min_gain}: {scores}')
    print(f'PASS: S2 {scores[0]:.2f} -> {scores[1]:.2f} ({delta:+.2f})')
