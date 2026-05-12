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
    'abc-ham6':              'abc HAM6 (-floyd)',
    'abc-sham6':             'abc SHAM6 (-floyd)',
    'hc-ham6':               'ham_convert HAM6_q7 (dither_fs)',
    'hc-ham6-fast':          'ham_convert HAM6_q1 (dither_fs)',
    'hc-ehb':                'ham_convert EHB (dither_fs)',
    'hc-sham6':              'ham_convert SHAM6 (dither_fs)',
    'p2a-ham6':              'png2amiga HAM6 (FS)',
    'p2a-ham6-noncap-best':  'png2amiga HAM6 + best (FS)',
    'p2a-ham6-cap':          'png2amiga HAM6 + SLICED (FS)',
    'p2a-ham6-best':         'png2amiga HAM6 + SLICED + best (FS)',
    'p2a-ehb-plain':         'png2amiga EHB (FS)',
    'p2a-ehb-plain-best':    'png2amiga EHB + best (FS)',
    'p2a-ehb-best':          'png2amiga EHB + STRIPS + best (FS)',
    # Lores 32-colour head-to-head: png2amiga lores-d5 vs ham_convert
    # ocs32 vs libimagequant (pngquant). Same colour count, same FS
    # dither — isolates quantizer + dither without HAM-style ops.
    'p2a-lores-d5':          'png2amiga lores d=5 (FS)',
    'p2a-lores-d5-best':     'png2amiga lores d=5 + best (FS)',
    'hc-lores-d5':           'ham_convert ocs32 (dither_fs)',
    'abc-lores-d5':          'abc lores d=5 (-floyd)',
    'libimagequant-32':      'libimagequant 32 (FS)',
    # 256-colour AGA lores head-to-head with libimagequant. Tests the
    # quantizer in the high-colour regime where the OCS-12-bit
    # constraint is gone (chipset=aga = continuous 24-bit palette).
    'p2a-lores-d8':          'png2amiga lores d=8 AGA (FS)',
    'p2a-lores-d8-best':     'png2amiga lores d=8 AGA + best (FS)',
    'libimagequant-256':     'libimagequant 256 (FS)',
    # ImageMagick / Netpbm baselines. No "force one slot, quantize
    # the rest" flag in either, so their palette is fully unconstrained
    # (unlike png2amiga / ham_convert / abc, which all pin slot 0 to
    # black). Slight palette-size advantage to them, but the perceptual
    # gap is much larger than 1 slot's worth at any N.
    'imagemagick-32':        'ImageMagick 32 (FS)',
    'imagemagick-256':       'ImageMagick 256 (FS)',
    'netpbm-32':             'Netpbm 32 (pnmquant -floyd)',
    'netpbm-256':            'Netpbm 256 (pnmquant -floyd)',
    'ffmpeg-32':             'ffmpeg 32 (palettegen + FS)',
    'ffmpeg-256':            'ffmpeg 256 (palettegen + FS)',
    'gifsicle-32':           'gifsicle 32 (--dither floyd-steinberg)',
    'gifsicle-256':          'gifsicle 256 (--dither floyd-steinberg)',
    'pngnq-32':              'pngnq 32 (NeuQuant + FS, -s 1)',
    'pngnq-256':             'pngnq 256 (NeuQuant + FS, -s 1)',
    'didder-32':             'didder 32 (mmcq + FS edm)',
    'didder-256':            'didder 256 (mmcq + FS edm)',
}

def read_time(stem: str) -> float | None:
    """Returns wall-clock encode seconds from <stem>.time, or None."""
    p = os.path.join(output_dir, f'{stem}.time')
    try:
        with open(p) as f:
            return float(f.read().strip())
    except (OSError, ValueError):
        return None

results = []
for stem, label in LABELS.items():
    # Accept either <stem>.png (most encoders) or <stem>.gif (the
    # GIF benchmark entries: p2a-gif-d8, imagemagick-gif, gifsicle).
    # ssimulacra2's binary needs PNG input on both sides, so when
    # the encoder produced a GIF we transcode it to a temp PNG once
    # via Pillow before scoring — the indices/palette decode is
    # lossless, just a container change.
    candidates = [os.path.join(output_dir, f'{stem}.png'),
                  os.path.join(output_dir, f'{stem}.gif')]
    src = next((p for p in candidates if os.path.exists(p)), None)
    if src is None:
        results.append((label, None, None, None, "MISSING"))
        continue
    img = Image.open(src).convert('RGB')
    if img.size != (target.shape[1], target.shape[0]):
        img = img.resize((target.shape[1], target.shape[0]), Image.NEAREST)
    arr = np.asarray(img, dtype=np.int32)
    db = psnr(target, arr)
    # SSIMULACRA2 binary wants PNG; for GIF outputs save a temp PNG.
    if src.endswith('.gif'):
        png_tmp = src + '.s2tmp.png'
        Image.open(src).convert('RGB').save(png_tmp)
        s2 = ssimulacra2(target_path, png_tmp)
    else:
        s2 = ssimulacra2(target_path, src)
    t  = read_time(stem)
    results.append((label, db, s2, t, "OK"))

# Sort by SSIMULACRA2 if available (better correlates with subjective
# quality on HAM output), else PSNR. Missing entries last.
def sort_key(r):
    label, db, s2, t, status = r
    if status != "OK":
        return (1, 0.0)
    return (0, -(s2 if s2 is not None else (db or -1e9)))
results.sort(key=sort_key)

print()
header = (f"  {'Encoder':<48s}  {'PSNR (dB)':>9s}  "
          f"{'SSIMULACRA2':>11s}  {'Time (s)':>9s}")
sep    = (f"  {'-'*48:<48s}  {'-'*9:>9s}  "
          f"{'-'*11:>11s}  {'-'*9:>9s}")
print(f"Encoder quality vs target "
      f"({target.shape[1]}×{target.shape[0]}):")
if not HAS_S2:
    print(f"  (SSIMULACRA2 binary not built; run ./setup.sh first.)")
print()
print(header)
print(sep)
for label, db, s2, t, status in results:
    if status != "OK":
        print(f"  {label:<48s}  {status:>9s}  {'':<11s}  {'':<9s}")
    else:
        s2_str = f"{s2:.2f}" if s2 is not None else "—"
        t_str  = f"{t:.2f}"  if t  is not None else "—"
        print(f"  {label:<48s}  {db:>9.2f}  {s2_str:>11s}  {t_str:>9s}")
print()

# Winner reported by SSIMULACRA2 if available, else PSNR.
ok = [r for r in results if r[4] == "OK"]
if ok:
    if HAS_S2 and any(r[2] is not None for r in ok):
        winner = max((r for r in ok if r[2] is not None), key=lambda r: r[2])
        print(f"Winner (by SSIMULACRA2): {winner[0]} "
              f"({winner[2]:.2f} SSIMULACRA2, {winner[1]:.2f} dB PSNR)")
    else:
        winner = max(ok, key=lambda r: r[1])
        print(f"Winner (by PSNR): {winner[0]} ({winner[1]:.2f} dB)")
