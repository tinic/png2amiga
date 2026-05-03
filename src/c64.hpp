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

// Per-cell error metric for c64 cell-mode brute force. Both run in
// OKLab (perceptually-uniform; matches png2c64).
//   mse  — per-pixel nearest-distance squared sum. Default.
//   blur — Pappas-Neuhoff 3×3 binomial blur of source vs rendered
//          cell. Models eye-on-CRT averaging.
enum class Metric : unsigned char { mse, blur };
Metric parse_metric(std::string_view s) noexcept;
std::string_view metric_name(Metric m) noexcept;

struct EncodeResult {
    Image rendered;                              // logical preview at the
                                                 // user-chosen W×H. For
                                                 // charset modes pixels
                                                 // are reconstructed from
                                                 // the dedup'd glyph table
                                                 // — what the hardware will
                                                 // actually display.
    std::vector<std::uint8_t> bitmap;            // bitmap modes: cells × 8.
                                                 // charset modes: charset
                                                 // data, unique_glyphs × 8.
    std::vector<std::uint8_t> screen_ram;        // 1 byte per cell. For
                                                 // charset modes this is
                                                 // the glyph index (0..255).
    std::vector<std::uint8_t> color_ram;         // 1 byte per cell.
    std::uint8_t bg_color = 0;                   // shared background 0..15.

    // Tilemap dims + dedup stats. Set for charset modes; bitmap modes
    // leave at 0 (locked to the hardware screen).
    std::size_t cols = 0;            // tilemap width in cells
    std::size_t rows = 0;            // tilemap height in cells
    std::size_t unique_glyphs = 0;   // distinct patterns post-dedup +
                                     // Hamming merge.
    // charset-multicolor only.
    std::uint8_t mc1 = 0;
    std::uint8_t mc2 = 0;
};

// Encode a 160×200 logical image to c64-multicolor with the chosen
// VIC-II palette, dither settings, and per-cell error metric. The
// outer brute-force pass picks 4 colours per cell (nearest-distance
// scoring under the chosen metric). The dither pass then runs over
// the per-cell 4-colour palette through diffuse_raw_buffer with the
// full dither suite (FS-family, gilbert, riemersma,
// structure-fs, ordered, knoll, opt-checker, yliluoma).
Result<EncodeResult> encode_multicolor(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::blur);

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
    const dither::Settings& settings = {},
    Metric metric = Metric::blur);

// Encode a 160×200 image to c64-FLI (Flexible Line Interpretation):
// multicolor with per-row (c1, c2) screen colours within each 4×8
// cell + per-cell color_ram (c3) + global background. The per-row
// swap is achieved at runtime by a raster-IRQ that reloads the
// screen-RAM pointer 8× per cell.
//
// EncodeResult layout (raw_frame is what api.cpp packs):
//   bitmap      8000 bytes (40 × 25 × 8 — same as multicolor)
//   screen_ram  8000 bytes (8 screen RAMs × 1000 bytes each, one
//               per cell-row)
//   color_ram   1000 bytes (c3 per cell)
//   bg_color    global background colour (currently fixed at 0)
//
// Per-cell brute force: 16 (color_ram) × 8 (rows) × C(15, 2) = 16 ×
// 8 × 105 ≈ 13 K row evaluations per cell. ~13 M for a 1000-cell
// image. Bg is fixed to 0 for the proof-of-fit; a global sweep is
// future work.
Result<EncodeResult> encode_fli(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::blur);

// Encode a 320×200 image to c64-AFLI: hires with per-row (c0, c1)
// pair within each 8×8 cell. The per-row pair lives in the same
// 8 × 1000-byte screen-RAM array FLI uses, but with the hires
// layout (1 bit per pixel, no shared bg / color_ram). Per-cell
// brute force: 8 rows × C(16, 2) = 8 × 120 = 960 row evaluations.
//
// EncodeResult layout:
//   bitmap      8000 bytes
//   screen_ram  8000 bytes (8 screen RAMs)
//   color_ram   empty
Result<EncodeResult> encode_afli(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::blur);

// Encode a 320×200 image to c64-PETSCII: text-mode glyph match.
// 40×25 cells, each 8×8. Per cell: pick (char, fg) ∈ 256 ROM glyphs
// × 16 VIC-II colours; bg is global (one of 16 colours, brute-
// forced over the whole image). Pappas-Neuhoff sRGB blur metric
// scores per-cell fits — the eye averages fg/bg through display
// blur, so PN-sRGB is the right perceptual model.
//
// EncodeResult layout:
//   bitmap     empty (text mode has no bitmap)
//   screen_ram 1000 bytes (per-cell PETSCII char code 0..255)
//   color_ram  1000 bytes (per-cell fg colour 0..15)
//   bg_color   global background (0..15)
//
// `dither::Settings` is currently ignored — PETSCII picks per cell
// without ED.
Result<EncodeResult> encode_petscii(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::blur,
    bool graphics_only = false);

// Encode an arbitrary-size image (W % 8 == 0, H % 8 == 0) to a c64
// hires charset. Per-cell brute force picks the best 2-colour pair
// (C(16,2) = 120). 8-byte glyph patterns are deduplicated; if more
// than (tile_budget - tile_reserve) unique patterns remain, the
// closest Hamming-distance pairs are merged until the budget fits.
//
// `tile_budget` (default 256) caps the unique glyph count. C64 chars
// are 8-bit-indexed so the encoder truncates to 256 in screen_ram
// even when budget > 256 (multi-bank work is the runtime's job).
// `tile_reserve` (default 0) reserves N slots so the dedup pass
// merges down to (budget - reserve).
Result<EncodeResult> encode_charset_hires(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::mse,
    std::size_t tile_budget = 256,
    std::size_t tile_reserve = 0);

// Encode an arbitrary-size image (W % 4 == 0, H % 8 == 0) to a c64
// multicolor charset. 4×8 cells with shared bg + mc1 + mc2 + per-cell
// fg. bg fixed at 0; (mc1, mc2) brute-forced globally; per-cell fg
// picks best of 16. Dedup + Hamming-merge to ≤ (budget - reserve).
// EncodeResult exposes mc1/mc2 alongside cols/rows/unique_glyphs.
Result<EncodeResult> encode_charset_multicolor(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::mse,
    std::size_t tile_budget = 256,
    std::size_t tile_reserve = 0);

// Generate a C header describing a charset EncodeResult. Includes:
//   - <NAME>_GLYPHS, <NAME>_COLS, <NAME>_ROWS, <NAME>_BG_COLOR
//     (and <NAME>_MC1 / <NAME>_MC2 for multicolor)
//   - <name>_charset[] — unique_glyphs × 8 bytes
//   - <name>_screen[] — cols*rows bytes (per-cell glyph index)
//   - <name>_color[]  — cols*rows bytes (per-cell colour pair / fg)
//   - <name>_palette[] — 16 OCS-format 12-bit RGB values
// The `multicolor` flag controls whether MC1/MC2 are emitted and
// whether <name>_color stores per-cell fg or hires-style nibble pair.
Result<std::string> charset_header(
    const EncodeResult& enc,
    std::string_view symbol_name,
    bool multicolor,
    Palette pal = Palette::colodore);

}  // namespace png2amiga::c64
