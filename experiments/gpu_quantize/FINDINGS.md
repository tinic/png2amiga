# GPU palette quantization experiment — findings

**Branch:** `gpu-quantize-experiment`
**Date:** 2026-05-04
**Hardware:** Apple M3 (8-core GPU, family 8, atomic_float supported)
**Reference:** pngquant 3.0.3 (`brew install pngquant`), 50 DIV2K images
**Score metric:** SSIMULACRA2 (S2) and PSNR via `png2amiga --score-vs`,
both tools running with no FS dither (quantizer-only A/B).

## Headline result

**Stage A — parallel-restart Lloyd k-means in OKLab — beats pngquant
on SSIMULACRA2 across most palette sizes**, on a 10-image DIV2K
sample at no-dither config:

| K   | Stage A ΔPSNR | Stage A ΔS2 | Verdict |
|----:|--------------:|------------:|---------|
|   4 |       -0.06   |     **+0.67** | win |
|   8 |       -0.16   |     **+0.48** | win |
|  16 |       -0.19   |     **+2.43** | win |
|  32 |       -0.43   |       -1.74   | mild loss |
| 256 |       -0.68   |     **+2.02** | win |

PSNR consistently slightly worse than pngquant (perceptual cost
function ≠ MSE), but SSIMULACRA2 ahead at K ∈ {4, 8, 16, 256}.
The K=32 dip is the same bimodal pattern the existing
`--palette-diversity` memory documents — restart-based methods
all hit it. With more restarts (≥ 32) it likely closes.

**Existing png2amiga (median-cut + OKLab k-means, on `liq-quantizer`
branch) was at ΔPSNR -0.43 / ΔS2 -3.61 at K=32 per
`project_png2amiga_pngquant_bench.md`.** Stage A halves that gap.

## Implementation summary

Five commits, 4 working stages:

```
cfbc125 Stage 0: Metal scaffolding (hello-kernel)
30852fe Stage A: parallel-restart Lloyd k-means
74a7dbb Stage B: soft k-means + deterministic annealing
9825b61 Stage C v1: real scolorq w/ Jacobi solver — DIVERGED
7e29bd5 Stage C v2: graph-coloured ICM + fresh-g + LDLT — works,
                    but dither pattern loses raw S2 at K > 4.
```

### Stage A — parallel-restart Lloyd (the working result)

CPU pipeline:
- Load PNG → linear RGB → OKLab f32 (vendored stb_image, custom
  colorspace.hpp port of color_space.hpp).
- k-means++ initialisation per restart on a uniform random 8K-pixel
  subsample (full image is overkill; init time was the original
  bottleneck — full-pixel init was 28.9 s for 32 restarts at
  K=256, subsampled init is 162 ms).

GPU pipeline (per Lloyd iteration):
- Kernel `assign_and_accumulate`: 1 thread/pixel, argmin of K
  centroid distances; atomic_float-add into per-cluster sums (3
  channels) + atomic_uint count + atomic_float SSE.
- Kernel `finalize_centroids`: K threads, divide sums by counts.

Output: rendered RGB PNG (palette[indices], pixel-perfect).
Bench harness in `bench.sh`.

Performance on Apple M3 at 1.5 MP, 32 restarts × 20 iters:
- kmeans++ init (CPU):   ~160 ms
- Lloyd loop (GPU):     ~2.8  s
- Total wall:           ~3.3  s

### Stage B — soft k-means with annealing (research dead-end)

Replaced argmin with softmax(-d²/T); cooled T by ×0.7 each iter
from T0=0.1 to ~2e-6 over 30 iters.

