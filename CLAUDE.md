# png2amiga

PNG/JPEG to Commodore Amiga graphics converter written in C++26.

## Build

```bash
cmake -B build -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 .
cmake --build build
ctest --test-dir build --output-on-failure
```

- Requires GCC 15 (`g++-15`, installed via Homebrew at `/opt/homebrew/bin/g++-15`)
- Uses `-std=c++2c` (C++26 draft), strict warnings-as-errors
- Warning flags in `cmake/CompilerWarnings.cmake`
- stb_image/stb_image_write vendored in `third_party/`, compiled as C (`stb_impl.c`) with `SYSTEM` include to suppress warnings
- The local clang LSP does NOT understand C++26 features — its diagnostics about `std::expected`, `std::print`, multidimensional `operator[]`, etc. are false positives. Always verify with the actual GCC 15 build.

## Project Structure

```
src/
  main.cpp            CLI entry point, argument parsing, pipeline orchestration
  types.hpp           Color3f, Image (multidim operator[]), Palette, Result<T>, concepts
  color_space.hpp     sRGB<->linear, linear<->OKLab (all constexpr, compile-time LUT)
  amiga.hpp           Mode enum, ModeParams, Chipset, resolution presets, default_width()
  palette.hpp         OCS 12-bit / AGA 24-bit palette types, quantization, find_nearest(),
                      EHB half-brite palette generation (make_ehb_palette)
  quantize.hpp/.cpp   Median-cut palette quantization (auto-generate optimal N-color palette)
  dither.hpp/.cpp     Ordered + structure-aware + palette-aware + error diffusion (Floyd-Steinberg,
                      Atkinson, Sierra Lite, Stucki, Jarvis), all in OKLab space
  png_io.hpp/.cpp     Load/save/encode PNG via stb_image
  palette_io.hpp/.cpp Load/save palettes: GIMP .gpl, IFF CMAP, hex text, OCS .pal binary
  scale.hpp/.cpp      Separable bicubic (Mitchell-Netravali / Catmull-Rom)
  preprocess.hpp/.cpp Brightness/contrast/saturation/gamma + OKLab palette range matching
  bitplane.hpp/.cpp   Bitplane encoder/decoder: indexed pixels <-> interleaved/standard bitplanes
  ham.hpp/.cpp        HAM encoder + decoder (HAM4-HAM8): greedy + DP beam search + dithering, render_ham()
  iff.hpp/.cpp        IFF ILBM writer: BMHD, CMAP, CAMG, BODY with ByteRun1 compression
  cheader.hpp/.cpp    C header output: UWORD bitplane arrays + palette for Amiga C projects
  api.hpp/.cpp        Clean API layer for CLI and future WASM frontend
  log.hpp             Logging helpers (std::print on native, no-op on WASM)
  degas.hpp/.cpp      Atari Degas .pi1/.pi2/.pi3 writer (STF low/med/hi)
  cga_font.hpp        IBM CGA 8x8 font (viler CGA.F08, public-domain ROM reconstruction)
  cga_text.hpp/.cpp   CGA text-mode glyph-matching encoder (80x100 super-chunky)
  cheader_dos_c.*     16-bit DOS C viewer generator (ia16-elf-gcc, real-mode 8088+)
```

## Amiga Graphics Modes

| `--mode` | Default Width | Bitplanes | Base Colors | Modify Bits | Notes |
|----------|---------------|-----------|-------------|-------------|-------|
| `lores`  | 320           | 1-6       | 2-64        | —           | Standard low-res |
| `hires`  | 640           | 1-4       | 2-16        | —           | High-res, half the colors |
| `ham4`   | 320           | 4         | 4           | 2           | HAM with 4 bitplanes |
| `ham5`   | 320           | 5         | 8           | 3           | HAM with 5 bitplanes |
| `ham6`   | 320           | 6         | 16          | 4           | Hold-And-Modify (OCS) |
| `ham7`   | 320           | 7         | 32          | 5           | HAM with 7 bitplanes |
| `ham8`   | 320           | 8         | 64          | 6           | Hold-And-Modify (AGA) |
| `ehb`    | 320           | 6         | 64          | —           | Extra Half-Brite |

