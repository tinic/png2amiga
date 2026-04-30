#!/usr/bin/env python3
"""Compute PSNR (sRGB-direct) and SSIMULACRA2 for every shootout preview
against the resized target. Print a sorted table.

Two metrics, each measuring a different thing:

- sRGB-PSNR (sRGB-direct, per-channel byte distance): neutral, widely
  quoted, but does NOT predict subjective HAM quality well — over-weights
  bright regions, treats dither noise as pure error even when the eye
  benefits from it.
- SSIMULACRA2 (Cloudinary, 2022 — see github.com/cloudinary/ssimulacra2):
  modern multi-scale perceptual quality metric, calibrated against human
  ratings. Score range -inf..100; 30=low, 50=fair, 70=high quality. The
  score swings dramatically between dithered and non-dithered HAM output
  in a way PSNR misses entirely (no-dither HAM scores ~18 even when PSNR
  is 32 dB; a dithered HAM at PSNR 30 dB scores ~65).

Both ham_convert and abc work in sRGB/linear so PSNR is the metric they
report in their own write-ups; SSIMULACRA2 is what every modern image-
codec evaluation uses. We print both so the comparison is honest.
"""
import sys
import os
import math
import subprocess
import numpy as np
from PIL import Image

if len(sys.argv) != 3:
    print("usage: psnr.py <target.png> <output_dir>", file=sys.stderr)
    sys.exit(1)

target_path = sys.argv[1]
output_dir  = sys.argv[2]

target_img = Image.open(target_path).convert('RGB')
target = np.asarray(target_img, dtype=np.int32)

def psnr(a: np.ndarray, b: np.ndarray) -> float:
    diff = a - b
    mse = float(np.mean(diff * diff))
    if mse <= 0:
        return float('inf')
    return 20.0 * math.log10(255.0) - 10.0 * math.log10(mse)

# Locate the ssimulacra2 binary (built by setup.sh into vendor/ssimulacra2/).
S2_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '..', 'vendor', 'ssimulacra2', 'ssimulacra2')
S2_BIN = os.path.normpath(S2_BIN)
HAS_S2 = os.path.isfile(S2_BIN) and os.access(S2_BIN, os.X_OK)

def ssimulacra2(target_png: str, distorted_png: str) -> float | None:
    """Returns the SSIMULACRA2 score, or None if the binary isn't built."""
    if not HAS_S2:
        return None
    try:
        # ssimulacra2 requires equal dimensions; downsample distorted with
        # NEAREST if it's a 2x preview.
        d_img = Image.open(distorted_png).convert('RGB')
        if d_img.size != (target.shape[1], target.shape[0]):
            d_img = d_img.resize((target.shape[1], target.shape[0]),
                                 Image.NEAREST)
            tmp = distorted_png + '.s2tmp.png'
            d_img.save(tmp)
            distorted_png = tmp
        out = subprocess.run([S2_BIN, target_png, distorted_png],
                             capture_output=True, text=True, timeout=30)
        if out.returncode != 0:
            return None
        return float(out.stdout.strip())
    except (subprocess.TimeoutExpired, ValueError, OSError):
        return None

# Friendly labels for entries we care about.
LABELS = {
    'abc-ham6':           'abc HAM6 (-floyd)',
    'abc-sham6':          'abc SHAM6 (-floyd)',
    'hc-ham6':            'ham_convert HAM6_q7 (dither_fs)',
    'hc-sham6':           'ham_convert SHAM6 (dither_fs)',
    'p2a-ham6':           'png2amiga HAM6 (FS)',
    'p2a-ham6-best':  'png2amiga HAM6 + CAP + best (FS)',
    'p2a-ehb-best':  'png2amiga EHB + SCAP + best (FS)',
}

results = []
for stem, label in LABELS.items():
    png = os.path.join(output_dir, f'{stem}.png')
    if not os.path.exists(png):
        results.append((label, None, None, "MISSING"))
        continue
    img = Image.open(png).convert('RGB')
    if img.size != (target.shape[1], target.shape[0]):
        img = img.resize((target.shape[1], target.shape[0]), Image.NEAREST)
    arr = np.asarray(img, dtype=np.int32)
    db = psnr(target, arr)
    s2 = ssimulacra2(target_path, png)
    results.append((label, db, s2, "OK"))

# Sort by SSIMULACRA2 if available (better correlates with subjective
# quality on HAM output), else PSNR. Missing entries last.
def sort_key(r):
    label, db, s2, status = r
    if status != "OK":
        return (1, 0.0)
    return (0, -(s2 if s2 is not None else (db or -1e9)))
results.sort(key=sort_key)

print()
header = f"  {'Encoder':<48s}  {'PSNR (dB)':>9s}  {'SSIMULACRA2':>11s}"
sep    = f"  {'-'*48:<48s}  {'-'*9:>9s}  {'-'*11:>11s}"
print(f"Encoder quality vs target "
      f"({target.shape[1]}×{target.shape[0]}):")
if not HAS_S2:
    print(f"  (SSIMULACRA2 binary not built; run ./setup.sh first.)")
print()
print(header)
print(sep)
for label, db, s2, status in results:
    if status != "OK":
        print(f"  {label:<48s}  {status:>9s}  {'':<11s}")
    else:
        s2_str = f"{s2:.2f}" if s2 is not None else "—"
        print(f"  {label:<48s}  {db:>9.2f}  {s2_str:>11s}")
print()

# Winner reported by SSIMULACRA2 if available, else PSNR.
ok = [r for r in results if r[3] == "OK"]
if ok:
    if HAS_S2 and any(r[2] is not None for r in ok):
        winner = max((r for r in ok if r[2] is not None), key=lambda r: r[2])
        print(f"Winner (by SSIMULACRA2): {winner[0]} "
              f"({winner[2]:.2f} SSIMULACRA2, {winner[1]:.2f} dB PSNR)")
    else:
        winner = max(ok, key=lambda r: r[1])
        print(f"Winner (by PSNR): {winner[0]} ({winner[1]:.2f} dB)")
