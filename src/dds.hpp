#pragma once

// Minimal DDS (DirectDraw Surface) writer for BC1 / BC7 block-compressed
// textures. DDS is Microsoft's texture container — the de-facto format on
// Windows and the format consumed by D3D, AMD Compressonator, NVTT, etc.
//
// Layout we produce (all little-endian):
//
//   [4]     magic "DDS "
//   [124]   DDS_HEADER (legacy)
//   [20]    DDS_HEADER_DXT10 (only when fourCC == "DX10" — required for BC7)
//   [N]     mip-level data, LARGEST level first (opposite of KTX2)
//
// BC1 uses the legacy FourCC "DXT1"; the DXT10 header is omitted.
// BC7 uses FourCC "DX10" and the DXT10 header carries DXGI_FORMAT_BC7_*.
// ETC2 is not part of the DDS spec — KTX2 stays the only ETC2 container.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::dds {

enum class Format : std::uint32_t {
    bc1_rgb_unorm,    // FourCC "DXT1"
    bc1_rgb_srgb,     // FourCC "DXT1" + miscFlags2 sRGB hint
    bc3_unorm,        // FourCC "DXT5"
    bc3_srgb,         // FourCC "DXT5" (sRGB tag only carried via DXGI when used)
    bc7_unorm,        // DX10 + DXGI_FORMAT_BC7_UNORM (98)
    bc7_srgb,         // DX10 + DXGI_FORMAT_BC7_UNORM_SRGB (99)
};

struct Inputs {
    Format format = Format::bc7_srgb;
    int image_w{};
    int image_h{};
    int bytes_per_block = 16;  // BC1 = 8, BC7 = 16

    // Single-level path (used when `levels` is empty).
    std::span<const std::uint8_t> block_bytes;

    // Mipmap chain. levels[0] = base (largest); subsequent entries halve
    // each dimension. DDS stores levels largest-first, so we write in
    // natural order.
    std::vector<std::vector<std::uint8_t>> levels;
};

// Build the .dds file as a byte vector.
std::vector<std::uint8_t> write(const Inputs& in);

}  // namespace png2amiga::dds
