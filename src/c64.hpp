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
    Image rendered;                              // 160×200 logical preview
    std::vector<std::uint8_t> bitmap;            // 8000 bytes (40 cols × 25 rows × 8)
    std::vector<std::uint8_t> screen_ram;        // 1000 bytes (1 nibble pair / cell)
    std::vector<std::uint8_t> color_ram;         // 1000 bytes (1 nibble / cell)
    std::uint8_t bg_color = 0;                   // shared background colour 0..15
};

// Encode a 160×200 logical image to c64-multicolor with the chosen
// VIC-II palette, dither settings, and per-cell error metric. The
// outer brute-force pass picks 4 colours per cell (nearest-distance
// scoring under the chosen metric). The dither pass then runs over
// the per-cell 4-colour palette through diffuse_raw_buffer with the
// full dither suite (FS-family, ostromoukhov, gilbert, riemersma,
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

// Encode a 320×200 image to c64 charset-hires. Per-cell brute force
// is the same as encode_hires (C(16, 2) = 120 colour pairs per
// 8×8 cell). The 8-byte glyph pattern is then deduplicated across
// cells; if more than 256 unique patterns remain, the closest
// Hamming-distance pairs are merged until the budget fits.
//
// EncodeResult layout:
//   bitmap     — empty (charset modes have no flat bitmap)
//   screen_ram — 1000 bytes: char index 0..255 per cell.
//   color_ram  — 1000 bytes: per-cell colour pair encoded as
//                upper nibble = c1 / fg, lower = c0 / bg.
//   bg_color   — 0 (slot reserved for the empty pattern; not used
//                as a global VIC-II background register here).
//
// The 256-glyph charset itself is currently surfaced via the
// EncodeResult.bitmap field's first 2048 bytes (tile data); we'll
// add a dedicated charset_data field when wiring the .h writer.
Result<EncodeResult> encode_charset_hires(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::mse);

// Encode a 160×200 logical image to c64 charset-multicolor. 4×8
// cells with shared bg + mc1 + mc2 (global) + per-cell fg. The
// 4×8 glyph pattern (2 bits per pixel = 16 bytes per cell, but
// only 8 unique 4-pixel rows × 4 colours per row → 8 bytes when
// tightly-packed). bg is currently fixed at 0 and (mc1, mc2) are
// brute-forced over C(15, 2) = 105 pairs globally; per-cell fg
// picks the best of 16 each cell. Dedup + Hamming-merge to ≤256
// glyphs.
//
// EncodeResult layout:
//   bitmap     — 2048-byte charset (256 × 8).
//   screen_ram — 1000 bytes: char index per cell.
//   color_ram  — 1000 bytes: per-cell fg in the low nibble (mc1
//                / mc2 / bg are global registers, not per-cell).
//   bg_color   — global background colour.
//
// (mc1 / mc2 are returned via fields not yet wired into the
// header writer; followup.)
Result<EncodeResult> encode_charset_multicolor(
    const Image& image,
    Palette pal = Palette::colodore,
    const dither::Settings& settings = {},
    Metric metric = Metric::mse);

}  // namespace png2amiga::c64
