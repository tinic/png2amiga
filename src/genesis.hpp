#pragma once

// Sega Genesis / Mega Drive encoder.
//
// Three responsibilities:
//
//   * cluster_tiles_into_palettes: assign each 8×8 tile of the source image
//     to one of N palette lines (Genesis fixes N at 4) using k-means in
//     OKLab over per-tile color-set centroids. Reuses quantize::quantize
//     to extract each cluster's 15 best entries (color 0 reserved as
//     transparent / backdrop).
//
//   * Per-pixel re-quantisation against the assigned per-tile palette
//     happens in api.cpp via dither::diffuse_raw_buffer with a picker
//     closure — no new ED scaffolding here.
//
//   * Format helpers: pack indices into Genesis 4-bpp tile bytes, encode
//     tilemap cells (16-bit: priority + palette + flips + tile index),
//     pack CRAM words (BGR333 padded, see console_color::to_bgr333_word).
//
// Design notes / what this file deliberately does NOT do:
//   * Does NOT reimplement median-cut — uses quantize::quantize on each
//     cluster's pixel set.
//   * Does NOT reimplement error diffusion — picker closure into the
//     central driver does the per-pixel work.
//   * Does NOT support Shadow/Highlight (S/H) yet — that's a follow-up.
//   * Does NOT do tile-deduplication / flip-detection yet — emits one
//     tile per 8×8 cell (palette-bound), tilemap is dense. A later pass
//     can dedupe by hashing tile pixel arrays.

#include "types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::genesis {

constexpr std::size_t kPaletteCount = 4;       // 4 palette lines
constexpr std::size_t kColorsPerPalette = 16;  // including transparent index 0
constexpr std::size_t kTileSide = 8;
constexpr std::size_t kBitsPerPixel = 4;

struct GenesisResult {
    // Per-tile palette index in [0, kPaletteCount).
    // Layout: row-major over tilemap, size = (W/8) * (H/8).
    std::vector<std::uint8_t> tile_palette;
    // Per-tile shadow flag (S/H modes only): 1 if the tile renders with
    // the hardware-shadowed palette (priority bit cleared), 0 for base.
    // Empty when S/H is not in use.
    std::vector<std::uint8_t> tile_shadow;

    // Per-pixel palette index in [0, kColorsPerPalette).
    // Layout: row-major over pixels, size = W*H. Index 0 is transparent;
    // for opaque source pixels the picker chooses 1..15.
    std::vector<std::uint8_t> pixel_index;

    // Resolved 4 palette lines, each kColorsPerPalette entries (linear RGB,
    // already snapped to BGR333). palette_lines[p][0] = backdrop color.
    std::array<std::vector<Color3f>, kPaletteCount> palette_lines;

    // Rendered preview (linear RGB, after BGR333 snap).
    Image preview;

    // Sum of squared OKLab error vs source — for PSNR / quant reporting.
    float total_error = 0.0f;
};

// Cluster each 8×8 tile into one of kPaletteCount palette lines and pick
// the 15 best colors (palette index 0 = transparent / backdrop) per line.
//
//   `image` is the BGR333-quantised source (already letterboxed to the
//   target Genesis resolution by the caller).
//   `palette_diversity` follows the same convention as the rest of the
//   pipeline (k-means seeding diversity tuning).
//
// Returns the per-tile palette assignment and the 4 palette lines. The
// `pixel_index` and `preview` fields are NOT populated by this call —
// caller fills them via the dither driver.
GenesisResult cluster_tiles_into_palettes(const Image& image, float palette_diversity);

// Same as above + per-tile shadow flag (for S/H modes). After base
// palettes are picked, each tile chooses normal vs shadowed based on
// which gives lower nearest-neighbor OKLab² error against the assigned
// palette. The shadowed palette is the hardware-derived halving of each
// base entry — no extra CRAM consumed.
GenesisResult cluster_tiles_into_palettes_sh(const Image& image, float palette_diversity);

// Compute the shadowed view of a base palette line: each entry's RGB
// halved in sRGB space (matches Genesis VDP's hardware shadow rule
// closely; bit-exact would halve the 3-bit DAC value with truncation —
// the float approximation is within 1 DAC step).
std::vector<Color3f> shadow_palette_line(std::span<const Color3f> base);

// Per-cell tilemap entry resolved after dedup. Mirrors the bit layout of
// encode_tilemap_cell — kept as a struct so the dedup output is clean
// without bit-twiddling at the call site.
struct TilemapCell {
    std::uint16_t tile_index = 0;
    std::uint8_t palette_line = 0;
    bool h_flip = false;
    bool v_flip = false;
};

// Tile + tilemap dedup with H/V/H+V flip detection. Two tiles with
// identical pixel-index patterns share the same VRAM tile *regardless of
// palette* (palette lives in the tilemap cell, not the tile bytes), so
// dedup keys on the 64-byte raw nibble pattern alone.
//
// `pixel_index` is row-major W×H palette indices in [0, 16).
// `tile_palette` is row-major (W/8)×(H/8), values in [0, kPaletteCount).
// Output `tiles_out` are the unique 32-byte VRAM tiles in dictionary
// order; `tilemap_out` has one cell per (tx, ty) pointing into them.
struct DedupResult {
    std::vector<std::array<std::uint8_t, 32>> tiles;
    std::vector<TilemapCell> tilemap;
    std::size_t identical_dedupes = 0;  // exact-match hits (no flip)
    std::size_t flipped_dedupes = 0;    // H, V, or H+V flip hits
};

DedupResult dedup_tiles(std::span<const std::uint8_t> pixel_index,
                        std::span<const std::uint8_t> tile_palette,
                        std::size_t width,
                        std::size_t height);

// Encode a single 8×8 tile of palette indices into Genesis VRAM bytes.
//
// VRAM tile layout: 32 bytes per tile, row-major. Each row is 4 bytes;
// each byte holds two 4-bit pixel indices, high nibble = left pixel.
// `pixel_indices` must be 64 entries in row-major order, values 0..15.
std::array<std::uint8_t, 32> encode_4bpp_tile(std::span<const std::uint8_t> pixel_indices);

// Encode a single tilemap cell (16-bit, big-endian on the wire):
//   bit 15:    priority (0 = low, 1 = high)
//   bits 14-13: palette line (0..3)
//   bit 12:    vertical flip
//   bit 11:    horizontal flip
//   bits 10-0: tile index in VRAM
constexpr std::uint16_t encode_tilemap_cell(std::uint16_t tile_index,
                                            std::uint8_t palette_line,
                                            bool h_flip = false,
                                            bool v_flip = false,
                                            bool priority = false) noexcept {
    std::uint16_t cell = static_cast<std::uint16_t>(tile_index & 0x07FF);
    cell |= static_cast<std::uint16_t>((palette_line & 0x03) << 13);
    if (h_flip) cell |= 0x0800;
    if (v_flip) cell |= 0x1000;
    if (priority) cell |= 0x8000;
    return cell;
}

}  // namespace png2amiga::genesis
