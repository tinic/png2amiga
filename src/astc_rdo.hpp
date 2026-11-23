#pragma once

// Rate-distortion optimisation for the ASTC encoder (--rdo). A post-pass
// over the finished block grid: each block may be replaced by a block
// that costs fewer bits after LZ compression (zstd) for a bounded rise
// of the encoder's own perceptual error. LDR blocks with and without
// alpha, every footprint. Internal to the ASTC encoder.

#include "astc.hpp"
#include "color_space.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::astc::rdo {

// What the main encoder hands to the RDO pass.
struct Side {
    int pad_w = 0;  // image size rounded up to whole blocks
    int pad_h = 0;
    // Per-texel OKLab target each block was scored against (source plus
    // diffused error), pad_w x pad_h.
    std::vector<color_space::OKLab> target;
};

// Optimise res.blocks in place; refreshes res.predicted_rgba and
// res.total_oklab2_error. strength > 0.
void optimize(EncodeResult& res, const Side& side, std::span<const std::uint8_t> rgba_srgb8,
              int image_w, int image_h, int block_w, int block_h, int strength);

}  // namespace png2amiga::astc::rdo
