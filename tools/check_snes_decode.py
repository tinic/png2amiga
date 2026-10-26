#!/usr/bin/env python3
"""SNES Mode 7 output decodes, by the hardware's rules, to png2amiga's own
preview: 256-colour (tilemap + tiles + BGR555 CGRAM) and Direct Color
(BBGGGRRR = R rrr00, G ggg00, B bb000 per 5-bit channel). The rules were
checked pixel for pixel against Mesen2 with tools/snes-mode7-viewer.

Prints key=value lines; exit status 0 on success.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image


def test_image(path):
    """Colour ramps, a grey ramp and full white: catches level and channel errors."""
    y, x = np.mgrid[0:224, 0:256].astype(float)
    img = np.zeros((224, 256, 3))
    for k in range(3):
        band = slice(k * 56, k * 56 + 56)
        img[band, :, k] = x[band] / 255 * 255
    img[168:212] = (x[168:212] / 255 * 255)[..., None]
    img[212:] = 255
    Image.fromarray(img.astype(np.uint8)).save(path)


def c5to8(c):
    return (c << 3) | (c >> 2)


def decode(bin_path, pal_path):
    b = Path(bin_path).read_bytes()  # tilemap, then the tiles in use
    if len(b) < 16384 or len(b) > 32768 or len(b) % 64:
        raise ValueError(f'{bin_path}: {len(b)} bytes')
    tmap = np.frombuffer(b[:16384], np.uint8).reshape(128, 128)
    tiles = np.zeros((256, 8, 8), np.uint8)
    tiles.reshape(-1)[:len(b) - 16384] = np.frombuffer(b[16384:], np.uint8)
    idx = tiles[tmap[:28, :32]].transpose(0, 2, 1, 3).reshape(224, 256).astype(np.int64)
    if pal_path is None:
        rgb = [(idx & 7) << 2, ((idx >> 3) & 7) << 2, ((idx >> 6) & 3) << 3]
    else:
        w = np.frombuffer(Path(pal_path).read_bytes(), '<u2').astype(np.int64)[idx]
        rgb = [w & 31, (w >> 5) & 31, (w >> 10) & 31]
    return np.stack([c5to8(c) for c in rgb], -1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True)
    cli = parser.parse_args().cli
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        src = tmp / 'ramps.png'
        test_image(src)
        for name, mode in (('256', 'snes-mode7-256'), ('direct', 'snes-mode7-direct')):
            for out in (tmp / f'{name}.bin', tmp / f'{name}.png'):
                proc = subprocess.run([cli, str(src), '--mode', mode, '-o', str(out)],
                                      capture_output=True, text=True)
                if proc.returncode:
                    print(f'error=cli {mode} exit {proc.returncode}: {proc.stderr.strip()}')
                    return 1
            hw = decode(tmp / f'{name}.bin', tmp / f'{name}.pal' if name == '256' else None)
            pv = Image.open(tmp / f'{name}.png').convert('RGB').resize((256, 224), Image.NEAREST)
            diff = hw - np.asarray(pv).astype(np.int64)
            mean_abs = np.abs(diff).mean()
            signed = [diff[..., k].mean() for k in range(3)]
            print(f'{name}_mean_abs={mean_abs:.2f} ' +
                  ' '.join(f'{name}_signed_{c}={v:.2f}' for c, v in zip('rgb', signed)))
            if mean_abs > 1.0 or max(abs(v) for v in signed) > 0.5:
                failures.append(f'{name}: preview does not match the hardware decode')
    print(f'failures={len(failures)}')
    for f in failures:
        print(f'failure={f}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
