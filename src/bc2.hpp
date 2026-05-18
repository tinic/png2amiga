#pragma once

// BC2 (DXT3) RGBA texture-compression encoder.
//
// Fixed 8 bpp; each 4×4 block is 16 bytes:
//
//   Bytes 0-7:  16 × 4-bit explicit alpha (no interpolation, no endpoint
//               coding — each pixel's alpha is just nibble-quantised).
//   Bytes 8-15: BC1 RGB color block (same format as src/bc1.hpp).
//
// BC2's alpha quality is 4-bit-explicit; BC3's interpolated alpha can hit
// 8-bit fidelity on smoothly-varying alpha. BC2 wins only when the
// per-pixel alpha values are quasi-random (no smooth ramp for BC3's
// endpoint pair to exploit). Mostly legacy today, but the format is in
// the DDS / KTX2 spec and trivial to add given the BC1 colour encoder.
//
// Containers: KTX2 vkFormat 135 (BC2_UNORM) / 136 (BC2_SRGB), DDS FourCC
// "DXT3".

#include "block_compress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::bc2 {

constexpr int kBlockW = 4;
constexpr int kBlockH = 4;
constexpr int kBlockPixels = kBlockW * kBlockH;
constexpr int kBlockBytes = 16;

using Block = std::array<std::uint8_t, kBlockBytes>;

struct Options {
    block_compress::BlockMetric metric = block_compress::BlockMetric::oklab2;
    int effort = 2;
    int jitter = 1;
    block_compress::BlockGridEdOptions block_ed;
};

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 4]);

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w, int image_h);

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

}  // namespace png2amiga::bc2
