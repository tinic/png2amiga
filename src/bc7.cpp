// BC7 RGBA encoder + decoder.
//
// Mode 6 layout (16 bytes, LE bitstream, bits indexed from byte 0 bit 0):
//   bits 0-5   mode (= 1<<6 — i.e. 6 zero bits then a 1)
//   bits 7-13  R0  (7 bits)
//   bits 14-20 R1
//   bits 21-27 G0
//   bits 28-34 G1
//   bits 35-41 B0
//   bits 42-48 B1
//   bits 49-55 A0
//   bits 56-62 A1
//   bit  63    P0  (P-bit for endpoint 0)
//   bit  64    P1  (P-bit for endpoint 1)
//   bits 65-127 selectors — pixel 0 uses 3 bits (anchor; MSB implicit 0),
//                            pixels 1..15 use 4 bits each. Total 63 bits.
//
// Endpoint decoding: 7-bit value × 2 | P-bit → 8-bit endpoint.
// Per-pixel decoded value: ((64-w)·e0 + w·e1 + 32) >> 6 where w is from
// the 4-bit weight table {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47,
// 51, 55, 60, 64}.
//
// Other modes (0..5, 7) are not yet implemented — decoder asserts on
// them.

#include "bc7.hpp"

#include "color_space.hpp"
#include "pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

namespace png2amiga::bc7 {

namespace {

// Weight tables (D3D11.3 spec, BC7 §3.2.3). kWeight2 / kWeight3 wired
// for the not-yet-implemented modes (0..5, 7) — kept here so adding
// those modes is a contained change.
[[maybe_unused]] constexpr int kWeight2[4]  = {0, 21, 43, 64};
[[maybe_unused]] constexpr int kWeight3[8]  = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30,
                              34, 38, 43, 47, 51, 55, 60, 64};

constexpr std::uint8_t clamp_u8(int v) {
    return std::uint8_t(std::clamp(v, 0, 255));
}

// Bit accumulator for 128-bit BC7 blocks. Writes go little-endian into
// `bytes` (the BC7 spec is LSB-first, byte 0 lowest).
struct BitWriter {
    std::array<std::uint8_t, kBlockBytes>& bytes;
    int pos = 0;

    void put(std::uint32_t v, int nbits) noexcept {
        for (int i = 0; i < nbits; ++i) {
            int bit = (v >> i) & 1u;
            if (bit) bytes[std::size_t(pos >> 3)] |= std::uint8_t(1u << (pos & 7));
            ++pos;
        }
    }
};

// Bit reader for the decode path. Reads bits LSB-first.
struct BitReader {
    const std::uint8_t* bytes;
    int pos = 0;
    std::uint32_t get(int nbits) noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < nbits; ++i) {
            int bit = (bytes[pos >> 3] >> (pos & 7)) & 1u;
            v |= std::uint32_t(bit) << i;
            ++pos;
        }
        return v;
    }
};

// Decode 7-bit + P-bit to 8-bit.
constexpr std::uint8_t expand7p(std::uint32_t v7, std::uint32_t p) {
    return std::uint8_t((v7 << 1) | (p & 1u));
}

// Mode 6 decoder.
void decode_mode6(const Block& blk, std::uint8_t out[kBlockPixels * 4]) {
    BitReader br{blk.data(), 7};  // skip mode prefix
    std::uint32_t r0 = br.get(7);
    std::uint32_t r1 = br.get(7);
    std::uint32_t g0 = br.get(7);
    std::uint32_t g1 = br.get(7);
    std::uint32_t b0 = br.get(7);
    std::uint32_t b1 = br.get(7);
    std::uint32_t a0 = br.get(7);
    std::uint32_t a1 = br.get(7);
    std::uint32_t p0 = br.get(1);
    std::uint32_t p1 = br.get(1);
    std::uint8_t e0[4] = {
        expand7p(r0, p0), expand7p(g0, p0), expand7p(b0, p0), expand7p(a0, p0)};
    std::uint8_t e1[4] = {
        expand7p(r1, p1), expand7p(g1, p1), expand7p(b1, p1), expand7p(a1, p1)};
    // Pixel 0 anchor: 3-bit selector. Pixels 1..15: 4-bit selectors.
    for (int i = 0; i < kBlockPixels; ++i) {
        int s = int(br.get(i == 0 ? 3 : 4));
        int w = kWeight4[s];
        int inv = 64 - w;
        for (int ch = 0; ch < 4; ++ch) {
            out[i * 4 + ch] = std::uint8_t((inv * int(e0[ch]) + w * int(e1[ch]) + 32) >> 6);
        }
    }
}

}  // namespace

