#pragma once

// BC7 RGBA texture-compression encoder + decoder.
//
// BC7 is a fixed 8 bpp block format: each 4×4 pixel block compresses
// to 16 bytes. Eight sub-modes (0..7) with different trade-offs:
//
//   Mode 0 — 3 subsets, 4-bit endpoints, no alpha
//   Mode 1 — 2 subsets, 6-bit endpoints, no alpha
//   Mode 2 — 3 subsets, 5-bit endpoints, no alpha
//   Mode 3 — 2 subsets, 7-bit endpoints + P-bit, no alpha
//   Mode 4 — 1 subset, separate alpha index, no partition
//   Mode 5 — 1 subset, full alpha, no partition
//   Mode 6 — 1 subset, 7-bit endpoints + P-bit, 4-bit selectors (RGBA)
//   Mode 7 — 2 subsets, 5-bit endpoints + alpha, partition
//
// First-pass implementation: Mode 6 only. Mode 6 has the strongest
// "single-subset" precision (16 levels of interpolation per pixel,
// effectively-8-bit endpoints) and trivially handles RGBA, so it's
// the right starting point. Other modes will land progressively
// (project_bc7_modes.md).
//
// Output container is KTX2 (see ktx2.hpp); vkFormat
// VK_FORMAT_BC7_UNORM_BLOCK = 145 (or _SRGB_BLOCK = 146).

#include "block_compress.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::bc7 {

// ---------------------------------------------------------------------------
// Block format
// ---------------------------------------------------------------------------

constexpr int kBlockW = 4;
constexpr int kBlockH = 4;
constexpr int kBlockPixels = kBlockW * kBlockH;
constexpr int kBlockBytes = 16;  // 8 bpp × 16 px = 16 bytes

using Block = std::array<std::uint8_t, kBlockBytes>;

// ---------------------------------------------------------------------------
// Encoder options
// ---------------------------------------------------------------------------

struct Options {
    block_compress::BlockMetric metric = block_compress::BlockMetric::oklab2;

    // Effort knob (0..3): gates search depth.
    //   0 = PCA seed + greedy selectors
    //   1 = + Lloyd refinement (default)
    //   2 = + endpoint jitter sweep
    //   3 = + per-block multi-mode comparison (slow)
    int effort = 1;

    // Endpoint jitter half-width in 7-bit units. Total candidates ≈ (2N+1)^3
    // (per endpoint). Default 0 keeps the encoder cheap; --best bumps.
    int jitter = 0;

    // Block-grid Floyd-Steinberg residual propagation (same mechanism
    // as ETC2 / BC1; see block_compress.hpp::propagate_block_residual).
    block_compress::BlockGridEdOptions block_ed;

    // --best path: multi-trial pre-image jitter sweep ranked by S2.
    int best_trials = 0;
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

// Decode any BC7 mode. Writes 16 RGBA8 pixels in row-major order
// (out[(y*4+x)*4 + ch]).  Alpha bytes are always written; for RGB-only
// modes the decoder sets alpha to 255.
void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 4]);

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w, int image_h);

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

// Encode a full image. Input is sRGBA8 (4 bytes per pixel, row-major).
// The encoder currently emits only Mode 6 blocks.
EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options);

// ---------------------------------------------------------------------------
// Internal: per-block candidate (exposed for tests)
// ---------------------------------------------------------------------------

struct Candidate {
    Block block{};
    std::uint8_t decoded[kBlockPixels][4]{};  // RGBA
    float err{};
};

}  // namespace png2amiga::bc7
