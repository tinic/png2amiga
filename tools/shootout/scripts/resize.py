#!/usr/bin/env python3
"""Pre-resize the shootout source to a fixed target size with Lanczos.
All encoders get this same input so PSNR doesn't measure the resize stage.
"""
import sys
from PIL import Image

if len(sys.argv) != 5:
    print("usage: resize.py <in.png> <out.png> <W> <H>", file=sys.stderr)
    sys.exit(1)

src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
img = Image.open(src).convert('RGB')
img.resize((w, h), Image.LANCZOS).save(dst, 'PNG', optimize=True)
print(f"resized {src} -> {dst} ({w}x{h})")
