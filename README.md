# png2amiga

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web App](https://img.shields.io/badge/Try_it-png2amiga.app-brightgreen)](https://www.png2amiga.app)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://en.cppreference.com/w/cpp/26)

PNG/JPEG/WebP → Commodore Amiga and Atari ST/STE graphics. Produces IFF
ILBM, Degas `.PI1`/`.PI2`, C headers, raw bitplanes, and self-contained
AmigaOS executables that boot from a floppy image and display the image.

**[Try it in your browser at png2amiga.app](https://www.png2amiga.app)** —
live preview via WebAssembly, compile to Amiga executables server-side.

[![png2amiga.app web interface](docs/screenshot.png)](https://www.png2amiga.app)

Built for Amiga and Atari demoscene production. All color operations use
[OKLab](https://bottosson.github.io/posts/oklab/) perceptual color space.
Beats `ham_convert`'s highest-quality HAM8 profile by 2–4 dB PSNR on
typical test images. Sister project to
[png2c64](https://github.com/tinic/png2c64).

## Features

**Amiga modes**: Lores / Hires (+ interlace), HAM6 (OCS) + HAM8 (AGA)
with hires and/or interlace variants, EHB. 1–8 bitplanes per chipset
limits. **CAP** (Copper-Augmented Palette, per-line swaps) and **SCAP**
(Super CAP, mid-line swaps) for thousands of unique colors per frame.

**Atari modes**: STF Low/Medium (9-bit palette), STE Low/Medium (12-bit
palette), Degas Elite `.PI1`/`.PI2` output.

**Palette quantizers**: OCS brute-force (histogram + greedy over all
4096 OCS colors), PNN agglomerative (auto-selected for HAM8 / AGA), and
median-cut + k-means refinement in OKLab.

**Dithering**: 22 methods including Floyd-Steinberg, Atkinson,
Sierra-Lite, Stucki, Jarvis, Ostromoukhov variable-coefficient, Gilbert
space-filling-curve, Bayer, blue-noise, and several analytical
per-pixel patterns. All operate in OKLab.

**HAM encoding**: DP beam search with a triple-pixel refinement pass
(default on) that catches the fringe-lag artifacts 1-pixel DP misses.
`--ham-fast` switches to the greedy encoder (~15× faster, ~0.04 dB
quality cost) for live preview or batch video processing.

**Output**: `.png` preview, `.iff` ILBM, `.h` C header, `.cpp`
standalone viewer source, `.exe` compiled AmigaOS executable, `.adf`
bootable floppy image, `.pi1`/`.pi2` Degas, `.raw` + `.pal` raw
bitplanes with palette.

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

# Per-line palette (CAP) — more colors per line via Copper-Augmented Palette
./build/png2amiga --mode lores --depth 5 --cap input.png output.iff
./build/png2amiga --mode ham6 --cap input.png output.iff

# Mid-line palette swaps (SCAP) — DPF or EHB only
./build/png2amiga --mode lores --dpf --scap input.png output.iff
./build/png2amiga --mode ehb --scap input.png output.iff

# HAM6 + CAP at maximum quality (~4-5× slower, +0.5 to +2 dB PSNR)
./build/png2amiga --mode ham6 --cap --cap-best input.png output.iff

# Generate a bootable Amiga floppy that displays the image
./build/png2amiga --mode ham6 input.png viewer.cpp
./build-amiga.sh viewer.cpp viewer.adf

# Launch in fs-uae (A1200 by default)
./run-amiga.sh viewer.adf
./run-amiga.sh viewer.adf A500 ntsc

# Atari ST/STE
./build/png2amiga --mode stf-low input.png output.pi1
./build/png2amiga --mode ste-low input.png output.pi1
```

Run `./build/png2amiga --help` for the full flag reference.

## Amiga Modes

| Mode | Resolution | Max Depth | Colors | Notes |
|------|-----------|-----------|--------|-------|
| `lores` | 320px | OCS:5 AGA:8 | 2–256 | Square pixels |
| `lores-lace` | 320px | OCS:5 AGA:8 | 2–256 | Interlaced (wide pixels) |
| `hires` | 640px | OCS:4 AGA:8 | 2–256 | Tall pixels |
| `hires-lace` | 640px | OCS:4 AGA:8 | 2–256 | Interlaced (square pixels) |
| `ham6` / `ham6-lace` / `ham6-hires` | 320/640px | 6 | 4096 | Hold-And-Modify (OCS) |
| `ham8` (+ lace/hires variants) | 320/640px | 8 | 16M | Hold-And-Modify (AGA) |
| `ehb` / `ehb-lace` | 320px | 6 | 64 | Extra Half-Brite |

## Atari Modes

| Mode | Resolution | Depth | Colors | Palette |
|------|-----------|-------|--------|---------|
| `stf-low` | 320×200 | 4 | 16 | 9-bit (512 colors) |
| `stf-med` | 640×200 | 2 | 4 | 9-bit (512 colors) |
| `ste-low` | 320×200 | 4 | 16 | 12-bit (4096 colors) |
| `ste-med` | 640×200 | 2 | 4 | 12-bit (4096 colors) |

## CAP — Copper-Augmented Palette (per-line swaps)

Add `--cap` (alias `--copper`) to any bitmap mode (lores, hires, EHB,
HAM6, HAM8) to let the Copper coprocessor rewrite palette registers in
the horizontal blank between every scanline. Each line displays with its
own palette state, and the planner picks per-line color swaps that
minimise OKLab error against the source row.

The encoder respects the real-hardware post-DDFSTOP DMA budget:
**14 MOVE instructions per line** (one of the 15 copper slots is the
per-line WAIT). Safe static budget is 14 palette swaps on OCS (one MOVE
per change) and 3 on AGA (4 MOVEs per change worst-case under banked
LOCT). Auto-mode tries K+3, K+2, K+1 and picks the highest K whose
worst-case cost fits the budget — typically 6 swaps/line at depths 3–5
on AGA.

`--cap-changes N` (alias `--copper-changes`) overrides the budget; use
if you want to experiment with configurations that may exceed real
hardware limits but still display correctly on emulators.

`--cap-best` enables a slower (~4–5×) HAM CAP planner that combines
multi-candidate slot search with joint base-palette refinement. HAM6
and HAM8 + CAP only — adds **+0.5 to +2 dB PSNR** on natural images,
sometimes pushing into lossless territory on smooth gradients. The
indexed CAP planner (lores/hires/EHB) already iterates predict-dither
with column-error feedback and isn't improved further by this flag.

## SCAP — Super CAP (mid-line swaps)

`--scap` extends CAP by issuing additional palette MOVEs at fixed
**mid-line** copper slots — so a single scanline can display multiple
palette banks across its width. Where plain CAP gives "this row's 64
colors", SCAP gives "this strip's 64 colors", with strips on a 16-pixel
grid. SCAP rides on top of CAP (each line opens with the CAP base, then
SCAP swaps walk it through the visible region).

Two SCAP modes:

* **DPF + SCAP** (`--mode lores --dpf --scap`) — OCS dual-playfield,
  3-plane PF2 (8 base colors). The 8 PF2 registers are unconditionally
  re-emitted in every line's hblank (~9 MOVEs, fixed) so SCAP swaps
  cannot leak state across lines. Up to 19 useful mid-line swaps per
  scanline; ~454 unique displayed colors per frame on a typical image.

* **EHB + SCAP** (`--mode ehb --scap`) — OCS Extra Half-Brite, 32 base
  registers + 32 hardware-derived half-brites. SCAP swaps a base
  register and the corresponding half-brite is updated automatically by
  the hardware DAC. Adaptive per-line hblank tracking keeps each line
  inside the 14-MOVE OCS hblank budget. ~1100+ unique displayed colors
  per frame.

DPF+SCAP and EHB+SCAP are OCS-only and require lores (no interlace).
Both compose with the existing CAP per-line palette evolution; the
planner runs 6 iterative refinement passes alternating index dither and
SCAP swap selection. SCAP slot positions were calibrated on real OCS
hardware via the `--scap-probe` mode (see `src/scap.hpp` for the
empirically-determined timing tables).

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

The generated viewer takes the system, sets up the Copper list
(including per-line CAP changes if `--cap` was used and mid-line SCAP
swaps if `--scap` was used), and waits for the left mouse button to
exit.

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
  OPTIONS  --cap --ham-beam 32
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
