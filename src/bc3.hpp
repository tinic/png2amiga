#pragma once

// BC3 (DXT5) RGBA texture-compression encoder.
//
// BC3 is a fixed 8 bpp block format: each 4×4 pixel block is 16 bytes.
// The layout is two halves:
//
//   Bytes 0-7:  BC4-style alpha block
//                 byte 0:    alpha endpoint 0 (8-bit)
//                 byte 1:    alpha endpoint 1 (8-bit)
//                 bytes 2-7: 48 bits = 16 × 3-bit alpha selectors
//   Bytes 8-15: BC1 RGB color block (same format as src/bc1.hpp)
//
// We reuse the existing OKLab²-scored BC1 encoder verbatim for the RGB
// half — that's where the encoder's perceptual advantage lives. The
// alpha half uses a 1D BC4-style endpoint + selector search (purely
// scalar MSE, no perceptual scoring since alpha is opacity, not colour).
//
// Output containers: KTX2 vkFormat VK_FORMAT_BC3_UNORM_BLOCK = 137 (or
// VK_FORMAT_BC3_SRGB_BLOCK = 138), DDS FourCC "DXT5".

#include "block_compress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::bc3 {

constexpr int kBlockW = 4;
constexpr int kBlockH = 4;
constexpr int kBlockPixels = kBlockW * kBlockH;
constexpr int kBlockBytes = 16;

using Block = std::array<std::uint8_t, kBlockBytes>;

struct Options {
    block_compress::BlockMetric metric = block_compress::BlockMetric::oklab2;
    int effort = 2;        // forwarded to bc1::Options.effort
    int jitter = 1;        // forwarded to bc1::Options.jitter
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

// Encode an RGBA8 buffer (4 bytes/pixel, row-major) to BC3 blocks.
EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options);

}  // namespace png2amiga::bc3