Height is computed from source aspect ratio (always square pixels).
Use `--interlace` to set the LACE bit in CAMG. Override dimensions with `--width` and/or `--height`.

## Atari ST/STE Modes

| `--mode` | Resolution | Bitplanes | Colors | Palette precision |
|----------|------------|-----------|--------|-------------------|
| `stf-low` | 320×200   | 4         | 16     | 9-bit (STF) |
| `stf-med` | 640×200   | 2         | 4      | 9-bit (STF) |
| `stf-hi`  | 640×400   | 1         | 2 (B/W)| monochrome monitor |
| `ste-low` | 320×200   | 4         | 16     | 12-bit (STE) |
| `ste-med` | 640×200   | 2         | 4      | 12-bit (STE) |
| `ste-hi`  | 640×400   | 1         | 2 (B/W)| monochrome monitor |

ST/STE hi-res is hardware-locked to monochrome (white + black). Output to
`.pi1` / `.pi2` / `.pi3` for Degas (extension determines low/med/hi).

## IBM PC / DOS Modes

| `--mode` | Resolution | Colors | Notes |
|----------|------------|--------|-------|
| `cga-320` | 320×200  | 4      | Fixed palettes via `--cga-palette` (p0-low/high, p1-low/high); auto-picked if unset |
| `cga-640` | 640×200  | 2      | Monochrome |
| `cga-composite` | 160×200 effective | 16 | NTSC artifact colors from 320×200 2bpp |
| `cga-text80x100` | 80×100 cells | 16 fg×16 bg | Glyph-matched text-mode graphics (AREA 5150 style); IBM CGA 8x8 font |
| `ega-320` | 320×200 | 16 of 64 | 4-plane IrgbIRGB gamut |
| `ega-640` | 640×200 | 16 of 64 | 4-plane IrgbIRGB gamut |
| `ega-hi`  | 640×350 | 16 of 64 | 4-plane IrgbIRGB gamut (full 6-bit per-slot) |
| `vga-13h` | 320×200 | 256    | 8bpp chunky, 18-bit DAC |
| `vga-10h` | 640×350 | 16     | 4-plane planar, 18-bit DAC |
| `vga-12h` | 640×480 | 16     | 4-plane planar, square pixels, 18-bit DAC |

`--native-par` preserves source aspect by letterboxing/pillarboxing into the
fixed hardware buffer (default is to stretch-fill).

## Architecture Notes