10-image @ K=32: ΔS2 -3.26 (worse than Stage A's -1.74). On
hardest image (0007) closed +4.7 S2 over Stage A but hurt
flat-content images where annealing pulls the palette toward the
global mean and can't recover the extremes. Net negative.

### Stage C v2 — real scolorq (works algorithmically, loses on raw S2)

Implements Bonn 1998 cost:
```
E(p, a) = Σ_x ||I(x) - F·p[a](x)||²,  F = [1,2,1;2,4,2;1,2,1]/16
```

Six kernels:
- `scolorq_filtered_output` — out(x) = F·p[a](x)
- `scolorq_compute_g` — g(x) = F·(I - out)(x)
- `scolorq_assign_subgrid(ox, oy)` — graph-coloured in-place ICM
  on stride-3 sub-grids (no two threads in the same dispatch are
  filter-neighbours; 9 dispatches per iter)
- `scolorq_build_Mb` — atomic-build the K×K matrix M[k,k']
  = Σ_x F_k(x)·F_k'(x) and vector b[k] = Σ_x F_k(x)·I(x), with
  sparse F_k bucketing in registers (≤9 non-zero entries per pixel,
  ≤81 atomic adds)

CPU step per iter: direct LDLT factorisation of M (K=32, ~5K ops).
Initial Gauss-Seidel diverged on poorly-conditioned M.

**Verified algorithm correctness**: blurred PSNR (σ=1.0) on image
0007 at K=32: scolorq 26.98 dB vs Lloyd 24.75 dB (+2.23). The
filtered cost IS lower, exactly as the paper claims.

**But raw SSIMULACRA2 hates the dither**:

| K | Stage C v2 ΔPSNR | Stage C v2 ΔS2 |
|---|---:|---:|
|   4 | -2.59 | +0.96 (marginal win) |
|   8 | -2.83 | -3.40 |
|  16 | -2.93 | -13.67 |
|  32 | -5.07 | -37.05 |

Root cause: the dither pattern emerging from naive ICM is
*structured* (checker/stripes in regions of similar colour) rather
than blue-noise. The eye and SSIMULACRA2 penalize high-frequency
content that *would* average right under a Gaussian filter but
doesn't hide under proper perceptual evaluation. Bonn's claimed
dominance is for K ≤ 16 against MSE-style metrics, not perceptual
ones.

To beat pngquant on raw S2 with scolorq would require:
- Random pixel order ICM (produces blue-noise-like patterns) — not
  trivially GPU-friendly without random-permutation kernels per iter
- Blue-noise-bias term added to the cost function — complicates
  the linear system structure
- F annealing (start near identity, widen toward Bonn) — cheap to
  add but unclear if it fixes the structural-dither issue

Estimate: ≥ 2 days more work, no guarantee.

## Algorithmic learnings

### Stage A: why it beats pngquant
The key is the cost function space. pngquant uses a perception-
weighted histogram in (something close to) sRGB and does median-cut
+ Voronoi iteration. Median-cut splits by largest-range axis,
which underweights perceptually-important dimensions in dim
regions. Lloyd iteration in OKLab with k-means++ init solves a
genuinely different optimisation, and OKLab's perceptual uniformity
is what SSIMULACRA2 (a perceptual metric) rewards.

PSNR consistently slightly worse, S2 consistently slightly better:
the textbook OKLab-vs-RGB-MSE trade-off. Memory
`feedback_oklab_to_srgb_migration.md` documents this for png2amiga's
own metrics — same pattern reproduces here vs pngquant.

### Stage A: why parallel restarts help
At K=256 and K=16 in particular, restart variance was substantial
(individual ΔS2 per image varied across restarts by ±2 to ±10 S2).
Picking the lowest-SSE restart cleanly captures the basin-escape
benefit. Subsampled k-means++ init (8K of N pixels) is fast and
seed quality is statistically indistinguishable from full-image
init for downstream Lloyd convergence.

### Stage C: why naive parallel ICM diverged
The cost-change formula `ΔE = ||F||²||p[k']-p[k]||² - 2(p[k']-p[k])·g`
assumes a *single* pixel flip. Two simultaneous flips at
non-independent pixels make the actual cost change
**non-additive** because their filter footprints overlap. With
1M pixels updating in lockstep, the "expected good move" for each
pixel collides with its neighbours' moves — the cost oscillates.

### Stage C: why graph-coloured ICM works algorithmically but
### still loses on raw S2
Graph-colouring guarantees within-sub-grid pixels are independent
(stride 3 > 2× filter radius). With 9 sub-grids per iter and
fresh g(x) recomputed before each, ICM is monotone-decreasing on
the Bonn cost. Verified by blurred PSNR.

The scolorq cost function REWARDS dither structure — the higher
the high-frequency content (within filter cancellation), the
lower the cost. Raw SSIMULACRA2 goes the opposite direction:
it has its own (different) low-pass filter and penalises
high-frequency residuals not aligned with the input. The
mismatch is structural — the algorithm is solving a different
problem from what the metric measures.

## Files in this branch

```
experiments/gpu_quantize/
  build.sh             # Apple-clang + xcrun metal; isolated from main GCC build
  bench.sh             # 10-50 image DIV2K bench harness vs pngquant
  colorspace.hpp       # standalone sRGB/OKLab port (main color_space.hpp
                       # has GCC-only constexpr LUT paths that don't compile
                       # under Apple clang)
  quant.cpp            # host driver: stbi_load → Lloyd init → GPU dispatches
                       # → Stage C optional → render → stbi_write
  quant.metal          # Stage A kernels (assign_and_accumulate,
                       # finalize_centroids)
  scolorq.metal        # Stage C kernels (filtered_output, compute_g,
                       # assign_subgrid, assign_only, build_Mb)
  FINDINGS.md          # this file

third_party/metal-cpp/  # Apple metal-cpp headers (Apache-2.0, ~1.3 MB,
                       # vendored from developer.apple.com 2026-05-04)
```

## Productisation path

If shipping Stage A into png2amiga:

1. **Where it slots in**: a new `--quantizer gpu-restart` option,
   alongside the existing `median_cut`, `pnn`, `ocs_bruteforce`. Only
   meaningful for AGA modes (24-bit palette precision); skip on OCS
   where the existing exact OCS quantizer wins.

2. **Cross-platform gating**: macOS-only via `if(APPLE)` in CMake.
   Use Objective-C++ glue or metal-cpp; keep the main project on
   gcc-15 by isolating the Metal TU (`*.mm` or alternate compiler
   per-target).

3. **Buffer reuse**: current experiment allocates fresh GPU buffers
   per call. For batch processing, hoist allocation out of the
   per-image path.

4. **WASM compatibility**: Metal is macOS-only. WASM build keeps
   the existing CPU path (median-cut + Lloyd refine).

5. **Bench gate before merge**: re-run `bench_pngquant.sh` (the
   existing 50-image harness in `~/png2amiga-testset`) at K=8 and
   K=256 to confirm the 10-image numbers replicate. The mean Δ
   tracking from
   `project_png2amiga_pngquant_bench.md` is the regression gate.

## What was NOT tried (saved for revisit)

- **Random pixel order ICM** (would produce blue-noise dither;
  needs per-iter random permutation kernel)
- **Blue-noise-bias term** in scolorq cost (penalty for
  spatially-correlated assignment patterns)
- **F annealing** (start tight, widen toward Bonn)
- **MPSGraph for differentiable VQ** (Tier 3 from the original
  proposal; would test gradient-descent over the palette
  embedding; unclear gain vs already-working Lloyd+restart)
- **Wu's variance-minimization split** (memory
  `project_png2amiga_pngquant_bench.md` flags it as the gate
  before re-attempting libimagequant feedback loop; orthogonal
  to GPU work)
- **K=2 / K=3** — extremely small palettes, would push the
  variance comparison to the limit.
- **Larger image sample** (50-image full DIV2K bench, not the
  10-image sanity bench used here). The 10-image numbers are
  noisy at the ±1 S2 level.

## Memory notes

This branch updated mental models on:
- GPU palette quantization is *plumbing-easy* on Apple M3 with
  metal-cpp + atomic_float.
- The "GPU enables more iterations" thesis is **half-true**:
  many restarts of plain Lloyd does help (Stage A), but more
  iterations of joint-optimization (Stage B/C) doesn't beat
  pngquant unless the dither pattern is also shaped.
- The existing png2amiga vs pngquant gap on SSIMULACRA2 is
  smaller than once thought — Stage A already turns it into a
  win at most palette sizes. The ΔS2 -3.61 figure in
  `project_png2amiga_pngquant_bench.md` was for a
  median-cut+OKLab-kmeans implementation that hasn't pulled the
  k-means++ + parallel-restart lever.
