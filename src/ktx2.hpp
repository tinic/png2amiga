#pragma once

// Minimal KTX2 container writer.
//
// KTX2 is the Khronos texture container (https://github.khronos.org/KTX-Specification/).
// We only emit:
//   - single 2D mip level (no mipmap chain, no array layers, no cubemap faces)
//   - no supercompression (Basis / Zstd not wired)
//   - basic DFD descriptor block
//   - empty KVD (no key/value metadata)
//
// Layout we produce (all little-endian per spec):
//
//   [80]    fixed header
//   [24*L]  level index (L=1)
//   [44]    DFD = total-size(u32) + basic descriptor block (40 bytes for 1 sample)
//   [0]     KVD
//   [0]     SGD
//   [N]     block data (ETC2: blocks * 8 bytes)
//
// vkFormat is taken from <vulkan_core.h>; we expose only the formats this
// project encodes today (ETC2 RGB8). Adding ASTC later means adding the
// matching format constants + a few DFD model-id branches.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::ktx2 {

// Vulkan vkFormat values we use (matches VK_FORMAT_* from vulkan_core.h).
// Listed explicitly so we don't pull in Vulkan headers from a converter.
enum class VkFormat : std::uint32_t {
    // BC1 (DXT1) RGB block formats.
    bc1_rgb_unorm_block = 133,      // VK_FORMAT_BC1_RGB_UNORM_BLOCK
    bc1_rgb_srgb_block = 134,       // VK_FORMAT_BC1_RGB_SRGB_BLOCK
    // BC7 RGBA block formats.
    bc7_unorm_block = 145,          // VK_FORMAT_BC7_UNORM_BLOCK
    bc7_srgb_block = 146,           // VK_FORMAT_BC7_SRGB_BLOCK
    etc2_r8g8b8_unorm_block = 147,  // VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK
    etc2_r8g8b8_srgb_block = 148,   // VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK
    // ASTC LDR sRGB block formats — wired in for the planned ASTC port,
    // not yet emitted by any encoder in this project.
    astc_4x4_srgb_block = 158,
    astc_5x5_srgb_block = 162,
    astc_6x6_srgb_block = 166,
    astc_8x8_srgb_block = 172,
};

// Texel block dimensions (the compressed block's pixel footprint).
// ETC2 = 4x4; ASTC has many footprints (4x4, 5x4, ... 12x12).
struct BlockDim {
    int w;
    int h;
    int d;  // depth (1 for 2D)
};

// Inputs to write_ktx2: a full image worth of compressed-block bytes
// plus the source dimensions and the compressed-block format metadata.
//
// `image_w` / `image_h` are the SOURCE pixel dimensions. KTX2 stores
// these and the runtime decoder pads internally to the block grid.
//
// `block_bytes` length must equal:
//   ceil(image_w / block_dim.w) * ceil(image_h / block_dim.h) * bytes_per_block
struct Inputs {
    VkFormat format = VkFormat::etc2_r8g8b8_srgb_block;
    BlockDim block_dim{4, 4, 1};
    int image_w{};
    int image_h{};
    int bytes_per_block = 8;
    std::span<const std::uint8_t> block_bytes;
};

// Build the .ktx2 file as a byte vector.
std::vector<std::uint8_t> write(const Inputs& in);

}  // namespace png2amiga::ktx2
