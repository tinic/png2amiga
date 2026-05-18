#pragma once

// ETC2 RGB8 texture-compression encoder + decoder.
//
// ETC2 RGB8 is a fixed 4 bpp block format: each 4×4 pixel block compresses
// to 8 bytes. Per-block the bitstream selects one of 5 sub-modes:
//
//   ETC1 individual    - 2 sub-blocks (h/v split), 4-bit/channel base each
//   ETC1 differential  - 5-bit base + 3-bit signed delta per channel
//   T-mode             - 2 base colors + 1 distance index (ETC2 extension)
//   H-mode             - 2 base colors with alternate geometry (ETC2 extension)
//   Planar             - O + ∂H·x + ∂V·y interpolated plane (ETC2 extension)
//
// Mode selection is implicit: ETC1 differential overflow conditions
// (R+dR or G+dG or B+dB out of [0,31]) re-purpose the bitstream to mean
// one of {T, H, planar}. The decoder follows the same dispatch.
//
// Architecturally this follows the HAM playbook (project_ham_aware_ed.md):
//   - templated metric (BlockMetric::oklab2 default, srgb_mse opt-in)
//   - per-block candidate generation with beam search where applicable
//   - block-grid Floyd-Steinberg residual propagation (novel; see
//     block_compress.hpp::propagate_block_residual)
//   - --best multi-trial sweep ranked by in-process ssimulacra2
//
// Output container is KTX2 (see ktx2.hpp); vkFormat
// VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK = 147.

#include "block_compress.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::etc2 {

// ---------------------------------------------------------------------------
// Block format
// ---------------------------------------------------------------------------

constexpr int kBlockW = 4;
constexpr int kBlockH = 4;
constexpr int kBlockPixels = kBlockW * kBlockH;
constexpr int kBlockBytes = 8;

using Block = std::array<std::uint8_t, kBlockBytes>;

enum class SubMode : std::uint8_t {
    etc1_individual,
    etc1_differential,
    t_mode,
    h_mode,
    planar,
};

// ---------------------------------------------------------------------------
// Encoder options
// ---------------------------------------------------------------------------

struct Options {
    // Scoring metric — defaults to OKLab² per project_ham_aware_ed.md.
    block_compress::BlockMetric metric = block_compress::BlockMetric::oklab2;

    // Effort knob (0..3) — gates which sub-modes the encoder considers.
    //   0 = planar only             (fast, smooth-content only)
    //   1 = + T + H                 (medium)
    //   2 = + ETC1 individual+diff  (default; full mode set, narrow beam)
    //   3 = wider beam, joint 9-block refinement post-pass (slow, best quality)
    int effort = 2;

    // Beam width for ETC1 mode search (analog to ham_beam). Top-K candidates
    // by per-block error are carried forward through the per-mode search.
    int beam = 32;

    // ETC1 sub-block jitter window — half-width in nibble units around the
    // sub-block mean. Total candidates per sub-block ≈ (2N+1)³ × 8 tables.
    //
    // Default 1: PSNR peaks here (-0.24 dB at jitter=2 on asterix, then
    // worse) and S2 still strong (78.75 vs 78.99 at j=2 — small drop for
    // 2.4× speedup). For maximum S2, --best bumps this to 4 internally.
    // Wider values are O((2N+1)³) cost growth; reserved for --best.
    int jitter = 1;

    // Block-grid Floyd-Steinberg propagation (project's key wager).
    // 0.0 disables; 1.0 is full FS strength on the block grid.
    block_compress::BlockGridEdOptions block_ed;

    // Multi-block joint refinement passes. 0 = off (default; the picker
    // already runs every sub-mode). N > 0 runs N additional passes where
    // each block's encoding is re-tried with edge-pixel targets biased
    // toward the average of (original source, neighbour blocks' decoded
    // pixels at the shared boundary). Reduces visible per-block seams
    // by making each block coherent with its neighbours.
    int refine_passes = 0;

    // --best path: multi-trial pre-image jitter sweep, rank by SSIMULACRA2.
    // 0 = off; N = N trials in parallel.
    int best_trials = 0;
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
//
// Self-dispatching: reads diff bit + delta-range to pick sub-mode internally.
// Writes 16 RGB888 pixels in row-major order (out[(y*4+x)*3 + ch]).

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 3]);

// Returns the sub-mode the decoder picked. Useful for diagnostics / mode-mix
// reporting after a full-image encode.
SubMode classify(const Block& blk);

// Decode a full image's worth of blocks into a packed RGB888 buffer.
// `blocks` is row-major (W/4) × (H/4); output is `image_w × image_h × 3`.
// Output dimensions must be multiples of 4 (encoder pads input on read,
// so decoded result may extend slightly past source).
std::vector<std::uint8_t> decode_image(std::span<const Block> blocks, int image_w, int image_h);

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
//
// Encodes a full image. Input is sRGB8 RGB pixels (3 bytes per pixel,
// row-major). Output is the row-major block stream.
//
// Padded output dimensions = (ceil(image_w/4)*4, ceil(image_h/4)*4).
// Padding pixels are replicated from the source edge inside sample_block.

struct EncodeResult {
    std::vector<Block> blocks;
    int block_cols{};  // image_w padded / 4
    int block_rows{};  // image_h padded / 4

    // Per-block sub-mode counts for diagnostics ("mode mix" reporting).
    std::array<int, 5> mode_counts{};

    // Final in-process error vs source (sum of per-block oklab2 errors).
    float total_oklab2_error{};
};

EncodeResult encode_image(std::span<const std::uint8_t> rgb_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options);

// ---------------------------------------------------------------------------
// Internal: per-mode candidate types (exposed for testing + benchmarking)
// ---------------------------------------------------------------------------
//
// Each encoder produces a Candidate carrying the encoded 8-byte block,
// the decoded pixels (so the picker can score without re-decoding), and
// the pre-computed error in the active metric.

struct Candidate {
    Block block{};
    std::uint8_t decoded[kBlockPixels][3]{};
    float err{};
    SubMode mode{};
};

}  // namespace png2amiga::etc2
