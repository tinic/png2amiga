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
  dither.hpp/.cpp     Ordered (Bayer 2x2/4x4/8x8) + error diffusion (Floyd-Steinberg,
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

## Architecture Notes

- **Sister project**: Architecture mirrors `png2c64` — same pipeline pattern, same coding conventions
- **Error handling**: `Result<T> = std::expected<T, Error>` throughout, no exceptions
- **Color math**: All perceptual operations use OKLab color space
- **Preprocessing pipeline**: gamma -> sharpen -> levels -> brightness/contrast/saturation/hue (OKLab)
- **Dithering**: 16 methods available — 10 ordered (Bayer 2x2/4x4/8x8, checker, h2x4, clustered-dot, line2, line-checker, line4, line8) and 6 error-diffusion (Floyd-Steinberg, Atkinson, Sierra Lite, Stucki, Jarvis, and no-dither). All error diffusion operates in OKLab space with serpentine scanning and configurable error clamping.
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