- **Sister project**: Architecture mirrors `png2c64` — same pipeline pattern, same coding conventions
- **Error handling**: `Result<T> = std::expected<T, Error>` throughout, no exceptions
- **Color math**: All perceptual operations use OKLab color space
- **Preprocessing pipeline**: gamma -> sharpen -> levels -> brightness/contrast/saturation/hue (OKLab)
- **Dithering**: 58 methods. Ordered: Bayer 2×2 / 4×4 / 8×8 plus dispersed-dot 3×3 / 5×5 / 6×6 / 7×7, non-square Bayer 4×2 / 2×4, h2x4 / v4x2, halftone8x8 / diagonal8x8 / spiral5x5 / clustered-dot, line / vline 2 / 4 / 8 / -checker, crosshatch, hex 5×5 / 8×8, radial, Aseprite "old", libcaca 3×3 / 6×6, Pegasus 8×8, Cranley-Patterson rotated Bayer, Niklasson 16×16 self-nested fractal, quasicrystal, Truchet. Aperiodic / noise: Ulichney void-and-cluster, cluster-noise, blue-noise, IGN, IGN-triangle (Wronski remap), R2, R2-triangle, value-noise, white-noise. Error diffusion: Floyd-Steinberg, Atkinson (OKLab-tuned 4-cell sum=1.0 — +0.48 dB over canonical Apple 1985 weights), Sierra Lite, Stucki (OKLab-tuned 12-cell weights with row-2-corner negatives — +0.41 dB over Stucki 1981 weights), Jarvis (OKLab-tuned 12-cell weights — +0.33 dB over Jarvis-Judice-Ninke 1976 weights), Ostromoukhov, Gilbert, Riemersma. Structure-aware ED: structure-fs (Laplacian-modulated), contrast-fs (Mould 2009), Zhou-Fang. Palette-aware ordered: Yliluoma method 1, Yliluoma method 2. All operate in OKLab with serpentine scanning and configurable error clamping. CAP and SCAP modes properly support all error-diffusion / structure-aware / Yliluoma variants — bias maps and Riemersma queues are precomputed once over the full image so per-scanline palette swaps don't degrade them to 1-row strips.
- **Chipset**: `--chipset ocs|aga` selects OCS (12-bit, 4096 colors) or AGA (24-bit, 16M colors). Auto-detected from mode: HAM7/HAM8 force AGA; all others default to OCS but can be overridden (e.g., `--chipset aga --mode ham6` for 24-bit base palette precision on AGA hardware).
- **Palette quantization**: Two algorithms depending on chipset. OCS: brute-force histogram + k-means refinement over all 4096 OCS colors with threaded cluster optimization — produces genuinely optimal OCS palettes. AGA: standard median-cut in continuous linear RGB space. Auto-palette is the default; `--palette <file>` loads an external palette instead.
- **EHB palette**: Extra Half-Brite mode auto-generates 32 base colors via median-cut, then derives 32 half-brightness copies (halving sRGB DAC values, matching Amiga hardware). Dithering uses all 64 colors. IFF CMAP only stores the 32 base colors; hardware generates the half-brite entries. CAMG flag 0x80.
- **Bitplane encoding**: The core Amiga-specific piece. Supports interleaved (hardware DMA order) and standard (plane-sequential) layouts. Word-aligned rows (16-pixel boundary). MSB-first bit packing.
- **HAM encoding**: Generalized for any bitplane depth 4-8 (HAM4/HAM5/HAM6/HAM7/HAM8). For N bitplanes: 2 control bits + (N-2) data bits, giving 2^(N-2) base palette colors and (N-2)-bit precision per modify channel. Two quality modes: `--ham-quality fast` uses greedy per-pixel optimization; `--ham-quality optimal` (default) uses DP beam search that considers all reachable color states at each pixel and prunes to the top `--ham-beam N` candidates (default 48) by cumulative OKLab perceptual error. Base palette auto-generated via median-cut. Operations per state = base_colors + 3 * 2^data_bits. Per-scanline cost is O(width * beam * ops).
- **HAM dithering**: Error diffusion can be applied during HAM encoding via `--dither` flag. For greedy mode: full per-pixel error diffusion with serpentine scanning in OKLab space. For DP beam search: inter-scanline error diffusion as a pre-pass (adjusts targets with accumulated errors from previous scanlines), then DP optimizes within the row, then output errors propagate to future scanlines. Supports Floyd-Steinberg, Atkinson, Sierra Lite, Stucki, and Jarvis kernels. Strength and error clamping configurable via `--dither-strength` and `--error-clamp`. Default for HAM modes: floyd-steinberg (same as standard modes).
- **HAM bit expansion**: `expand_to_8bit(val, bits)` converts an M-bit value to 8-bit sRGB via bit replication (e.g., 4-bit 0xN -> 0xNN, 6-bit abcdef -> abcdefab). General algorithm fills 8 bits by repeating the value's bits from MSB down. Used in both encoding (to compute actual output colors) and decoding (render_ham).
- **HAM preview rendering**: `ham::render_ham()` decodes HAM bitplane data by simulating the hardware: extracts control bits (top 2) and data bits per pixel, processes left-to-right per scanline starting from palette[0], applying SET/MODIFY-R/MODIFY-G/MODIFY-B operations. Works for any data_bits (2-6). This is required for correct PNG preview -- simple palette[index] lookup does NOT work for HAM because the index encodes operations, not palette colors.
- **IFF ILBM output**: Standard Amiga interchange format with BMHD, CMAP, CAMG, BODY chunks. ByteRun1 compression supported. EHB mode writes only 32 base colors in CMAP.
- **C header output**: Generates .h files with `const UWORD planeN[]` arrays for each bitplane, OCS 12-bit palette array, and metadata #defines (WIDTH, HEIGHT, DEPTH, BPR, CAMG, COLORS). Supports per-plane or interleaved layout. EHB mode writes only 32 base palette entries. Symbol names auto-derived from filename or user-specified via --symbol.
- **Cropping**: Manual crop (`--crop x,y,w,h`) before scaling. Auto-crop (`--crop-auto`) center-crops source to target mode aspect ratio.
- **Palette**: OCS uses 12-bit color (4 bits/channel, 4096 possible colors). AGA uses 24-bit (8 bits/channel). Auto-palette snaps to OCS precision via nibble replication (0xN -> 0xNN).
- **Palette I/O**: Loads palettes from GIMP .gpl (text with "GIMP Palette" header), IFF CMAP (reads CMAP chunk from any IFF file), or text hex (one RRGGBB per line). Auto-detects format from file content. Loaded colors are quantized to OCS 12-bit. Writes OCS palette as big-endian 16-bit 0x0RGB values (.pal format).
- **Raw bitplane output**: Writes raw interleaved bitplane data with no container (.raw). For direct inclusion in assembly or bootblock projects. Companion .pal file provides the palette.
- **DOS viewer output**: Output path ending in `.c` + an IBM PC mode emits a 16-bit freestanding C viewer compilable with `ia16-elf-gcc`. `cheader_dos_c::generate` dispatches per mode and inlines the raw hardware-layout bytes as far-data (`__far`) arrays since EGA/VGA planar frames exceed 64 KB. For CGA-320 the auto-picked palette variant is encoded into the viewer's `outb(0x3D9, ...)` so the hardware matches the encoder's choice; without that wiring the viewer would show wrong colors when the encoder picked a non-default variant.
- **CGA text-mode graphics**: `cga-text80x100` does glyph-per-cell matching against the IBM CGA 8x8 font (`cga_font_data.inc`, viler CGA.F08, 2 KB). Per cell, picks `(char, fg, bg)` from the fixed 16-color master palette to best match the source cell. Raw output = `cols × rows × 2` bytes (standard PC text buffer, char + attr pairs); no palette appended — attribute byte carries both fg and bg indices. Metric: `--cga-text-metric blur` (default, Pappas-Neuhoff-ish blurred OKLab distance) or `mse`.
- **Atari Degas I/O**: `.pi1` / `.pi2` / `.pi3` extension on output path writes Degas format (legacy 32-byte header + word-interleaved bitplanes) for `stf_low`/`ste_low`, `stf_med`/`ste_med`, `stf_hi`/`ste_hi` respectively. Atari uses word-interleaved bitplane layout (`bitplane::Layout::word_interleaved`), not the Amiga row-interleaved layout. ST hi-res is hardware-locked to monochrome; both `stf_hi` and `ste_hi` force `{white, black}` regardless of chipset palette precision.

