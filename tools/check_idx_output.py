#!/usr/bin/env python3
"""Check `-o x.idx` in Amiga modes: 1 byte per pixel, scan order, no header.

Input is a generated 37x23 image, black left of x=18 and white from x=18, so
the known bytes are fixed without dither. Prints key=value; exit 1 on failure.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image

W, H, EDGE = 37, 23, 18


def run(cli, src, out, *flags):
    subprocess.run([cli, *flags, '--width', str(W), '--height', str(H), '--dither', 'none',
                    str(src), '-o', str(out)], check=True, capture_output=True)
    return out.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True)
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory() as directory:
        d = Path(directory)
        src = d / 'split.png'
        image = Image.new('RGB', (W, H), (0, 0, 0))
        image.paste((255, 255, 255), (EDGE, 0, W, H))
        image.save(src)

        cases = {
            # name: (flags, max value + 1, expected bytes at (0,0) and (W-1,H-1))
            'lores-d1': (('--mode', 'lores', '--depth', '1'), 2, (0, 1)),
            'lores-d3': (('--mode', 'lores', '--depth', '3'), 8, None),
            'ham6': (('--mode', 'ham6'), 64, (0, None)),
            'ehb-sliced': (('--mode', 'ehb', '--sliced'), 64, None),
            'lores-strips': (('--mode', 'lores', '--depth', '3', '--sliced', '--strips'), 8,
                             None),
        }
        for name, (flags, limit, known) in cases.items():
            data = run(args.cli, src, d / f'{name}.idx', *flags)
            ok = len(data) == W * H and max(data) < limit
            if known is not None:
                first, last = known
                ok = ok and data[0] == first and (last is None or data[-1] == last)
            # Every row holds one value left of the edge and one from it on.
            if name == 'lores-d1':
                ok = ok and all(data[y * W + x] == (x >= EDGE) for y in range(H) for x in range(W))
            print(f'case={name} bytes={len(data)} expect={W * H} max={max(data)} '
                  f'first={data[0]} last={data[-1]} status={"pass" if ok else "FAIL"}')
            if not ok:
                failures.append(name)

        # -o .idx and --output-indexed write the same bytes.
        flags = ('--mode', 'lores', '--depth', '3')
        a = run(args.cli, src, d / 'o.idx', *flags, '--output-indexed', str(d / 'oi.idx'))
        b = (d / 'oi.idx').read_bytes()
        same = a == b
        print(f'case=output-indexed-equal status={"pass" if same else "FAIL"}')
        if not same:
            failures.append('output-indexed-equal')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
