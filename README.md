# png2amiga

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web App](https://img.shields.io/badge/Try_it-png2amiga.app-brightgreen)](https://www.png2amiga.app)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://en.cppreference.com/w/cpp/26)

PNG/JPEG to Commodore Amiga and Atari ST/STE graphics converter. Produces IFF ILBM files, Degas .PI1/.PI2, C headers, raw bitplane data, and self-contained AmigaOS executables with built-in viewers.

**[Try it in your browser at png2amiga.app](https://www.png2amiga.app)** — live preview via WebAssembly, compile to Amiga executables server-side.

[![png2amiga.app web interface](docs/screenshot.png)](https://www.png2amiga.app)

Built for Amiga and Atari demo scene production. All color operations use [OKLab](https://bottosson.github.io/posts/oklab/) perceptual color space. Multithreaded native CLI + WASM web app. Sister project to [png2c64](https://github.com/tinic/png2c64).

## Features

**Amiga Display Modes**
- Lores (320px), Hires (640px), with interlace variants
- HAM6 (OCS, 4096 colors) and HAM8 (AGA, 16M colors)
- All HAM compound modes: ham6-lace, ham6-hires, ham8-hires-lace, etc.
- EHB (Extra Half-Brite, 64 colors)
- 1-8 bitplanes (OCS: max 5 lores / 4 hires, AGA: up to 8)
- KILLEHB for 6-plane non-EHB on AGA

**Atari ST/STE Display Modes**
- STF Low (320x200, 16 colors, 9-bit palette / 512 colors)
- STF Medium (640x200, 4 colors, 9-bit palette)
- STE Low (320x200, 16 colors, 12-bit palette / 4096 colors)
- STE Medium (640x200, 4 colors, 12-bit palette)
- Degas Elite .PI1/.PI2 output

**Copper Per-Scanline Palettes**
- Change palette registers every scanline via the Copper coprocessor
- Empirically tested DMA limits per mode/depth/chipset
- Works with all standard and HAM modes
- AGA bank switching for >32 color palettes (sorted by bank to minimize DMA)

**Image Processing**
- 22 dithering methods (16 ordered + 6 error diffusion, all in OKLab perceptual space)
- Ordered: Bayer 2x2/4x4/8x8, checker, H 2x4, clustered dot, line2/4/8, line-checker, halftone 8x8 (45-degree), diagonal 8x8 (newspaper), spiral 5x5, hexagonal 5x5/8x8, blue noise 64x64
- Error diffusion: Floyd-Steinberg, Atkinson, Sierra Lite, Stucki, Jarvis
- OCS brute-force palette quantization (k-means over all 4096 OCS colors, threaded)
- STF brute-force quantization (512 colors, 9-bit)
- AGA palette: median-cut + k-means refinement in OKLab
- HAM beam-search DP encoder (optimal quality) + greedy fallback (fast)
- Transparency support with alpha threshold and ordered alpha dithering
- Preprocessing: gamma, brightness, contrast, saturation, hue shift, sharpen, levels

**Output Formats**
- `.png` — Preview image (with pixel aspect correction and transparency)
- `.iff` — Standard IFF ILBM (compatible with DPaint, PPaint, XnView)
- `.h` — C header with UWORD bitplane arrays + palette + copper list
- `.cpp` — Self-contained AmigaOS viewer source (compile with included toolchain)
- `.exe` — Compiled AmigaOS executable (via web app server-side compilation)
- `.adf` — Bootable Amiga floppy disk image
- `.pi1`/`.pi2` — Degas Elite (Atari ST/STE)
- `.raw` — Raw interleaved bitplane data
- `.pal` — OCS 12-bit palette (big-endian 0x0RGB)

**Amiga Executable Generation**
- Generates standalone `.cpp` viewer files with all image data inline
- Compile to AmigaOS executables using the included cross-compiler (vscode-amiga-debug submodule)
- Supports all display modes including FMODE=3, AGA bank switching, interlace field switching
- AGA chipset detection with user-friendly error message on OCS machines
- Generated source includes detailed comments explaining all hardware registers
- Web app compiles to .exe/.adf server-side via sandboxed cross-compiler

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

# Production web build
./build-web.sh
```

## Usage

```bash
# Basic conversion
./build/png2amiga input.png output.iff
./build/png2amiga input.jpg output.png

# HAM6 with optimal quality
./build/png2amiga --mode ham6 input.png output.iff

# HAM8 hires interlace on AGA
./build/png2amiga --mode ham8-hires-lace --chipset aga input.png output.iff

# Copper per-scanline palettes
./build/png2amiga --mode lores --depth 5 --copper input.png output.cpp

# Hires with 5 bitplanes (AGA, FMODE=3)
./build/png2amiga --mode hires --depth 5 --chipset aga input.png output.iff

# Generate Amiga executable
./build/png2amiga --mode ham6 input.png viewer.cpp
./build-amiga.sh viewer.cpp viewer.exe
./run-amiga.sh viewer.exe              # Launch in fs-uae (A1200)
./run-amiga.sh viewer.exe A500         # Launch as A500
./run-amiga.sh viewer.exe A1200 ntsc   # NTSC mode

# Atari ST/STE
./build/png2amiga --mode stf-low input.png output.pi1
./build/png2amiga --mode ste-low input.png output.pi1
./build/png2amiga --mode stf-med input.png output.pi2
```

## Amiga Modes

| Mode | Resolution | Max Depth | Colors | Notes |
|------|-----------|-----------|--------|-------|
| `lores` | 320px | OCS:5 AGA:8 | 2-256 | Square pixels |
| `lores-lace` | 320px | OCS:5 AGA:8 | 2-256 | Interlaced (wide pixels) |
| `hires` | 640px | OCS:4 AGA:8 | 2-256 | Tall pixels, FMODE=3 for >4 planes |
| `hires-lace` | 640px | OCS:4 AGA:8 | 2-256 | Interlaced (square pixels) |
| `ham6` | 320px | 6 (fixed) | 4096 | Hold-And-Modify (OCS) |
| `ham6-lace` | 320px | 6 (fixed) | 4096 | HAM6 interlaced |
| `ham6-hires` | 640px | 6 (fixed) | 4096 | HAM6 hires (AGA) |
| `ham8` | 320px | 8 (fixed) | 16M | Hold-And-Modify (AGA) |
| `ham8-lace` | 320px | 8 (fixed) | 16M | HAM8 interlaced |
| `ham8-hires` | 640px | 8 (fixed) | 16M | HAM8 hires |
| `ham8-hires-lace` | 640px | 8 (fixed) | 16M | HAM8 hires interlaced |
| `ehb` | 320px | 6 (fixed) | 64 | Extra Half-Brite |
| `ehb-lace` | 320px | 6 (fixed) | 64 | EHB interlaced |

## Atari Modes

| Mode | Resolution | Depth | Colors | Palette |
|------|-----------|-------|--------|---------|
| `stf-low` | 320x200 | 4 (fixed) | 16 | 9-bit (512 colors) |
| `stf-med` | 640x200 | 2 (fixed) | 4 | 9-bit (512 colors) |
| `ste-low` | 320x200 | 4 (fixed) | 16 | 12-bit (4096 colors) |
| `ste-med` | 640x200 | 2 (fixed) | 4 | 12-bit (4096 colors) |

## Copper DMA Budget (Empirically Tested)

| Mode | Depth | Chipset | Max Changes/Line |
|------|-------|---------|-----------------|
| Lores | 1-5 | OCS | 1<<depth |
| Lores | 5 | AGA | 16 |
| Lores | 6 | AGA | 8 |
| Lores | 7-8 | AGA | 4 |
| Hires | 5 | AGA | 16 |
| Hires | 6 | AGA | 8 |
| Hires | 7-8 | AGA | 4 |
| HAM6 | 6 | OCS | 8 |
| HAM6 | 6 | AGA (lores) | 8 |
| HAM6 | 6 | AGA (hires) | 4 |
| HAM8 | 8 | AGA (lores) | 4 |
| HAM8 | 8 | AGA (hires) | 8 |

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

- `m68k-amiga-elf-gcc` 14.2.0 cross-compiler (macOS, Linux, Windows)
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
./run-amiga.sh viewer.exe A500 ntsc
```

## Web Deployment

```bash
# Build everything
./build-web.sh

# Server setup (Debian 13)
apt install nginx bubblewrap python3
ln -s /var/www/png2amiga/service/nginx.conf /etc/nginx/sites-enabled/png2amiga
cp service/png2amiga-compile.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now png2amiga-compile
systemctl reload nginx
```

The compile backend runs in a bubblewrap sandbox with `-nostdinc` to prevent file disclosure.

## Architecture

Sister project to [png2c64](https://github.com/tinic/png2c64) — same pipeline pattern, same coding conventions.

- **C++26** (GCC 15), C++23 (Emscripten), strict warnings-as-errors
- **Error handling**: `Result<T> = std::expected<T, Error>`, no exceptions
- **Color math**: OKLab perceptual color space throughout
- **Quantization**: OCS brute-force k-means (4096 discrete candidates, threaded), STF (512 colors), or median-cut (AGA)
- **HAM encoding**: Greedy per-pixel or DP beam search with configurable beam width
- **Performance**: fast_cbrt (IEEE 754 bit hack), precomputed OKLab, WASM SIMD + LTO
- **Terminal preview**: iTerm2 inline image protocol
- **Web**: Vue 3 + PrimeVue, WASM worker thread, compile backend with bwrap sandbox

## License

MIT