## Key Design Decisions

- Bitplane depth is user-configurable (--depth), not locked to mode defaults
- Always square pixels (IFF BMHD aspect 10:10). No PAL/NTSC video standard concept.
- Height computed from source aspect ratio. Mode only defines default width (320 or 640).
- Interlace is a boolean flag (--interlace), sets LACE bit (0x0004) in CAMG.
- Default dithering: Floyd-Steinberg error diffusion (strength 1.0)
- Default palette: auto-generated optimal palette via median-cut
- OCS 12-bit color quantization: sRGB -> linear -> nearest OCS color via nibble replication
- IFF writer produces spec-compliant ILBM files readable by DPaint, PPaint, XnView, etc.
- HAM encoding defaults to DP beam search (--ham-quality optimal, --ham-beam 48); --ham-quality fast selects the greedy fallback
- Dithering IS applied to HAM modes (default: floyd-steinberg). Use `--dither none` to disable. Both error diffusion (floyd-steinberg, atkinson, sierra-lite, stucki, jarvis) and ordered dithering (bayer, checker, line, etc.) are supported for HAM. Ordered dithering applies threshold bias to target colors before HAM encoding.
- **Transparency**: When input has alpha channel, color 0 is reserved as transparent. Two modes: `--alpha-threshold <0-1>` (default 0.5, hard cutoff) and `--alpha-dither` (dithers alpha to 1-bit using the selected dither method). IFF output sets BMHD masking=2 (transparent color) when transparency is present.
- EHB is always 6 bitplanes; the --depth flag is ignored for EHB mode
- EHB half-brightness is computed in sRGB space (matching Amiga hardware DAC halving)
- C header palette uses OCS 12-bit format (0x0RGB) for all modes
- Cropping happens before scaling in the pipeline (crop -> scale -> preprocess -> dither -> encode)
- Auto-crop center-crops to the target aspect ratio
- --palette <file> loads an external palette (GIMP .gpl, IFF CMAP, or hex text); colors are quantized to OCS 12-bit and trimmed to max_colors for the mode
- .raw output writes interleaved bitplane data only (no container); .pal output writes companion OCS 12-bit palette (2 bytes/color, big-endian)

