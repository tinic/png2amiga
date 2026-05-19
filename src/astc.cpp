// ASTC encoder — phase 1: 4×4 single-partition single-plane LDR RGB Direct.
//
// Bit layout we emit (single fixed block-mode):
//
//   bit  0..10  block mode = 0x53
//                 → x_weights = 4, y_weights = 4
//                 → weight quant = QUANT_8 (3-bit weights, 8 levels)
//                 → single plane, low precision range
//   bit 11..12  partition count - 1 = 0  (single partition)
//   bit 13..16  color endpoint mode = 8  (LDR RGB direct)
//   bit 17..64  endpoint BISE: 6 × QUANT_256 (straight 8-bit) = 48 bits
//                 order: r0, r1, g0, g1, b0, b1
//   bit 65..79  unused (15 bits — could pack tighter later)
//   bit 80..127 weight BISE: 16 × QUANT_8 (straight 3-bit) = 48 bits,
//                 stored MSB→LSB through the bytes, then bit-reversed
//                 per byte at byte 0..15 position 15..0 (the ASTC top-
//                 down packing convention; see spec §16.7).
//
// Endpoint search: BC7 Mode 6 shape:
//   1. PCA seed in OKLab → 2 sRGB endpoints
//   2. Pick per-pixel 3-bit weights against the 8-level paint ramp,
//      scored in OKLab²
//   3. LSQ refit endpoints given fixed weights
//   4. Re-pick weights, iterate to convergence
//
// Block-grid Floyd-Steinberg residual diffusion between 4×4 neighbours
// uses the shared block_compress::propagate_block_residual scaffold,
// same as BC7.
//
// astcenc lives under third_party/ for decode (here) + bench (later);
// it is NOT used to encode any block.

#include "astc.hpp"

#include "color_space.hpp"
#include "dither.hpp"
#include "pipeline.hpp"
#include "astcenc.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace png2amiga::astc {

