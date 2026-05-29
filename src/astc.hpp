#pragma once

// ASTC RGBA texture-compression encoder.
//
// Native OKLab²-scored encoder with block-grid Floyd-Steinberg residual
// diffusion, mirroring the BC1/BC7/ETC2 design. astcenc lives under
// third_party/ for decode + bench-opponent use only — it is NOT what
// `--mode astc` runs through.
//
// Footprint coverage rolls out per-block-size:
//   Phase 1 (here): 4x4, single-partition, single-plane, LDR RGB direct
//   Future:         RGBA · multi-partition · all 14 LDR 2D footprints ·
//                   dual-plane
//
// All ASTC LDR 2D blocks are 16 bytes / 128 bits regardless of footprint.

#include "block_compress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::astc {

constexpr int kBlockBytes = 16;
using Block = std::array<std::uint8_t, kBlockBytes>;

struct Options {
    int block_w = 4;
    int block_h = 4;
    block_compress::BlockMetric metric = block_compress::BlockMetric::oklab2;
    int effort = 1;
    block_compress::BlockGridEdOptions block_ed;
    // Per-TEXEL error diffusion across blocks instead of the per-block
    // mean-residual grid ED. Wins on wide/coarse footprints (single-
    // gradient blocks) where the mean shift smears the boundary error;
    // loses on fine footprints. Set per-footprint by the caller.
    bool pixel_ed = false;
};

struct EncodeResult {
    std::vector<Block> blocks;
    int block_cols{};
    int block_rows{};
    float total_oklab2_error{};
};

EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options);

// Decode a full ASTC block grid back to packed RGBA8. Delegates to
// astcenc — the format is fully spec-determined so no encoder advantage
// is at stake on the decode side.
std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w,
                                       int image_h,
                                       int block_w,
                                       int block_h);

}  // namespace png2amiga::astc
