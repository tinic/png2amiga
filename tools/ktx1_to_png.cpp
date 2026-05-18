// ktx1_to_png — decode a KTX1 file containing ETC1 / ETC2 RGB8 blocks
// back to PNG via iOrange's etcdec.h (third_party/etcdec). Used to feed
// etc2comp's output (which writes KTX1, not KTX2) into our bench
// harness on the same decode + scoring path our own KTX2 outputs use.
//
// KTX1 layout (from Khronos spec):
//   [12]   identifier: AB 4B 54 58 20 31 31 BB 0D 0A 1A 0A
//   [4]    endianness (0x04030201 = little-endian as we write/expect)
//   [4]    glType (compressed → 0)
//   [4]    glTypeSize (compressed → 1)
//   [4]    glFormat (compressed → 0)
//   [4]    glInternalFormat
//   [4]    glBaseInternalFormat
//   [4]    pixelWidth
//   [4]    pixelHeight
//   [4]    pixelDepth (2D = 0)
//   [4]    numberOfArrayElements (non-array = 0)
//   [4]    numberOfFaces (non-cube = 1)
//   [4]    numberOfMipmapLevels
//   [4]    bytesOfKeyValueData
//   [N]    KVD bytes
//   per mip level:
//     [4]    imageSize
//     [imageSize] blocks
//     [pad]  to 4-byte boundary
//
// We support 2D only, non-array, single face, single mip level, with
// glInternalFormat ∈ { GL_COMPRESSED_RGB8_ETC2, GL_ETC1_RGB8_OES }.

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

constexpr std::uint8_t kKtx1Identifier[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
};

// GL token values from <GLES3/gl3.h> — vendored here to avoid pulling
// in any GL header (this is a pure decoder tool).
constexpr std::uint32_t GL_COMPRESSED_RGB8_ETC2 = 0x9274;
constexpr std::uint32_t GL_COMPRESSED_SRGB8_ETC2 = 0x9275;
constexpr std::uint32_t GL_ETC1_RGB8_OES = 0x8D64;

std::uint32_t read_u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: ktx1_to_png input.ktx output.png\n");
        return 64;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "ktx1_to_png: cannot read %s\n", argv[1]);
        return 66;
    }
    std::vector<std::uint8_t> file(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    if (file.size() < 64) {
        std::fprintf(stderr, "ktx1_to_png: file too small\n");
        return 1;
    }
    if (std::memcmp(file.data(), kKtx1Identifier, 12) != 0) {
        std::fprintf(stderr, "ktx1_to_png: bad KTX1 magic\n");
        return 1;
    }

    std::uint32_t endianness = read_u32(file.data() + 12);
    if (endianness != 0x04030201u) {
        std::fprintf(stderr,
                     "ktx1_to_png: unsupported endianness 0x%x (need little-endian)\n",
                     endianness);
        return 1;
    }
    std::uint32_t gl_type = read_u32(file.data() + 16);
    std::uint32_t gl_type_size = read_u32(file.data() + 20);
    std::uint32_t gl_format = read_u32(file.data() + 24);
    std::uint32_t gl_internal_format = read_u32(file.data() + 28);
    // gl_base_internal_format at +32 (informative; ignored)
    std::uint32_t pixel_width = read_u32(file.data() + 36);
    std::uint32_t pixel_height = read_u32(file.data() + 40);
    std::uint32_t pixel_depth = read_u32(file.data() + 44);
    std::uint32_t num_arr_elems = read_u32(file.data() + 48);
    std::uint32_t num_faces = read_u32(file.data() + 52);
    std::uint32_t num_mip_levels = read_u32(file.data() + 56);
    std::uint32_t kvd_size = read_u32(file.data() + 60);

    (void)gl_type_size;
    if (gl_type != 0 || gl_format != 0) {
        std::fprintf(stderr,
                     "ktx1_to_png: not a compressed format (glType=%u glFormat=%u)\n",
                     gl_type, gl_format);
        return 1;
    }
    if (gl_internal_format != GL_COMPRESSED_RGB8_ETC2 &&
        gl_internal_format != GL_COMPRESSED_SRGB8_ETC2 &&
        gl_internal_format != GL_ETC1_RGB8_OES) {
        std::fprintf(stderr,
                     "ktx1_to_png: unsupported internalFormat 0x%x "
                     "(need ETC1 RGB8 / ETC2 RGB8 / SRGB8)\n",
                     gl_internal_format);
        return 1;
    }
    if (pixel_depth != 0 || num_arr_elems != 0 || num_faces != 1) {
        std::fprintf(stderr,
                     "ktx1_to_png: only 2D non-array non-cube supported "
                     "(depth=%u arr=%u faces=%u)\n",
                     pixel_depth, num_arr_elems, num_faces);
        return 1;
    }
    if (num_mip_levels == 0) num_mip_levels = 1;  // 0 means autogenerate; treat as 1
    if (num_mip_levels != 1) {
        std::fprintf(stderr,
                     "ktx1_to_png: %u mip levels — only level 0 will be decoded\n",
                     num_mip_levels);
    }

    std::size_t offset = 64u + kvd_size;
    if (offset + 4 > file.size()) {
        std::fprintf(stderr, "ktx1_to_png: truncated (kvd overflow)\n");
        return 1;
    }
    std::uint32_t image_size = read_u32(file.data() + offset);
    offset += 4;
    if (offset + image_size > file.size()) {
        std::fprintf(stderr, "ktx1_to_png: truncated (imageSize=%u)\n", image_size);
        return 1;
    }

    const int W = int(pixel_width);
    const int H = int(pixel_height);
    constexpr int kBlockBytes = 8;
    const int bcols = (W + 3) / 4;
    const int brows = (H + 3) / 4;
    const std::size_t expected_blocks = std::size_t(bcols) * std::size_t(brows);
    if (image_size < expected_blocks * kBlockBytes) {
        std::fprintf(stderr,
                     "ktx1_to_png: imageSize %u < expected %zu for %dx%d ETC2\n",
                     image_size, expected_blocks * std::size_t(kBlockBytes), W, H);
        return 1;
    }

    const std::uint8_t* blocks = file.data() + offset;
    const int padded_w = bcols * 4;
    const int padded_h = brows * 4;
    std::vector<std::uint8_t> rgba(std::size_t(padded_w) * std::size_t(padded_h) * 4u, 0);
    for (int by = 0; by < brows; ++by) {
        for (int bx = 0; bx < bcols; ++bx) {
            const std::uint8_t* blk =
                blocks + (std::size_t(by) * std::size_t(bcols) + std::size_t(bx)) * kBlockBytes;
            std::uint8_t* dst = rgba.data() +
                                (std::size_t(by * 4) * std::size_t(padded_w) +
                                 std::size_t(bx * 4)) * 4u;
            int pitch = padded_w * 4;
            etcdec_etc_rgb(blk, dst, pitch);
        }
    }

    // Crop padded → (W, H), drop alpha (we know it's RGB8 ETC2).
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
        std::fprintf(stderr, "ktx1_to_png: stbi_write_png failed for %s\n", argv[2]);
        return 73;
    }
    std::fprintf(stderr,
                 "ktx1_to_png: wrote %s (%dx%d, %d blocks, glInternalFormat 0x%x)\n",
                 argv[2], W, H, int(expected_blocks), gl_internal_format);
    return 0;
}
