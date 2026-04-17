#pragma once

// Binary manifest file produced by the `tileset` verb and consumed by the
// `map` verb. Carries just enough state to let `map` re-hash level cells
// with identical canonicalization semantics:
//
//   magic       "PATM"                                    (4 bytes)
//   version     u16, big-endian                           (2 bytes)
//   tile_w      u16, big-endian                           (2 bytes)
//   tile_h      u16, big-endian                           (2 bytes)
//   depth       u8                                        (1 byte)
//   dedup       u8  (0 = exact, 1 = flip)                 (1 byte)
//   num_tiles   u32, big-endian                           (4 bytes)
//   key_bytes   u32, big-endian (== tile_w*tile_h*4)      (4 bytes)
//   palette_n   u32, big-endian                           (4 bytes)
//   namespace_len u16, big-endian                         (2 bytes)
//   namespace  (UTF-8, no terminator)
//   tiles      num_tiles * key_bytes
//   palette    palette_n * 6 bytes (u16 R, u16 G, u16 B — full float quantum)
//
// All integers big-endian so the file is byte-identical on any host and
// cross-readable by a future tool that runs on real Amiga hardware.

#include "tile_pack.hpp"
#include "types.hpp"

#include <string>
#include <string_view>

namespace png2amiga::tileset_manifest {

struct Manifest {
    tile_pack::TilePool pool;
    std::string source_namespace;   // from tileset generator, e.g. "assets::tiles_1_1"
};

Result<void> write(std::string_view path,
                   const tile_pack::TilePool& pool,
                   std::string_view source_namespace);

Result<Manifest> read(std::string_view path);

} // namespace png2amiga::tileset_manifest
