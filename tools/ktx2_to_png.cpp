// ktx2_to_png — decode a KTX2 file containing ETC2 RGB8 blocks back to
// PNG using iOrange's standalone reference decoder (third_party/etcdec).
//
// Purpose: independent verification of the encoder's output. Our own
// decoder (src/etc2.cpp) is a line-for-line port of Ericsson's etcdec
// reference, so any block we encode should also decode byte-identically
// through the iOrange reference (which is a different port of the same
// spec). If outputs ever diverge, that's a real correctness signal.
//
// Usage:
//   ktx2_to_png input.ktx2 output.png
//
// Limitations:
//   - Single 2D mip level only (matches our encoder's output)
//   - vkFormat must be VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK (147) or SRGB (148)
//   - No supercompression, no array layers, no cubemap faces

#define ETCDEC_IMPLEMENTATION 1
#include "etcdec/etcdec.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION 1
#include "stb_image_write.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

constexpr std::uint32_t kVkFormatEtc2RgbUnorm = 147;
constexpr std::uint32_t kVkFormatEtc2RgbSrgb = 148;

constexpr std::uint8_t kKtx2Identifier[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
};

std::uint32_t read_u32_le(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

std::uint64_t read_u64_le(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(p[i]) << (i * 8);
    return v;
}

struct Ktx2Level {
    std::uint64_t byte_offset;
    std::uint64_t byte_length;
};

struct Ktx2Image {
    std::uint32_t vk_format;
    std::uint32_t width;
    std::uint32_t height;
    std::vector<Ktx2Level> levels;
    std::vector<std::uint8_t> bytes;
};

bool parse_ktx2(const std::vector<std::uint8_t>& f, Ktx2Image& out) {
    if (f.size() < 80) {
        std::fprintf(stderr, "ktx2_to_png: file too small to be KTX2\n");
        return false;
    }
    if (std::memcmp(f.data(), kKtx2Identifier, 12) != 0) {
        std::fprintf(stderr, "ktx2_to_png: bad KTX2 magic\n");
        return false;
    }
    out.vk_format = read_u32_le(f.data() + 12);
    // typeSize at +16
    out.width = read_u32_le(f.data() + 20);
    out.height = read_u32_le(f.data() + 24);
    std::uint32_t pixel_depth = read_u32_le(f.data() + 28);
    std::uint32_t layer_count = read_u32_le(f.data() + 32);
    std::uint32_t face_count = read_u32_le(f.data() + 36);
    std::uint32_t level_count = read_u32_le(f.data() + 40);
    std::uint32_t supercompression = read_u32_le(f.data() + 44);

    if (pixel_depth != 0) {
        std::fprintf(stderr, "ktx2_to_png: 3D textures not supported\n");
        return false;
    }
    if (layer_count != 0) {
        std::fprintf(stderr, "ktx2_to_png: array textures not supported\n");
        return false;
    }
    if (face_count != 1) {
        std::fprintf(stderr, "ktx2_to_png: cubemap textures not supported\n");
        return false;
    }
    if (supercompression != 0) {
        std::fprintf(stderr, "ktx2_to_png: supercompression not supported\n");
        return false;
    }
    if (out.vk_format != kVkFormatEtc2RgbUnorm && out.vk_format != kVkFormatEtc2RgbSrgb) {
        std::fprintf(stderr,
                     "ktx2_to_png: unsupported vkFormat %u (need 147 ETC2_RGB_UNORM or 148 SRGB)\n",
                     out.vk_format);
        return false;
    }

    out.levels.resize(level_count);
    for (std::uint32_t i = 0; i < level_count; ++i) {
        const std::uint8_t* p = f.data() + 80 + i * 24u;
        out.levels[i].byte_offset = read_u64_le(p);
        out.levels[i].byte_length = read_u64_le(p + 8);
        // uncompressedByteLength at p + 16 — ignored (no supercompression).
    }
    out.bytes = f;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: ktx2_to_png input.ktx2 output.png\n");
        return 64;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "ktx2_to_png: cannot read %s\n", argv[1]);
        return 66;
    }
    std::vector<std::uint8_t> file_bytes(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Ktx2Image img;
    if (!parse_ktx2(file_bytes, img)) return 1;

    if (img.levels.size() != 1) {
        std::fprintf(stderr, "ktx2_to_png: %zu mip levels — only level 0 will be decoded\n",
                     img.levels.size());
    }
    const auto& lvl = img.levels[0];
    if (lvl.byte_offset + lvl.byte_length > img.bytes.size()) {
        std::fprintf(stderr, "ktx2_to_png: level data out of bounds\n");
        return 1;
    }

    // Decode 4×4 ETC2 RGB blocks via etcdec into a tightly-packed RGBA8 buffer.
    const int W = int(img.width);
    const int H = int(img.height);
    const int bcols = (W + 3) / 4;
    const int brows = (H + 3) / 4;
    constexpr int kBlockBytes = 8;
    const std::size_t expected_blocks = std::size_t(bcols) * std::size_t(brows);
    if (lvl.byte_length < expected_blocks * kBlockBytes) {
        std::fprintf(stderr,
                     "ktx2_to_png: level data too short for %dx%d ETC2 (got %llu need %zu)\n",
                     W, H,
                     static_cast<unsigned long long>(lvl.byte_length),
                     expected_blocks * std::size_t(kBlockBytes));
        return 1;
    }
    const std::uint8_t* blocks = img.bytes.data() + lvl.byte_offset;

    const int padded_w = bcols * 4;
    const int padded_h = brows * 4;
    std::vector<std::uint8_t> rgba(std::size_t(padded_w) * std::size_t(padded_h) * 4u, 0);
    for (int by = 0; by < brows; ++by) {
        for (int bx = 0; bx < bcols; ++bx) {
            const std::uint8_t* blk =
                blocks + (std::size_t(by) * std::size_t(bcols) + std::size_t(bx)) * kBlockBytes;
            std::uint8_t* dst =
                rgba.data() + (std::size_t(by * 4) * std::size_t(padded_w) + std::size_t(bx * 4)) * 4u;
            int pitch = padded_w * 4;
            etcdec_etc_rgb(blk, dst, pitch);
        }
    }

    // Crop the padded result to (W, H) and convert RGBA→RGB for PNG output.
    std::vector<std::uint8_t> rgb(std::size_t(W) * std::size_t(H) * 3u);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            std::size_t src = (std::size_t(y) * std::size_t(padded_w) + std::size_t(x)) * 4u;
            std::size_t dst = (std::size_t(y) * std::size_t(W) + std::size_t(x)) * 3u;
            rgb[dst + 0] = rgba[src + 0];
            rgb[dst + 1] = rgba[src + 1];
            rgb[dst + 2] = rgba[src + 2];
        }
    }

    if (!stbi_write_png(argv[2], W, H, 3, rgb.data(), W * 3)) {
        std::fprintf(stderr, "ktx2_to_png: stbi_write_png failed for %s\n", argv[2]);
        return 73;
    }
    std::fprintf(stderr, "ktx2_to_png: wrote %s (%dx%d, %d blocks, vkFormat %u)\n",
                 argv[2], W, H, int(expected_blocks), img.vk_format);
    return 0;
}
