#pragma once

#include "bitplane.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace png2amiga::tile_pack {

// ---------------------------------------------------------------------------
// Tile dedup modes.
//
//   exact — only byte-identical cells collapse to one tile.
//   flip  — also collapse cells that match under H-flip / V-flip / 180° rotate.
//           The Amiga blitter does flips for free via descending-mode blits,
//           so this is the natural dedup granularity for a tile engine.
// ---------------------------------------------------------------------------

enum class DedupMode : std::uint8_t {
    exact,
    flip,
};

// ---------------------------------------------------------------------------
// Per-cell tilemap entry.
//
// Layout (matches `MapCell` in the emitted C++26 header):
//   bit 15: h_flip
//   bit 14: v_flip
//   bits 13..0: tile index
// ---------------------------------------------------------------------------

struct MapCell {
    std::uint16_t raw;

    [[nodiscard]] constexpr std::uint16_t index() const noexcept {
        return static_cast<std::uint16_t>(raw & 0x3FFFu);
    }
    [[nodiscard]] constexpr bool h_flip() const noexcept {
        return (raw & 0x8000u) != 0;
    }
    [[nodiscard]] constexpr bool v_flip() const noexcept {
        return (raw & 0x4000u) != 0;
    }

    static constexpr MapCell make(std::uint16_t idx, bool h, bool v) noexcept {
        return MapCell{static_cast<std::uint16_t>(
            (h ? 0x8000u : 0u) | (v ? 0x4000u : 0u) | (idx & 0x3FFFu))};
    }
};

static_assert(sizeof(MapCell) == 2);

// ---------------------------------------------------------------------------
// Options for pack_tiles().
// ---------------------------------------------------------------------------

struct Options {
    std::size_t tile_w = 16;
    std::size_t tile_h = 16;
    std::size_t depth = 4;              // bitplane depth (palette caps at 2^depth)
    DedupMode dedup = DedupMode::exact;
    bool reserve_color0 = false;        // reserve palette[0] for transparent
    bitplane::Layout layout = bitplane::Layout::interleaved;
};

// ---------------------------------------------------------------------------
// Pack result — everything the writer needs.
// ---------------------------------------------------------------------------

struct PackResult {
    bitplane::BitplaneData planes;      // concatenated unique tiles, bitplanes
    std::vector<Color3f> palette;        // linear RGB, size <= 2^depth
    std::vector<MapCell> tilemap;        // map_w * map_h entries
    std::size_t map_w{};
    std::size_t map_h{};
    std::size_t num_unique_tiles{};
    std::size_t num_cells{};             // map_w * map_h
    std::size_t bytes_per_row{};         // per-plane row pitch for one tile
    std::size_t tile_bytes{};            // bytes per tile (all planes)
};

// ---------------------------------------------------------------------------
// Slice a tilesheet PNG into cells, dedupe, and bitplane-encode the unique
// tile set.
//
// The input image must be a pre-quantized tilesheet with ≤ 2^depth colors.
// Non-16-px-multiple widths will error (tile_w must divide image.width()).
// ---------------------------------------------------------------------------

Result<PackResult> pack_tiles(const Image& image, const Options& opts);

// ---------------------------------------------------------------------------
// TilePool — output of the shared-tileset workflow. Holds the canonical
// bytes of each unique tile plus the associated palette. The separate
// `tileset` and `map` verbs both operate on this structure so a shared
// tile pool can be produced once (from a tileset PNG) and referenced by
// many level maps.
//
// The per-tile "canonical bytes" are the exact byte blob used to dedup
// cells (packed sRGB + alpha, tile_w * tile_h * 4 bytes each). This lets
// `map` re-hash level cells with identical semantics.
// ---------------------------------------------------------------------------

struct TilePool {
    bitplane::BitplaneData planes;            // concatenated unique tiles
    std::vector<Color3f>   palette;           // linear RGB, size ≤ 2^depth
    std::vector<std::vector<std::uint8_t>> canonical_keys;  // per-tile pixel key
    std::size_t tile_w{};
    std::size_t tile_h{};
    std::size_t depth{};
    std::size_t bytes_per_row{};              // per-plane row pitch for one tile
    std::size_t tile_bytes{};                 // total bytes per tile
    DedupMode   dedup{DedupMode::flip};
};

// Slice a tilesheet-style PNG into unique tiles (pool only, no tilemap).
// Use this for the shared-tileset workflow: one tileset feeds many maps.
Result<TilePool> build_tile_pool(const Image& image, const Options& opts);

// ---------------------------------------------------------------------------
// Given a pre-built TilePool and a level PNG, slice the level into cells and
// emit a tilemap that references the pool's tile indices. Level cells must
// match one of the pool's canonical keys (up to flip-equivalence). Unknown
// cells are reported via the return error.
// ---------------------------------------------------------------------------

struct MapResult {
    std::vector<MapCell> tilemap;
    std::size_t map_w{};
    std::size_t map_h{};
    std::size_t num_cells{};
    // Diagnostics:
    std::size_t unknown_cells{};   // count of level cells not in the pool
};

Result<MapResult> slice_tilemap(const Image& level,
                                const TilePool& pool);

} // namespace png2amiga::tile_pack
