#!/usr/bin/env python3
"""Smooth dither on a generated gradient: less banding and a closer blurred
tone than Floyd-Steinberg, in plain, sliced and strips modes; on strips no
boundary column further off than Floyd-Steinberg's, for Smooth and DBS;
rejected on HAM.

Prints key=value lines; exit status 0 on success.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image


def lin(a):
    a = a / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def oklab(c):
    m1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                   [0.2119034982, 0.6806995451, 0.1073969566],
                   [0.0883024619, 0.2817188376, 0.6299787005]])
    m2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                   [1.9779984951, -2.4285922050, 0.4505937099],
                   [0.0259040371, 0.7827717662, -0.8086757660]])
    return np.cbrt(c @ m1.T) @ m2.T


def blur(a, s):
    r = int(np.ceil(3 * s))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / s) ** 2)
    k /= k.sum()
    p = np.pad(a, ((r, r), (r, r), (0, 0)), mode='edge')
    p = sum(k[i] * p[:, i:i + a.shape[1]] for i in range(2 * r + 1))
    return sum(k[i] * p[i:i + a.shape[0]] for i in range(2 * r + 1))


def gradient(path):
    """Orange-to-violet band over a slow grey ramp: desktop-background style."""
    y, x = np.mgrid[0:256, 0:320].astype(float)
    t = (y / 119)[..., None]
    img = (1 - t) * np.array([255, 140, 40]) + t * np.array([90, 40, 140])
    img[120:] = (110 + x[120:] / 319 * 30)[..., None]
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(path)


def metrics(source, preview):
    out = Image.open(preview).convert('RGB')
    out = out.resize((out.width // 2, out.height // 2), Image.NEAREST)  # 2x preview
    s = lin(np.asarray(Image.open(source).convert('RGB'), float))
    o = lin(np.asarray(out, float))
    blur_err = np.linalg.norm(oklab(blur(s, 1)) - oklab(blur(o, 1)), axis=2).mean()
    # Banding: second differences of the sigma-2-blurred lightness beyond the source's.
    def d2(a):
        return np.abs(a[1:-1, 2:] - 2 * a[1:-1, 1:-1] + a[1:-1, :-2]) + \
            np.abs(a[2:, 1:-1] - 2 * a[1:-1, 1:-1] + a[:-2, 1:-1])
    ls, lo = oklab(blur(s, 2))[..., 0], oklab(blur(o, 2))[..., 0]
    band = np.maximum(d2(lo) - d2(ls), 0).mean() * 1000
    # Worst column of the grey ramp (rows 120+), blurred, against the source:
    # strip boundaries show up as single columns.
    cs = oklab(blur(s, 1))[120:].mean(0)
    co = oklab(blur(o, 1))[120:].mean(0)
    column = np.linalg.norm(co - cs, axis=1)[3:-3].max() * 1000
    return blur_err, band, column


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True)
    cli = parser.parse_args().cli
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        src = tmp / 'gradient.png'
        gradient(src)
        modes = {'plain': ['lores', '--depth', '5'],
                 'sliced': ['lores', '--depth', '4', '--sliced'],
                 'strips': ['lores', '--depth', '4', '--strips'],
                 'ehb-strips': ['ehb', '--strips']}
        for name, flags in modes.items():
            result = {}
            for d in ('floyd-steinberg', 'smooth') + (('dbs',) if 'strips' in name else ()):
                out = tmp / f'{name}-{d}.png'
                proc = subprocess.run([cli, str(src), '--mode', *flags,
                                       '--dither', d, '-o', str(out)],
                                      capture_output=True, text=True)
                if proc.returncode:
                    print(f'error=cli {name} {d} exit {proc.returncode}: {proc.stderr.strip()}')
                    return 1
                result[d] = metrics(src, out)
                print(f'{name}_{d}_blur_err={result[d][0]:.4f} {name}_{d}_band={result[d][1]:.3f} '
                      f'{name}_{d}_column={result[d][2]:.1f}')
            if result['smooth'][1] >= result['floyd-steinberg'][1]:
                failures.append(f'{name}: smooth bands as much as floyd-steinberg')
            if result['smooth'][0] >= result['floyd-steinberg'][0]:
                failures.append(f'{name}: smooth tone no closer than floyd-steinberg')
            for d in ('smooth', 'dbs') if 'strips' in name else ():
                if result[d][2] > result['floyd-steinberg'][2] + 0.5:
                    failures.append(f'{name}: {d} strip boundary column worse than floyd-steinberg')
        proc = subprocess.run([cli, str(src), '--mode', 'ham6', '--dither', 'smooth',
                               '-o', str(tmp / 'ham.png')], capture_output=True, text=True)
        print(f'ham_rejected={int(proc.returncode != 0)}')
        if proc.returncode == 0:
            failures.append('smooth accepted on HAM')
    print(f'failures={len(failures)}')
    for f in failures:
        print(f'failure={f}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
