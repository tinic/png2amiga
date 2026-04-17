#pragma once

#include "bitplane.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace png2amiga::strip_pack {

// ---------------------------------------------------------------------------
// Sprite-strip options.
//
// Input is a horizontal animation strip (N frames laid out left-to-right).
// Frame dimensions must be provided by the caller — there's no reliable
// auto-detection for arbitrary strips (handeye.png is 672×36 but the frame
// width could be 16, 24, or 32 depending on convention).
//
// Pre-shift generation (--preshift N) emits N copies per frame, each shifted
// by (16/N) × k pixels for k = 0..N-1. Runtime lookup is frame*N + (x & 15)/sh.
// ---------------------------------------------------------------------------

struct Options {
    std::size_t frame_w{};
    std::size_t frame_h{};
    std::size_t depth = 4;
    bitplane::Layout layout = bitplane::Layout::interleaved;

    // Generate a 1-plane mask from alpha. Opaque iff alpha ≥ alpha_threshold.
    bool emit_mask = false;
    float alpha_threshold = 0.5f;

    // Reserve palette[0] for transparency (required if emitting masked sprites
    // into a playfield that expects index-0 to be the color key).
    bool reserve_color0 = true;

    // Pre-shift copies. Legal values: 1 (no preshift), 2, 4, 8, 16.
    // Emits `preshift` copies of each frame, each horizontally offset by
    // k * (16 / preshift) pixels. The runtime picks copy k = (x & 15) /
    // (16 / preshift), eliminating the A-shifter cost on a BOB blit.
    //
    // Storage width per copy = round-up(frame_w + 16, 16), which leaves room
    // for the shift plus the standard 16-pixel trailing slack a blitter blit
    // needs when its width isn't an exact multiple of 16 source words.
    std::size_t preshift = 1;
};

struct PackResult {
    bitplane::BitplaneData planes;       // (preshift * num_frames) copies
                                          // stacked vertically, interleaved
    std::vector<std::uint8_t> mask_data;  // one-plane mask, same layout
    std::vector<Color3f> palette;
    std::size_t num_frames{};
    std::size_t frame_w{};                // logical visible width
    std::size_t frame_h{};
    std::size_t storage_w{};              // padded width used in-memory
                                          // (round-up(frame_w + 16, 16) when
                                          //  preshift > 1, else round-up(frame_w,16))
    std::size_t bytes_per_row{};          // per-plane row pitch for one copy
    std::size_t frame_bytes{};            // storage bytes per (frame, shift) tuple
    std::size_t mask_bytes{};             // storage bytes per (frame, shift) mask
    std::size_t preshift{1};
    bool has_mask{};
};

// ---------------------------------------------------------------------------
// Slice a horizontal-strip PNG into frames and bitplane-encode.
//
// The source image must have width divisible by opts.frame_w and height equal
// to opts.frame_h. Mask generation uses the PNG's alpha if present.
// ---------------------------------------------------------------------------

Result<PackResult> pack_strip(const Image& image, const Options& opts);

} // namespace png2amiga::strip_pack
