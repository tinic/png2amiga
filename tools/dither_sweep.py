#!/usr/bin/env python3
"""Reproducible dither-strength sweep across modes/methods/chipsets.

Outputs per-(mode, method) tables of strength vs MS-SSIM (and PSNR for
sanity check), with a final summary suggesting the best strength per
bucket. We DON'T auto-edit dither_tuning.cpp — the user maintains the
table by hand based on the sweep numbers, which preserves the
big-picture structure (palette-aware-vs-FS, mode/depth/CAP/SCAP/HAM
buckets).

Usage:
    tools/dither_sweep.py                       # all families, MS-SSIM
    tools/dither_sweep.py --family c64          # c64 modes only
    tools/dither_sweep.py --family amiga        # amiga modes only
    tools/dither_sweep.py --mode c64-hires      # single mode
    tools/dither_sweep.py --method opt-checker  # single method
    tools/dither_sweep.py --strengths 0.5,1.0   # subset of strengths

Block-based modes (c64 / snes / genesis) use examples/block-lowres/
images sized to the mode-natural raster. Amiga lores/hires/ham/ehb use
the full examples/ set sized to mode width.

The runtime grows multiplicatively (modes × methods × strengths × images),
so default scope is conservative; use --quick for a fast smoke run.
"""
from __future__ import annotations

import argparse
import math
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import numpy as np
from PIL import Image


REPO = Path(__file__).resolve().parents[1]
PNG2AMIGA = REPO / "build" / "png2amiga"
EXAMPLES = REPO / "examples"
BLOCK_LOWRES = EXAMPLES / "block-lowres"


# ---------------------------------------------------------------------------
# MS-SSIM (Wang et al. 2003) — pure numpy, 5-scale Gaussian-window variant.
# Validated empirically: 1.000 self-match; ~0.78–0.80 typical for c64
# multicolor at 0.50–1.00 dither.
# ---------------------------------------------------------------------------

