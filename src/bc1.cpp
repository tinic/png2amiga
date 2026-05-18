// BC1 (DXT1) RGB encoder + decoder.
//
// BC1 block layout (8 bytes, little-endian):
//   bytes 0..1 : color0 as uint16 RGB565 (R:bits15..11, G:bits10..5, B:bits4..0)
//   bytes 2..3 : color1 as uint16 RGB565
//   bytes 4..7 : 32-bit selector stream — pixel (x,y) at bit position (y*4+x)*2
//
// Two sub-modes are decoded based on raw uint16 comparison:
//   color0 >  color1 : 4-color RGB block — paints {c0, c1, (2c0+c1)/3, (c0+2c1)/3}
//   color0 <= color1 : 3-color + 1-bit-alpha block — paints {c0, c1, (c0+c1)/2, A=0}
//
// This encoder always produces 4-color blocks (no alpha). Packing
// enforces raw_c0 > raw_c1 by swap + selector remap, or by perturbing
// c1 down one nibble if the chosen endpoints are equal in RGB565.

#include "bc1.hpp"

#include "color_space.hpp"
#include "pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

namespace png2amiga::bc1 {

namespace {

constexpr std::uint8_t clamp_u8(int v) {
    return std::uint8_t(std::clamp(v, 0, 255));
}

// 5-bit → 8-bit (bit-replication): x → (x << 3) | (x >> 2).
constexpr std::uint8_t expand5(std::uint32_t v) {
    return std::uint8_t((v << 3) | (v >> 2));
}
// 6-bit → 8-bit: x → (x << 2) | (x >> 4).
constexpr std::uint8_t expand6(std::uint32_t v) {
    return std::uint8_t((v << 2) | (v >> 4));
}
// 8-bit → 5-bit / 6-bit round-to-nearest with clamp.
constexpr std::uint32_t reduce5(int v) {
    int q = (std::clamp(v, 0, 255) * 31 + 127) / 255;
    return std::uint32_t(std::clamp(q, 0, 31));
}
constexpr std::uint32_t reduce6(int v) {
    int q = (std::clamp(v, 0, 255) * 63 + 127) / 255;
    return std::uint32_t(std::clamp(q, 0, 63));
}

struct RGB565 {
    std::uint8_t r5;  // 0..31
    std::uint8_t g6;  // 0..63
    std::uint8_t b5;  // 0..31