void decode_block(const Block& blk, std::uint8_t out[kBlockPixels * 4]) {
    // Find the mode prefix: low N bits = 0 then bit N = 1. We only
    // support mode 6 in this initial implementation; other modes fall
    // back to opaque black so the decoder is at least defined.
    std::uint32_t byte0 = blk[0];
    int mode = -1;
    for (int m = 0; m < 8; ++m) {
        if ((byte0 >> m) & 1u) { mode = m; break; }
    }
    if (mode == 6) {
        decode_mode6(blk, out);
        return;
    }
    // Unsupported mode (only Mode 6 emitted by our encoder today;
    // round-trip ctests use bcdec.h to verify across all modes).
    for (int i = 0; i < kBlockPixels; ++i) {
        out[i * 4 + 0] = 0;
        out[i * 4 + 1] = 0;
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
    }
}

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w, int image_h) {
    int bc = (image_w + kBlockW - 1) / kBlockW;
    int br = (image_h + kBlockH - 1) / kBlockH;
    int pad_w = bc * kBlockW;
    int pad_h = br * kBlockH;
    std::vector<std::uint8_t> out(std::size_t(pad_w) * std::size_t(pad_h) * 4u, 0);
    std::uint8_t blk_out[kBlockPixels * 4];
    for (int by = 0; by < br; ++by) {
        for (int bx = 0; bx < bc; ++bx) {
            std::size_t bidx = std::size_t(by) * std::size_t(bc) + std::size_t(bx);
            decode_block(blocks[bidx], blk_out);
            for (int dy = 0; dy < kBlockH; ++dy) {
                for (int dx = 0; dx < kBlockW; ++dx) {
                    int sx = bx * kBlockW + dx;
                    int sy = by * kBlockH + dy;
                    if (sx >= image_w || sy >= image_h) continue;
                    std::size_t d = (std::size_t(sy) * std::size_t(image_w) + std::size_t(sx)) * 4u;
                    int spx = dy * kBlockW + dx;
                    out[d + 0] = blk_out[spx * 4 + 0];
                    out[d + 1] = blk_out[spx * 4 + 1];
                    out[d + 2] = blk_out[spx * 4 + 2];
                    out[d + 3] = blk_out[spx * 4 + 3];
                }
            }
        }
    }
    out.resize(std::size_t(image_w) * std::size_t(image_h) * 4u);
    return out;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

namespace {

struct Sample16 {
    std::uint8_t rgba8[16][4];           // source sRGBA (4 channels)
    color_space::OKLab lab[16];          // source OKLab (alpha-blended-to-black premultiply skipped — alpha treated as separate)
    std::uint8_t alpha[16];              // source alpha (separate channel)
};

void load_sample(Sample16& s,
                 std::span<const std::uint8_t> padded_rgba,
                 std::size_t pad_w,
                 int px,
                 int py,
                 color_space::OKLab shift = {0.f, 0.f, 0.f}) {
    for (int dy = 0; dy < kBlockH; ++dy) {
        for (int dx = 0; dx < kBlockW; ++dx) {
            std::size_t idx = (std::size_t(py + dy) * pad_w + std::size_t(px + dx)) * 4u;
            int i = dy * kBlockW + dx;
            s.rgba8[i][0] = padded_rgba[idx + 0u];
            s.rgba8[i][1] = padded_rgba[idx + 1u];
            s.rgba8[i][2] = padded_rgba[idx + 2u];
            s.rgba8[i][3] = padded_rgba[idx + 3u];
            s.alpha[i] = padded_rgba[idx + 3u];
        }
    }
    for (int g = 0; g < 16; g += 4) {
        std::uint8_t rgb4[4][3];
        for (int j = 0; j < 4; ++j) {
            rgb4[j][0] = s.rgba8[g + j][0];
            rgb4[j][1] = s.rgba8[g + j][1];
            rgb4[j][2] = s.rgba8[g + j][2];
        }
        auto labs = color_space::srgb8_to_oklab_batch4(rgb4);
        for (int j = 0; j < 4; ++j) {
            s.lab[g + j].L = labs.labs[j].L + shift.L;
            s.lab[g + j].a = labs.labs[j].a + shift.a;
            s.lab[g + j].b = labs.labs[j].b + shift.b;
        }
    }
}

// Score a decoded 16-pixel block against a Sample16. Weights alpha
// at the same scale as a colour channel in OKLab² mode (perceptual
// alpha contribution is approximated by 8-bit MSE since the perceptual
// weighting on alpha is content-dependent).
template<block_compress::BlockMetric M>
float score_decoded(const Sample16& s, const std::uint8_t dec[16][4]) {
    float acc = 0.0f;
    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
        int sum = 0;
        for (int i = 0; i < 16; ++i) {
            int dr = int(s.rgba8[i][0]) - int(dec[i][0]);
            int dg = int(s.rgba8[i][1]) - int(dec[i][1]);
            int db = int(s.rgba8[i][2]) - int(dec[i][2]);
            int da = int(s.rgba8[i][3]) - int(dec[i][3]);
            sum += dr * dr + dg * dg + db * db + da * da;
        }
        acc = float(sum) * (1.0f / 65536.0f);
    } else {
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
                float dA = s.lab[g + j].a - d.a;
                float dB = s.lab[g + j].b - d.b;
                acc += color_space::fma_dist_sq(dL, dA, dB);
                // Alpha term as normalized sRGB delta (OKLab is RGB-only).
                float dAlpha = (float(s.alpha[g + j]) - float(dec[g + j][3])) * (1.f / 255.f);
                acc += dAlpha * dAlpha;
            }
        }
    }
    return acc;
}

