#pragma once

// Sega Master System / Game Gear VDP mode 4 encoder.
//
//   sms_mode4: 256×192, 32×24 cells, RGB222 CRAM (1 byte/entry, --BBGGRR).
//   gg_mode4:  160×144, 20×18 cells, RGB444 CRAM (2 bytes/entry LE,
//              ----BBBBGGGGRRRR).
//
// Two 16-color palettes: CRAM 0-15 (background) and CRAM 16-31 (sprite);
// a background tilemap entry picks either with bit 11. Every entry is a
// visible color (no transparent slot for background tiles).
//
// Pipeline (reuses the Genesis / c64 building blocks):
//   genesis::cluster_tiles (2 palettes) → per-cluster 16-color palette
//   (quantize::ega_histogram for RGB222, OCS brute-force for RGB444) →
//   dither::diffuse_cells_mirrored → genesis::dedup_tiles (H/V flips) →
//   tile_merge::merge_to_budget (≤ 448 tiles) → pack → decode preview.
//
// Tile format: 32 bytes/tile, 4 bytes per row, byte b = bitplane b of the
// row (bit 7 = leftmost pixel). Tilemap entry (u16 LE):
//   bits 0-8 tile index, bit 9 H flip, bit 10 V flip, bit 11 palette
//   (0 = CRAM 0-15, 1 = CRAM 16-31), bit 12 priority (always 0 here).

#include "amiga.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::sms {

constexpr std::size_t kTileSide = 8;
constexpr std::size_t kPalettes = 2;
constexpr std::size_t kColorsPerPalette = 16;
constexpr std::size_t kTileBytes = 32;
// Tiles that fit below the name table at $3800 (standard layout with the
// sprite attribute table at $3F00): $3800 / 32 = 448.
constexpr std::size_t kMaxTiles = 448;

struct EncodeResult {
    Image rendered;                      // decoded from tiles + tilemap + palette
    std::vector<std::uint8_t> tiles;     // unique × 32 bytes (planar)
    std::vector<std::uint16_t> tilemap;  // cols × rows entries
    std::vector<std::uint8_t> palette;   // CRAM bytes: SMS 32, GG 64
    std::vector<Color3f> palette_rgb;    // 32 entries, linear RGB
    std::size_t cols = 0;
    std::size_t rows = 0;
    std::size_t unique_after_dedup = 0;
    std::size_t unique_tiles = 0;
    float total_error = 0.0f;  // OKLab² sum vs source
};

using ProgressCb = std::function<void(float, std::string_view)>;

// `image` must already be at the mode's buffer size (256×192 / 160×144).
Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings,
                            const ProgressCb& on_progress = nullptr);

// CRAM bytes (32 or 64) → 32 linear-RGB colors.
std::vector<Color3f> decode_palette(amiga::Mode mode, std::span<const std::uint8_t> cram);

// Render tiles + tilemap + CRAM exactly as the VDP displays them.
Image decode(amiga::Mode mode,
             std::span<const std::uint8_t> tiles,
             std::span<const std::uint16_t> tilemap,
             std::span<const std::uint8_t> cram,
             std::size_t cols,
             std::size_t rows);

// Raw .bin layout: tiles ++ tilemap (u16 LE) ++ CRAM bytes.
std::vector<std::uint8_t> raw_bytes(const EncodeResult& r);

}  // namespace png2amiga::sms
