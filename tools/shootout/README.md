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

## How PSNR is computed

`scripts/psnr.py` reads each preview, resizes to target dims with
nearest-neighbour (no resampling loss), and computes PSNR as
`20·log10(255) − 10·log10(MSE)` over per-channel sRGB byte values.

This is **not** the metric png2amiga's `--cap-best-metric psnr` option
uses internally (that's OKLab-blurred). For cross-encoder comparison
the neutral, widely-quoted sRGB-direct PSNR is the right pick:

- `abc` and `ham_convert` work in sRGB/linear and quote sRGB PSNR in
  their own write-ups
- OKLab-domain PSNR would unfairly favour png2amiga, which optimises
  against it during quantisation

## Dependencies

- macOS-arm64 (Linux probably works; Linux-on-x86_64 abc Vulkan path
  is also wired up via the Makefile but I haven't tested it)
- Java 17+ at `/opt/homebrew/opt/openjdk/bin/java` (fallback to `java`
  on PATH); `brew install openjdk` if missing
- Python 3 with `Pillow` and `numpy` (`pip install Pillow numpy`)
- A built `png2amiga` at `<repo>/build/png2amiga` (run the project
  CMake build first)