// PCA seed in OKLab for the 3 colour channels. Returns sRGBA endpoints
// (alpha computed separately from per-pixel min/max).
inline void pca_seed_rgba(const Sample16& s,
                          std::uint8_t e0[4], std::uint8_t e1[4]) {
    float mL = 0, mA = 0, mB = 0;
    for (int i = 0; i < 16; ++i) {
        mL += s.lab[i].L; mA += s.lab[i].a; mB += s.lab[i].b;
    }
    mL *= 1.f / 16.f; mA *= 1.f / 16.f; mB *= 1.f / 16.f;
    float cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
    for (int i = 0; i < 16; ++i) {
        float dx = s.lab[i].L - mL;
        float dy = s.lab[i].a - mA;
        float dz = s.lab[i].b - mB;
        cxx += dx * dx; cxy += dx * dy; cxz += dx * dz;
        cyy += dy * dy; cyz += dy * dz;
        czz += dz * dz;
    }
    int imin_lo = 0, imax_lo = 0;
    if (cxx + cyy + czz < 1e-7f) {
        // Degenerate (monochrome) — use bounding-box per channel.
        for (int ch = 0; ch < 3; ++ch) {
            int lo = 255, hi = 0;
            for (int i = 0; i < 16; ++i) {
                int v = int(s.rgba8[i][ch]);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            e0[ch] = std::uint8_t(lo);
            e1[ch] = std::uint8_t(hi);
        }
    } else {
        float vx = cxx + cxy + cxz;
        float vy = cxy + cyy + cyz;
        float vz = cxz + cyz + czz;
        for (int it = 0; it < 4; ++it) {
            float nx = cxx * vx + cxy * vy + cxz * vz;
            float ny = cxy * vx + cyy * vy + cyz * vz;
            float nz = cxz * vx + cyz * vy + czz * vz;
            float m = std::max({std::abs(nx), std::abs(ny), std::abs(nz)});
            if (m < 1e-9f) break;
            float inv = 1.f / m;
            vx = nx * inv; vy = ny * inv; vz = nz * inv;
        }
        float pmin = std::numeric_limits<float>::infinity();
        float pmax = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < 16; ++i) {
            float t = (s.lab[i].L - mL) * vx +
                      (s.lab[i].a - mA) * vy +
                      (s.lab[i].b - mB) * vz;
            if (t < pmin) { pmin = t; imin_lo = i; }
            if (t > pmax) { pmax = t; imax_lo = i; }
        }
        e0[0] = s.rgba8[imin_lo][0]; e0[1] = s.rgba8[imin_lo][1]; e0[2] = s.rgba8[imin_lo][2];
        e1[0] = s.rgba8[imax_lo][0]; e1[1] = s.rgba8[imax_lo][1]; e1[2] = s.rgba8[imax_lo][2];
    }
    // Alpha: per-pixel min / max (Mode 6 interpolates alpha along the
    // same 16-level ramp as RGB, so the endpoints should track the
    // alpha extremes too).
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; ++i) {
        int a = int(s.alpha[i]);
        if (a < amin) amin = a;
        if (a > amax) amax = a;
    }
    e0[3] = std::uint8_t(amin);
    e1[3] = std::uint8_t(amax);
}

