#!/usr/bin/env python3
"""Sweep (metric, dither_method, dither_strength) for cga-text80x100.

For each image in the test set, encode with each combo, decode the
preview PNG, score with PSNR + SSIMULACRA2 (if vendor binary is present),
and print a per-image winner table plus the global mean.
"""
import os, sys, subprocess, math, statistics, tempfile
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PNG2AMIGA = ROOT / "build" / "png2amiga"
S2_BIN    = ROOT / "tools" / "shootout" / "vendor" / "ssimulacra2" / "ssimulacra2"
HAS_S2    = S2_BIN.is_file() and os.access(S2_BIN, os.X_OK)

METRICS  = ["blur", "mse"]
DITHERS  = ["none", "floyd-steinberg", "atkinson", "stucki", "jarvis"]
STRENGTHS = [0.6, 0.8, 1.0]

# cga-text80x100 hardware buffer: 640x200 (80 cols * 8 px, 100 rows * 2 scanlines).
TARGET_W, TARGET_H = 640, 200

def psnr(a, b):
    diff = a.astype(np.int32) - b.astype(np.int32)
    mse = float(np.mean(diff * diff))
    if mse <= 0: return float('inf')
    return 20.0 * math.log10(255.0) - 10.0 * math.log10(mse)

def s2(target_png: Path, distorted_png: Path):
    if not HAS_S2: return None
    try:
        out = subprocess.run([str(S2_BIN), str(target_png), str(distorted_png)],
                             capture_output=True, text=True, timeout=30)
        if out.returncode != 0: return None
        return float(out.stdout.strip())
    except Exception:
        return None

def encode(src: Path, out_png: Path, metric: str, dither: str, strength: float):
    cmd = [str(PNG2AMIGA), "--quiet", "--mode", "cga-text80x100",
           "--cga-text-metric", metric, "--dither", dither,
           str(src), "-o", str(out_png)]
    if dither != "none":
        cmd += ["--dither-strength", str(strength)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return False
    return True

def make_target(src: Path, td: Path) -> Path:
    """Resize to 640x200 stretch (matches cga-text encoder's internal scaling)."""
    img = Image.open(src).convert("RGB")
    img = img.resize((TARGET_W, TARGET_H), Image.LANCZOS)
    out = td / "target.png"
    img.save(out)
    return out

def main(image_paths):
    rows = []  # (image, metric, dither, strength, psnr, s2)
    for ip in image_paths:
        name = ip.stem
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            target = make_target(ip, td)
            t_arr = np.asarray(Image.open(target).convert("RGB"))
            for metric in METRICS:
                for dither in DITHERS:
                    strs = [1.0] if dither == "none" else STRENGTHS
                    for s in strs:
                        tag = f"{metric}-{dither}-{s:.1f}"
                        out = td / f"{tag}.png"
                        if not encode(ip, out, metric, dither, s):
                            print(f"  FAIL {name} {tag}", file=sys.stderr)
                            continue
                        # png2amiga emits 2x preview; downsample with NEAREST.
                        prev = Image.open(out).convert("RGB")
                        if prev.size != (TARGET_W, TARGET_H):
                            prev = prev.resize((TARGET_W, TARGET_H), Image.NEAREST)
                            tmp = out.with_suffix(".match.png")
                            prev.save(tmp)
                            d = psnr(t_arr, np.asarray(prev))
                            sc = s2(target, tmp)
                        else:
                            d = psnr(t_arr, np.asarray(prev))
                            sc = s2(target, out)
                        rows.append((name, metric, dither, s, d, sc))
        # Per-image best.
        last = [r for r in rows if r[0] == name]
        last.sort(key=lambda r: -(r[5] if r[5] is not None else r[4]))
        print(f"\n== {name} ==")
        for r in last[:5]:
            print(f"  {r[1]:>5s} {r[2]:>15s} {r[3]:>4.1f}  PSNR={r[4]:6.2f}  "
                  f"S2={r[5] if r[5] is not None else 'n/a'}")
    # Aggregate by combo.
    print("\n=== GLOBAL MEAN (sorted by S2 if available, else PSNR) ===")
    combos = {}
    for r in rows:
        k = (r[1], r[2], r[3])
        combos.setdefault(k, []).append(r)
    agg = []
    for k, lst in combos.items():
        psnrs = [r[4] for r in lst]
        s2s   = [r[5] for r in lst if r[5] is not None]
        agg.append((k, statistics.mean(psnrs),
                    statistics.mean(s2s) if s2s else None,
                    len(psnrs)))
    if HAS_S2:
        agg.sort(key=lambda x: -(x[2] if x[2] is not None else x[1]))
    else:
        agg.sort(key=lambda x: -x[1])
    print(f"{'metric':>6s} {'dither':>15s} {'str':>4s}  "
          f"{'PSNR':>7s}  {'S2':>7s}  N")
    for k, p, s, n in agg:
        s_str = f"{s:6.2f}" if s is not None else "  n/a "
        print(f"{k[0]:>6s} {k[1]:>15s} {k[2]:>4.1f}  {p:7.2f}  {s_str:>7s}  {n}")

if __name__ == "__main__":
    paths = [Path(p) for p in sys.argv[1:]] or sorted(
        (ROOT / "test-suite-lores").glob("*.png"))[:8]
    main(paths)
