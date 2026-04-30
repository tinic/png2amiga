#pragma once

// Commodore 64 / VIC-II encoder. Initial scope: c64-multicolor only,
// 160×200 logical resolution at 4 colours per 4×8 cell (1 shared
// background + 3 per-cell foregrounds). Brute-force per-cell triple
// search using the fixed 16-colour Pepto palette. The encoder
// produces a rendered preview Image at the logical 160×200 raster;
// the pipeline's preview_scale handles 2:1 hardware doubling.
//
// This is the first proof-of-fit of the png2c64 → png2amiga merge:
// c64 chipset + one mode end-to-end. Future work adds c64-hires,
// FLI/AFLI, sprite, charset, and .prg / .crt writers.

#include "amiga.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <span>
#include <string_view>

namespace png2amiga::c64 {

// Available VIC-II palettes. The C64's analogue composite output doesn't
// have a unique sRGB ground-truth; pick the one whose look you prefer.
// Default: colodore (measurement-based, matches png2c64). Names match
// png2c64.
enum class Palette : unsigned char {
    pepto,
    vice,
    colodore,
    deekay,
    godot,
    c64wiki,
    levy,
};

Palette parse_palette(std::string_view s) noexcept;
std::string_view palette_name(Palette p) noexcept;
std::span<const Color3f, 16> palette_colors(Palette p);

struct EncodeResult {
    Image rendered;                              // 160×200 logical preview
    std::vector<std::uint8_t> bitmap;            // 8000 bytes (40 cols × 25 rows × 8)
    std::vector<std::uint8_t> screen_ram;        // 1000 bytes (1 nibble pair / cell)
    std::vector<std::uint8_t> color_ram;         // 1000 bytes (1 nibble / cell)
    std::uint8_t bg_color = 0;                   // shared background colour 0..15
};

// Encode a 160×200 logical image to c64-multicolor with the chosen
// VIC-II palette and dither settings. Two-pass:
//   1. Brute-force per-cell quad selection (16 bg × C(15,3) ≈ 7280
//      quads per cell; nearest-OKLab² scoring against undithered
//      source).
//   2. Per-pixel dither via dither::diffuse_raw_buffer with a
//      per-cell palette callback that returns the 4 OKLab colours
//      chosen for the cell at (x, y). Supports every method that
//      diffuse_raw_buffer routes — FS-family, Atkinson, Sierra-Lite,
//      Stucki, Jarvis, Ostromoukhov, Riemersma, Gilbert,
//      structure-fs / contrast-fs / Zhou-Fang, all ordered methods,
//      and the Yliluoma / Knoll / opt-checker family.
Result<EncodeResult> encode_multicolor(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {});

// Encode a 320×200 image to c64-hires. 8×8 cells, 2 colours per cell
// (no shared bg). Per-cell brute force is C(16, 2) = 120 pairs.
// Same dither pipeline as multicolor — diffuse_raw_buffer + per-cell
// 2-colour palette callback. EncodeResult layout:
//   bitmap     8000 bytes (40 cols × 25 rows × 8)
//   screen_ram 1000 bytes (upper nibble = c1 / fg, lower = c0 / bg)
//   color_ram  empty (hires uses only screen RAM)
Result<EncodeResult> encode_hires(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {});

}  // namespace png2amiga::c64