namespace {

constexpr int kBlockW = 4;
constexpr int kBlockH = 4;

// Fixed-block-mode encoding constants.
constexpr std::uint32_t kBlockMode = 0x53;        // see header table
constexpr int kWeightLevels = 8;                  // QUANT_8 (3-bit, straight)

// Pre-computed 3-bit weight ramp scaled to the canonical 0..64 range
// used in the ASTC paint formula `(64-w)*e0 + w*e1`. The ASTC spec
// rounds these to specific values per quant table; for QUANT_8 the
// "scrambled" map is identity (no trit/quint packing), so the simple
// linear mapping below matches the spec exactly.
constexpr int kWeightToInterp[kWeightLevels] = {0, 9, 18, 27, 37, 46, 55, 64};

// Sample16 — same shape as bc7.cpp. Holds source RGBA8 + per-pixel
// OKLab values for fast scoring.
struct Sample16 {
    std::uint8_t rgba8[16][4];
    color_space::OKLab lab[16];
    std::uint8_t alpha[16];
};

void load_sample(Sample16& s,
                 const std::vector<std::uint8_t>& padded,
                 std::size_t pad_w,
                 int bx_px,
                 int by_px,
                 color_space::OKLab shift) {
    for (int dy = 0; dy < kBlockH; ++dy) {
        for (int dx = 0; dx < kBlockW; ++dx) {
            int i = dy * kBlockW + dx;
            std::size_t p = (std::size_t(by_px + dy) * pad_w + std::size_t(bx_px + dx)) * 4u;
            s.rgba8[i][0] = padded[p + 0];
            s.rgba8[i][1] = padded[p + 1];
            s.rgba8[i][2] = padded[p + 2];
            s.rgba8[i][3] = padded[p + 3];
            s.alpha[i] = padded[p + 3];
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

// PCA seed in OKLab — pulls extreme sRGB endpoints along the principal
// axis. Same algorithm as bc7.cpp's pca_seed_rgba but RGB-only.
void pca_seed_rgb(const Sample16& s, std::uint8_t e0[3], std::uint8_t e1[3]) {
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
    if (cxx + cyy + czz < 1e-7f) {
        // Monochrome block — bounding-box seed.
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
        return;
    }
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
    int imin = 0, imax = 0;
    float pmin = std::numeric_limits<float>::infinity();
    float pmax = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < 16; ++i) {
        float t = (s.lab[i].L - mL) * vx + (s.lab[i].a - mA) * vy + (s.lab[i].b - mB) * vz;
        if (t < pmin) { pmin = t; imin = i; }
        if (t > pmax) { pmax = t; imax = i; }
    }
    e0[0] = s.rgba8[imin][0]; e0[1] = s.rgba8[imin][1]; e0[2] = s.rgba8[imin][2];
    e1[0] = s.rgba8[imax][0]; e1[1] = s.rgba8[imax][1]; e1[2] = s.rgba8[imax][2];
}

// Pick the 8-level weight per pixel minimising OKLab² (and return the
// total err). Mirrors pick_selectors_m6 from bc7.cpp.
template <block_compress::BlockMetric M>
float pick_weights(const Sample16& s,
                   const std::uint8_t e0[3],
                   const std::uint8_t e1[3],
                   std::uint8_t out_w[16],
                   std::uint8_t decoded[16][3]) {
    std::uint8_t paint[kWeightLevels][3];
    for (int w_i = 0; w_i < kWeightLevels; ++w_i) {
        int w = kWeightToInterp[w_i];
        int inv = 64 - w;
        for (int ch = 0; ch < 3; ++ch) {
            // ASTC paint formula: (inv*e0 + w*e1 + 32) / 64.
            paint[w_i][ch] =
                std::uint8_t((inv * int(e0[ch]) + w * int(e1[ch]) + 32) >> 6);
        }
    }
    alignas(16) float paint_L[kWeightLevels], paint_A[kWeightLevels], paint_B[kWeightLevels];
    if constexpr (M == block_compress::BlockMetric::oklab2) {
        // 8 paints — 2 batches of 4.
        for (int g = 0; g < kWeightLevels; g += 4) {
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
    float tot = 0.f;
    for (int p = 0; p < 16; ++p) {
        int best = 0;
        float best_e = std::numeric_limits<float>::infinity();
        if constexpr (M == block_compress::BlockMetric::srgb_mse) {
            int sr = int(s.rgba8[p][0]);
            int sg = int(s.rgba8[p][1]);
            int sb = int(s.rgba8[p][2]);
            for (int w_i = 0; w_i < kWeightLevels; ++w_i) {
                int dr = sr - int(paint[w_i][0]);
                int dg = sg - int(paint[w_i][1]);
                int db = sb - int(paint[w_i][2]);
                float e = float(dr * dr + dg * dg + db * db);
                if (e < best_e) { best_e = e; best = w_i; }
            }
        } else {
            float sL = s.lab[p].L, sA = s.lab[p].a, sB = s.lab[p].b;
            for (int w_i = 0; w_i < kWeightLevels; ++w_i) {
                float dL = sL - paint_L[w_i];
                float dA = sA - paint_A[w_i];
                float dB = sB - paint_B[w_i];
                float e = dL * dL + dA * dA + dB * dB;
                if (e < best_e) { best_e = e; best = w_i; }
            }
        }
        out_w[p] = std::uint8_t(best);
        decoded[p][0] = paint[best][0];
        decoded[p][1] = paint[best][1];
        decoded[p][2] = paint[best][2];
        tot += best_e;
    }
    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
        tot *= (1.f / 65536.f);
    }
    return tot;
}

// LSQ refit endpoints given fixed weights (closed-form 2×2 normal-eq
// solve in 8-bit sRGB space). Same shape as BC1/BC7 LSQ.
bool refit_endpoints(const Sample16& s,
                     const std::uint8_t w[16],
                     std::uint8_t e0[3],
                     std::uint8_t e1[3]) {
    int n[kWeightLevels] = {};
    int sum[kWeightLevels][3] = {};
    for (int p = 0; p < 16; ++p) {
        int k = w[p];
        ++n[k];
        sum[k][0] += s.rgba8[p][0];
        sum[k][1] += s.rgba8[p][1];
        sum[k][2] += s.rgba8[p][2];
    }
    float A00 = 0, A11 = 0, A01 = 0;
    float B[3] = {0, 0, 0}, Bb[3] = {0, 0, 0};
    for (int k = 0; k < kWeightLevels; ++k) {
        if (n[k] == 0) continue;
        float w1 = float(kWeightToInterp[k]) * (1.f / 64.f);
        float w0 = 1.f - w1;
        float nk = float(n[k]);
        A00 += nk * w0 * w0;
        A11 += nk * w1 * w1;
        A01 += nk * w0 * w1;
        for (int ch = 0; ch < 3; ++ch) {
            B[ch] += w0 * float(sum[k][ch]);
            Bb[ch] += w1 * float(sum[k][ch]);
        }
    }
    float det = A00 * A11 - A01 * A01;
    if (std::abs(det) < 1e-6f) return false;
    float inv = 1.f / det;
    for (int ch = 0; ch < 3; ++ch) {
        float c0 = (A11 * B[ch] - A01 * Bb[ch]) * inv;
        float c1 = (-A01 * B[ch] + A00 * Bb[ch]) * inv;
        e0[ch] = std::uint8_t(std::clamp(int(std::lround(c0)), 0, 255));
        e1[ch] = std::uint8_t(std::clamp(int(std::lround(c1)), 0, 255));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bit-packing helpers (ASTC physical layout).
// ---------------------------------------------------------------------------

// Write `nbits` of `value` (LSB-first) into `buf` starting at bit
// offset `bit_off`. Matches astcenc's write_bits convention.
void write_bits(std::uint32_t value, int nbits, int bit_off, std::uint8_t buf[16]) {
    while (nbits > 0) {
        int byte_idx = bit_off >> 3;
        int bit_in_byte = bit_off & 7;
        int n_this_byte = std::min(nbits, 8 - bit_in_byte);
        std::uint32_t mask = ((1u << n_this_byte) - 1u) << bit_in_byte;
        buf[byte_idx] = std::uint8_t((buf[byte_idx] & ~mask) |
                                     ((value << bit_in_byte) & mask));
        value >>= n_this_byte;
        nbits -= n_this_byte;
        bit_off += n_this_byte;
    }
}

inline std::uint8_t bitrev8(std::uint8_t v) {
    v = static_cast<std::uint8_t>((v >> 4) | (v << 4));
    v = static_cast<std::uint8_t>(((v >> 2) & 0x33) | ((v << 2) & 0xCC));
    v = static_cast<std::uint8_t>(((v >> 1) & 0x55) | ((v << 1) & 0xAA));
    return v;
}

// Pack one 4×4 block:
//   - 11-bit block mode
//   - 2-bit partition count - 1
//   - 4-bit CEM (LDR RGB direct = 8)
//   - 48-bit endpoint BISE (6 × straight-8-bit, QUANT_256)
//   - 48-bit weight BISE (16 × straight-3-bit, QUANT_8) packed top-down
//     with per-byte bit-reversal at the file's top (bytes 10..15 carry
//     the reversed weight stream).
void pack_block(const std::uint8_t e0[3], const std::uint8_t e1[3],
                const std::uint8_t weights[16], Block& out) {
    std::uint8_t pcb[16] = {};

    // Build the weight buffer (bottom-up): 16 × 3-bit values written
    // LSB-first into a temporary 16-byte buffer, then reversed and
    // bit-reversed into the top of the physical block.
    std::uint8_t weightbuf[16] = {};
    for (int i = 0; i < 16; ++i) {
        write_bits(std::uint32_t(weights[i] & 0x7), 3, i * 3, weightbuf);
    }
    // Per ASTC §16.7: weights are written top-down from byte 15 of the
    // physical block, with each weight-stream byte bit-reversed.
    for (int i = 0; i < 16; ++i) {
        pcb[i] = bitrev8(weightbuf[15 - i]);
    }

    // Now overwrite the low bits with header + endpoints. The endpoint
    // section ends at bit 64; bits 65..79 are unused. The weight area
    // (bits 80..127) was prepopulated above and is not overwritten by
    // these write_bits calls (max offset = 17 + 47 = 64).
    write_bits(kBlockMode, 11, 0, pcb);
    write_bits(0, 2, 11, pcb);  // partition_count - 1
    write_bits(8, 4, 13, pcb);  // CEM = LDR RGB direct

    // Endpoint order for CEM 8 (LDR RGB direct): r0, r1, g0, g1, b0, b1.
    // Each value is 8-bit (QUANT_256 straight encoding; scramble table
    // is identity for power-of-2 quant levels).
    const std::uint8_t ep[6] = {e0[0], e1[0], e0[1], e1[1], e0[2], e1[2]};
    int off = 17;
    for (int i = 0; i < 6; ++i) {
        write_bits(std::uint32_t(ep[i]), 8, off, pcb);
        off += 8;
    }

    for (std::size_t i = 0; i < kBlockBytes; ++i) out[i] = pcb[i];
}

// ---------------------------------------------------------------------------
// Per-block encoder (BC7 Mode 6 shape, hardcoded ASTC block mode).
// ---------------------------------------------------------------------------

struct Candidate {
    Block block{};
    std::uint8_t decoded[16][3]{};
    float err{};
};

template <block_compress::BlockMetric M>
Candidate encode_block(const Sample16& s) {
    Candidate out{};

    // 1. PCA seed.
    std::uint8_t e0[3], e1[3];
    pca_seed_rgb(s, e0, e1);

    // 2. Initial weight pick.
    std::uint8_t weights[16];
    float err = pick_weights<M>(s, e0, e1, weights, out.decoded);

    // 3. LSQ refit + re-pick loop. 4 iters mirrors BC7 Mode 6's convergence.
    for (int it = 0; it < 4; ++it) {
        std::uint8_t ne0[3], ne1[3];
        if (!refit_endpoints(s, weights, ne0, ne1)) break;
        std::uint8_t new_w[16];
        std::uint8_t new_dec[16][3];
        float new_err = pick_weights<M>(s, ne0, ne1, new_w, new_dec);
        if (new_err >= err - 1e-7f) break;
        err = new_err;
        std::memcpy(e0, ne0, 3);
        std::memcpy(e1, ne1, 3);
        std::memcpy(weights, new_w, 16);
        std::memcpy(out.decoded, new_dec, sizeof(new_dec));
    }

    pack_block(e0, e1, weights, out.block);
    out.err = err;
    return out;
}

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    EncodeResult res;
    // Phase 1: 4×4 only — other footprints come later.
    if (options.block_w != 4 || options.block_h != 4) return res;

    res.block_cols = (image_w + kBlockW - 1) / kBlockW;
    res.block_rows = (image_h + kBlockH - 1) / kBlockH;
    const auto bcols = static_cast<std::size_t>(res.block_cols);
    res.blocks.assign(bcols * static_cast<std::size_t>(res.block_rows), Block{});

    // Pad source so load_sample doesn't need per-edge clamps.
    int pad_w = res.block_cols * kBlockW;
    int pad_h = res.block_rows * kBlockH;
    const auto pw = static_cast<std::size_t>(pad_w);
    const auto iw = static_cast<std::size_t>(image_w);
    std::vector<std::uint8_t> padded(pw * static_cast<std::size_t>(pad_h) * 4u, 0);
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

                    Candidate c = encode_block<M>(s);

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

            // Atomic float-add via CAS.
            float prev = total_err_atom.load(std::memory_order_relaxed);
            float next;
            do {
                next = prev + strip_err;
            } while (!total_err_atom.compare_exchange_weak(prev, next,
                                                            std::memory_order_relaxed));
        });
    };

    if (options.metric == block_compress::BlockMetric::srgb_mse) {
        run_one(std::integral_constant<block_compress::BlockMetric,
                                       block_compress::BlockMetric::srgb_mse>{});
    } else {
        run_one(std::integral_constant<block_compress::BlockMetric,
                                       block_compress::BlockMetric::oklab2>{});
    }
    res.total_oklab2_error = total_err_atom.load();
    return res;
}

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w,
                                       int image_h,
                                       int block_w,
                                       int block_h) {
    std::vector<std::uint8_t> out(std::size_t(image_w) * std::size_t(image_h) * 4u);

    astcenc_config cfg{};
    auto err = astcenc_config_init(ASTCENC_PRF_LDR_SRGB,
                                   std::uint32_t(block_w),
                                   std::uint32_t(block_h), 1u,
                                   0.0f, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg);
    if (err != ASTCENC_SUCCESS) return out;
    astcenc_context* ctx = nullptr;
    if (astcenc_context_alloc(&cfg, 1u, &ctx) != ASTCENC_SUCCESS) return out;

    astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    astcenc_image img{};
    img.dim_x = std::uint32_t(image_w);
    img.dim_y = std::uint32_t(image_h);
    img.dim_z = 1u;
    img.data_type = ASTCENC_TYPE_U8;
    void* slices[1] = {out.data()};
    img.data = slices;

    astcenc_decompress_image(ctx,
                             reinterpret_cast<const std::uint8_t*>(blocks.data()),
                             blocks.size() * std::size_t(kBlockBytes),
                             &img, &sw, 0u);
    astcenc_context_free(ctx);
    return out;
}

}  // namespace png2amiga::astc
