#pragma once

// ASTC (Adaptive Scalable Texture Compression) RGBA encoder + decoder.
//
// ASTC is Khronos's variable-rate block-compressed texture format. All
// 14 LDR 2D block footprints are 128 bits (16 bytes) per block; the
// footprint controls bits-per-pixel:
//
//   4x4   = 8.00 bpp     8x8   = 2.00 bpp     10x10 = 1.28 bpp
//   5x4   = 6.40         10x5  = 2.56         12x10 = 1.07
//   5x5   = 5.12         10x6  = 2.13         12x12 = 0.89
//   6x5   = 4.27         10x8  = 1.60
//   6x6   = 3.56         8x5   = 3.20
//   8x6   = 2.67
//
// Current implementation: wraps ARM's astcenc reference encoder for
// production output. The decoder routes through astcenc too. A native
// OKLab²-scored encoder is on the roadmap; the wrapper provides
// functional ASTC output + a quality baseline to A/B against.
//
// HDR ASTC is not supported here (no half-float input pipeline yet).
// 3D block sizes are not supported (not relevant for image content).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::astc {

// All ASTC LDR 2D blocks are 128 bits = 16 bytes.
constexpr int kBlockBytes = 16;
using Block = std::array<std::uint8_t, kBlockBytes>;

struct Options {
    int block_w = 4;
    int block_h = 4;
    bool srgb = true;
    // Quality preset, maps to astcenc's quality knob (0.0 = -fastest,
    // 100.0 = -exhaustive). Default is the astcenc "medium" preset.
    float quality = 60.0f;
};

struct EncodeResult {
    std::vector<Block> blocks;
    int block_cols{};
    int block_rows{};
    float total_oklab2_error{};  // reserved; not populated by the astcenc backend
};

// Encode a tightly-packed sRGBA8 buffer (4 bytes/pixel, row-major) to
// ASTC blocks of the requested footprint.
EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options);

// Decode a full ASTC block grid back to packed RGBA8 (image_w*image_h*4
// bytes, row-major). Source dimensions may be non-multiple-of-footprint;
// the decoder pads internally.
std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w,
                                       int image_h,
                                       int block_w,
                                       int block_h);

}  // namespace png2amiga::astc
