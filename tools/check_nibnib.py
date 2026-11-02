#!/usr/bin/env python3
"""nibnib output decodes, by the format's rules (github.com/raphlinus/nibnib
decode.rs), to png2amiga's own preview, and stays close to the source.

Decoding: per row, the first u32 holds two RGB565 pixels (low half left);
for each later pair, the last pixel is doubled into both halves and the
palette word for the next index byte is added, wrapping at 32 bits.

Prints key=value lines; exit status 0 on success.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image


def test_image(path, w, h):
    """Colour and grey ramps, hard edges and a dark gradient."""
    y, x = np.mgrid[0:h, 0:w].astype(float)
    img = np.zeros((h, w, 3))
    band = h // 6
    for k in range(3):
        img[k * band:(k + 1) * band, :, k] = x[k * band:(k + 1) * band] / (w - 1) * 255
    img[3 * band:4 * band] = (x[3 * band:4 * band] / (w - 1) * 255)[..., None]
    img[4 * band:5 * band] = (((x[4 * band:5 * band] // 8 + y[4 * band:5 * band] // 8) % 2) * 255)[..., None]
    img[5 * band:] = (x[5 * band:] / (w - 1) * 40)[..., None]
    Image.fromarray(img.astype(np.uint8)).save(path)


def stride(w):
    return (w // 2 + 3 + 3) // 4 * 4


def rgb565_to_rgb8(p):
    r, g, b = p >> 11, (p >> 5) & 63, p & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def decode(data):
    w = int.from_bytes(data[0:2], 'little')
    h = int.from_bytes(data[2:4], 'little')
    if w == 0 or w % 2:
        raise ValueError(f'bad width {w}')
    if len(data) != 1028 + stride(w) * h:
        raise ValueError(f'{len(data)} bytes, expected {1028 + stride(w) * h}')
    pal = np.frombuffer(data[4:1028], '<u4').astype(np.uint64)
    out = np.zeros((h, w), np.uint32)
    for y in range(h):
        i = 1028 + y * stride(w)
        pix = int.from_bytes(data[i:i + 4], 'little')
        out[y, 0], out[y, 1] = pix & 0xffff, pix >> 16
        codes = data[i + 4:i + 4 + (w - 2) // 2]
        for k, c in enumerate(codes):
            pix = ((pix >> 16) * 0x10001 + int(pal[c])) & 0xffffffff
            out[y, 2 + 2 * k], out[y, 3 + 2 * k] = pix & 0xffff, pix >> 16
    return rgb565_to_rgb8(out.astype(np.int64)).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    args = ap.parse_args()
    w, h = 96, 72
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / 'src.png'
        test_image(src, w, h)
        failed = False
        # Ordered offsets have a fixed size, coarse on a grid as fine as
        # RGB565, so the bound for bayer is looser.
        for dither, bound in (('floyd-steinberg', 2.0), ('none', 2.0), ('bayer4x4', 12.0)):
            base = Path(td) / dither
            for ext in ('nibnib', 'png'):
                subprocess.run([args.cli, str(src), '--mode', 'nibnib', '--no-scale',
                                '--dither', dither, '-o', f'{base}.{ext}'],
                               check=True, capture_output=True)
            dec = decode((base.with_suffix('.nibnib')).read_bytes())
            prev = np.asarray(Image.open(base.with_suffix('.png')).convert('RGB'))
            source = np.asarray(Image.open(src).convert('RGB')).astype(float)
            mismatch = int(np.any(dec != prev, axis=-1).sum())
            err = float(np.abs(dec.astype(float) - source).mean())
            print(f'{dither}_mismatched_pixels={mismatch}')
            print(f'{dither}_mean_abs_vs_source={err:.2f}')
            if mismatch or err > bound:
                failed = True
        return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
