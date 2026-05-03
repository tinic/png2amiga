# png2amiga

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web App](https://img.shields.io/badge/Try_it-png2amiga.app-brightgreen)](https://www.png2amiga.app)
[![GitHub](https://img.shields.io/github/stars/tinic/png2amiga?style=social)](https://github.com/tinic/png2amiga)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://en.cppreference.com/w/cpp/26)

**Open-source** PNG/JPEG/WebP → Commodore Amiga, Atari ST/STE, and
IBM PC (CGA / EGA / VGA) image converter. Supports OCS/AGA bitplane,
HAM6/HAM8, EHB, and **sliced-HAM (SHAM) via copper palettes** with
perceptual OKLab color matching. Writes IFF ILBM, Degas `.PI1`/`.PI2`,
C headers, raw bitplanes, and standalone AmigaOS viewer `.cpp` source.
The bundled `build-amiga.sh` wrapper runs the included
`m68k-amiga-elf-gcc` + `exe2adf` toolchain to turn the `.cpp` into a
runnable `.exe` and bootable `.adf`. DOS-mode `.c` output compiles
with `ia16-elf-gcc` into a 16-bit real-mode viewer.

**[Try it in your browser at png2amiga.app](https://www.png2amiga.app)** —
live preview via WebAssembly, server-side compile to Amiga executables.

[![png2amiga.app web interface](docs/screenshot.png)](https://www.png2amiga.app)

Aimed at retro-platform asset pipelines (Amiga / Atari / IBM PC
demoscene, hobby AmigaOS games, MS-DOS coding). All color operations
use [OKLab](https://bottosson.github.io/posts/oklab/) perceptual color
space. Sister project to [png2c64](https://github.com/tinic/png2c64).

## Features

**Amiga modes**: Lores / Hires (+ interlace), HAM6 (OCS) + HAM8 (AGA)
with hires and/or interlace variants, EHB. 1–8 bitplanes per chipset
limits. Optional **sliced palette** (per-line copper swaps — the same
technique behind [Sliced HAM / SHAM](https://en.wikipedia.org/wiki/Hold-And-Modify#Sliced_HAM),
in use since 1989) and **strip palette** (additional mid-line swaps in
the active scanline, used in demoscene productions like Desire's
*Shuffling Around the Christmas Tree*).

**Atari modes**: STF Low/Medium/Hi, STE Low/Medium/Hi (9-bit palette
on STF, 12-bit on STE; ST-Hi is hardware-locked monochrome). Degas
Elite `.PI1`/`.PI2`/`.PI3` output.

**IBM PC modes**: CGA 320×200 / 640×200 / composite (NTSC artifact
colors) / 80×100 text-mode glyph matching, EGA 320×200 / 640×200 /
640×350 (16 of the 64-color IrgbIRGB gamut), VGA Mode 13h (320×200,
256-color chunky), Mode 10h (640×350, 16-color planar), Mode 12h
(640×480, 16-color planar). 16-bit DOS viewer `.c` output for
`ia16-elf-gcc` compilation.

**Palette quantizers**: OCS brute-force (histogram + greedy over all
4096 OCS colors), PNN agglomerative (auto-selected for HAM8 / AGA), and
median-cut + k-means refinement in OKLab.

**Dithering**: 58 methods spanning ordered (Bayer 2×2…8×8 plus 3×3 /
5×5 / 6×6 / 7×7 dispersed-dot, non-square Bayer, halftone, hatching,
hexagonal, Aseprite/libcaca/Pegasus hand-tuned matrices, Cranley–
Patterson rotated Bayer, Niklasson 16×16 self-nested fractal,
quasicrystal, Truchet), aperiodic (Ulichney
void-and-cluster, cluster-noise, blue-noise, IGN ± triangle remap,
R2 ± triangle remap, value-noise, white-noise), error-diffusion
(Floyd–Steinberg, Atkinson, Sierra-Lite, Stucki, Jarvis,
Ostromoukhov variable-coefficient, Riemersma Hilbert-curve, Gilbert
space-filling-curve), structure-aware variants
(structure-FS / contrast-FS / Zhou–Fang), and palette-aware pattern
(Yliluoma method 1 + 2). All operate in OKLab.

**HAM encoding**: DP beam search with a triple-pixel refinement pass
(default on) that catches the fringe-lag artifacts 1-pixel DP misses.
`--ham-fast` switches to the greedy encoder (~15× faster, ~0.04 dB
quality cost) for live preview or batch video processing.

**Output**: `.png` preview, `.iff` ILBM, `.h` C header, `.cpp`
standalone viewer source (Amiga) or `.c` (DOS), `.pi1`/`.pi2`/`.pi3`
Degas, `.raw` + `.pal` raw bitplanes with palette. The `build-amiga.sh`
helper compiles `.cpp` → `.exe` → `.adf` via the bundled toolchain.

## Build

```bash
# Native CLI (requires GCC 15 for C++26)
cmake -B build -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 .
cmake --build build
ctest --test-dir build --output-on-failure

# WASM (requires Emscripten)
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release .
cmake --build build-wasm

# Web frontend
cd web && npm install && npm run dev

# Production web bundle (writes to docs/)
./build-web.sh
```

Pre-built Linux / macOS / Windows binaries are attached to each
[GitHub release](https://github.com/tinic/png2amiga/releases).

## Usage

```bash
# Basic conversion
./build/png2amiga input.png output.iff
./build/png2amiga input.jpg output.png

# HAM8 on AGA (default — triple refinement, FS pre-dither, PNN palette)
./build/png2amiga --mode ham8 --chipset aga input.png output.iff

# HAM8 realtime / batch profile (greedy, ~15× faster)
./build/png2amiga --mode ham8 --chipset aga --ham-fast input.png output.png

# Sliced palette — per-line copper swaps (more colors per scanline).
./build/png2amiga --mode lores --depth 5 --sliced input.png output.iff
./build/png2amiga --mode ham6 --sliced input.png output.iff

# Strip palette — mid-line swaps inside the active scanline. DPF or EHB only.
# IFF has no chunk for mid-line MOVEs, so use .cpp (runnable AmigaOS viewer)
# or .h (data-only) instead.
./build/png2amiga --mode lores --dpf --strips input.png viewer.cpp
./build/png2amiga --mode ehb --strips input.png data.h

# HAM6 + sliced palette, multi-restart search (~4-5× slower, +0.5 to +2 dB PSNR)
./build/png2amiga --mode ham6 --sliced --best input.png output.iff

# Generate a bootable Amiga floppy that displays the image
./build/png2amiga --mode ham6 input.png viewer.cpp
./build-amiga.sh viewer.cpp viewer.adf

# Launch in fs-uae (A1200 by default)
./run-amiga.sh viewer.adf
./run-amiga.sh viewer.adf A500 ntsc

# Atari ST/STE
./build/png2amiga --mode stf-low input.png output.pi1
./build/png2amiga --mode ste-low input.png output.pi1

# IBM PC (CGA / EGA / VGA)
./build/png2amiga --mode vga-13h input.png output.png        # preview
./build/png2amiga --mode ega-320 input.png viewer.c          # 16-bit DOS viewer
./build/png2amiga --mode cga-320 --cga-palette p1-high \
    input.png output.png
```

Run `./build/png2amiga --help` for the full flag reference.

## Amiga Modes

| Mode | Resolution | Max Depth | Colors | Notes |
|------|-----------|-----------|--------|-------|
| `lores` | 320px | OCS:5 AGA:8 | 2–256 | Square pixels |
| `lores-lace` | 320px | OCS:5 AGA:8 | 2–256 | Interlaced (wide pixels) |
| `hires` | 640px | OCS:4 AGA:8 | 2–256 | Tall pixels |
| `hires-lace` | 640px | OCS:4 AGA:8 | 2–256 | Interlaced (square pixels) |
| `ham6` (+ lace/hires variants) | 320/640px | 6 | 4096 | Hold-And-Modify (OCS) |
| `ham8` (+ lace/hires variants) | 320/640px | 8 | 16M | Hold-And-Modify (AGA) |
| `ehb` / `ehb-lace` | 320px | 6 | 64 | Extra Half-Brite |

## Atari Modes

| Mode | Resolution | Depth | Colors | Palette |
|------|-----------|-------|--------|---------|
| `stf-low` | 320×200 | 4 | 16 | 9-bit (512 colors) |
| `stf-med` | 640×200 | 2 | 4 | 9-bit (512 colors) |
| `stf-hi` / `ste-hi` | 640×400 | 1 | 2 (B/W) | hardware-locked monochrome |
| `ste-low` | 320×200 | 4 | 16 | 12-bit (4096 colors) |
| `ste-med` | 640×200 | 2 | 4 | 12-bit (4096 colors) |

## IBM PC Modes

| Mode | Resolution | Colors | Notes |
|------|-----------|--------|-------|
| `cga-320` | 320×200 | 4 | Fixed palettes (`--cga-palette p0-low/p0-high/p1-low/p1-high`) |
| `cga-640` | 640×200 | 2 | Monochrome |
| `cga-composite` | 160×200 effective | 16 | NTSC artifact colors from 320×200 2bpp |
| `cga-text80x100` | 80×100 cells | 16 fg × 16 bg | Glyph + attribute matching against the IBM CGA 8×8 font |
| `ega-320` / `ega-640` / `ega-hi` | 320×200 / 640×200 / 640×350 | 16 of 64 | 4-plane IrgbIRGB gamut |
| `vga-13h` | 320×200 | 256 | 8bpp chunky, 18-bit DAC |
| `vga-10h` | 640×350 | 16 | 4-plane planar, 18-bit DAC |
| `vga-12h` | 640×480 | 16 | 4-plane planar, square pixels |

`--native-par` letterboxes/pillarboxes the source into the fixed DOS
buffer; the default is to stretch-fill.

## Sliced palette (per-line copper swaps)

Add `--sliced` to any bitmap mode (lores, hires, EHB, HAM6, HAM8) to let
the Copper coprocessor rewrite palette registers in the horizontal blank
between every scanline. Each line displays with its own palette state,
and the planner picks per-line color swaps that minimise OKLab error
against the source row.

This is the same technique that's been used in Amiga demos and HAM
converters since the late 1980s — the [Wikipedia article on
HAM](https://en.wikipedia.org/wiki/Hold-And-Modify#Sliced_HAM) covers
the lineage as "Sliced HAM" / SHAM / dynamic HAM. The reference HAM
encoder [ham_convert](http://mrsebe.bplaced.net/blog/wordpress/?page_id=374)
and Leonard's [Brute Force Colors](https://arnaud-carre.github.io/2022-12-30-amiga-ham/)
both implement the per-line variant. png2amiga aims at the same target
with a perceptual error metric and applies the technique to indexed
modes too (lores, hires, EHB) rather than just HAM.

The encoder respects the real-hardware post-DDFSTOP DMA budget: **14
MOVE instructions per line** (one of the 15 copper slots is the per-line
WAIT). Safe static budget is 14 palette swaps on OCS (one MOVE per
change) and 3 on AGA (4 MOVEs per change worst-case under banked LOCT).
Auto-mode tries K+3, K+2, K+1 and picks the highest K whose worst-case
cost fits the budget — typically 6 swaps/line at depths 3–5 on AGA.

`--slice-changes N` overrides the budget; use if you want to experiment
with configurations that may exceed real hardware limits but still
display correctly on emulators.

`--best` runs a multi-restart sweep over jitter seeds, dither
strengths, and palette-diversity values, picking the trial with the
best result against `--best-metric` (SSIMULACRA2 by default). Available
on plain HAM6/HAM8, plain EHB, and any combination with sliced or
strip palette. Cost is ~20–30× the single-pass time on most modes
(HAM-CAP / strips can land closer to ~5×); typical gain is
+0.5 to +2 dB PSNR.

## Strip palette (mid-line swaps inside the active scanline)

`--strips` extends the sliced palette by issuing additional palette
MOVEs at fixed **mid-line** copper slots — so a single scanline can
display multiple palette banks across its width. Where the per-line
sliced palette gives "this row's 64 colors", strips gives "this strip's
64 colors", with strips on a 16-pixel grid. Strips ride on top of the
sliced base (each line opens with the sliced palette reload in hblank,
then mid-line swaps walk it through the visible region).

Mid-line copper register changes have been a demoscene staple for
decades — Shadow of the Beast (1989) used single-color bars, Spaceballs'
[State of the Art](https://www.pouet.net/prod.php?which=99) (1992)
pushed full mid-line palette manipulation, and recent productions like
Desire's [Shuffling Around the Christmas Tree](https://www.pouet.net/prod.php?which=90358)
(2021, code by Platon42) and [Copper Chunky](https://www.powerprograms.nl/amiga/copper-chunky.html)
by Jeroen Knoester (2021) showcase how dense the per-line copper traffic
can get. png2amiga's contribution is wiring this style of per-strip
palette change into a still-image converter on top of an OKLab
error-diffused dither.

Two strip modes:

* **DPF + strips** (`--mode lores --dpf --strips`) — OCS dual-playfield,
  3-plane PF2 (8 base colors). The 8 PF2 registers are unconditionally
  re-emitted in every line's hblank (~9 MOVEs, fixed) so mid-line swaps
  cannot leak state across lines. Up to 19 useful mid-line swaps per
  scanline; ~454 unique displayed colors per frame on a typical image.

* **EHB + strips** (`--mode ehb --strips`) — OCS Extra Half-Brite, 32
  base registers + 32 hardware-derived half-brites. Each base swap also
  updates the matching half-brite slot via the hardware DAC. Adaptive
  per-line hblank tracking keeps each line inside the 14-MOVE OCS hblank
  budget. ~1100+ unique displayed colors per frame.

Both modes are OCS-only, lores, no interlace. The planner runs 6
iterative refinement passes alternating index dither and mid-line swap
selection. Slot positions were calibrated empirically on real OCS
hardware via `--strips-probe` (see `src/strips.hpp`); the published
hardware budget is ~14 hblank MOVEs + ~20 visible-area MOVEs per line in
6-plane modes, and the calibrated slot tables sit comfortably within
that.

![strip palette copper-list density and bus usage in vAmiga's debug overlay](docs/scap.png)

The vAmiga debug overlay shows one frame's copper list and bus usage:
every visible scanline runs a near-saturated MOVE stream through the
displayed area — each band of activity is one scanline's sliced-palette
reload in hblank plus ~19 mid-line swaps inside the visible area.

## Cross-fade between images (`--fade-to`)

Encode one bitmap and morph the palette through one or more target
images at runtime — joint k-means clusters every (source ⊕ target₁ ⊕ …)
slot together so a single shared index buffer reproduces every stop.
The emitted `.cpp` viewer cycles `cop1lc` through per-frame value tables
on real hardware. Lores / hires / EHB only (sliced is rejected — see
`project_joint_cap_encoder.md`).

![day → night fade demo](docs/fade-demo.gif)

```
png2amiga --depth 5 --fade-to night.png --fade-to dawn.png day.png day.cpp
```

## How does it compare?

Source: `examples/electrichues02.jpg` resized to 320×213 (Lanczos),
all encoders run with Floyd-Steinberg dither at their highest-quality
setting. Metrics: PSNR (sRGB byte distance) and SSIMULACRA2
(Cloudinary 2022 — perceptual, calibrated against human ratings;
30=low, 50=fair, 70=high quality).

| Encoder     | Mode                              | PSNR (dB) | SSIMULACRA2 | Time (s) |
|-------------|-----------------------------------|----------:|------------:|---------:|
| **png2amiga** | **EHB + strips + best**       | 31.14     | **71.36**   |    50.03 |
| png2amiga   | HAM6 + sliced + best              | 30.94     | 69.15       |    37.38 |
| png2amiga   | HAM6 + sliced                     | 30.32     | 65.41       |     0.49 |
| ham_convert | SHAM6 (`ham6_sliced`, `dither_fs`)| 31.18     | 64.81       |    68.11 |
| png2amiga   | EHB + best (no copper)            | 29.58     | 62.88       |     7.16 |
| png2amiga   | HAM6 + best (no copper)           | 29.75     | 62.44       |    41.34 |
| ham_convert | HAM6 q7 (max quality, `dither_fs`)| 29.92     | 62.37       |   134.82 |
| png2amiga   | HAM6 (no copper)                  | 29.95     | 62.22       |     1.67 |
| ham_convert | HAM6 q1 (fastest, `dither_fs`)    | 29.67     | 57.91       |     4.11 |
| ham_convert | EHB (`dither_fs`)                 | 30.18     | 57.78       |     6.08 |
| png2amiga   | EHB (no copper)                   | 29.13     | 52.55       |     0.10 |
| abc         | HAM6 (`-floyd`)                   | 29.02     | 49.86       |     0.86 |
| abc         | SHAM6 (`-floyd`)                  | 26.40     | 42.50       |     1.71 |

The harness lives at `tools/shootout/`:

```bash
cd tools/shootout
./setup.sh   # downloads ham_convert.jar, clones + builds abc on macOS
./run.sh     # encodes examples/electrichues02.jpg (or pass your own)
```

`tools/shootout/README.md` has the full method, the rationale for the
metric, and notes on why amigagfxmangle / DPaint.js / AGAConv were
excluded.

## Amiga Executable Generation

The project includes
[vscode-amiga-debug](https://github.com/BartmanAbyss/vscode-amiga-debug)
as a submodule, which provides the `m68k-amiga-elf-gcc` cross-compiler,
`elf2hunk`, `exe2adf`, `fs-uae`, and AmigaOS SDK headers — everything
needed to produce bootable disk images locally.

```bash
git submodule update --init

./build/png2amiga --mode ham6 input.png viewer.cpp
./build-amiga.sh viewer.cpp viewer.adf
./run-amiga.sh viewer.adf
```

The generated viewer takes the system, sets up the copper list
(including per-line sliced-palette changes if `--sliced` was used and
mid-line strip swaps if `--strips` was used), and waits for the left
mouse button to exit.

## Build-system integration (CMake / Make / Ninja)

png2amiga is designed to slot into a CMake-driven asset pipeline (e.g.
VSCode + vscode-amiga-debug + WinUAE). Relevant flags:

| Flag | Purpose |
|---|---|
| `-q` / `--quiet` | Suppress stdout status; errors still go to stderr |
| `--json` | Emit a JSON status object on success (implies `--quiet`) |
| `--depfile <path>` | Write a Make-format depfile so changes to `--palette` files trigger a rebuild |
| `--list-modes` | Print supported modes and exit (pair with `--json` for machine-readable catalog) |

**Exit codes** follow `sysexits.h` so `RESULT_VARIABLE` distinguishes
failure categories: `0` ok, `1` internal/encode error, `64` usage error
(bad CLI args), `66` input file unreadable, `73` output write failed.

**CMake helper module** (`cmake/Png2amiga.cmake`) provides
`png2amiga_add_image()`:

```cmake
include(/path/to/png2amiga/cmake/Png2amiga.cmake)

png2amiga_add_image(
  TARGET   sprites
  INPUT    ${CMAKE_CURRENT_SOURCE_DIR}/art/title.png
  OUTPUT   ${CMAKE_CURRENT_BINARY_DIR}/title.h
           ${CMAKE_CURRENT_BINARY_DIR}/title.iff
  MODE     ham6
  OPTIONS  --sliced --ham-beam 32
  PALETTE  ${CMAKE_CURRENT_SOURCE_DIR}/palette.gpl   # optional
)
```

Each `OUTPUT` becomes its own `add_custom_command` so `make -jN` /
`ninja` build them in parallel. Each command writes a `.d` depfile next
to its output for accurate dependency tracking.

**Determinism**: encoding is deterministic — same input + same flags
always produces byte-identical output. Multithreading (HAM beam search,
OCS palette quantization) uses lock-free per-row work distribution with
deterministic merge order. Safe to use under `ccache` / build cache
hashing.

## License

MIT
