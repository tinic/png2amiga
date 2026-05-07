# png2amiga

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web App](https://img.shields.io/badge/Try_it-png2amiga.app-brightgreen)](https://www.png2amiga.app)
[![GitHub](https://img.shields.io/github/stars/tinic/png2amiga?style=social)](https://github.com/tinic/png2amiga)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://en.cppreference.com/w/cpp/26)

**Open-source** PNG/JPEG/WebP → Commodore Amiga, Atari ST/STE,
IBM PC (CGA / EGA / VGA), Commodore 64, Sega Genesis, and SNES Mode 7
image converter. Supports OCS/AGA bitplane, HAM6/HAM8, EHB, and
**sliced-HAM (SHAM) via copper palettes** with perceptual OKLab color
matching. Writes IFF ILBM, Degas `.PI1`/`.PI2`/`.PI3`, C64 `.prg` /
`.koa` / `.hir`, Genesis SGDK headers, SNES tile/tilemap `.bin`, C
headers, raw bitplanes, and standalone AmigaOS viewer `.cpp` source.
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
with hires and/or interlace variants, EHB. 1–8 bitplanes within
chipset limits. Optional **sliced palette** (per-line copper swaps —
the same technique behind [Sliced HAM / SHAM](https://en.wikipedia.org/wiki/Hold-And-Modify#Sliced_HAM),
in use since 1989) and **strip palette** (additional mid-line swaps in
the active scanline, used in demoscene productions like Desire's
*Shuffling Around the Christmas Tree*).

**Atari modes**: STF Low/Medium/Hi, STE Low/Medium/Hi (9-bit palette
on STF, 12-bit on STE; ST-Hi is hardware-locked monochrome). Degas
Elite `.PI1`/`.PI2`/`.PI3` output.

**IBM PC modes**: CGA 320×200 / 640×200 / composite (NTSC artifact
colors), CGA text-mode glyph matching at 80x{200,100,50,25} and
40x{200,100} cell grids, EGA 320×200 / 640×200 / 640×350 (16 of the
64-color IrgbIRGB gamut), VGA Mode 13h (320×200, 256-color chunky),
Mode 10h (640×350, 16-color planar), Mode 12h (640×480, 16-color
planar). 16-bit DOS viewer `.c` output for `ia16-elf-gcc` compilation.

**Commodore 64**: VIC-II hires (320×200, 2 colors per 8×8 cell),
multicolor (160×200, 4 per 4×8), FLI / AFLI (per-row screen-RAM
swaps for more colors per cell), PETSCII (40×25 text-mode glyph
matching against the C64 character ROM), and custom-charset modes
(hires + multicolor) that build a per-image 256-glyph charset with
Hamming-distance pair merging when content overflows. Outputs
`.prg` / `.koa` / `.hir` for direct loading on real hardware.

**Sega Genesis / Mega Drive**: H32 (256×224) and H40 (320×224) with
optional Shadow/Highlight extension. 4 palette lines × 16 BGR333,
8×8 4bpp tiles + tilemap. SGDK `.h` / `.bin` output.

**SNES Mode 7**: 256-color BGR555-palette and 2048-color Direct
Color (BBGGGRRR) variants. Affine-transformable 8bpp BG1, ≤256
unique 8×8 tiles via greedy distance-merging when content overflows.

**Palette quantizers**: GPU-accelerated parallel-restart Lloyd
k-means in OKLab on Apple GPU (default for AGA / VGA when Xcode's
Metal toolchain is available; mean ΔS2 +2.6..+3.4 vs pngquant on
DIV2K-100+Kodak-24 across K=8..256), OCS brute-force (histogram +
k-means over all 4096 OCS colors), PNN agglomerative (Ward's
linkage in OKLab — default for HAM AGA), and median-cut + k-means
refinement in OKLab (CPU fallback when Metal isn't available).

**Dithering**: 64 methods.

- **Error Diffusion** (19) — Floyd–Steinberg, Sierra-Lite, Atkinson,
  Jarvis, Stucki, Gilbert, Riemersma, DBS (slow); palette-aware
  planning: Optimal Checker, Optimal Line, Optimal Line-Checker,
  Tri-tone, Knoll, Yliluoma 1 (exhaustive) and Yliluoma 2 (greedy +
  luma-weighted variant); structure-aware: Structure-FS,
  Contrast-FS, Zhou–Fang.
- **Bayer** (14) — Bayer 2×2 / 4×4 / 8×8 / 3×3 / 5×5 / 6×6 / 7×7,
  non-square Bayer 4×2 and 2×4, plus the matrices Aseprite (old
  4×4), libcaca (3×3 and 6×6), Pegasus 8×8 shipped, and
  Cranley–Patterson rotated Bayer.
- **Halftone** (4) — Halftone 8×8, Diagonal Newspaper, Spiral 5×5,
  Clustered Dot.
- **Hatching** (9) — horizontal Lines 2 / 4 / 8 + Line Checker,
  vertical VLines 2 / 4 / 8 + VLine Checker, Crosshatch.
- **Pattern** (8) — Checker, Wide 2×4, Tall 4×2, Hexagonal 8×8 and
  5×5, Radial, Quasicrystal, Truchet.
- **Noise** (10) — Blue Noise, Void & Cluster, Cluster Noise,
  Niklasson 16×16 Fractal, IGN and IGN-triangle, R2 and
  R2-triangle, Value Noise, White Noise.

**HAM encoding**: DP beam search with a triple-pixel refinement pass
(default on) that catches the fringe-lag artifacts 1-pixel DP misses.
`--ham-fast` switches to the greedy encoder (~15× faster, ~0.04 dB
quality cost) for live preview or batch video processing.

**Output**: `.png` preview, `.iff` ILBM (Amiga), `.pi1`/`.pi2`/`.pi3`
Degas (Atari ST/STE), `.prg`/`.koa`/`.hir` (C64), `.bin` + `.h` (SNES
tile/tilemap, Genesis SGDK), `.h` C header, `.cpp` standalone viewer
source (Amiga) or `.c` (DOS, ia16-elf-gcc), `.raw` + `.pal` raw
bitplanes with palette. The `build-amiga.sh` helper compiles
`.cpp` → `.exe` → `.adf` via the bundled toolchain.

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

# Commodore 64
./build/png2amiga --mode c64-multicolor input.png output.prg    # bootable .prg
./build/png2amiga --mode c64-petscii input.png output.png       # text-mode preview

# Sega Genesis (H40 + Shadow/Highlight, SGDK header)
./build/png2amiga --mode genesis-h40-sh input.png output.h

# SNES Mode 7 (256-color palette + tilemap + tile data)
./build/png2amiga --mode snes-mode7-256 input.png output.bin
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
| `cga-text80x{200,100,50,25}` / `cga-text40x{200,100}` | 80×N or 40×N cells | 16 fg × 16 bg | Glyph + attribute matching against the IBM CGA 8×8 font; all variants except 80×200 fit in 16 KB CGA VRAM |
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
the lineage as "Sliced HAM" / SHAM / dynamic HAM. On hires the same
trick is known as **Dynamic HiRes (DHIRES)** — copper-driven 16-color
palette swaps per scanline; `--mode hires --sliced` is png2amiga's
DHIRES path. The reference HAM encoder
[ham_convert](http://mrsebe.bplaced.net/blog/wordpress/?page_id=374)
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

## Cross-fade between two images (`--fade-to`)

Encode one bitmap and morph its palette toward a second image at
runtime — joint k-means clusters every (source ⊕ target) slot together
so the same index buffer reproduces both stops. The emitted `.cpp`
viewer patches per-frame value tables on real hardware. Lores / hires
/ EHB only.

![source → target fade demo](docs/fade-demo.gif)

```
png2amiga --depth 5 --fade-to target.png source.png viewer.cpp
```

## How does it compare?

Source: `examples/makena.jpg` resized to 320×213 (Lanczos),
all encoders run with Floyd-Steinberg dither at their highest-quality
setting. Metrics: PSNR (sRGB byte distance) and SSIMULACRA2
(Cloudinary 2022 — perceptual, calibrated against human ratings;
30=low, 50=fair, 70=high quality).

All entries reserve palette index 0 for black: png2amiga's `--lock-color0`
default, ham_convert's `black_bkd` flag, and abc's `-forcecolor 0 000`
(apples-to-apples).

| Encoder     | Mode                              | PSNR (dB) | SSIMULACRA2 | Time (s) |
|-------------|-----------------------------------|----------:|------------:|---------:|
| **png2amiga** | **lores d=8 AGA + best**       | 32.30     | **82.83**   |    37.64 |
| png2amiga   | lores d=8 AGA                     | 32.38     | 82.52       |     1.54 |
| pngquant    | libimagequant 256 (`--speed 1`)   | 33.61     | 80.13       |     0.06 |
| png2amiga   | HAM6 + sliced + best              | 30.99     | 76.15       |    24.80 |
| png2amiga   | HAM6 + sliced                     | 30.50     | 75.60       |     0.38 |
| ham_convert | SHAM6 (`ham6_sliced`, `dither_fs`)| 31.81     | 74.82       |    16.15 |
| png2amiga   | HAM6 (no copper)                  | 30.22     | 72.94       |     0.26 |
| png2amiga   | HAM6 + best (no copper)           | 30.22     | 72.94       |    10.52 |
| png2amiga   | EHB + strips + best               | 29.39     | 71.60       |    20.84 |
| ham_convert | HAM6 q1 (fastest, `dither_fs`)    | 29.45     | 70.41       |     4.07 |
| ham_convert | HAM6 q7 (max quality, `dither_fs`)| 30.04     | 70.05       |    46.42 |
| abc         | HAM6 (`-floyd`)                   | 28.31     | 63.24       |     0.75 |
| png2amiga   | EHB + best (no copper)            | 24.09     | 61.16       |     7.13 |
| abc         | SHAM6 (`-floyd`)                  | 26.66     | 60.59       |     1.25 |
| png2amiga   | lores d=5 + best                  | 23.04     | 57.65       |    11.32 |
| png2amiga   | EHB (no copper)                   | 25.03     | 52.38       |     0.11 |
| png2amiga   | lores d=5                         | 24.96     | 51.55       |     0.13 |
| pngquant    | libimagequant 32 (`--speed 1`)    | 26.08     | 51.14       |     0.06 |
| ham_convert | EHB (`dither_fs`)                 | 25.82     | 49.68       |     6.07 |
| ham_convert | ocs32 (`dither_fs`)               | 24.42     | 37.70       |     6.05 |
| abc         | lores d=5 (`-floyd`, `-bpc 5`)    | 25.35     | 36.32       |     2.34 |

The harness lives at `tools/shootout/`:

```bash
cd tools/shootout
./setup.sh   # downloads ham_convert.jar, clones + builds abc on macOS
./run.sh     # encodes examples/makena.jpg (or pass your own)
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