// Mode 6 endpoint quantisation: 8-bit → (7-bit, P-bit) per endpoint,
// per channel. The P-bit is shared across all 4 channels of one
// endpoint, so we pick the P-bit (0 or 1) that minimises the total
// per-channel rounding error for that endpoint.
inline void quantise_endpoint_m6(const std::uint8_t e8[4],
                                 std::uint8_t v7[4],
                                 std::uint32_t& p_bit) {
    // Decode is `(v7 << 1) | p`, so reconstructed 8-bit = 2*v7 + p.
    // For both P=0 and P=1, the best v7 minimising |2*v7 + p - e| is
    // `(e - p) / 2` rounded — which simplifies to `e >> 1` for both
    // parities (drops the LSB). Reconstructed values then differ by p.
    float err0 = 0.f, err1 = 0.f;
    std::uint8_t v7_p0[4], v7_p1[4];
    for (int ch = 0; ch < 4; ++ch) {
        int e = int(e8[ch]);
        int v_p0 = std::clamp(e >> 1, 0, 127);
        int v_p1 = std::clamp(e >> 1, 0, 127);
        v7_p0[ch] = std::uint8_t(v_p0);
        v7_p1[ch] = std::uint8_t(v_p1);
        int rec_p0 = (v_p0 << 1) | 0;
        int rec_p1 = (v_p1 << 1) | 1;
        float d0 = float(rec_p0 - e);
        float d1 = float(rec_p1 - e);
        err0 += d0 * d0;
        err1 += d1 * d1;
    }
    if (err0 <= err1) {
        v7[0] = v7_p0[0]; v7[1] = v7_p0[1]; v7[2] = v7_p0[2]; v7[3] = v7_p0[3];
        p_bit = 0;
    } else {
        v7[0] = v7_p1[0]; v7[1] = v7_p1[1]; v7[2] = v7_p1[2]; v7[3] = v7_p1[3];
        p_bit = 1;
    }
}

// Pick the per-pixel 4-bit selector minimising error against the
// 16-level interpolated ramp between (e0_full, e1_full) in 8-bit. The
// metric matches the block scorer (OKLab² + alpha² for oklab2,
// channel-MSE for srgb_mse) so per-pixel argmin agrees with the
// global block score — picking sRGB-best when OKLab² is the metric
// leaves quality on the table on saturated content.
template<block_compress::BlockMetric M>
inline void pick_selectors_m6(const Sample16& s,
                              const std::uint8_t e0_full[4],
                              const std::uint8_t e1_full[4],
                              std::uint8_t out_sel[16],
                              std::uint8_t decoded[16][4]) {
    std::uint8_t paint[16][4];
    for (int w_i = 0; w_i < 16; ++w_i) {
        int w = kWeight4[w_i];
        int inv = 64 - w;
        for (int ch = 0; ch < 4; ++ch) {
            paint[w_i][ch] =
                std::uint8_t((inv * int(e0_full[ch]) + w * int(e1_full[ch]) + 32) >> 6);
        }
    }
    // Pre-compute paint OKLabs once (4 batches × 4 paints = 16 paints).
    alignas(16) float paint_L[16], paint_A[16], paint_B[16];
    if constexpr (M == block_compress::BlockMetric::oklab2) {
        for (int g = 0; g < 16; g += 4) {
            std::uint8_t rgb4[4][3];
            for (int j = 0; j < 4; ++j) {
                rgb4[j][0] = paint[g + j][0];
                rgb4[j][1] = paint[g + j][1];
                rgb4[j][2] = paint[g + j][2];
            }
            auto labs = color_space::srgb8_to_oklab_batch4(rgb4);
            for (int j = 0; j < 4; ++j) {
                paint_L[g + j] = labs.labs[j].L;
                paint_A[g + j] = labs.labs[j].a;
                paint_B[g + j] = labs.labs[j].b;
            }
        }
    }
    for (int p = 0; p < 16; ++p) {
        int best_w = 0;
        float best_e = std::numeric_limits<float>::infinity();
        if constexpr (M == block_compress::BlockMetric::srgb_mse) {
            int sr = int(s.rgba8[p][0]);
            int sg = int(s.rgba8[p][1]);
            int sb = int(s.rgba8[p][2]);
            int sa = int(s.rgba8[p][3]);
            for (int w_i = 0; w_i < 16; ++w_i) {
                int dr = sr - int(paint[w_i][0]);
                int dg = sg - int(paint[w_i][1]);
                int db = sb - int(paint[w_i][2]);
                int da = sa - int(paint[w_i][3]);
                float e = float(dr * dr + dg * dg + db * db + da * da);
                if (e < best_e) { best_e = e; best_w = w_i; }
            }
        } else {
            float sL = s.lab[p].L, sA = s.lab[p].a, sB = s.lab[p].b;
            int sa = int(s.alpha[p]);
            for (int w_i = 0; w_i < 16; ++w_i) {
                float dL = sL - paint_L[w_i];
                float dA = sA - paint_A[w_i];
                float dB = sB - paint_B[w_i];
                float e = dL * dL + dA * dA + dB * dB;
                // Alpha as normalised sRGB term (matches score_decoded).
                float dAlpha = float(sa - int(paint[w_i][3])) * (1.f / 255.f);
                e += dAlpha * dAlpha;
                if (e < best_e) { best_e = e; best_w = w_i; }
            }
        }
        out_sel[p] = std::uint8_t(best_w);
        decoded[p][0] = paint[best_w][0];
        decoded[p][1] = paint[best_w][1];
        decoded[p][2] = paint[best_w][2];
        decoded[p][3] = paint[best_w][3];
    }
}