def _gaussian_kernel_1d(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    k = np.exp(-(x * x) / (2.0 * sigma * sigma))
    return k / k.sum()


def _conv_separable(img: np.ndarray, k: np.ndarray) -> np.ndarray:
    pad = len(k) // 2
    p = np.pad(img, ((0, 0), (pad, pad), (pad, pad)), mode='reflect')
    h_out = np.zeros_like(img)
    for i, w in enumerate(k):
        h_out += w * p[:, pad:pad + img.shape[1], i:i + img.shape[2]]
    p = np.pad(h_out, ((0, 0), (pad, pad), (pad, pad)), mode='reflect')
    v_out = np.zeros_like(img)
    for i, w in enumerate(k):
        v_out += w * p[:, i:i + img.shape[1], pad:pad + img.shape[2]]
    return v_out


def _ssim_components(x: np.ndarray, y: np.ndarray, k: np.ndarray,
                     C1: float, C2: float) -> tuple[float, float]:
    mu_x = _conv_separable(x, k)
    mu_y = _conv_separable(y, k)
    mu_x2, mu_y2, mu_xy = mu_x * mu_x, mu_y * mu_y, mu_x * mu_y
    sx2 = _conv_separable(x * x, k) - mu_x2
    sy2 = _conv_separable(y * y, k) - mu_y2
    sxy = _conv_separable(x * y, k) - mu_xy
    luminance = float(np.mean((2.0 * mu_xy + C1) / (mu_x2 + mu_y2 + C1)))
    contrast = float(np.mean((2.0 * sxy + C2) / (sx2 + sy2 + C2)))
    return luminance, contrast


def _downsample(img: np.ndarray) -> np.ndarray:
    h, w = img.shape[1], img.shape[2]
    h2, w2 = h - (h % 2), w - (w % 2)
    img = img[:, :h2, :w2]
    return 0.25 * (img[:, 0::2, 0::2] + img[:, 1::2, 0::2]
                   + img[:, 0::2, 1::2] + img[:, 1::2, 1::2])


def msssim(target: Path, dithered: Path) -> float:
    t = Image.open(target).convert('RGB')
    d = Image.open(dithered).convert('RGB')
    if d.size != t.size:
        d = d.resize(t.size, Image.NEAREST)
    a = np.asarray(t, dtype=np.float64).transpose(2, 0, 1) / 255.0
    b = np.asarray(d, dtype=np.float64).transpose(2, 0, 1) / 255.0
    weights = [0.0448, 0.2856, 0.3001, 0.2363, 0.1333]
    k = _gaussian_kernel_1d(11, 1.5)
    C1, C2 = 0.01 ** 2, 0.03 ** 2
    cs_vals: list[float] = []
    l_final = 1.0
    for i, _ in enumerate(weights):
        l, cs = _ssim_components(a, b, k, C1, C2)
        cs_vals.append(max(cs, 1e-9))
        if i == len(weights) - 1:
            l_final = max(l, 1e-9)
            break
        a, b = _downsample(a), _downsample(b)
    score = l_final ** weights[-1]
    for i, cs in enumerate(cs_vals):
        score *= cs ** weights[i]
    return score


def psnr(target: Path, dithered: Path) -> float:
    t = Image.open(target).convert('RGB')
    d = Image.open(dithered).convert('RGB')
    if d.size != t.size:
        d = d.resize(t.size, Image.NEAREST)
    a = np.asarray(t, dtype=np.int32)
    b = np.asarray(d, dtype=np.int32)
    mse = float(np.mean((a - b) ** 2))
    if mse <= 0:
        return float('inf')
    return 20.0 * math.log10(255.0) - 10.0 * math.log10(mse)


# ---------------------------------------------------------------------------
# Mode catalogue. Each entry says: how to size the input, which images to
# use, which dither methods are applicable, and any extra encoder flags.
# ---------------------------------------------------------------------------

# Image subsets — these are deliberately small (3 images) so a full sweep
# stays under ~10 minutes. Add more via --extra-images if you want a deeper
# signal during table-baking.
BLOCK_IMAGES = ['dragon.png', 'fantasy.png', 'face.png']
BLOCK_GLYPH_IMAGES = ['dragon.png', 'fantasy.png', 'head.png']  # head has glyph-friendly detail
AMIGA_IMAGES = ['electrichues02.jpg', 'chuck31.png', 'lovers.jpg']

# Method buckets — matches the structure of dither_tuning.cpp.
PALETTE_AWARE = [
    'opt-checker', 'opt-line', 'opt-line-checker',
    'knoll', 'tri-tone', 'yliluoma1', 'yliluoma', 'yliluoma2',
]
ERROR_DIFFUSION = [
    'floyd-steinberg', 'atkinson', 'sierra-lite', 'stucki', 'jarvis',
    'ostromoukhov', 'gilbert', 'riemersma',
]
STRUCTURE_AWARE = ['structure-fs', 'contrast-fs', 'zhoufang']
ALL_METHODS = PALETTE_AWARE + ERROR_DIFFUSION + STRUCTURE_AWARE


@dataclass
class ModeSpec:
    name: str             # CLI mode name
    family: str           # 'amiga' | 'c64' | 'snes' | 'genesis' | 'atari' | 'pc'
    width: int            # source resize width
    height: int           # source resize height
    images: list[str]
    image_dir: Path
    methods: list[str] = field(default_factory=lambda: list(ALL_METHODS))
    extra_args: list[str] = field(default_factory=list)


# Block-based modes: small fixed buffers, use block-lowres source set.
BLOCK_MODES: list[ModeSpec] = [
    ModeSpec('c64-multicolor', 'c64', 160, 200, BLOCK_IMAGES, BLOCK_LOWRES,
             extra_args=['--c64-metric', 'mse']),
    ModeSpec('c64-hires', 'c64', 320, 200, BLOCK_IMAGES, BLOCK_LOWRES,
             extra_args=['--c64-metric', 'mse']),
    ModeSpec('c64-fli', 'c64', 160, 200, BLOCK_IMAGES, BLOCK_LOWRES,
             extra_args=['--c64-metric', 'mse']),
    ModeSpec('c64-afli', 'c64', 320, 200, BLOCK_IMAGES, BLOCK_LOWRES,
             extra_args=['--c64-metric', 'mse']),
    ModeSpec('c64-charset-hires', 'c64', 320, 200, BLOCK_GLYPH_IMAGES, BLOCK_LOWRES,
             extra_args=['--c64-metric', 'mse']),
    ModeSpec('c64-charset-multicolor', 'c64', 160, 200, BLOCK_GLYPH_IMAGES,
             BLOCK_LOWRES, extra_args=['--c64-metric', 'mse']),
    ModeSpec('snes-mode7-256', 'snes', 256, 224, BLOCK_IMAGES, BLOCK_LOWRES),
    ModeSpec('snes-mode7-direct', 'snes', 256, 224, BLOCK_IMAGES, BLOCK_LOWRES),
    ModeSpec('genesis-h32', 'genesis', 256, 224, BLOCK_IMAGES, BLOCK_LOWRES),
    ModeSpec('genesis-h40', 'genesis', 320, 224, BLOCK_IMAGES, BLOCK_LOWRES),
    ModeSpec('genesis-h32-sh', 'genesis', 256, 224, BLOCK_IMAGES, BLOCK_LOWRES),
    ModeSpec('genesis-h40-sh', 'genesis', 320, 224, BLOCK_IMAGES, BLOCK_LOWRES),
]

# Amiga modes: lores/hires across depths, ham6/8, ehb. Use full examples set.
AMIGA_MODES: list[ModeSpec] = []
for depth in (2, 3, 4, 5, 6):
    AMIGA_MODES.append(ModeSpec(
        f'lores-d{depth}', 'amiga', 320, 200, AMIGA_IMAGES, EXAMPLES,
        extra_args=['--mode', 'lores', '--depth', str(depth)],
    ))
for depth in (2, 3, 4):
    AMIGA_MODES.append(ModeSpec(
        f'hires-d{depth}', 'amiga', 640, 200, AMIGA_IMAGES, EXAMPLES,
        extra_args=['--mode', 'hires', '--depth', str(depth)],
    ))
AMIGA_MODES += [
    ModeSpec('ham6', 'amiga', 320, 200, AMIGA_IMAGES, EXAMPLES),
    ModeSpec('ham8', 'amiga', 320, 200, AMIGA_IMAGES, EXAMPLES,
             extra_args=['--chipset', 'aga']),
    ModeSpec('ehb',  'amiga', 320, 200, AMIGA_IMAGES, EXAMPLES),
]

ALL_MODES = AMIGA_MODES + BLOCK_MODES


# ---------------------------------------------------------------------------
# Image preparation and encoder invocation.
# ---------------------------------------------------------------------------

def prepare_images(spec: ModeSpec, work_dir: Path) -> list[Path]:
    """Resize each source image to (spec.width, spec.height) once, cache it."""
    out_paths: list[Path] = []
    for img in spec.images:
        src = spec.image_dir / img
        if not src.is_file():
            print(f"  [skip] missing source: {src}", file=sys.stderr)
            continue
        dst = work_dir / f"{spec.name}_{img.replace('/', '_')}.png"
        if not dst.is_file():
            i = Image.open(src).convert('RGB').resize(
                (spec.width, spec.height), Image.LANCZOS)
            i.save(dst)
        out_paths.append(dst)
    return out_paths


def run_encoder(spec: ModeSpec, src: Path, method: str,
                strength: float, work_dir: Path) -> Path:
    """Invoke png2amiga with the given (mode, method, strength). Output PNG."""
    name = f"{spec.name}_{src.stem}_{method}_{strength:.2f}.png"
    out = work_dir / name
    args: list[str] = [
        str(PNG2AMIGA), '--quiet', '--no-scale',
        '--dither', method, '--dither-strength', f'{strength:.2f}',
    ]
    # If extra_args includes --mode (amiga depth variants), use it; otherwise
    # spec.name IS the CLI mode name.
    if '--mode' not in spec.extra_args:
        args += ['--mode', spec.name]
    args += spec.extra_args
    args += [str(src), '-o', str(out)]
    res = subprocess.run(args, capture_output=True, text=True)
    if res.returncode != 0 or not out.is_file():
        return Path('')  # signal failure
    return out


# ---------------------------------------------------------------------------
# Sweep + reporting.
# ---------------------------------------------------------------------------

def sweep_one(spec: ModeSpec, methods: list[str], strengths: list[float],
              metric: str, work_dir: Path) -> dict:
    """Returns {method: {'per_image': {img_name: [scores...]}, 'avg': [scores]}}.
    Prints per-method table inline."""
    print(f"\n=== {spec.name} ({spec.family}, {spec.width}x{spec.height}) ===")
    sources = prepare_images(spec, work_dir)
    if not sources:
        print("  no usable source images, skipping")
        return {}
    score_fn = msssim if metric == 'msssim' else psnr
    fmt = '{:.4f}' if metric == 'msssim' else '{:.2f}'
    fwid = 7 if metric == 'msssim' else 6
    results: dict = {}
    for method in methods:
        if method not in spec.methods:
            continue
        per_img: dict[str, list[float]] = {}
        avg: list[float] = []
        ok_method = True
        for s in strengths:
            scores: list[float] = []
            for src in sources:
                out = run_encoder(spec, src, method, s, work_dir)
                if out == Path(''):
                    ok_method = False
                    break
                v = score_fn(src, out)
                scores.append(v)
                per_img.setdefault(src.stem.split('_', 1)[1], []).append(v)
            if not ok_method:
                break
            avg.append(sum(scores) / len(scores) if scores else 0.0)
        if not ok_method:
            print(f"  {method:<18}  (mode does not support this method, skipped)")
            continue
        # Print table: rows = images + avg, columns = strengths.
        header = f"  {'method/img':<18}"
        for s in strengths:
            header += f"  {s:>{fwid}.2f}"
        header += '    best'
        print(header)
        print(f"  {method:<18}")
        for img_key, vals in per_img.items():
            row = f"    {img_key:<16}"
            for v in vals:
                row += f"  {v:>{fwid}.{4 if metric == 'msssim' else 2}f}"
            print(row)
        # Avg row + best.
        best_i = max(range(len(avg)), key=lambda i: avg[i])
        avg_row = f"    {'avg':<16}"
        for i, v in enumerate(avg):
            marker = '*' if i == best_i else ' '
            cell = fmt.format(v)
            avg_row += f" {marker}{cell:>{fwid}}"
        avg_row += f"    {strengths[best_i]:.2f}"
        print(avg_row)
        results[method] = {
            'per_image': per_img, 'avg': avg,
            'best_strength': strengths[best_i],
            'best_score': avg[best_i],
        }
    return results


def print_summary(all_results: dict[str, dict], strengths: list[float],
                  metric: str) -> None:
    """Final per-(mode, method) suggested-strength table."""
    print("\n\n" + "=" * 72)
    print(f"SUGGESTED STRENGTHS  (metric: {metric})")
    print("=" * 72)
    print(f"{'mode':<26}  {'method':<20}  {'best':>6}  {'score':>8}  spread")
    print("-" * 72)
    fmt = '{:.4f}' if metric == 'msssim' else '{:.2f}'
    for mode_name, methods in all_results.items():
        for method, data in methods.items():
            avg = data['avg']
            spread = max(avg) - min(avg) if avg else 0.0
            best = data['best_strength']
            score = data['best_score']
            print(f"{mode_name:<26}  {method:<20}  {best:>6.2f}  "
                  f"{fmt.format(score):>8}  {fmt.format(spread)}")


# ---------------------------------------------------------------------------
# CLI.
# ---------------------------------------------------------------------------

def parse_csv(s: str) -> list[str]:
    return [x.strip() for x in s.split(',') if x.strip()]


def parse_strengths(s: str) -> list[float]:
    return [float(x) for x in parse_csv(s)]


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--family',
                   choices=['all', 'amiga', 'c64', 'snes', 'genesis', 'block'],
                   default='all',
                   help='Mode family ("block" = c64+snes+genesis)')
    p.add_argument('--mode', help='Single mode name (matches ModeSpec.name)')
    p.add_argument('--method', help='Single dither method (else all applicable)')
    p.add_argument('--methods', help='CSV of methods')
    p.add_argument('--strengths',
                   default='0.50,0.60,0.70,0.80,0.85,0.90,0.95,1.00',
                   help='CSV of dither strengths to test')
    p.add_argument('--metric', choices=['msssim', 'psnr'], default='msssim')
    p.add_argument('--quick', action='store_true',
                   help='Smoke run: 3 strengths, opt-checker + floyd-steinberg only')
    p.add_argument('--work-dir', help='Cache dir for resized inputs and outputs '
                                       '(default: tempfile)')
    args = p.parse_args()

    if not PNG2AMIGA.is_file():
        print(f"error: {PNG2AMIGA} not found — run cmake --build build first",
              file=sys.stderr)
        return 1

    strengths = parse_strengths(args.strengths)
    if args.quick:
        strengths = [0.50, 0.85, 1.00]

    if args.method:
        methods = [args.method]
    elif args.methods:
        methods = parse_csv(args.methods)
    elif args.quick:
        methods = ['opt-checker', 'floyd-steinberg']
    else:
        methods = list(ALL_METHODS)

    if args.mode:
        modes = [m for m in ALL_MODES if m.name == args.mode]
        if not modes:
            print(f"error: unknown mode {args.mode}", file=sys.stderr)
            return 1
    elif args.family == 'all':
        modes = ALL_MODES
    elif args.family == 'amiga':
        modes = AMIGA_MODES
    elif args.family == 'block':
        modes = BLOCK_MODES
    else:
        modes = [m for m in BLOCK_MODES if m.family == args.family]

    work_ctx = None
    if args.work_dir:
        work_dir = Path(args.work_dir)
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        work_ctx = tempfile.TemporaryDirectory(prefix='dither_sweep_')
        work_dir = Path(work_ctx.name)

    print(f"sweeping {len(modes)} mode(s) × {len(methods)} method(s) "
          f"× {len(strengths)} strength(s)  (metric={args.metric})")
    print(f"work_dir: {work_dir}")

    all_results: dict[str, dict] = {}
    try:
        for spec in modes:
            r = sweep_one(spec, methods, strengths, args.metric, work_dir)
            if r:
                all_results[spec.name] = r
        print_summary(all_results, strengths, args.metric)
    finally:
        if work_ctx is not None:
            work_ctx.cleanup()
    return 0


if __name__ == '__main__':
    sys.exit(main())
