#pragma once

// Sega Genesis / Mega Drive — SGDK-compatible C header output.
//
// Emits:
//   * const u32 SYM_tiles[N*8]       — VRAM tile bytes (4 u32 per row × 8 rows)
//   * const u16 SYM_tilemap[W*H]     — tilemap cells (palette + flips + tile idx)
//   * const u16 SYM_palette[64]      — CRAM, 4 palette lines × 16 entries each
//   * const TileSet  SYM_tileset     — wraps tiles for VDP_loadTileSet()
//   * const TileMap  SYM_tilemap_st  — wraps tilemap for VDP_setMap()
//   * const Palette  SYM_palette_st  — wraps palette for VDP_setPalette()
//
// Caller can hand these structs straight to SGDK without any glue.

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace png2amiga::cheader_genesis {

struct GenesisHeaderOptions {
    std::span<const std::uint8_t> tile_bytes;   // unique_tiles * 32 bytes
    std::span<const std::uint16_t> tilemap;     // tilemap cells (W/8 × H/8)
    std::span<const std::uint16_t> palette;     // 64 BGR333 words (4 lines × 16)
    std::size_t width_pixels;
    std::size_t height_pixels;
    std::string symbol;                          // base symbol name (no suffix)
};

Result<std::vector<std::uint8_t>> generate(const GenesisHeaderOptions& opts);

} // namespace png2amiga::cheader_genesis