// Lloyd refit for Mode 6: given current selectors, solve the 2×2 LSQ
// for (e0, e1) in 8-bit space minimising
//   Σ_p (S[p] - w0(sel[p])·e0 - w1(sel[p])·e1)²
// where w0(s) = (64 - kWeight4[s]) / 64, w1(s) = kWeight4[s] / 64. The
// matrix depends only on selector counts (one row per selector value
// 0..15); the RHS reduces to per-selector per-channel pixel sums.
// Closed-form like BC1's refit_endpoints but with 16-level weights.
// Returns false if the matrix is singular (all selectors identical).
inline bool refit_endpoints_m6(const Sample16& s,
                               const std::uint8_t sel[16],
                               std::uint8_t e0[4],
                               std::uint8_t e1[4]) {
    int n[16] = {};
    int sum[16][4] = {};
    for (int p = 0; p < 16; ++p) {
        int k = sel[p];
        ++n[k];
        sum[k][0] += s.rgba8[p][0];
        sum[k][1] += s.rgba8[p][1];
        sum[k][2] += s.rgba8[p][2];
        sum[k][3] += s.rgba8[p][3];
    }
    float A00 = 0.f, A11 = 0.f, A01 = 0.f;
    float B[4] = {0.f, 0.f, 0.f, 0.f};
    float Bb[4] = {0.f, 0.f, 0.f, 0.f};
    for (int k = 0; k < 16; ++k) {
        if (n[k] == 0) continue;
        float w1 = float(kWeight4[k]) * (1.f / 64.f);
        float w0 = 1.f - w1;
        float nk = float(n[k]);
        A00 += nk * w0 * w0;
        A11 += nk * w1 * w1;
        A01 += nk * w0 * w1;
        for (int ch = 0; ch < 4; ++ch) {
            B[ch] += w0 * float(sum[k][ch]);
            Bb[ch] += w1 * float(sum[k][ch]);
        }
    }
    float det = A00 * A11 - A01 * A01;
    if (std::abs(det) < 1e-6f) return false;
    float inv_det = 1.f / det;
    for (int ch = 0; ch < 4; ++ch) {
        float c0_f = (A11 * B[ch] - A01 * Bb[ch]) * inv_det;
        float c1_f = (-A01 * B[ch] + A00 * Bb[ch]) * inv_det;
        e0[ch] = clamp_u8(int(std::lround(c0_f)));
        e1[ch] = clamp_u8(int(std::lround(c1_f)));
    }
    return true;
}

