#!/usr/bin/env python3
"""Generate seamless test sources for --tile demos.

Each input is mathematically periodic in W and H so the source itself
already wraps perfectly. The interesting question is whether the
ED-dithered output also wraps — that's the part the 3x3 replicate is
trying to fix. These inputs deliberately stress the FS state
convergence: smooth gradients (where one-row FS warm-up barely
matters because the dither pattern is dominated by the gradient
slope), and high-frequency repeats (where FS actually needs to
synchronise across the seam).

Usage: python3 gen_sources.py
Outputs (PNG, 128x128 or 256x256) are written next to this script.
"""
import math
from pathlib import Path
from PIL import Image

W = H = 128

def write_png(name, get_rgb):
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            r, g, b = get_rgb(x, y)
            px[x, y] = (
                max(0, min(255, int(r))),
                max(0, min(255, int(g))),
                max(0, min(255, int(b))),
            )
    out = Path(__file__).parent / name
    img.save(out)
    print(f"wrote {out}")

# 1. Horizontal sine ramp — wraps in X, constant in Y.
# A smooth gradient where the dither pattern is the only thing that
# can break tiling. ED state has nothing strong to lock onto.
write_png(
    "ramp_horizontal.png",
    lambda x, y: (
        128 + 127 * math.sin(2 * math.pi * x / W),
        128 + 127 * math.sin(2 * math.pi * x / W + math.pi / 3),
        128 + 127 * math.sin(2 * math.pi * x / W + 2 * math.pi / 3),
    ),
)

# 2. Vertical sine ramp — wraps in Y, constant in X.
# Same content as horizontal but rotated 90°. Useful to confirm
# that the FS convergence story is anisotropic (FS scans rows
# left→right, so vertical stripes are easier to dither tile-ably).
write_png(
    "ramp_vertical.png",
    lambda x, y: (
        128 + 127 * math.sin(2 * math.pi * y / H),
        128 + 127 * math.sin(2 * math.pi * y / H + math.pi / 3),
        128 + 127 * math.sin(2 * math.pi * y / H + 2 * math.pi / 3),
    ),
)

# 3. Diagonal ramp — periodic in both X and Y.
# Stresses both axes simultaneously.
write_png(
    "ramp_diagonal.png",
    lambda x, y: (
        128 + 127 * math.sin(2 * math.pi * (x + y) / W),
        128 + 127 * math.sin(2 * math.pi * (x - y) / W),
        128 + 127 * math.sin(2 * math.pi * (x * 2 + y) / W),
    ),
)

# 4. Diamond — 8 diamonds tiled in 4x4, max contrast in centre,
# zero on the diamond edges. The diamond edges land exactly on
# tile boundaries, so seamless tiling means the diamond outline
# stays continuous when repeated.
def diamond(x, y):
    cx, cy = W / 2, H / 2
    # Manhattan distance from centre, normalised to wrap at half-W.
    dx = abs(((x - cx + W / 2) % W) - W / 2)
    dy = abs(((y - cy + H / 2) % H) - H / 2)
    d = (dx + dy) / (W / 2)  # 0 at centre, 1 at corners
    v = max(0.0, 1.0 - d) * 255
    return (v, v * 0.6, v * 0.3)

write_png("diamond.png", diamond)

# 5. Plasma — three superposed sine fields at different frequencies
# that each independently wrap in W. Smooth photoreal-ish gradient
# texture; easy on the eye, hard for FS to perfectly tile.
def plasma(x, y):
    a = math.sin(2 * math.pi * x / W) + math.sin(2 * math.pi * y / H)
    b = math.sin(2 * math.pi * (x + y) / W) + math.sin(2 * math.pi * (x - y) / W)
    c = math.sin(4 * math.pi * x / W + 2 * math.pi * y / H)
    return (
        128 + 60 * a + 40 * c,
        128 + 60 * b + 30 * c,
        128 + 60 * (a + b) / 2 + 50 * c,
    )

write_png("plasma.png", plasma)

# 6. High-frequency stripes that wrap (period divides W). Sharp
# transitions stress FS convergence the most — each stripe edge
# kicks FS state into a different basin.
def stripes(x, y):
    # 8-wide stripes in X (16 stripes, divides W=128 evenly)
    sx = (x // 8) % 2
    sy = (y // 16) % 2
    if sx == sy:
        return (240, 200, 60)
    return (40, 60, 180)

write_png("stripes.png", stripes)
