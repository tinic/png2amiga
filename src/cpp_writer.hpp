#pragma once

#include "atlas_pack.hpp"
#include "strip_pack.hpp"
#include "tile_pack.hpp"
#include "types.hpp"

#include <string>
#include <string_view>

namespace png2amiga::cpp_writer {

// ---------------------------------------------------------------------------
// C++26 output options.
//
// chipset selects palette encoding (OCS 12-bit words vs AGA 24-bit).
// namespace_name wraps all declarations; e.g. "assets::tileset_1_1".
// chip_attr is pasted verbatim before big data definitions; intended to hold
// a linker-section attribute like `[[gnu::section(".MEMF_CHIP")]]`. Left empty
// by default so the user's engine's default placement applies.
// ---------------------------------------------------------------------------

enum class Chipset : std::uint8_t { ocs, aga };

struct Options {
    std::string namespace_name;     // e.g. "assets::turr_1_1"
    std::string chip_attr;          // e.g. "[[gnu::section(\".MEMF_CHIP\")]]"
    Chipset chipset = Chipset::ocs;
    bool inline_data = false;       // put data in header as inline constexpr
                                    // (not recommended for big tiles)
};

// ---------------------------------------------------------------------------
// Emit a .hpp header + .cpp source for a tile pack result.
//
// base_path: output path without extension; writes "<base>.hpp" and "<base>.cpp".
// ---------------------------------------------------------------------------

Result<void> write_tile_pack(std::string_view base_path,
                             const tile_pack::PackResult& pack,
                             const Options& opts);

Result<void> write_strip_pack(std::string_view base_path,
                              const strip_pack::PackResult& pack,
                              const Options& opts);

Result<void> write_atlas_pack(std::string_view base_path,
                              const atlas_pack::PackResult& pack,
                              const Options& opts);

// Pool-only: writes a .hpp/.cpp pair that exposes `tiles_data`, `palette`,
// and the tile metadata (tile_w, tile_h, depth, num_tiles, tile_bytes, bpr)
// WITHOUT a tilemap. Used by the `tileset` verb to emit a shared tile pool
// that many level-map headers can re-export.
Result<void> write_tile_pool(std::string_view base_path,
                             const tile_pack::TilePool& pool,
                             const Options& opts);

// Map-only: writes a .hpp/.cpp pair that exposes a `tilemap` array, plus
// size constants. Re-exports the shared pool's symbols via `using` so the
// engine can call `TileLayer<MyLevel>` directly — MyLevel both owns the
// tilemap and re-exports tile_ptr/cell_at.
struct MapWriteOptions {
    std::string pool_namespace;    // e.g. "assets::tiles_1_1"
    std::string pool_header;       // relative include, e.g. "tiles_1_1.hpp"
};

Result<void> write_tilemap(std::string_view base_path,
                           const tile_pack::MapResult& mr,
                           const Options& opts,
                           const MapWriteOptions& mopts);

} // namespace png2amiga::cpp_writer
