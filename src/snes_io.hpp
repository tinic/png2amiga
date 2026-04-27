#pragma once

// SNES Mode 7 packer.
//
// Packs a 256×224 chunky pixel buffer into a hardware-loadable Mode 7
// frame. The chunky byte stream is treated as encoder INPUT — no
// chunky output is exposed because it doesn't load on real hardware
// without a separate packing step.

#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace png2amiga::snes_io {

// ---------------------------------------------------------------------------
// One-shot Mode 7 encoder.
//
// Quantises + dithers the source image AND packs it into a hardware-
// loadable Mode 7 frame in a single call. The chunky intermediate
// (palette indices for the 256 variant, BBGGGRRR pixel bytes for
// Direct) lives only inside this function — callers see the post-pack
// bytes + the post-merge preview, never the raw chunky stream.
//
// `direct_color` selects between the 256-palette variant (false) and
// Direct Color (true). `dither` and `palette_diversity` follow the
// usual conventions; transparency is not handled here (caller must
// black out transparent pixels before calling).
// ---------------------------------------------------------------------------

struct Mode7EncodedFrame {
    std::vector<std::uint8_t> packed_bytes; // 16 KB tilemap + ≤ 16 KB tiles
    std::vector<Color3f> palette;            // 256 mode only; empty for Direct
    Image rendered;                          // post-merge preview
    float quant_error;                       // sum of squared OKLab error
    std::size_t unique_after_dedup;
    std::size_t unique_after_merge;
    std::size_t merges_done;
};

Result<Mode7EncodedFrame> encode_snes_mode7(
    const Image& source, bool direct_color,
    const dither::Settings& dither, int palette_diversity);

// ---------------------------------------------------------------------------
// SNES Mode 7 packer (low-level, used by encode_snes_mode7).
//
// Pack a 256×224 chunky pixel buffer into a hardware-loadable Mode 7
// frame: 16 KB tilemap (128×128 cells, each a 1-byte tile index 0..255)
// followed by ≤ 256 unique 8×8 tiles × 64 bytes = ≤ 16 KB tile data.
// Total raw output ≤ 32 KB.
//
// Mode 7 BG1 holds at most 256 unique 8×8 tiles in VRAM and supports
// NO tile flipping. The packer:
//   1. Encodes the buffer as 32×28 = 896 tile cells (top-left of the
//      128×128 virtual canvas; the rest of the tilemap points to tile 0
//      which the runtime can fill with backdrop).
//   2. Identity-only dedupes identical patterns.
//   3. If unique count > 256, greedy-merges the closest pattern pairs
//      (algorithm ported from png2c64's charset merger) until ≤ 256
//      slots remain — perceptual quality degrades but the frame fits.
//
// `distance` scores two 64-byte tile patterns; the caller supplies it so
// the metric can be palette-aware (256 mode: expand bytes through the
// palette to OKLab; Direct mode: expand BBGGGRRR to RGB).
// ---------------------------------------------------------------------------

struct Mode7PackResult {
    std::vector<std::uint8_t> tilemap;     // 16384 bytes (128×128)
    std::vector<std::uint8_t> tile_bytes;  // unique × 64
    std::size_t unique_after_dedup;        // before merging
    std::size_t unique_after_merge;        // = tile_bytes.size() / 64
    std::size_t merges_done;
};

Result<Mode7PackResult> pack_snes_mode7_frame(
    std::span<const std::uint8_t> pixels,
    std::size_t width, std::size_t height,
    std::function<float(std::span<const std::uint8_t> a,
                        std::span<const std::uint8_t> b)> distance);

} // namespace png2amiga::snes_io