    constexpr std::uint16_t raw() const noexcept {
        return std::uint16_t((std::uint16_t(r5) << 11) |
                             (std::uint16_t(g6) << 5)  |
                              std::uint16_t(b5));
    }
    static constexpr RGB565 from_raw(std::uint16_t w) noexcept {
        return {std::uint8_t((w >> 11) & 0x1Fu),
                std::uint8_t((w >> 5)  & 0x3Fu),
                std::uint8_t(w & 0x1Fu)};
    }
    constexpr void expand8(std::uint8_t out[3]) const noexcept {
        out[0] = expand5(r5);
        out[1] = expand6(g6);
        out[2] = expand5(b5);
    }
};

// Pack/unpack the 4 selector bytes (little-endian) at bytes 4..7.
inline std::uint32_t read_selectors(const Block& blk) {
    return std::uint32_t(blk[4]) |
           (std::uint32_t(blk[5]) << 8) |
           (std::uint32_t(blk[6]) << 16) |
           (std::uint32_t(blk[7]) << 24);
}
inline void write_selectors(Block& blk, std::uint32_t s) {
    blk[4] = std::uint8_t(s & 0xFFu);
    blk[5] = std::uint8_t((s >> 8) & 0xFFu);
    blk[6] = std::uint8_t((s >> 16) & 0xFFu);
    blk[7] = std::uint8_t((s >> 24) & 0xFFu);
}

inline std::uint16_t read_u16le(const Block& blk, int off) {
    return std::uint16_t(std::uint16_t(blk[std::size_t(off)]) |
                         (std::uint16_t(blk[std::size_t(off + 1)]) << 8));
}
inline void write_u16le(Block& blk, int off, std::uint16_t v) {
    blk[std::size_t(off)] = std::uint8_t(v & 0xFFu);
    blk[std::size_t(off + 1)] = std::uint8_t((v >> 8) & 0xFFu);
}

}  // namespace

// ---------------------------------------------------------------------------
// Decoder — matches the Khronos BC1 spec / Microsoft S3TC reference.
// ---------------------------------------------------------------------------

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 3]) {
    std::uint16_t raw0 = read_u16le(blk, 0);
    std::uint16_t raw1 = read_u16le(blk, 2);
    RGB565 c0 = RGB565::from_raw(raw0);
    RGB565 c1 = RGB565::from_raw(raw1);

    std::uint8_t paint[4][3];
    c0.expand8(paint[0]);
    c1.expand8(paint[1]);
    if (raw0 > raw1) {
        // 4-color mode: 2/3 and 1/3 lerps per channel.
        for (int ch = 0; ch < 3; ++ch) {
            paint[2][ch] = std::uint8_t((2 * int(paint[0][ch]) + int(paint[1][ch]) + 1) / 3);
            paint[3][ch] = std::uint8_t((int(paint[0][ch]) + 2 * int(paint[1][ch]) + 1) / 3);
        }
    } else {
        // 3-color mode + 1-bit alpha. We always decode the "punchthrough"
        // index 3 as opaque black (no alpha output channel here).
        for (int ch = 0; ch < 3; ++ch) {
            paint[2][ch] = std::uint8_t((int(paint[0][ch]) + int(paint[1][ch]) + 1) / 2);
            paint[3][ch] = 0;
        }
    }

    std::uint32_t sel = read_selectors(blk);
    for (int y = 0; y < kBlockH; ++y) {
        for (int x = 0; x < kBlockW; ++x) {
            int i = y * kBlockW + x;
            int s = int((sel >> (i * 2)) & 0x3u);
            out[i * 3 + 0] = paint[s][0];
            out[i * 3 + 1] = paint[s][1];
            out[i * 3 + 2] = paint[s][2];
        }
    }
}

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks, int image_w, int image_h) {
    int bc = (image_w + kBlockW - 1) / kBlockW;
    int br = (image_h + kBlockH - 1) / kBlockH;
    int pad_w = bc * kBlockW;
    int pad_h = br * kBlockH;
    std::vector<std::uint8_t> out(std::size_t(pad_w) * std::size_t(pad_h) * 3u, 0);
    std::uint8_t blk_out[kBlockPixels * 3];
    for (int by = 0; by < br; ++by) {
        for (int bx = 0; bx < bc; ++bx) {
            std::size_t bidx = std::size_t(by) * std::size_t(bc) + std::size_t(bx);
            decode_block(blocks[bidx], blk_out);
            for (int dy = 0; dy < kBlockH; ++dy) {
                for (int dx = 0; dx < kBlockW; ++dx) {
                    int sx = bx * kBlockW + dx;
                    int sy = by * kBlockH + dy;
                    if (sx >= image_w || sy >= image_h) continue;
                    std::size_t d = (std::size_t(sy) * std::size_t(image_w) + std::size_t(sx)) * 3u;
                    int spx = dy * kBlockW + dx;
                    out[d + 0] = blk_out[spx * 3 + 0];
                    out[d + 1] = blk_out[spx * 3 + 1];
                    out[d + 2] = blk_out[spx * 3 + 2];
                }
            }
        }
    }
    out.resize(std::size_t(image_w) * std::size_t(image_h) * 3u);
    return out;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

namespace {

struct Sample16 {
    std::uint8_t srgb8[16][3];
    color_space::OKLab lab[16];
};

// Load 16 source pixels into the sample buffer, optionally with an OKLab
// shift applied to s.lab[] (block-grid ED carry from prior blocks).
void load_sample(Sample16& s,
                 std::span<const std::uint8_t> padded_rgb,
                 std::size_t pad_w,
                 int px,
                 int py,
                 color_space::OKLab shift = {0.f, 0.f, 0.f}) {
    for (int dy = 0; dy < kBlockH; ++dy) {
        for (int dx = 0; dx < kBlockW; ++dx) {
            std::size_t idx = (std::size_t(py + dy) * pad_w + std::size_t(px + dx)) * 3u;
            int i = dy * kBlockW + dx;
            s.srgb8[i][0] = padded_rgb[idx + 0u];
            s.srgb8[i][1] = padded_rgb[idx + 1u];
            s.srgb8[i][2] = padded_rgb[idx + 2u];
        }
    }
    // Batched OKLab convert (4 pixels per batch, 4 batches).
    for (int g = 0; g < 16; g += 4) {
        std::uint8_t rgb4[4][3];
        for (int j = 0; j < 4; ++j) {
            rgb4[j][0] = s.srgb8[g + j][0];
            rgb4[j][1] = s.srgb8[g + j][1];
            rgb4[j][2] = s.srgb8[g + j][2];
        }
        auto labs = color_space::srgb8_to_oklab_batch4(rgb4);
        for (int j = 0; j < 4; ++j) {
            s.lab[g + j].L = labs.labs[j].L + shift.L;
            s.lab[g + j].a = labs.labs[j].a + shift.a;
            s.lab[g + j].b = labs.labs[j].b + shift.b;
        }
    }
}

// Score a decoded 16-pixel block against a Sample16 in the chosen metric.
template<block_compress::BlockMetric M>
float score_decoded(const Sample16& s, const std::uint8_t dec[16][3]) {
    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
        int acc = 0;
        for (int i = 0; i < 16; ++i) {
            int dr = int(s.srgb8[i][0]) - int(dec[i][0]);
            int dg = int(s.srgb8[i][1]) - int(dec[i][1]);
            int db = int(s.srgb8[i][2]) - int(dec[i][2]);
            acc += dr * dr + dg * dg + db * db;
        }
        return float(acc) * (1.0f / 65536.0f);
    } else {
        float acc = 0.0f;
        for (int g = 0; g < 16; g += 4) {
            std::uint8_t rgb4[4][3];
            for (int j = 0; j < 4; ++j) {
                rgb4[j][0] = dec[g + j][0];
                rgb4[j][1] = dec[g + j][1];
                rgb4[j][2] = dec[g + j][2];
            }
            auto labs = color_space::srgb8_to_oklab_batch4(rgb4);
            for (int j = 0; j < 4; ++j) {
                const auto& d = labs.labs[j];
                float dL = s.lab[g + j].L - d.L;
                float da = s.lab[g + j].a - d.a;
                float db = s.lab[g + j].b - d.b;
                acc += color_space::fma_dist_sq(dL, da, db);
            }
        }
        return acc;
    }
}

// Compute the 4 paint colors (8-bit expanded) from a pair of RGB565
// endpoints in 4-color mode. Spec rounding: (2*a + b + 1) / 3.
inline void compute_paints(RGB565 c0, RGB565 c1, std::uint8_t paint[4][3]) {
    c0.expand8(paint[0]);
    c1.expand8(paint[1]);
    for (int ch = 0; ch < 3; ++ch) {
        paint[2][ch] = std::uint8_t((2 * int(paint[0][ch]) + int(paint[1][ch]) + 1) / 3);
        paint[3][ch] = std::uint8_t((int(paint[0][ch]) + 2 * int(paint[1][ch]) + 1) / 3);
    }
}

// 3-color mode: paint[2] = midpoint, paint[3] = unused (transparent in
// the BC1A variant). For our RGB-only encoder we treat selector 3 as
// forbidden and constrain the per-pixel argmin to {0, 1, 2}. Useful for
// blocks that are essentially 2-color + a midpoint band, where the
// 4-color 1/3-2/3 quartiles overshoot the actual colour distribution.
inline void compute_paints_3c(RGB565 c0, RGB565 c1, std::uint8_t paint[4][3]) {
    c0.expand8(paint[0]);
    c1.expand8(paint[1]);
    for (int ch = 0; ch < 3; ++ch) {
        paint[2][ch] = std::uint8_t((int(paint[0][ch]) + int(paint[1][ch]) + 1) / 2);
        paint[3][ch] = 0;  // sentinel; never picked
    }
}

// Pick the per-pixel selector that minimises block error for the given
// endpoints. Returns total error in the active metric and writes 16
// selector indices into out_sel.
// Mode3c=false → 4-color block (4 paints, all selectors valid).
// Mode3c=true  → 3-color block (paint[3] is the unused punchthrough
// slot — selectors are constrained to {0, 1, 2}).
template<block_compress::BlockMetric M, bool Mode3c = false>
float pick_selectors_and_score(const Sample16& s,
                               RGB565 c0,
                               RGB565 c1,
                               std::uint8_t out_sel[16],
                               std::uint8_t decoded[16][3]) {
    std::uint8_t paint[4][3];
    if constexpr (Mode3c) {
        compute_paints_3c(c0, c1, paint);
    } else {
        compute_paints(c0, c1, paint);
    }

    alignas(16) float paint_L[4], paint_A[4], paint_B[4];
    if constexpr (M == block_compress::BlockMetric::oklab2) {
        auto labs = color_space::srgb8_to_oklab_batch4(paint);
        for (int k = 0; k < 4; ++k) {
            paint_L[k] = labs.labs[k].L;
            paint_A[k] = labs.labs[k].a;
            paint_B[k] = labs.labs[k].b;
        }
    }

    float tot = 0.0f;
    for (int p = 0; p < 16; ++p) {
        float e[4];
        if constexpr (M == block_compress::BlockMetric::srgb_mse) {
            int sr = int(s.srgb8[p][0]);
            int sg = int(s.srgb8[p][1]);
            int sb = int(s.srgb8[p][2]);
            for (int k = 0; k < 4; ++k) {
                int dr = sr - int(paint[k][0]);
                int dg = sg - int(paint[k][1]);
                int db = sb - int(paint[k][2]);
                e[k] = float(dr * dr + dg * dg + db * db);
            }
        } else {
            float sL = s.lab[p].L, sA = s.lab[p].a, sB = s.lab[p].b;
            for (int k = 0; k < 4; ++k) {
                float dL = sL - paint_L[k];
                float dA = sA - paint_A[k];
                float dB = sB - paint_B[k];
                e[k] = dL * dL + dA * dA + dB * dB;
            }
        }
        int best_k = 0;
        float best_e = e[0];
        // 3-color mode forbids selector 3 — exclude it from argmin.
        const int k_end = Mode3c ? 3 : 4;
        for (int k = 1; k < k_end; ++k) {
            if (e[k] < best_e) { best_e = e[k]; best_k = k; }
        }
        out_sel[p] = std::uint8_t(best_k);
        decoded[p][0] = paint[best_k][0];
        decoded[p][1] = paint[best_k][1];
        decoded[p][2] = paint[best_k][2];
        tot += best_e;
    }
    return tot;
}

// Bounding-box endpoint seed in 8-bit sRGB space. Cheap fallback used
// when PCA reports a degenerate (~zero variance) block.
inline void bbox_seed(const Sample16& s, std::uint8_t e0[3], std::uint8_t e1[3]) {
    e0[0] = 255; e0[1] = 255; e0[2] = 255;
    e1[0] = 0;   e1[1] = 0;   e1[2] = 0;
    for (int i = 0; i < 16; ++i) {
        for (int ch = 0; ch < 3; ++ch) {
            std::uint8_t v = s.srgb8[i][ch];
            if (v < e0[ch]) e0[ch] = v;
            if (v > e1[ch]) e1[ch] = v;
        }
    }
}

// PCA-based endpoint seed in OKLab space. Project pixels onto the
// dominant perceptual covariance axis and take the OKLab-projection
// extremes (returning the actual sRGB8 pixel values at those positions).
// Vs sRGB-space PCA: the axis is perceptually-weighted, so the
// endpoints align with the direction the OKLab² scorer rewards.
inline void pca_seed_oklab(const Sample16& s, std::uint8_t e0[3], std::uint8_t e1[3]) {
    // Mean in OKLab.
    float mL = 0, mA = 0, mB = 0;
    for (int i = 0; i < 16; ++i) {
        mL += s.lab[i].L; mA += s.lab[i].a; mB += s.lab[i].b;
    }
    mL *= 1.f / 16.f; mA *= 1.f / 16.f; mB *= 1.f / 16.f;
    // Covariance matrix (symmetric 3×3) in OKLab.
    float cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
    for (int i = 0; i < 16; ++i) {
        float dx = s.lab[i].L - mL;
        float dy = s.lab[i].a - mA;
        float dz = s.lab[i].b - mB;
        cxx += dx * dx; cxy += dx * dy; cxz += dx * dz;
        cyy += dy * dy; cyz += dy * dz;
        czz += dz * dz;
    }
    if (cxx + cyy + czz < 1e-7f) {
        bbox_seed(s, e0, e1);
        return;
    }
    // Power iteration: 4 sweeps suffice for clean convergence at 16 px.
    float vx = cxx + cxy + cxz;
    float vy = cxy + cyy + cyz;
    float vz = cxz + cyz + czz;
    for (int it = 0; it < 4; ++it) {
        float nx = cxx * vx + cxy * vy + cxz * vz;
        float ny = cxy * vx + cyy * vy + cyz * vz;
        float nz = cxz * vx + cyz * vy + czz * vz;
        float m = std::max({std::abs(nx), std::abs(ny), std::abs(nz)});
        if (m < 1e-9f) { bbox_seed(s, e0, e1); return; }
        float inv = 1.f / m;
        vx = nx * inv; vy = ny * inv; vz = nz * inv;
    }
    // Project all pixels onto the OKLab axis; track min/max projections.
    float pmin = std::numeric_limits<float>::infinity();
    float pmax = -std::numeric_limits<float>::infinity();
    int imin = 0, imax = 0;
    for (int i = 0; i < 16; ++i) {
        float dx = s.lab[i].L - mL;
        float dy = s.lab[i].a - mA;
        float dz = s.lab[i].b - mB;
        float t = dx * vx + dy * vy + dz * vz;
        if (t < pmin) { pmin = t; imin = i; }
        if (t > pmax) { pmax = t; imax = i; }
    }
    e0[0] = s.srgb8[imin][0]; e0[1] = s.srgb8[imin][1]; e0[2] = s.srgb8[imin][2];
    e1[0] = s.srgb8[imax][0]; e1[1] = s.srgb8[imax][1]; e1[2] = s.srgb8[imax][2];
}

// sRGB-space PCA — same as pca_seed_oklab but covariance computed in
// 8-bit sRGB. Different basin for blocks where the dominant sRGB axis
// disagrees with the OKLab axis (saturated colours, near-grayscale).
// Currently unused (kept for future --best multi-trial path).
[[maybe_unused]] inline void pca_seed_srgb(const Sample16& s, std::uint8_t e0[3], std::uint8_t e1[3]) {
    float mx = 0, my = 0, mz = 0;
    for (int i = 0; i < 16; ++i) {
        mx += float(s.srgb8[i][0]);
        my += float(s.srgb8[i][1]);
        mz += float(s.srgb8[i][2]);
    }
    mx *= 1.f / 16.f; my *= 1.f / 16.f; mz *= 1.f / 16.f;
    float cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
    for (int i = 0; i < 16; ++i) {
        float dx = float(s.srgb8[i][0]) - mx;
        float dy = float(s.srgb8[i][1]) - my;
        float dz = float(s.srgb8[i][2]) - mz;
        cxx += dx * dx; cxy += dx * dy; cxz += dx * dz;
        cyy += dy * dy; cyz += dy * dz;
        czz += dz * dz;
    }
    if (cxx + cyy + czz < 1e-3f) { bbox_seed(s, e0, e1); return; }
    float vx = cxx + cxy + cxz;
    float vy = cxy + cyy + cyz;
    float vz = cxz + cyz + czz;
    for (int it = 0; it < 4; ++it) {
        float nx = cxx * vx + cxy * vy + cxz * vz;
        float ny = cxy * vx + cyy * vy + cyz * vz;
        float nz = cxz * vx + cyz * vy + czz * vz;
        float m = std::max({std::abs(nx), std::abs(ny), std::abs(nz)});
        if (m < 1e-6f) { bbox_seed(s, e0, e1); return; }
        float inv = 1.f / m;
        vx = nx * inv; vy = ny * inv; vz = nz * inv;
    }
    float pmin = std::numeric_limits<float>::infinity();
    float pmax = -std::numeric_limits<float>::infinity();
    int imin = 0, imax = 0;
    for (int i = 0; i < 16; ++i) {
        float dx = float(s.srgb8[i][0]) - mx;
        float dy = float(s.srgb8[i][1]) - my;
        float dz = float(s.srgb8[i][2]) - mz;
        float t = dx * vx + dy * vy + dz * vz;
        if (t < pmin) { pmin = t; imin = i; }
        if (t > pmax) { pmax = t; imax = i; }
    }
    e0[0] = s.srgb8[imin][0]; e0[1] = s.srgb8[imin][1]; e0[2] = s.srgb8[imin][2];
    e1[0] = s.srgb8[imax][0]; e1[1] = s.srgb8[imax][1]; e1[2] = s.srgb8[imax][2];
}

// Farthest-pair seed in OKLab: the two pixels with the largest OKLab²
// distance. A purely "structural" seed that ignores covariance and
// goes for the most extreme observed difference. Useful for blocks
// with two well-separated colour clusters where PCA averages them.
// Currently unused (kept for future --best multi-trial path).
[[maybe_unused]] inline void farthest_pair_seed(const Sample16& s,
                                                std::uint8_t e0[3], std::uint8_t e1[3]) {
    int bi = 0, bj = 1;
    float best = -1.f;
    for (int i = 0; i < 15; ++i) {
        for (int j = i + 1; j < 16; ++j) {
            float dL = s.lab[i].L - s.lab[j].L;
            float dA = s.lab[i].a - s.lab[j].a;
            float dB = s.lab[i].b - s.lab[j].b;
            float d = dL * dL + dA * dA + dB * dB;
            if (d > best) { best = d; bi = i; bj = j; }
        }
    }
    e0[0] = s.srgb8[bi][0]; e0[1] = s.srgb8[bi][1]; e0[2] = s.srgb8[bi][2];
    e1[0] = s.srgb8[bj][0]; e1[1] = s.srgb8[bj][1]; e1[2] = s.srgb8[bj][2];
}

// Lloyd-style closed-form refit: given fixed selectors, solve the 2-var
// least-squares for (c0, c1) in 8-bit space minimising
//   Σ_p (S[p] - w0[sel[p]]·c0 - w1[sel[p]]·c1)²
// where (w0, w1) by selector = (1,0), (0,1), (2/3,1/3), (1/3,2/3).
// Returns new endpoints in 8-bit; caller re-quantises to RGB565.
inline bool refit_endpoints(const Sample16& s,
                            const std::uint8_t sel[16],
                            std::uint8_t e0[3],
                            std::uint8_t e1[3]) {
    // Weights × 3 (scaled to integer-friendly form): (3,0) (0,3) (2,1) (1,2)
    constexpr int W0[4] = {3, 0, 2, 1};
    constexpr int W1[4] = {0, 3, 1, 2};
    float A00 = 0, A01 = 0, A11 = 0;
    float B0[3] = {0, 0, 0};
    float B1[3] = {0, 0, 0};
    for (int p = 0; p < 16; ++p) {
        int k = sel[p];
        float w0 = float(W0[k]) * (1.0f / 3.0f);
        float w1 = float(W1[k]) * (1.0f / 3.0f);
        A00 += w0 * w0;
        A01 += w0 * w1;
        A11 += w1 * w1;
        for (int ch = 0; ch < 3; ++ch) {
            float v = float(s.srgb8[p][ch]);
            B0[ch] += w0 * v;
            B1[ch] += w1 * v;
        }
    }
    float det = A00 * A11 - A01 * A01;
    if (std::abs(det) < 1e-6f) return false;  // singular — keep current endpoints
    float inv_det = 1.0f / det;
    for (int ch = 0; ch < 3; ++ch) {
        float c0_f = (A11 * B0[ch] - A01 * B1[ch]) * inv_det;
        float c1_f = (-A01 * B0[ch] + A00 * B1[ch]) * inv_det;
        e0[ch] = clamp_u8(int(std::lround(c0_f)));
        e1[ch] = clamp_u8(int(std::lround(c1_f)));
    }
    return true;
}

// Quantise 8-bit endpoints to RGB565.
inline RGB565 to_rgb565(const std::uint8_t e[3]) {
    return RGB565{
        std::uint8_t(reduce5(e[0])),
        std::uint8_t(reduce6(e[1])),
        std::uint8_t(reduce5(e[2])),
    };
}

// Selector permutation search — the rgbcx "orderings" trick.
//
// After Lloyd converges on (c0, c1), the per-pixel argmin assignment is
// the local optimum for THAT (c0, c1). But a different selector
// assignment can refit to a different (c0_new, c1_new) that scores
// better overall — Lloyd can't escape into that basin since it only
// considers small endpoint perturbations.
//
// We enumerate all 680 monotonic partitions (0 ≤ a ≤ b ≤ c ≤ 16) of
// the pixels sorted by projection onto the c0→c1 axis. For each:
//   - The 2×2 LSQ matrix for the refit depends only on partition
//     counts (n0, n2, n3, n1) — closed-form O(1).
//   - The per-channel RHS reduces to differences of prefix sums of
//     the sorted pixel values — also O(1) per channel.
//   - Solve, quantise, pick selectors, score. Keep the global best.
//
// This is much wider than Lloyd's basin but still completes in a few
// milliseconds at full image scale.
template<block_compress::BlockMetric M>
inline void selector_perm_search(const Sample16& s,
                                 RGB565& c0_inout, RGB565& c1_inout,
                                 std::uint8_t sel_inout[16],
                                 std::uint8_t dec_inout[16][3],
                                 float& err_inout) {
    std::uint8_t e0[3], e1[3];
    c0_inout.expand8(e0);
    c1_inout.expand8(e1);
    auto lab0 = color_space::srgb8_to_oklab(e0[0], e0[1], e0[2]);
    auto lab1 = color_space::srgb8_to_oklab(e1[0], e1[1], e1[2]);
    float ax = lab1.L - lab0.L;
    float ay = lab1.a - lab0.a;
    float az = lab1.b - lab0.b;
    if (ax * ax + ay * ay + az * az < 1e-9f) return;

    // Project each pixel onto the c0→c1 OKLab axis, sort ascending.
    float proj[16];
    int idx[16];
    for (int p = 0; p < 16; ++p) {
        idx[p] = p;
        proj[p] = (s.lab[p].L - lab0.L) * ax +
                 (s.lab[p].a - lab0.a) * ay +
                 (s.lab[p].b - lab0.b) * az;
    }
    std::sort(idx, idx + 16,
              [&](int a, int b) { return proj[a] < proj[b]; });

    // Prefix sums of sRGB8 values in sorted order — group sums become
    // O(1) per partition.
    int psum[17][3];
    psum[0][0] = psum[0][1] = psum[0][2] = 0;
    for (int i = 0; i < 16; ++i) {
        psum[i + 1][0] = psum[i][0] + int(s.srgb8[idx[i]][0]);
        psum[i + 1][1] = psum[i][1] + int(s.srgb8[idx[i]][1]);
        psum[i + 1][2] = psum[i][2] + int(s.srgb8[idx[i]][2]);
    }

    for (int a = 0; a <= 16; ++a) {
        for (int b = a; b <= 16; ++b) {
            for (int c = b; c <= 16; ++c) {
                int n0 = a, n2 = b - a, n3 = c - b, n1 = 16 - c;
                // 2×2 LSQ matrix (×9 scaling absorbed into the float).
                float A00 = float(n0) + (4.f / 9.f) * float(n2)
                                     + (1.f / 9.f) * float(n3);
                float A11 = float(n1) + (1.f / 9.f) * float(n2)
                                     + (4.f / 9.f) * float(n3);
                float A01 = (2.f / 9.f) * float(n2 + n3);
                float det = A00 * A11 - A01 * A01;
                if (std::abs(det) < 1e-6f) continue;
                float inv_det = 1.f / det;
                std::uint8_t e0n[3], e1n[3];
                for (int ch = 0; ch < 3; ++ch) {
                    int s0 = psum[a][ch];
                    int s2 = psum[b][ch] - psum[a][ch];
                    int s3 = psum[c][ch] - psum[b][ch];
                    int s1 = psum[16][ch] - psum[c][ch];
                    float B0 = float(s0) + (2.f / 3.f) * float(s2)
                                        + (1.f / 3.f) * float(s3);
                    float B1 = float(s1) + (1.f / 3.f) * float(s2)
                                        + (2.f / 3.f) * float(s3);
                    float c0f = (A11 * B0 - A01 * B1) * inv_det;
                    float c1f = (-A01 * B0 + A00 * B1) * inv_det;
                    e0n[ch] = clamp_u8(int(std::lround(c0f)));
                    e1n[ch] = clamp_u8(int(std::lround(c1f)));
                }
                RGB565 tc0 = to_rgb565(e0n);
                RGB565 tc1 = to_rgb565(e1n);
                if (tc0.raw() == c0_inout.raw() &&
                    tc1.raw() == c1_inout.raw()) continue;
                std::uint8_t tsel[16];
                std::uint8_t tdec[16][3];
                float terr = pick_selectors_and_score<M>(s, tc0, tc1, tsel, tdec);
                if (terr < err_inout) {
                    err_inout = terr;
                    c0_inout = tc0;
                    c1_inout = tc1;
                    std::memcpy(sel_inout, tsel, 16);
                    std::memcpy(dec_inout, tdec, sizeof(tdec));
                }
            }
        }
    }
}

// Pack a finished (c0, c1, selectors[16]) into the 8-byte BC1 block.
// mode_3c=false: 4-color block, enforces raw_c0 > raw_c1 (swap+remap if
// needed; perturbs c1 down one quanta when c0 == c1).
// mode_3c=true:  3-color block, enforces raw_c0 ≤ raw_c1 (swap+remap if
// needed; selectors must be in {0, 1, 2} — 0/1 swap on endpoint swap).
inline void pack_block(RGB565 c0, RGB565 c1,
                       const std::uint8_t selectors[16],
                       Block& out,
                       bool mode_3c = false) {
    std::uint16_t raw0 = c0.raw();
    std::uint16_t raw1 = c1.raw();
    std::uint8_t sel[16];
    std::memcpy(sel, selectors, sizeof(sel));
    if (mode_3c) {
        // 3-color mode requires raw_c0 ≤ raw_c1.
        if (raw0 > raw1) {
            std::swap(c0, c1);
            std::swap(raw0, raw1);
            // After swap, paint 0 ↔ 1; paint 2 (midpoint) unchanged.
            for (int i = 0; i < 16; ++i) {
                if (sel[i] < 2) sel[i] = std::uint8_t(sel[i] ^ 0x1u);
            }
        }
        // If raw0 == raw1, the block is genuinely monochrome — 3-color
        // mode with c0=c1 makes all three valid paints the same colour,
        // selector 0/1/2 give identical results. No perturbation needed.
    } else {
        // 4-color mode requires raw_c0 > raw_c1.
        if (raw0 < raw1) {
            std::swap(c0, c1);
            std::swap(raw0, raw1);
            for (int i = 0; i < 16; ++i) sel[i] = std::uint8_t(sel[i] ^ 0x1u);
        }
        if (raw0 == raw1) {
            if (c1.b5 > 0) --c1.b5;
            else if (c1.g6 > 0) --c1.g6;
            else if (c1.r5 > 0) --c1.r5;
            else { ++c0.b5; }
            raw0 = c0.raw();
            raw1 = c1.raw();
        }
    }
    write_u16le(out, 0, raw0);
    write_u16le(out, 2, raw1);
    std::uint32_t s = 0;
    for (int i = 0; i < 16; ++i) {
        s |= std::uint32_t(sel[i] & 0x3u) << (i * 2);
    }
    write_selectors(out, s);
}

// Encode one BC1 block. Pipeline:
//   1. Bounding-box seed (min/max per channel in sRGB) → e0, e1
//   2. Quantise to RGB565
//   3. Pick selectors (1 pass)
//   4. Lloyd refit + re-pick — N iters or until convergence
//   5. Jitter sweep ±k in each RGB565 channel of (c0, c1), keep best
//   6. Pack
template<block_compress::BlockMetric M>
Candidate encode_block(const Sample16& s, const Options& opts) {
    RGB565 c0{}, c1{};
    std::uint8_t sel[16] = {};
    std::uint8_t decoded[16][3] = {};
    float err = std::numeric_limits<float>::infinity();

    // Lloyd refinement.
    auto lloyd_refine = [&](RGB565& cc0, RGB565& cc1,
                            std::uint8_t lsel[16],
                            std::uint8_t ldec[16][3],
                            float& lerr) {
        for (int it = 0; it < 4; ++it) {
            std::uint8_t new_e0[3], new_e1[3];
            if (!refit_endpoints(s, lsel, new_e0, new_e1)) break;
            RGB565 nc0 = to_rgb565(new_e0);
            RGB565 nc1 = to_rgb565(new_e1);
            if (nc0.raw() == cc0.raw() && nc1.raw() == cc1.raw()) break;
            std::uint8_t new_sel[16];
            std::uint8_t new_dec[16][3];
            float new_err = pick_selectors_and_score<M>(s, nc0, nc1, new_sel, new_dec);
            if (new_err >= lerr - 1e-7f) break;
            cc0 = nc0; cc1 = nc1;
            lerr = new_err;
            std::memcpy(lsel, new_sel, sizeof(new_sel));
            std::memcpy(ldec, new_dec, sizeof(new_dec));
        }
    };

    // Seed + initial pick. OKLab PCA is the default — empirically tied
    // with multi-seed (sRGB-PCA / farthest-pair) at significantly higher
    // wall cost, so single-seed wins on the speed-vs-S2 frontier.
    std::uint8_t seed_e0[3], seed_e1[3];
    pca_seed_oklab(s, seed_e0, seed_e1);
    c0 = to_rgb565(seed_e0);
    c1 = to_rgb565(seed_e1);
    err = pick_selectors_and_score<M>(s, c0, c1, sel, decoded);
    if (opts.effort >= 1) lloyd_refine(c0, c1, sel, decoded, err);

    // Jitter sweep around the converged endpoints in RGB565 space.
    // Each candidate gets a full Lloyd refit, so the sweep explores
    // distinct local-optima basins rather than just neighbouring lattice
    // points. Cost: (2J+1)³ × 2 × Lloyd iters per block, but BC1 is fast
    // enough at full image scale that this stays well under budget.
    if (opts.effort >= 2 && opts.jitter > 0) {
        const int J = std::clamp(opts.jitter, 1, 4);
        for (int dr0 = -J; dr0 <= J; ++dr0) {
            int r0 = int(c0.r5) + dr0;
            if (r0 < 0 || r0 > 31) continue;
            for (int dg0 = -J; dg0 <= J; ++dg0) {
                int g0 = int(c0.g6) + dg0;
                if (g0 < 0 || g0 > 63) continue;
                for (int db0 = -J; db0 <= J; ++db0) {
                    int b0 = int(c0.b5) + db0;
                    if (b0 < 0 || b0 > 31) continue;
                    for (int sign = -1; sign <= 1; sign += 2) {
                        int r1 = int(c1.r5) + sign * dr0;
                        int g1 = int(c1.g6) + sign * dg0;
                        int b1 = int(c1.b5) + sign * db0;
                        if (r1 < 0 || r1 > 31 || g1 < 0 || g1 > 63 ||
                            b1 < 0 || b1 > 31) continue;
                        RGB565 tc0{std::uint8_t(r0), std::uint8_t(g0), std::uint8_t(b0)};
                        RGB565 tc1{std::uint8_t(r1), std::uint8_t(g1), std::uint8_t(b1)};
                        std::uint8_t tsel[16];
                        std::uint8_t tdec[16][3];
                        float tot = pick_selectors_and_score<M>(s, tc0, tc1, tsel, tdec);
                        // Lloyd-refine this candidate (typical 1-2 iters
                        // before convergence; usually a no-op if seed
                        // already optimal).
                        lloyd_refine(tc0, tc1, tsel, tdec, tot);
                        if (tot < err) {
                            err = tot;
                            c0 = tc0;
                            c1 = tc1;
                            std::memcpy(sel, tsel, sizeof(sel));
                            std::memcpy(decoded, tdec, sizeof(decoded));
                        }
                    }
                }
            }
        }
    }

    Candidate out;
    // Selector permutation search — exhaustive partition search wins
    // PSNR universally but regresses S2 on pixel-art / limited-palette
    // content (per-block OKLab² optimum diverges from perceptual quality
    // when block-internal structure dominates). Gate behind effort=3.
    if (opts.effort >= 3) {
        selector_perm_search<M>(s, c0, c1, sel, decoded, err);
        lloyd_refine(c0, c1, sel, decoded, err);
    }

    // 3-color mode trial: same (c0, c1) converged from the 4-color
    // search; just swap the paint set to {c0, c1, midpoint, x} and
    // constrain selectors to {0, 1, 2}. Wins on blocks that are
    // essentially two colours + a midpoint band, where the 4-color
    // 1/3-2/3 quartiles overshoot the actual distribution. Cost: one
    // pick_selectors_and_score call per block.
    bool use_3c = false;
    if (opts.effort >= 1) {
        std::uint8_t sel3c[16];
        std::uint8_t dec3c[16][3];
        float err3c = pick_selectors_and_score<M, true>(s, c0, c1, sel3c, dec3c);
        if (err3c < err) {
            err = err3c;
            use_3c = true;
            std::memcpy(sel, sel3c, sizeof(sel));
            std::memcpy(decoded, dec3c, sizeof(decoded));
        }
    }

    pack_block(c0, c1, sel, out.block, use_3c);
    std::memcpy(out.decoded, decoded, sizeof(decoded));
    out.err = err;
    return out;
}

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgb_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    EncodeResult res;
    res.block_cols = (image_w + kBlockW - 1) / kBlockW;
    res.block_rows = (image_h + kBlockH - 1) / kBlockH;
    const auto bcols = static_cast<std::size_t>(res.block_cols);
    res.blocks.assign(bcols * static_cast<std::size_t>(res.block_rows), Block{});

    // Pad source so we don't need per-edge clamps in load_sample.
    std::vector<std::uint8_t> padded;
    int pad_w = res.block_cols * kBlockW;
    int pad_h = res.block_rows * kBlockH;
    const auto pw = static_cast<std::size_t>(pad_w);
    const auto iw = static_cast<std::size_t>(image_w);
    padded.assign(pw * static_cast<std::size_t>(pad_h) * 3u, 0);
    for (int y = 0; y < pad_h; ++y) {
        int sy = std::min(y, image_h - 1);
        for (int x = 0; x < pad_w; ++x) {
            int sx = std::min(x, image_w - 1);
            std::size_t s = static_cast<std::size_t>(sy) * iw + static_cast<std::size_t>(sx);
            std::size_t d = static_cast<std::size_t>(y) * pw + static_cast<std::size_t>(x);
            padded[d * 3u + 0u] = rgb_srgb8[s * 3u + 0u];
            padded[d * 3u + 1u] = rgb_srgb8[s * 3u + 1u];
            padded[d * 3u + 2u] = rgb_srgb8[s * 3u + 2u];
        }
    }

    // Block-grid ED (same mechanism as ETC2 — re-uses the dither kernel
    // catalogue. HARD RULE: don't hand-code FS weights.)
    const bool use_block_ed = options.block_ed.strength > 0.0f;
    auto ed_kernel = dither::error_diffusion_kernel(options.block_ed.method);

    int n_strips = int(std::max(std::thread::hardware_concurrency(), 1u));
    n_strips = std::min(n_strips, res.block_rows);
    if (n_strips < 1) n_strips = 1;
    const int rows_per_strip = (res.block_rows + n_strips - 1) / n_strips;

    std::atomic<float> total_err_atom{0.0f};

    auto run_one = [&](auto metric_tag) {
        [[maybe_unused]] constexpr block_compress::BlockMetric M = decltype(metric_tag)::value;
        pipeline::parallel_for(std::size_t(n_strips), [&](std::size_t strip) {
            const int by_lo = int(strip) * rows_per_strip;
            const int by_hi = std::min(by_lo + rows_per_strip, res.block_rows);
            if (by_lo >= by_hi) return;

            block_compress::BlockGrid<color_space::OKLab> err_carry(
                res.block_cols, by_hi - by_lo);
            for (auto& v : err_carry.as_span()) v = {0.f, 0.f, 0.f};

            Sample16 s{};
            float strip_err = 0.0f;

            for (int by = by_lo; by < by_hi; ++by) {
                const int local_by = by - by_lo;
                bool reverse_row = use_block_ed && options.block_ed.serpentine &&
                                   (local_by & 1);
                int bx_start = reverse_row ? res.block_cols - 1 : 0;
                int bx_end = reverse_row ? -1 : res.block_cols;
                int bx_step = reverse_row ? -1 : 1;
                for (int bx = bx_start; bx != bx_end; bx += bx_step) {
                    color_space::OKLab shift =
                        use_block_ed ? err_carry[bx, local_by]
                                     : color_space::OKLab{0.f, 0.f, 0.f};
                    load_sample(s, padded, pw, bx * kBlockW, by * kBlockH, shift);

                    Candidate c = encode_block<M>(s, options);

                    if (use_block_ed && !ed_kernel.empty()) {
                        float tL = 0.f, ta = 0.f, tb = 0.f;
                        float dL = 0.f, da = 0.f, db = 0.f;
                        for (int i = 0; i < 16; ++i) {
                            tL += s.lab[i].L; ta += s.lab[i].a; tb += s.lab[i].b;
                            auto dec_lab = color_space::srgb8_to_oklab(
                                c.decoded[i][0], c.decoded[i][1], c.decoded[i][2]);
                            dL += dec_lab.L; da += dec_lab.a; db += dec_lab.b;
                        }
                        color_space::OKLab residual{(tL - dL) * (1.f / 16.f),
                                                    (ta - da) * (1.f / 16.f),
                                                    (tb - db) * (1.f / 16.f)};
                        block_compress::propagate_block_residual(
                            err_carry, {bx, local_by}, residual, ed_kernel,
                            options.block_ed, reverse_row);
                    }

                    std::size_t bidx = static_cast<std::size_t>(by) * bcols +
                                       static_cast<std::size_t>(bx);
                    res.blocks[bidx] = c.block;
                    strip_err += c.err;
                }
            }
            float prev = total_err_atom.load(std::memory_order_relaxed);
            while (!total_err_atom.compare_exchange_weak(
                prev, prev + strip_err, std::memory_order_relaxed)) {
            }
        });
    };
    if (options.metric == block_compress::BlockMetric::srgb_mse) {
        run_one(std::integral_constant<block_compress::BlockMetric,
                                       block_compress::BlockMetric::srgb_mse>{});
    } else {
        run_one(std::integral_constant<block_compress::BlockMetric,
                                       block_compress::BlockMetric::oklab2>{});
    }
    res.total_oklab2_error = total_err_atom.load(std::memory_order_relaxed);
    return res;
}

}  // namespace png2amiga::bc1
