# PNG-to-Amiga HAM/SHAM converter shootout

This is the project that produces the comparison table in the main
README. Reproducible end-to-end.

## What's compared

| Encoder | HAM6 | SHAM6 |
|---------|:----:|:-----:|
| `abc` (arnaud-carre) | ✓ `-floyd` | ✓ `-floyd` |
| `ham_convert` (Solo761, max-quality `q7`) | ✓ `dither_fs` | ✓ `dither_fs` (`ham6_sliced`) |
| `png2amiga` plain HAM6 | ✓ `--dither floyd-steinberg` | — |
| `png2amiga` HAM6 + CAP + cap-best | ✓ FS | — |
| `png2amiga` EHB + SCAP + cap-best | — | (SCAP fills the SHAM-equivalent role) |

All entries use Floyd-Steinberg as the error-diffusion kernel for
apples-to-apples comparison. Each encoder's other tuning knobs are set
to their respective "best quality" configuration.

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
