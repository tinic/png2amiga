#pragma once

// BC1 (DXT1) RGB texture-compression encoder + decoder.
//
// BC1 is a fixed 4 bpp block format: each 4×4 pixel block compresses
// to 8 bytes — two 16-bit RGB565 endpoints + 16 × 2-bit selectors.
// Two block sub-modes are distinguished by endpoint comparison:
//
//   color0 >  color1  : 4-color block — paints are {c0, c1, (2c0+c1)/3, (c0+2c1)/3}
//   color0 <= color1  : 3-color + 1-bit alpha block —
//                       paints are {c0, c1, (c0+c1)/2, transparent_black}
//
// This encoder targets the RGB (4-color) sub-mode only — alpha is not
// supported yet (will come with BC3/BC7/ASTC when first needed).
//
// Architecturally this mirrors src/etc2.cpp:
//   - templated metric (BlockMetric::oklab2 default, srgb_mse opt-in)
//   - PCA-seeded endpoint pair search with Lloyd refinement + beam top-K
//   - block-grid Floyd-Steinberg residual propagation
//     (block_compress.hpp::propagate_block_residual)
//   - --best multi-trial sweep ranked by in-process ssimulacra2
//
// Output container is KTX2 (see ktx2.hpp); vkFormat
// VK_FORMAT_BC1_RGB_UNORM_BLOCK = 133 (or _SRGB_BLOCK = 134).

#include "block_compress.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::bc1 {

// ---------------------------------------------------------------------------
// Block format
// ---------------------------------------------------------------------------

constexpr int kBlockW = 4;
constexpr int kBlockH = 4;
constexpr int kBlockPixels = kBlockW * kBlockH;
constexpr int kBlockBytes = 8;

using Block = std::array<std::uint8_t, kBlockBytes>;

// ---------------------------------------------------------------------------
// Encoder options
// ---------------------------------------------------------------------------

struct Options {
    block_compress::BlockMetric metric = block_compress::BlockMetric::oklab2;

    // Effort knob (0..3): gates search depth.
    //   0 = single-pass PCA endpoints + greedy selectors (fastest)
    //   1 = + Lloyd refinement
    //   2 = + endpoint jitter ± window in RGB565 space (default)
    //   3 = wider beam + multi-trial (slow; --best)
    int effort = 2;

    // Beam width for endpoint-pair search.
    int beam = 16;

    // Endpoint jitter half-width (in RGB565 nibble-like units: 0..31 R/B,
    // 0..63 G). Total candidates ≈ (2N+1)^3 per endpoint.
    int jitter = 1;

    // Block-grid Floyd-Steinberg propagation (project's key wager —
    // shared mechanism with ETC2).
    block_compress::BlockGridEdOptions block_ed;

    // --best path: multi-trial pre-image jitter sweep, rank by SSIMULACRA2.
    int best_trials = 0;
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 3]);

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks, int image_w, int image_h);

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct EncodeResult {
    std::vector<Block> blocks;
    int block_cols{};
    int block_rows{};

    // Final in-process error vs source (sum of per-block metric errors).
    float total_oklab2_error{};
};

EncodeResult encode_image(std::span<const std::uint8_t> rgb_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options);

// ---------------------------------------------------------------------------
// Internal: per-block candidate (exposed for testing)
// ---------------------------------------------------------------------------

struct Candidate {
    Block block{};
    std::uint8_t decoded[kBlockPixels][3]{};
    float err{};
};

}  // namespace png2amiga::bc1
