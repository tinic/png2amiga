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

// Pick the per-pixel selector that minimises block error for the given
// endpoints. Returns total error in the active metric and writes 16
// selector indices into out_sel.
template<block_compress::BlockMetric M>
float pick_selectors_and_score(const Sample16& s,
                               RGB565 c0,
                               RGB565 c1,
                               std::uint8_t out_sel[16],
                               std::uint8_t decoded[16][3]) {
    std::uint8_t paint[4][3];
    compute_paints(c0, c1, paint);

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
        for (int k = 1; k < 4; ++k) {
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

// Bounding-box endpoint seed in 8-bit sRGB space. Cheap to compute; the
// rgbcx-style "best on PCA axis" path is incremental quality on top.
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

// Pack a finished (c0, c1, selectors[16]) into the 8-byte BC1 block.
// Enforces raw_c0 > raw_c1 (4-color mode) — swaps endpoints + remaps
// selectors if needed; perturbs c1 down one quanta when c0 == c1.
inline void pack_block(RGB565 c0, RGB565 c1,
                       const std::uint8_t selectors[16],
                       Block& out) {
    std::uint16_t raw0 = c0.raw();
    std::uint16_t raw1 = c1.raw();
    std::uint8_t sel[16];
    std::memcpy(sel, selectors, sizeof(sel));
    if (raw0 < raw1) {
        std::swap(c0, c1);
        std::swap(raw0, raw1);
        // After swap, paints 0/1 swap and 2/3 swap.
        for (int i = 0; i < 16; ++i) sel[i] = std::uint8_t(sel[i] ^ 0x1u);
    }
    if (raw0 == raw1) {
        // Perturb c1 down by one quanta to stay in 4-color mode.
        if (c1.b5 > 0) --c1.b5;
        else if (c1.g6 > 0) --c1.g6;
        else if (c1.r5 > 0) --c1.r5;
        else { ++c0.b5; }  // c0 is also (0,0,0) — bump it instead
        raw0 = c0.raw();
        raw1 = c1.raw();
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
    std::uint8_t seed_e0[3], seed_e1[3];
    bbox_seed(s, seed_e0, seed_e1);

    RGB565 c0 = to_rgb565(seed_e0);
    RGB565 c1 = to_rgb565(seed_e1);
    std::uint8_t sel[16];
    std::uint8_t decoded[16][3];
    float err = pick_selectors_and_score<M>(s, c0, c1, sel, decoded);

    // Lloyd refinement.
    const int kIters = (opts.effort >= 1) ? 4 : 0;
    for (int it = 0; it < kIters; ++it) {
        std::uint8_t new_e0[3], new_e1[3];
        if (!refit_endpoints(s, sel, new_e0, new_e1)) break;
        RGB565 nc0 = to_rgb565(new_e0);
        RGB565 nc1 = to_rgb565(new_e1);
        if (nc0.raw() == c0.raw() && nc1.raw() == c1.raw()) break;
        std::uint8_t new_sel[16];
        std::uint8_t new_dec[16][3];
        float new_err = pick_selectors_and_score<M>(s, nc0, nc1, new_sel, new_dec);
        if (new_err >= err - 1e-7f) break;
        c0 = nc0; c1 = nc1;
        err = new_err;
        std::memcpy(sel, new_sel, sizeof(sel));
        std::memcpy(decoded, new_dec, sizeof(decoded));
    }

    // Jitter sweep around the converged endpoints in RGB565 space.
    if (opts.effort >= 2 && opts.jitter > 0) {
        const int J = std::clamp(opts.jitter, 1, 4);
        RGB565 best_c0 = c0, best_c1 = c1;
        for (int dr0 = -J; dr0 <= J; ++dr0) {
            int r0 = int(c0.r5) + dr0;
            if (r0 < 0 || r0 > 31) continue;
            for (int dg0 = -J; dg0 <= J; ++dg0) {
                int g0 = int(c0.g6) + dg0;
                if (g0 < 0 || g0 > 63) continue;
                for (int db0 = -J; db0 <= J; ++db0) {
                    int b0 = int(c0.b5) + db0;
                    if (b0 < 0 || b0 > 31) continue;
                    // For c1: only perturb in the SAME direction (axis-aligned
                    // ±J) — full 6D would be O((2J+1)^6) candidates.
                    for (int sign = -1; sign <= 1; sign += 2) {
                        int r1 = int(c1.r5) + sign * dr0;
                        int g1 = int(c1.g6) + sign * dg0;
                        int b1 = int(c1.b5) + sign * db0;
                        if (r1 < 0 || r1 > 31 || g1 < 0 || g1 > 63 || b1 < 0 || b1 > 31) continue;
                        RGB565 tc0{std::uint8_t(r0), std::uint8_t(g0), std::uint8_t(b0)};
                        RGB565 tc1{std::uint8_t(r1), std::uint8_t(g1), std::uint8_t(b1)};
                        std::uint8_t tsel[16];
                        std::uint8_t tdec[16][3];
                        float tot = pick_selectors_and_score<M>(s, tc0, tc1, tsel, tdec);
                        if (tot < err) {
                            err = tot;
                            best_c0 = tc0;
                            best_c1 = tc1;
                            std::memcpy(sel, tsel, sizeof(sel));
                            std::memcpy(decoded, tdec, sizeof(decoded));
                        }
                    }
                }
            }
        }
        c0 = best_c0;
        c1 = best_c1;
    }

    Candidate out;
    pack_block(c0, c1, sel, out.block);
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
        constexpr block_compress::BlockMetric M = decltype(metric_tag)::value;
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