## WASM Build

```bash
# Requires Emscripten SDK (emcmake / emcc)
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release .
cmake --build build-wasm
```

- Produces `build-wasm/png2amiga.js` and `build-wasm/png2amiga.wasm`
- Uses C++23 (`-std=c++23`) under Emscripten (C++26 features not available in clang/emscripten)
- Embind (`--bind`) for JS interop; ES6 module output (`-sEXPORT_ES6=1 -sMODULARIZE=1`)
- Module factory exported as `createPng2Amiga`
- No filesystem (`-sNO_FILESYSTEM=1`) — all I/O via Uint8Array
- `src/wasm_bindings.cpp` exposes: `convert`, `convertRGBA`, `convertIFF`, `convertHeader`, `convertRaw`
- `src/log.hpp` compiles to no-ops under `__EMSCRIPTEN__` (no std::print)
- Uses `typed_memory_view` for efficient bulk copy (no per-byte loop)

## Web Frontend

```bash
cd web
npm install
npm run dev      # dev server with HMR
npm run build    # production build to ../docs/
```

- Vue 3 + PrimeVue + Aura dark theme (same stack as png2c64)
- Vite resolves `@wasm` alias to `../build-wasm/` for WASM imports
- `web/src/composables/useWasm.js` — loads WASM module, exposes typed wrappers
- `web/src/composables/useImageUpload.js` — drag-drop / file picker
- `web/src/lib/options.js` — mode/chipset/dither definitions, defaults
- `web/src/components/Converter.vue` — main UI: controls left, sticky preview right
- Live preview via `convertRGBA` (nearest-neighbor 2x upscale on canvas)
- Export buttons: PNG, IFF ILBM, C header (.h), raw bitplanes
- Debounced conversion (150ms) on any option change

## What's Next

(All planned features implemented. Future ideas:)
1. **More example images**: Add demo images to `web/public/examples/`
2. **CRT filter**: PAL scanline/bloom/vignette post-processing for preview (like png2c64)
3. **Batch mode**: Process multiple images with shared palette
