#!/usr/bin/env python3
"""Compute PSNR (sRGB-direct, per-channel byte distance) for every shootout
preview against the resized target. Print a sorted table.

Why sRGB-direct PSNR rather than OKLab or MS-SSIM: the goal is to be
encoder-independent — both abc and ham_convert work in sRGB/linear, and
PSNR there is what their authors quote in their own posts. png2amiga
internally ranks cap-best by OKLab-blurred PSNR or MS-SSIM, but that's
its training metric; for cross-encoder comparison we want the most
neutral, widely-quoted reference.
"""
import sys
import os
import re
import math
import numpy as np
from PIL import Image

if len(sys.argv) != 3:
    print("usage: psnr.py <target.png> <output_dir>", file=sys.stderr)
    sys.exit(1)

target_path = sys.argv[1]
output_dir  = sys.argv[2]

target = np.asarray(Image.open(target_path).convert('RGB'), dtype=np.int32)

def psnr(a: np.ndarray, b: np.ndarray) -> float:
    diff = a - b
    mse = float(np.mean(diff * diff))
    if mse <= 0:
        return float('inf')
    return 20.0 * math.log10(255.0) - 10.0 * math.log10(mse)

# Friendly labels for entries we care about.
LABELS = {
    'abc-ham6':           'abc HAM6 (-floyd)',
    'abc-sham6':          'abc SHAM6 (-floyd)',
    'hc-ham6':            'ham_convert HAM6_q7 (dither_fs)',
    'hc-sham6':           'ham_convert SHAM6 (dither_fs)',
    'p2a-ham6':           'png2amiga HAM6 (FS)',
    'p2a-ham6-cap-best':  'png2amiga HAM6 + CAP + cap-best (FS)',
    'p2a-ehb-scap-best':  'png2amiga EHB + SCAP + cap-best (FS)',
}

results = []
for stem, label in LABELS.items():
    png = os.path.join(output_dir, f'{stem}.png')
    if not os.path.exists(png):
        results.append((label, None, "MISSING"))
        continue
    img = Image.open(png).convert('RGB')
    if img.size != (target.shape[1], target.shape[0]):
        img = img.resize((target.shape[1], target.shape[0]), Image.NEAREST)
    arr = np.asarray(img, dtype=np.int32)
    db = psnr(target, arr)
    results.append((label, db, "OK"))

# Sort by PSNR desc; missing entries last.
results.sort(key=lambda r: (r[1] is None, -(r[1] or -1e9)))

print()
print(f"PSNR vs target ({target.shape[1]}×{target.shape[0]}, sRGB MSE):")
print()
print(f"  {'Encoder':<48s}  {'PSNR (dB)':>9s}")
print(f"  {'-'*48:<48s}  {'-'*9:>9s}")
for label, db, status in results:
    if db is None:
        print(f"  {label:<48s}  {status:>9s}")
    else:
        print(f"  {label:<48s}  {db:>9.2f}")
print()

# Winner.
ok = [r for r in results if r[1] is not None]
if ok:
    winner = max(ok, key=lambda r: r[1])
    print(f"Winner: {winner[0]} ({winner[1]:.2f} dB)")
