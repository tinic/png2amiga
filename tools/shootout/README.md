# PNG-to-Amiga converter shootout

This is the project that produces the comparison table in the main
README. Reproducible end-to-end.

## What's compared

| Encoder | HAM6 | SHAM6 | EHB | lores 32-col |
|---------|:----:|:-----:|:---:|:------------:|
| `abc` (arnaud-carre) | ✓ `-floyd` | ✓ `-floyd` | — | — |
| `ham_convert` (Solo761) | ✓ `ham6_q7` | ✓ `ham6_sliced` | ✓ `ehb` | ✓ `ocs32` |
| `pngquant` (libimagequant) | — | — | — | ✓ `--speed 1 32` |
| `png2amiga` plain | ✓ HAM6 | — | ✓ EHB | ✓ lores d=5 |
| `png2amiga` + best | ✓ HAM6 | — | ✓ EHB | ✓ lores d=5 |
| `png2amiga` + sliced/strips + best | ✓ HAM6 SLICED | — | ✓ EHB STRIPS | — |

All entries use Floyd-Steinberg as the error-diffusion kernel for
apples-to-apples comparison. Each encoder's other tuning knobs are set
to their respective "best quality" configuration.

The lores 32-colour comparison is png2amiga's plain indexed encoder
vs ham_convert's `ocs32` (5-bitplane OCS) vs libimagequant's pngquant
at 32 colours — same colour count, different palette quantizers. This
isolates the quantizer + dither head-to-head independent of HAM-style
per-pixel modify ops.

ham_convert's `ocs32` lags by ~16 SSIMULACRA2 points despite identical
colour count and dither kernel. Confirmed via a 19-combo sweep over
`propagation_{15,30,50,70,85,100}` × `color_{rgb,lab_cie76,lab_cie94,ictcp}`:
`propagation_85` is its honest peak (31.28); lower / higher prop and
non-FS dithers all score worse. Two underlying reasons: (a) ham_convert's
median-cut quantizer is HAM-tuned (palette = base + modify ops, not
plain indexed), and (b) it picks the palette once before dithering with
no post-FS refinement. libimagequant's Lab-perceptual median-cut and
png2amiga's brute-force OCS k-means + dither-aware refinement both
adapt the palette to the post-dither error distribution.


## Why no other tools

| Tool | Why excluded |
|------|--------------|
| `amigagfxmangle` (rvalles) | No headless CLI, no SHAM file output, no Floyd-Steinberg. |
| `DPaint.js` (steffest) | Browser-only GUI; no scripted invocation. |
| `AGAConv` (mschordan) | Wraps `ham_convert` internally — not an independent encoder. |
| Personal Paint / DPaint / ImageFX | Run on actual Amiga hardware; can't be scripted from macOS. |

## Running

```bash
cd tools/shootout
./setup.sh        # downloads ham_convert.jar, clones + builds abc
./run.sh          # encodes ../../examples/fantasy1.png with every entry
./run.sh path/to/your.png    # custom source
```

Outputs land in `output/`:

- `target.png` — source pre-resized to 320×213 (Lanczos), fed to all encoders
- `<entry>.png` — decoded preview from each encoder
- `<entry>.iff` — IFF where applicable
- `results.txt` — PSNR table

## How quality is measured

`scripts/psnr.py` reads each preview, resizes to target dims with
nearest-neighbour (no resampling loss), and computes two metrics:

- **PSNR** (per-channel sRGB byte distance) — encoder-independent
  reference; both `abc` and `ham_convert` quote sRGB PSNR in their own
  write-ups. Drawback: doesn't predict subjective HAM quality well —
  no-dither HAM can hit PSNR 32 dB while looking obviously banded.
- **SSIMULACRA2** (Cloudinary 2022, calibrated against human ratings;
  see `third_party/ssimulacra2/`). 30=low / 50=fair / 70=high quality.
  Modern image-codec evaluation standard. The shootout sorts winner-
  by-SSIMULACRA2 when the binary is available.

Why SSIMULACRA2 became necessary: in late 2026 we found that pure
PSNR was misleading us — sRGB-MSE-optimised HAM output won PSNR but
lost SSIMULACRA2 by ~9 points vs perceptually-optimised output, and
the perceptual metric matches what a viewer would call out. PSNR
stays in the table for cross-checks and continuity; SSIMULACRA2 is
the metric we now optimise against.

## Dependencies

- macOS-arm64 (Linux probably works; Linux-on-x86_64 abc Vulkan path
  is also wired up via the Makefile but I haven't tested it)
- Java 17+ at `/opt/homebrew/opt/openjdk/bin/java` (fallback to `java`
  on PATH); `brew install openjdk` if missing
- Python 3 with `Pillow` and `numpy` (`pip install Pillow numpy`)
- `brew install highway lcms2 jpeg-turbo libpng cmake ninja` for the
  ssimulacra2 binary (built once by setup.sh from the upstream
  submodule at `third_party/ssimulacra2/`)
- A built `png2amiga` at `<repo>/build/png2amiga` (run the project
  CMake build first)
- `pngquant` (libimagequant CLI) for the lores 32-colour comparison
  (`brew install pngquant`). The shootout skips that entry with a
  warning if pngquant isn't on PATH.