// Anchor-bit fix: BC7 Mode 6 reserves the MSB of pixel-0's selector
// (must be 0). If the picked selector for pixel 0 has its MSB set
// (selector ≥ 8), we must swap endpoints e0 ↔ e1 and complement all
// selectors (w → 15 - w) so the interpolated values are unchanged.
inline void normalise_anchor_m6(std::uint8_t e0_full[4],
                                std::uint8_t e1_full[4],
                                std::uint8_t sel[16]) {
    if (sel[0] >= 8) {
        for (int ch = 0; ch < 4; ++ch) std::swap(e0_full[ch], e1_full[ch]);
        for (int i = 0; i < 16; ++i) sel[i] = std::uint8_t(15 - sel[i]);
    }
}

// Pack the encoded Mode 6 block into the 16-byte BC7 layout.
inline void pack_mode6(const std::uint8_t v7_e0[4], std::uint32_t p0,
                       const std::uint8_t v7_e1[4], std::uint32_t p1,
                       const std::uint8_t sel[16],
                       Block& out) {
    out.fill(0);
    BitWriter bw{out, 0};
    // Mode prefix: 6 zero bits + 1 one bit = bit position 6 is 1.
    bw.put(0, 6);
    bw.put(1, 1);
    // Endpoints: R0, R1, G0, G1, B0, B1, A0, A1 — each 7 bits.
    bw.put(v7_e0[0], 7); bw.put(v7_e1[0], 7);
    bw.put(v7_e0[1], 7); bw.put(v7_e1[1], 7);
    bw.put(v7_e0[2], 7); bw.put(v7_e1[2], 7);
    bw.put(v7_e0[3], 7); bw.put(v7_e1[3], 7);
    bw.put(p0, 1);
    bw.put(p1, 1);
    // Selectors: pixel 0 = 3 bits, pixels 1..15 = 4 bits.
    bw.put(sel[0] & 0x7u, 3);
    for (int i = 1; i < 16; ++i) bw.put(sel[i] & 0xFu, 4);
}

