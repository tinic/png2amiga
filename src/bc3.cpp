// BC3 (DXT5) encoder — wraps the existing OKLab²-scored BC1 RGB
// encoder for the colour half and pairs it with a 1D BC4-style alpha
// block. Layout per block:
//   [0..7]   BC4 alpha block (2 endpoint bytes + 48 bits of 3-bit selectors)
//   [8..15]  BC1 RGB block

#include "bc3.hpp"

#include "bc1.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

namespace png2amiga::bc3 {

namespace {

// ---------- BC4-style alpha block (private helper) -------------------------

// Compute the 8-level interpolation ramp used when a0 > a1.
inline void make_ramp_8(int a0, int a1, int ramp[8]) {
    ramp[0] = a0;
    ramp[1] = a1;
    // (7-i)*a0 + i*a1 over /7 for i = 1..6 → indices 2..7.
    // Standard formula uses div /7 + rounding.
    for (int i = 1; i <= 6; ++i) {
        ramp[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    }
}

// Compute the 6-interp + 0/255 ramp used when a0 <= a1.
inline void make_ramp_6(int a0, int a1, int ramp[8]) {
    ramp[0] = a0;
    ramp[1] = a1;
    for (int i = 1; i <= 4; ++i) {
        ramp[i + 1] = ((5 - i) * a0 + i * a1) / 5;
    }
    ramp[6] = 0;
    ramp[7] = 255;
}

// Encode 16 alpha samples into a BC4-style alpha block (8 bytes).
// Returns the squared-error sum (raw byte² units).
inline std::uint64_t encode_alpha_block(const std::uint8_t a16[16], std::uint8_t out[8]) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; ++i) {
        int v = a16[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }

    // Candidate A: 8-interp (a0 > a1). Endpoints stretch a bit inward
    // to compensate for the integer-divided lerp not hitting the exact
    // min/max — same trick rgbcx/squish use.
    auto try_mode_8 = [&](std::uint8_t e0, std::uint8_t e1,
                          std::uint64_t& best_se,
                          std::uint8_t best_sel[16],
                          std::uint8_t best_endpoints[2]) {
        if (e0 == e1) return;  // ramp degenerate; 6-interp handles flat blocks
        if (e0 < e1) std::swap(e0, e1);  // enforce a0 > a1 for 8-interp
        int ramp[8];
        make_ramp_8(int(e0), int(e1), ramp);
        std::uint8_t sel[16];
        std::uint64_t se = 0;
        for (int i = 0; i < 16; ++i) {
            int v = a16[i];
            int best_s = 0;
            int best_d = std::numeric_limits<int>::max();
            for (int s = 0; s < 8; ++s) {
                int d = v - ramp[s];
                int dd = d * d;
                if (dd < best_d) { best_d = dd; best_s = s; }
            }
            sel[i] = std::uint8_t(best_s);
            se += std::uint64_t(best_d);
        }
        if (se < best_se) {
            best_se = se;
            std::memcpy(best_sel, sel, 16);
            best_endpoints[0] = e0;
            best_endpoints[1] = e1;
        }
    };

    // Candidate B: 6-interp + 0/255 anchors (a0 <= a1). Helps blocks
    // that include exact black / white as outliers.
    auto try_mode_6 = [&](std::uint8_t e0, std::uint8_t e1,
                          std::uint64_t& best_se,
                          std::uint8_t best_sel[16],
                          std::uint8_t best_endpoints[2]) {
        if (e0 > e1) std::swap(e0, e1);  // enforce a0 <= a1 for 6-interp
        int ramp[8];
        make_ramp_6(int(e0), int(e1), ramp);
        std::uint8_t sel[16];
        std::uint64_t se = 0;
        for (int i = 0; i < 16; ++i) {
            int v = a16[i];
            int best_s = 0;
            int best_d = std::numeric_limits<int>::max();
            for (int s = 0; s < 8; ++s) {
                int d = v - ramp[s];
                int dd = d * d;
                if (dd < best_d) { best_d = dd; best_s = s; }
            }
            sel[i] = std::uint8_t(best_s);
            se += std::uint64_t(best_d);
        }
        if (se < best_se) {
            best_se = se;
            std::memcpy(best_sel, sel, 16);
            best_endpoints[0] = e0;
            best_endpoints[1] = e1;
        }
    };

    std::uint64_t best_se = std::numeric_limits<std::uint64_t>::max();
    std::uint8_t best_sel[16] = {};
    std::uint8_t best_endpoints[2] = {std::uint8_t(mx), std::uint8_t(mn)};

    try_mode_8(std::uint8_t(mx), std::uint8_t(mn), best_se, best_sel, best_endpoints);
    try_mode_6(std::uint8_t(mn), std::uint8_t(mx), best_se, best_sel, best_endpoints);

    // Pack: endpoint bytes + 48 bits of 3-bit selectors LSB-first.
    out[0] = best_endpoints[0];
    out[1] = best_endpoints[1];
    std::uint64_t sel_bits = 0;
    for (int i = 0; i < 16; ++i) {
        sel_bits |= std::uint64_t(best_sel[i] & 0x7u) << (i * 3);
    }
    for (int i = 0; i < 6; ++i) {
        out[2 + i] = std::uint8_t((sel_bits >> (i * 8)) & 0xFF);
    }
    return best_se;
}

// Decode one BC4-style alpha block into 16 alpha samples.
inline void decode_alpha_block(const std::uint8_t in[8], std::uint8_t out[16]) {
    int a0 = in[0], a1 = in[1];
    int ramp[8];
    if (a0 > a1) make_ramp_8(a0, a1, ramp);
    else         make_ramp_6(a0, a1, ramp);

    std::uint64_t sel_bits = 0;
    for (int i = 0; i < 6; ++i) {
        sel_bits |= std::uint64_t(in[2 + i]) << (i * 8);
    }
    for (int i = 0; i < 16; ++i) {
        int s = int((sel_bits >> (i * 3)) & 0x7u);
        out[i] = std::uint8_t(ramp[s]);
    }
}

}  // namespace

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 4]) {
    // BC3 layout: alpha first, then BC1 RGB.
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
    // Trim to image_w × image_h.
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

    // Split into RGB + alpha planes. The BC1 RGB encoder shoulders the
    // perceptual work; the alpha encoder is a straightforward scalar
    // search.
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

    // Alpha pass — per block, gather the 16 source alphas (with edge
    // clamping) and encode via the BC4-style helper.
    std::uint64_t total_alpha_se = 0;
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
            total_alpha_se += encode_alpha_block(a16, alpha_block);

            // Pack BC3 block: alpha first, then BC1 RGB.
            Block& dst = res.blocks[std::size_t(by) * std::size_t(res.block_cols) +
                                    std::size_t(bx)];
            std::memcpy(dst.data(), alpha_block, 8);
            std::memcpy(dst.data() + 8,
                        bc1_res.blocks[std::size_t(by) * std::size_t(res.block_cols) +
                                       std::size_t(bx)].data(),
                        8);
        }
    }

    res.total_oklab2_error = bc1_res.total_oklab2_error;  // colour half only
    // Alpha SE is reported separately if a future caller wants it.
    (void)total_alpha_se;
    return res;
}

}  // namespace png2amiga::bc3
