# png2amiga

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web App](https://img.shields.io/badge/Try_it-png2amiga.app-brightgreen)](https://www.png2amiga.app)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://en.cppreference.com/w/cpp/26)

PNG/JPEG to Commodore Amiga graphics converter. Produces IFF ILBM files, C headers, raw bitplane data, and self-contained AmigaOS executables with built-in viewers.

**[Try it in your browser at png2amiga.app](https://www.png2amiga.app)** — live preview via WebAssembly, compile to Amiga executables server-side.

Built for Amiga demo scene production. All color operations use [OKLab](https://bottosson.github.io/posts/oklab/) perceptual color space. Multithreaded native CLI + WASM web app. Sister project to [png2c64](https://github.com/tinic/png2c64).

## Features

**Display Modes**
- Lores (320px), Hires (640px), with interlace variants
- HAM6 (OCS, 4096 colors) and HAM8 (AGA, 16M colors)
- EHB (Extra Half-Brite, 64 colors)
- 1-8 bitplanes (OCS: max 5 lores / 4 hires, AGA: up to 8)

**Copper Per-Scanline Palettes**
- Change palette registers every scanline via the Copper coprocessor
- Empirically tested DMA limits per mode (A500 and A1200)
- Works with all standard and HAM modes

**Image Processing**
- 16 dithering methods (10 ordered + 6 error diffusion, all in OKLab perceptual space)
- OCS brute-force palette quantization (k-means over all 4096 OCS colors, threaded)
- AGA palette: Wu's quantization + k-means refinement in OKLab
- HAM beam-search DP encoder (optimal quality) + greedy fallback (fast)
- Transparency support with alpha threshold and ordered alpha dithering
- Preprocessing: gamma, brightness, contrast, saturation, hue shift, sharpen, levels

**Output Formats**
- `.png` — Preview image (with pixel aspect correction and transparency)
- `.iff` — Standard IFF ILBM (compatible with DPaint, PPaint, XnView)
- `.h` — C header with UWORD bitplane arrays + palette + copper list
- `.cpp` — Self-contained AmigaOS viewer source (compile with included toolchain)
- `.raw` — Raw interleaved bitplane data
- `.pal` — OCS 12-bit palette (big-endian 0x0RGB)

**Amiga Executable Generation**
- Generates standalone `.cpp` viewer files with all image data inline
- Compile to AmigaOS executables using the included cross-compiler (vscode-amiga-debug submodule)
- Supports all display modes including FMODE=3, AGA bank switching, interlace field switching
- AGA chipset detection with user-friendly error message on OCS machines

## Build

```bash
# Native CLI (requires GCC 15 with C++26 support)
cmake -B build -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 .
cmake --build build
ctest --test-dir build --output-on-failure

# WASM (requires Emscripten SDK)
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release .
cmake --build build-wasm

# Web frontend
cd web && npm install && npm run dev
```

## Usage

```bash
# Basic conversion
./build/png2amiga input.png output.iff
./build/png2amiga input.jpg output.png

# HAM6 with optimal quality
./build/png2amiga --mode ham6 input.png output.iff

# HAM8 on AGA with copper per-scanline palettes
./build/png2amiga --mode ham8 --copper --chipset aga input.png output.iff

# Hires with 5 bitplanes (AGA, FMODE=3)
./build/png2amiga --mode hires --depth 5 --chipset aga input.png output.iff

# Generate Amiga executable
./build/png2amiga --mode ham6 --copper input.png viewer.cpp
./build-amiga.sh viewer.cpp viewer.exe
./run-amiga.sh viewer.exe          # Launch in fs-uae (A1200)
./run-amiga.sh viewer.exe A500     # Launch as A500
```

## Modes

| Mode | Resolution | Max Depth | Colors | Notes |
|------|-----------|-----------|--------|-------|
| `lores` | 320px | OCS:5 AGA:8 | 2-256 | Square pixels |
| `lores-lace` | 320px | OCS:5 AGA:8 | 2-256 | Interlaced (wide pixels) |
| `hires` | 640px | OCS:4 AGA:5+ | 2-32 | Tall pixels, FMODE=3 for >4 planes |
| `hires-lace` | 640px | OCS:4 AGA:5+ | 2-32 | Interlaced (square pixels) |
| `ham6` | 320px | 6 (fixed) | 4096 | Hold-And-Modify (OCS) |
| `ham8` | 320px | 8 (fixed) | 16M | Hold-And-Modify (AGA) |
| `ehb` | 320px | 6 (fixed) | 64 | Extra Half-Brite |

## Copper DMA Budget (Empirically Tested)

| Mode | Planes | FMODE | Max Changes/Line | Tested On |
|------|--------|-------|-----------------|-----------|
| Lores 1-5p | 1-5 | 0 | 16 | A500, A1200 |
| Lores 7-8p | 7-8 | 0/3 | 2 | A1200 |
| Hires 1-4p | 1-4 | 0 | 16 | A500, A1200 |
| HAM6 | 6 | 0 | 16 | A500, A1200 |
| HAM8 | 8 | 3 | 2 | A1200 |

## FMODE=3 (64-bit Fetch)

Decoded from the [AmigaOS graphics library source](https://github.com/Arquivotheca/amiga-os-src):

| Parameter | FMODE=0 | FMODE=3 |
|-----------|---------|---------|
| ddfstrt (lores) | 0x38 | 0x30 |
| ddfstrt (hires) | 0x3C | 0x34 |
| Modulo (planar) | 0 | -8 |
| Data alignment | 2 bytes | 8 bytes |
| Layout | interleaved | planar |

## Amiga Cross-Compilation

The project includes [vscode-amiga-debug](https://github.com/BartmanAbyss/vscode-amiga-debug) as a submodule, providing:

- `m68k-amiga-elf-gcc` 14.2.0 cross-compiler
- `elf2hunk` ELF to Amiga Hunk converter
- `exe2adf` executable to bootable floppy image
- `fs-uae` Amiga emulator for testing
- AmigaOS SDK headers

```bash
# Initialize submodule
git submodule update --init

# Convert image to Amiga executable
./build/png2amiga --mode ham6 input.png viewer.cpp
./build-amiga.sh viewer.cpp viewer.exe

# Run in emulator
./run-amiga.sh viewer.exe
```

## Architecture

Sister project to [png2c64](https://github.com/tinic/png2c64) — same pipeline pattern, same coding conventions.

- **C++26** (GCC 15), strict warnings-as-errors
- **Error handling**: `Result<T> = std::expected<T, Error>`, no exceptions
- **Color math**: OKLab perceptual color space throughout
- **Quantization**: OCS brute-force k-means (4096 discrete candidates, threaded) or Wu + k-means in OKLab (AGA)
- **HAM encoding**: Greedy per-pixel or DP beam search with configurable beam width
- **Terminal preview**: iTerm2 inline image protocol

## License

MIT
