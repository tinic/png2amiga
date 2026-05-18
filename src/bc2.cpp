// BC2 (DXT3) encoder — BC1 RGB block paired with 16 × 4-bit explicit
// alpha. Layout:
//   [0..7]  explicit alpha (64 bits, 4 bits/pixel, LSB-first)
//   [8..15] BC1 RGB block

#include "bc2.hpp"

#include "bc1.hpp"

#include <algorithm>
#include <cstring>
#include <span>

namespace png2amiga::bc2 {

namespace {

// Pack 16 × 4-bit alpha values into 8 bytes, LSB-first within each byte:
// byte 0 = (a[0] >> 4) | ((a[1] >> 4) << 4) etc. We round-to-nearest
// nibble — i.e. shift to 4-bit then add 8 before truncating, except
// at the high boundary where the shift saturates correctly already.
inline void encode_alpha_block(const std::uint8_t a16[16], std::uint8_t out[8]) {
    for (int i = 0; i < 8; ++i) {
        // Round-to-nearest 4-bit, then bit-replicate so the decoded
        // nibble matches what the hardware reconstructs (nib<<4 | nib).
        // To get round-to-nearest we approximate: alpha4 = (a + 8) >> 4,
        // clamped at 15.
        int n0 = std::min((int(a16[i * 2 + 0]) + 8) >> 4, 15);
        int n1 = std::min((int(a16[i * 2 + 1]) + 8) >> 4, 15);
        out[i] = std::uint8_t((n1 << 4) | (n0 & 0x0F));
    }
}

inline void decode_alpha_block(const std::uint8_t in[8], std::uint8_t out[16]) {
    for (int i = 0; i < 8; ++i) {
        int n0 = in[i] & 0x0F;
        int n1 = (in[i] >> 4) & 0x0F;
        // Bit-replicate 4-bit → 8-bit: nib * 17 (= nib<<4 | nib).
        out[i * 2 + 0] = std::uint8_t(n0 * 17);
        out[i * 2 + 1] = std::uint8_t(n1 * 17);
    }
}

}  // namespace

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 4]) {
    std::uint8_t a16[16];
    decode_alpha_block(blk.data(), a16);

    bc1::Block c_block{};
    std::memcpy(c_block.data(), blk.data() + 8, 8);
    std::uint8_t rgb[48];
    bc1::decode_block(c_block, rgb);

    for (int i = 0; i < 16; ++i) {
        out[i * 4 + 0] = rgb[i * 3 + 0];
        out[i * 4 + 1] = rgb[i * 3 + 1];
        out[i * 4 + 2] = rgb[i * 3 + 2];
        out[i * 4 + 3] = a16[i];
    }
}

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w, int image_h) {
    const int bcols = (image_w + kBlockW - 1) / kBlockW;
    const int brows = (image_h + kBlockH - 1) / kBlockH;
    const int pad_w = bcols * kBlockW;
    const int pad_h = brows * kBlockH;
    std::vector<std::uint8_t> rgba(std::size_t(pad_w) * std::size_t(pad_h) * 4u);
    std::uint8_t pix[kBlockPixels * 4];
    for (int by = 0; by < brows; ++by) {
        for (int bx = 0; bx < bcols; ++bx) {
            const auto& blk = blocks[std::size_t(by) * std::size_t(bcols) + std::size_t(bx)];
            decode_block(blk, pix);
            for (int dy = 0; dy < kBlockH; ++dy) {
                for (int dx = 0; dx < kBlockW; ++dx) {
                    int x = bx * kBlockW + dx;
                    int y = by * kBlockH + dy;
                    std::size_t d = (std::size_t(y) * std::size_t(pad_w) + std::size_t(x)) * 4u;
                    int s = (dy * kBlockW + dx) * 4;
                    rgba[d + 0] = pix[s + 0];
                    rgba[d + 1] = pix[s + 1];
                    rgba[d + 2] = pix[s + 2];
                    rgba[d + 3] = pix[s + 3];
                }
            }
        }
    }
    if (pad_w == image_w && pad_h == image_h) return rgba;
    std::vector<std::uint8_t> out(std::size_t(image_w) * std::size_t(image_h) * 4u);
    for (int y = 0; y < image_h; ++y) {
        std::memcpy(out.data() + std::size_t(y) * std::size_t(image_w) * 4u,
                    rgba.data() + std::size_t(y) * std::size_t(pad_w) * 4u,
                    std::size_t(image_w) * 4u);
    }
    return out;
}

EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    EncodeResult res;
    res.block_cols = (image_w + kBlockW - 1) / kBlockW;
    res.block_rows = (image_h + kBlockH - 1) / kBlockH;
    const std::size_t nblocks =
        std::size_t(res.block_cols) * std::size_t(res.block_rows);
    res.blocks.assign(nblocks, Block{});

    const std::size_t pix = std::size_t(image_w) * std::size_t(image_h);
    std::vector<std::uint8_t> rgb(pix * 3u);
    std::vector<std::uint8_t> alpha(pix);
    for (std::size_t i = 0; i < pix; ++i) {
        rgb[i * 3u + 0] = rgba_srgb8[i * 4u + 0];
        rgb[i * 3u + 1] = rgba_srgb8[i * 4u + 1];
        rgb[i * 3u + 2] = rgba_srgb8[i * 4u + 2];
        alpha[i] = rgba_srgb8[i * 4u + 3];
    }

    bc1::Options bopts;
    bopts.metric = options.metric;
    bopts.effort = options.effort;
    bopts.jitter = options.jitter;
    bopts.block_ed = options.block_ed;
    auto bc1_res = bc1::encode_image(rgb, image_w, image_h, bopts);

    for (int by = 0; by < res.block_rows; ++by) {
        for (int bx = 0; bx < res.block_cols; ++bx) {
            std::uint8_t a16[16];
            for (int dy = 0; dy < kBlockH; ++dy) {
                int sy = std::min(by * kBlockH + dy, image_h - 1);
                for (int dx = 0; dx < kBlockW; ++dx) {
                    int sx = std::min(bx * kBlockW + dx, image_w - 1);
                    a16[dy * kBlockW + dx] =
                        alpha[std::size_t(sy) * std::size_t(image_w) + std::size_t(sx)];
                }
            }
            std::uint8_t alpha_block[8];
            encode_alpha_block(a16, alpha_block);

            Block& dst = res.blocks[std::size_t(by) * std::size_t(res.block_cols) +
                                    std::size_t(bx)];
            std::memcpy(dst.data(), alpha_block, 8);
            std::memcpy(dst.data() + 8,
                        bc1_res.blocks[std::size_t(by) * std::size_t(res.block_cols) +
                                       std::size_t(bx)].data(),
                        8);
        }
    }

    res.total_oklab2_error = bc1_res.total_oklab2_error;
    return res;
}

}  // namespace png2amiga::bc2
