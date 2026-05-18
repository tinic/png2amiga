// DDS writer — see dds.hpp for layout.
//
// Spec: https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header
// DX10 extension: https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header-dxt10

#include "dds.hpp"

#include <cstring>

namespace png2amiga::dds {

namespace {

constexpr std::uint32_t kMagic = 0x20534444u;  // "DDS "

// DDS_HEADER.dwFlags bits we set:
constexpr std::uint32_t kFlagCaps      = 0x1;
constexpr std::uint32_t kFlagHeight    = 0x2;
constexpr std::uint32_t kFlagWidth     = 0x4;
constexpr std::uint32_t kFlagPixelFmt  = 0x1000;
constexpr std::uint32_t kFlagMipCount  = 0x20000;
constexpr std::uint32_t kFlagLinearSz  = 0x80000;

// dwCaps bits:
constexpr std::uint32_t kCapsTexture   = 0x1000;
constexpr std::uint32_t kCapsComplex   = 0x8;
constexpr std::uint32_t kCapsMipmap    = 0x400000;

// DDS_PIXELFORMAT.dwFlags:
constexpr std::uint32_t kPfFourCC      = 0x4;

// DXGI formats we emit through the DX10 header.
constexpr std::uint32_t kDxgiBC7_UNORM       = 98;
constexpr std::uint32_t kDxgiBC7_UNORM_SRGB  = 99;

// D3D10_RESOURCE_DIMENSION_TEXTURE2D
constexpr std::uint32_t kResDim2D = 3;

void put_u32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(std::uint8_t((v >> (i * 8)) & 0xFF));
}

void put_fourcc(std::vector<std::uint8_t>& o, const char (&fcc)[5]) {
    for (int i = 0; i < 4; ++i) o.push_back(std::uint8_t(fcc[i]));
}

bool is_bc7(Format f) {
    return f == Format::bc7_unorm || f == Format::bc7_srgb;
}

bool is_srgb(Format f) {
    return f == Format::bc1_rgb_srgb || f == Format::bc7_srgb;
}

std::uint32_t dxgi_for(Format f) {
    switch (f) {
    case Format::bc7_unorm: return kDxgiBC7_UNORM;
    case Format::bc7_srgb:  return kDxgiBC7_UNORM_SRGB;
    default:                return 0;  // unused for legacy FourCC formats
    }
}

}  // namespace

std::vector<std::uint8_t> write(const Inputs& in) {
    std::vector<std::uint8_t> out;

    // Collect levels.
    std::vector<std::span<const std::uint8_t>> levels;
    if (!in.levels.empty()) {
        for (const auto& lv : in.levels) levels.emplace_back(lv);
    } else {
        levels.emplace_back(in.block_bytes);
    }
    const std::uint32_t mip_count = static_cast<std::uint32_t>(levels.size());

    // Reserve.
    std::size_t total_data = 0;
    for (auto s : levels) total_data += s.size();
    out.reserve(4 + 124 + (is_bc7(in.format) ? 20 : 0) + total_data);

    // --- Magic ----
    put_u32(out, kMagic);

    // --- DDS_HEADER (124 bytes) ----
    std::uint32_t flags = kFlagCaps | kFlagHeight | kFlagWidth | kFlagPixelFmt | kFlagLinearSz;
    if (mip_count > 1) flags |= kFlagMipCount;
    put_u32(out, 124);                       // dwSize
    put_u32(out, flags);                     // dwFlags
    put_u32(out, std::uint32_t(in.image_h)); // dwHeight
    put_u32(out, std::uint32_t(in.image_w)); // dwWidth
    // dwPitchOrLinearSize: for compressed = size of the level-0 surface in bytes
    put_u32(out, std::uint32_t(levels[0].size()));
    put_u32(out, 0);                         // dwDepth
    put_u32(out, mip_count);                 // dwMipMapCount
    for (int i = 0; i < 11; ++i) put_u32(out, 0);  // dwReserved1[11]

    // --- DDS_PIXELFORMAT (32 bytes) ----
    put_u32(out, 32);                        // dwSize
    put_u32(out, kPfFourCC);                 // dwFlags
    if (is_bc7(in.format)) {
        put_fourcc(out, "DX10");
    } else {
        // BC1 = "DXT1".
        put_fourcc(out, "DXT1");
    }
    put_u32(out, 0);                         // dwRGBBitCount
    put_u32(out, 0);                         // dwRBitMask
    put_u32(out, 0);                         // dwGBitMask
    put_u32(out, 0);                         // dwBBitMask
    put_u32(out, 0);                         // dwABitMask

    // --- Caps (16 bytes) ----
    std::uint32_t caps = kCapsTexture;
    if (mip_count > 1) caps |= kCapsMipmap | kCapsComplex;
    put_u32(out, caps);                      // dwCaps
    put_u32(out, 0);                         // dwCaps2
    put_u32(out, 0);                         // dwCaps3
    put_u32(out, 0);                         // dwCaps4
    put_u32(out, 0);                         // dwReserved2

    // --- DDS_HEADER_DXT10 (20 bytes) — only for BC7 ----
    if (is_bc7(in.format)) {
        put_u32(out, dxgi_for(in.format));   // dxgiFormat
        put_u32(out, kResDim2D);             // resourceDimension
        put_u32(out, 0);                     // miscFlag
        put_u32(out, 1);                     // arraySize
        // miscFlags2: low 3 bits = DDS_ALPHA_MODE_*. Use UNKNOWN (0) — the
        // user is in charge of premul; BC7 is RGBA-with-independent-alpha.
        put_u32(out, 0);
    }

    // --- Level data, LARGEST first ----
    for (auto lv : levels) {
        out.insert(out.end(), lv.begin(), lv.end());
    }

    (void)is_srgb;  // sRGB hint is carried in DXGI format for BC7; BC1
                    // legacy DXT1 has no sRGB bit (consumers infer or use
                    // the misc flags).
    return out;
}

}  // namespace png2amiga::dds