template<block_compress::BlockMetric M>
Candidate encode_block(const Sample16& s, const Options& /*opts*/) {
    // 1. PCA seed (RGB) + alpha min/max.
    std::uint8_t e0[4], e1[4];
    pca_seed_rgba(s, e0, e1);

    // 2. Quantise to 7-bit + P-bit per endpoint.
    std::uint8_t v7_e0[4], v7_e1[4];
    std::uint32_t p0 = 0, p1 = 0;
    quantise_endpoint_m6(e0, v7_e0, p0);
    quantise_endpoint_m6(e1, v7_e1, p1);
    std::uint8_t e0_full[4] = {
        expand7p(v7_e0[0], p0), expand7p(v7_e0[1], p0),
        expand7p(v7_e0[2], p0), expand7p(v7_e0[3], p0)};
    std::uint8_t e1_full[4] = {
        expand7p(v7_e1[0], p1), expand7p(v7_e1[1], p1),
        expand7p(v7_e1[2], p1), expand7p(v7_e1[3], p1)};

    // 3. Pick per-pixel 4-bit selectors against the 16-level ramp.
    std::uint8_t sel[16];
    std::uint8_t decoded[16][4];
    pick_selectors_m6<M>(s, e0_full, e1_full, sel, decoded);
    float err = score_decoded<M>(s, decoded);

    // 3b. Lloyd refit: re-fit endpoints from current selectors via 2×2
    // LSQ, re-quantise to (v7, P), re-pick selectors. Iterate until
    // err stops dropping (4 iters is plenty in practice).
    for (int it = 0; it < 4; ++it) {
        std::uint8_t new_e0[4], new_e1[4];
        if (!refit_endpoints_m6(s, sel, new_e0, new_e1)) break;
        std::uint8_t new_v7_e0[4], new_v7_e1[4];
        std::uint32_t new_p0 = 0, new_p1 = 0;
        quantise_endpoint_m6(new_e0, new_v7_e0, new_p0);
        quantise_endpoint_m6(new_e1, new_v7_e1, new_p1);
        std::uint8_t new_e0_full[4] = {
            expand7p(new_v7_e0[0], new_p0), expand7p(new_v7_e0[1], new_p0),
            expand7p(new_v7_e0[2], new_p0), expand7p(new_v7_e0[3], new_p0)};
        std::uint8_t new_e1_full[4] = {
            expand7p(new_v7_e1[0], new_p1), expand7p(new_v7_e1[1], new_p1),
            expand7p(new_v7_e1[2], new_p1), expand7p(new_v7_e1[3], new_p1)};
        bool same = true;
        for (int ch = 0; ch < 4; ++ch) {
            if (new_e0_full[ch] != e0_full[ch] || new_e1_full[ch] != e1_full[ch]) {
                same = false; break;
            }
        }
        if (same) break;
        std::uint8_t new_sel[16];
        std::uint8_t new_dec[16][4];
        pick_selectors_m6<M>(s, new_e0_full, new_e1_full, new_sel, new_dec);
        float new_err = score_decoded<M>(s, new_dec);
        if (new_err >= err - 1e-7f) break;
        err = new_err;
        std::memcpy(e0_full, new_e0_full, 4);
        std::memcpy(e1_full, new_e1_full, 4);
        std::memcpy(v7_e0, new_v7_e0, 4);
        std::memcpy(v7_e1, new_v7_e1, 4);
        p0 = new_p0; p1 = new_p1;
        std::memcpy(sel, new_sel, 16);
        std::memcpy(decoded, new_dec, sizeof(new_dec));
    }

    // 4. Normalise anchor bit (pixel 0 MSB must be 0).
    normalise_anchor_m6(e0_full, e1_full, sel);
    // Re-derive (v7, P) from the (possibly swapped) full endpoints so
    // the pack matches the decoded values exactly.
    quantise_endpoint_m6(e0_full, v7_e0, p0);
    quantise_endpoint_m6(e1_full, v7_e1, p1);
    // Re-pick selectors with the re-expanded endpoints in case the
    // round-trip changed them by 1 LSB.
    e0_full[0] = expand7p(v7_e0[0], p0); e0_full[1] = expand7p(v7_e0[1], p0);
    e0_full[2] = expand7p(v7_e0[2], p0); e0_full[3] = expand7p(v7_e0[3], p0);
    e1_full[0] = expand7p(v7_e1[0], p1); e1_full[1] = expand7p(v7_e1[1], p1);
    e1_full[2] = expand7p(v7_e1[2], p1); e1_full[3] = expand7p(v7_e1[3], p1);
    pick_selectors_m6<M>(s, e0_full, e1_full, sel, decoded);
    normalise_anchor_m6(e0_full, e1_full, sel);
    quantise_endpoint_m6(e0_full, v7_e0, p0);
    quantise_endpoint_m6(e1_full, v7_e1, p1);

    Candidate c;
    pack_mode6(v7_e0, p0, v7_e1, p1, sel, c.block);
    std::memcpy(c.decoded, decoded, sizeof(decoded));
    c.err = score_decoded<M>(s, c.decoded);
    return c;
}

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    EncodeResult res;
    res.block_cols = (image_w + kBlockW - 1) / kBlockW;
    res.block_rows = (image_h + kBlockH - 1) / kBlockH;
    const auto bcols = static_cast<std::size_t>(res.block_cols);
    res.blocks.assign(bcols * static_cast<std::size_t>(res.block_rows), Block{});

    // Pad source so load_sample doesn't need per-edge clamps.
    std::vector<std::uint8_t> padded;
    int pad_w = res.block_cols * kBlockW;
    int pad_h = res.block_rows * kBlockH;
    const auto pw = static_cast<std::size_t>(pad_w);
    const auto iw = static_cast<std::size_t>(image_w);
    padded.assign(pw * static_cast<std::size_t>(pad_h) * 4u, 0);
    for (int y = 0; y < pad_h; ++y) {
        int sy = std::min(y, image_h - 1);
        for (int x = 0; x < pad_w; ++x) {
            int sx = std::min(x, image_w - 1);
            std::size_t s = static_cast<std::size_t>(sy) * iw + static_cast<std::size_t>(sx);
            std::size_t d = static_cast<std::size_t>(y) * pw + static_cast<std::size_t>(x);
            padded[d * 4u + 0u] = rgba_srgb8[s * 4u + 0u];
            padded[d * 4u + 1u] = rgba_srgb8[s * 4u + 1u];
            padded[d * 4u + 2u] = rgba_srgb8[s * 4u + 2u];
            padded[d * 4u + 3u] = rgba_srgb8[s * 4u + 3u];
        }
    }

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

}  // namespace png2amiga::bc7
