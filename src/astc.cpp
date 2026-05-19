// ASTC encoder — 4×4 LDR single-plane.
//
// Per-block layout (mixed):
//   - RGB blocks (alpha=255):    block_mode 0x53, CEM 8,  QUANT_8 weights,
//                                QUANT_256 endpoints, single partition
//   - RGBA blocks (any alpha):   block_mode 0x42, CEM 12, QUANT_4 weights,
//                                QUANT_256 endpoints, single partition
//   - 2-partition RGB:           block_mode 0x42, CEM 8,  QUANT_4 weights,
//                                QUANT_40  endpoints (BISE quint-pack),
//                                partition_index from spec hash.
//                                Selected when 2-partition err < 1-partition.
//
// 2-partition fits 12 endpoint values into 67 color-bits via QUANT_40
// (3 chars per BISE quint-block: 3 data bits + 1 packed quint = 16
// bits; 12 chars = 4 full blocks = 64 bits). The ASTC decoder picks
// the largest quant level that the available color-bits allow, which
// at 67 bits is QUANT_40 — writing any other scheme would be
// reinterpreted as QUANT_40 BISE and decode to garbage.

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

// Two fixed block modes are used per block:
//
//   RGB-only blocks (alpha = 255 everywhere):
//     block_mode = 0x53,   CEM = 8  (LDR RGB direct, 6 endpoint values)
//     weights = QUANT_8 (3-bit, 8 levels, 48 weight bits)
//     endpoints = QUANT_256 (8-bit straight, 6 × 8 = 48 endpoint bits)
//
//   RGBA blocks (any non-trivial alpha):
//     block_mode = 0x42,   CEM = 12 (LDR RGBA direct, 8 endpoint values)
//     weights = QUANT_4 (2-bit, 4 levels, 32 weight bits)
//     endpoints = QUANT_256 (8-bit straight, 8 × 8 = 64 endpoint bits)
//
// Both quant levels have identity scramble maps (power-of-2 ranges so
// no trit/quint BISE encoding required).
constexpr std::uint32_t kBlockModeRgb  = 0x53;   // QUANT_8 weights, 4x4 grid
constexpr std::uint32_t kBlockModeRgba = 0x42;   // QUANT_4 weights, 4x4 grid

constexpr int kWeightLevels8 = 8;
constexpr int kWeightLevels4 = 4;

// Pre-computed weight ramps scaled to the canonical 0..64 range used
// in the ASTC paint formula `((64-w)*e0 + w*e1) / 64`. Per ASTC §16:
//   QUANT_8 = {0, 9, 18, 27, 37, 46, 55, 64}     (table row above kBlockMode)
//   QUANT_4 = {0, 21, 43, 64}
constexpr int kWeightToInterp8[kWeightLevels8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kWeightToInterp4[kWeightLevels4] = {0, 21, 43, 64};

// Legacy alias for phase-1 RGB path.
constexpr int kWeightLevels = kWeightLevels8;
constexpr const int* kWeightToInterp = kWeightToInterp8;

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
                   std::uint8_t decoded[16][4]) {
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
        decoded[p][3] = 255;
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
    write_bits(kBlockModeRgb, 11, 0, pcb);
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

// Pack one 4×4 RGBA block (CEM 12 = LDR RGBA direct):
//   - 11-bit block mode = 0x42 (QUANT_4 weights, 4×4 grid)
//   - 2-bit partition count - 1 = 0
//   - 4-bit CEM = 12 (LDR RGBA direct, 8 endpoint values)
//   - 64-bit endpoint BISE (8 × straight-8-bit, QUANT_256, identity scramble)
//   - 32-bit weight BISE (16 × straight-2-bit, QUANT_4) bit-reversed at top
void pack_block_rgba(const std::uint8_t e0[4], const std::uint8_t e1[4],
                     const std::uint8_t weights[16], Block& out) {
    std::uint8_t pcb[16] = {};

    // Weights: 16 × 2 bits = 32 bits at the top of the block, bit-reversed
    // per byte per ASTC §16.7.
    std::uint8_t weightbuf[16] = {};
    for (int i = 0; i < 16; ++i) {
        write_bits(std::uint32_t(weights[i] & 0x3), 2, i * 2, weightbuf);
    }
    for (int i = 0; i < 16; ++i) {
        pcb[i] = bitrev8(weightbuf[15 - i]);
    }

    write_bits(kBlockModeRgba, 11, 0, pcb);
    write_bits(0, 2, 11, pcb);   // partition_count - 1
    write_bits(12, 4, 13, pcb);  // CEM = LDR RGBA direct

    // Endpoint order for CEM 12 (LDR RGBA direct):
    //   r0, r1, g0, g1, b0, b1, a0, a1
    // Each 8 bits straight (QUANT_256, identity scramble).
    const std::uint8_t ep[8] = {e0[0], e1[0], e0[1], e1[1],
                                e0[2], e1[2], e0[3], e1[3]};
    int off = 17;
    for (int i = 0; i < 8; ++i) {
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
    std::uint8_t decoded[16][4]{};  // RGBA — alpha=255 for CEM 8 outputs
    float err{};
};

// ---------------------------------------------------------------------------
// RGBA path (CEM 12): 4-level weights × 4 channels.
// ---------------------------------------------------------------------------

template <block_compress::BlockMetric M>
float pick_weights_q4_rgba(const Sample16& s,
                           const std::uint8_t e0[4],
                           const std::uint8_t e1[4],
                           std::uint8_t out_w[16],
                           std::uint8_t decoded[16][4]) {
    std::uint8_t paint[kWeightLevels4][4];
    for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
        int w = kWeightToInterp4[w_i];
        int inv = 64 - w;
        for (int ch = 0; ch < 4; ++ch) {
            paint[w_i][ch] =
                std::uint8_t((inv * int(e0[ch]) + w * int(e1[ch]) + 32) >> 6);
        }
    }
    alignas(16) float paint_L[kWeightLevels4], paint_A[kWeightLevels4], paint_B[kWeightLevels4];
    if constexpr (M == block_compress::BlockMetric::oklab2) {
        std::uint8_t rgb4[4][3];
        for (int j = 0; j < 4; ++j) {
            rgb4[j][0] = paint[j][0];
            rgb4[j][1] = paint[j][1];
            rgb4[j][2] = paint[j][2];
        }
        auto labs = color_space::srgb8_to_oklab_batch4(rgb4);
        for (int j = 0; j < 4; ++j) {
            paint_L[j] = labs.labs[j].L;
            paint_A[j] = labs.labs[j].a;
            paint_B[j] = labs.labs[j].b;
        }
    }
    float tot = 0.f;
    for (int p = 0; p < 16; ++p) {
        int best = 0;
        float best_e = std::numeric_limits<float>::infinity();
        int sa = int(s.alpha[p]);
        if constexpr (M == block_compress::BlockMetric::srgb_mse) {
            int sr = int(s.rgba8[p][0]);
            int sg = int(s.rgba8[p][1]);
            int sb = int(s.rgba8[p][2]);
            for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
                int dr = sr - int(paint[w_i][0]);
                int dg = sg - int(paint[w_i][1]);
                int db = sb - int(paint[w_i][2]);
                int da = sa - int(paint[w_i][3]);
                float e = float(dr * dr + dg * dg + db * db + da * da);
                if (e < best_e) { best_e = e; best = w_i; }
            }
        } else {
            float sL = s.lab[p].L, sA = s.lab[p].a, sB = s.lab[p].b;
            for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
                float dL = sL - paint_L[w_i];
                float dA = sA - paint_A[w_i];
                float dB = sB - paint_B[w_i];
                float e = dL * dL + dA * dA + dB * dB;
                float dAlpha = float(sa - int(paint[w_i][3])) * (1.f / 255.f);
                e += dAlpha * dAlpha;
                if (e < best_e) { best_e = e; best = w_i; }
            }
        }
        out_w[p] = std::uint8_t(best);
        decoded[p][0] = paint[best][0];
        decoded[p][1] = paint[best][1];
        decoded[p][2] = paint[best][2];
        decoded[p][3] = paint[best][3];
        tot += best_e;
    }
    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
        tot *= (1.f / 65536.f);
    }
    return tot;
}

// LSQ refit for 4-channel RGBA at QUANT_4 weights. Closed-form 2×2
// normal-equation solve per channel.
bool refit_endpoints_rgba_q4(const Sample16& s,
                             const std::uint8_t w[16],
                             std::uint8_t e0[4],
                             std::uint8_t e1[4]) {
    int n[kWeightLevels4] = {};
    int sum[kWeightLevels4][4] = {};
    for (int p = 0; p < 16; ++p) {
        int k = w[p];
        ++n[k];
        sum[k][0] += s.rgba8[p][0];
        sum[k][1] += s.rgba8[p][1];
        sum[k][2] += s.rgba8[p][2];
        sum[k][3] += int(s.alpha[p]);
    }
    float A00 = 0, A11 = 0, A01 = 0;
    float B[4] = {0, 0, 0, 0}, Bb[4] = {0, 0, 0, 0};
    for (int k = 0; k < kWeightLevels4; ++k) {
        if (n[k] == 0) continue;
        float w1 = float(kWeightToInterp4[k]) * (1.f / 64.f);
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
    float inv = 1.f / det;
    for (int ch = 0; ch < 4; ++ch) {
        float c0 = (A11 * B[ch] - A01 * Bb[ch]) * inv;
        float c1 = (-A01 * B[ch] + A00 * Bb[ch]) * inv;
        e0[ch] = std::uint8_t(std::clamp(int(std::lround(c0)), 0, 255));
        e1[ch] = std::uint8_t(std::clamp(int(std::lround(c1)), 0, 255));
    }
    return true;
}

template <block_compress::BlockMetric M>
Candidate encode_block_rgba(const Sample16& s) {
    Candidate out{};

    // PCA seed in OKLab for RGB; alpha endpoints from min/max.
    std::uint8_t e0[4], e1[4];
    pca_seed_rgb(s, e0, e1);
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; ++i) {
        int a = int(s.alpha[i]);
        if (a < amin) amin = a;
        if (a > amax) amax = a;
    }
    e0[3] = std::uint8_t(amin);
    e1[3] = std::uint8_t(amax);

    std::uint8_t weights[16];
    float err = pick_weights_q4_rgba<M>(s, e0, e1, weights, out.decoded);

    for (int it = 0; it < 4; ++it) {
        std::uint8_t ne0[4], ne1[4];
        if (!refit_endpoints_rgba_q4(s, weights, ne0, ne1)) break;
        std::uint8_t new_w[16];
        std::uint8_t new_dec[16][4];
        float new_err = pick_weights_q4_rgba<M>(s, ne0, ne1, new_w, new_dec);
        if (new_err >= err - 1e-7f) break;
        err = new_err;
        std::memcpy(e0, ne0, 4);
        std::memcpy(e1, ne1, 4);
        std::memcpy(weights, new_w, 16);
        std::memcpy(out.decoded, new_dec, sizeof(new_dec));
    }

    // Blue-contract sum-normalisation (rgba_unpack applies the same swap
    // rule as rgb_unpack — RGB sum only, not including alpha). Complement
    // weights w → 3 - w (QUANT_4 ramp satisfies kWeightToInterp4[s] +
    // kWeightToInterp4[3-s] == 64).
    int s0 = int(e0[0]) + int(e0[1]) + int(e0[2]);
    int s1 = int(e1[0]) + int(e1[1]) + int(e1[2]);
    if (s0 > s1) {
        std::swap(e0[0], e1[0]);
        std::swap(e0[1], e1[1]);
        std::swap(e0[2], e1[2]);
        std::swap(e0[3], e1[3]);
        for (int i = 0; i < 16; ++i) weights[i] = std::uint8_t(3 - int(weights[i]));
    }

    pack_block_rgba(e0, e1, weights, out.block);
    out.err = err;
    return out;
}

template <block_compress::BlockMetric M>
Candidate encode_block_rgb(const Sample16& s) {
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
        std::uint8_t new_dec[16][4];
        float new_err = pick_weights<M>(s, ne0, ne1, new_w, new_dec);
        if (new_err >= err - 1e-7f) break;
        err = new_err;
        std::memcpy(e0, ne0, 3);
        std::memcpy(e1, ne1, 3);
        std::memcpy(weights, new_w, 16);
        std::memcpy(out.decoded, new_dec, sizeof(new_dec));
    }

    // ASTC RGB-direct decoder applies the "blue-contract" inverse when
    // sum(e0_rgb) > sum(e1_rgb): it swaps the endpoint pair AND
    // uncontracts the colors. If we don't pre-contract on the encoder
    // side, that path corrupts the output (inverted blocks). Force the
    // encoded endpoint pair to satisfy sum(e0) ≤ sum(e1) by swapping
    // endpoints + complementing weights (w → 7 - w), which is a
    // decode-preserving identity transform (kWeightToInterp[s] +
    // kWeightToInterp[7-s] == 64).
    int s0 = int(e0[0]) + int(e0[1]) + int(e0[2]);
    int s1 = int(e1[0]) + int(e1[1]) + int(e1[2]);
    if (s0 > s1) {
        std::swap(e0[0], e1[0]);
        std::swap(e0[1], e1[1]);
        std::swap(e0[2], e1[2]);
        for (int i = 0; i < 16; ++i) weights[i] = std::uint8_t(7 - int(weights[i]));
    }

    pack_block(e0, e1, weights, out.block);
    out.err = err;
    return out;
}

// ---------------------------------------------------------------------------
// 2-partition CEM-8 RGB path (QUANT_4 weights, QUANT_32 endpoints).
// ---------------------------------------------------------------------------

// Spec partition hash (ASTC §C.2.21). Ported verbatim from astcenc — pure
// format-spec math, not an encoder quality choice.
std::uint8_t spec_select_partition(int seed, int x, int y, int z,
                                   int partition_count, bool small_block) {
    if (small_block) { x <<= 1; y <<= 1; z <<= 1; }
    seed += (partition_count - 1) * 1024;
    std::uint32_t rnum = std::uint32_t(seed);
    rnum ^= rnum >> 15;
    rnum *= 0xEEDE0891u;
    rnum ^= rnum >> 5;
    rnum += rnum << 16;
    rnum ^= rnum >> 7;
    rnum ^= rnum >> 3;
    rnum ^= rnum << 6;
    rnum ^= rnum >> 17;

    std::uint32_t s1 = rnum & 0xF, s2 = (rnum >> 4) & 0xF,
                  s3 = (rnum >> 8) & 0xF, s4 = (rnum >> 12) & 0xF,
                  s5 = (rnum >> 16) & 0xF, s6 = (rnum >> 20) & 0xF,
                  s7 = (rnum >> 24) & 0xF, s8 = (rnum >> 28) & 0xF,
                  s9 = (rnum >> 18) & 0xF, s10 = (rnum >> 22) & 0xF,
                  s11 = (rnum >> 26) & 0xF,
                  s12 = ((rnum >> 30) | (rnum << 2)) & 0xF;
    s1 *= s1; s2 *= s2; s3 *= s3; s4 *= s4;
    s5 *= s5; s6 *= s6; s7 *= s7; s8 *= s8;
    s9 *= s9; s10 *= s10; s11 *= s11; s12 *= s12;

    int sh1, sh2;
    if (seed & 1) {
        sh1 = (seed & 2) ? 4 : 5;
        sh2 = (partition_count == 3) ? 6 : 5;
    } else {
        sh1 = (partition_count == 3) ? 6 : 5;
        sh2 = (seed & 2) ? 4 : 5;
    }
    int sh3 = (seed & 0x10) ? sh1 : sh2;
    s1 >>= sh1; s2 >>= sh2; s3 >>= sh1; s4 >>= sh2;
    s5 >>= sh1; s6 >>= sh2; s7 >>= sh1; s8 >>= sh2;
    s9 >>= sh3; s10 >>= sh3; s11 >>= sh3; s12 >>= sh3;

    int a = int(s1) * x + int(s2) * y + int(s11) * z + int(rnum >> 14);
    int b = int(s3) * x + int(s4) * y + int(s12) * z + int(rnum >> 10);
    int c = int(s5) * x + int(s6) * y + int(s9)  * z + int(rnum >> 6);
    int d = int(s7) * x + int(s8) * y + int(s10) * z + int(rnum >> 2);
    a &= 0x3F; b &= 0x3F; c &= 0x3F; d &= 0x3F;
    if (partition_count <= 3) d = 0;
    if (partition_count <= 2) c = 0;
    if (partition_count <= 1) b = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

// QUANT_40 unquant table (decoded uint8 levels for 40 stored values).
// Scrambled ordering — encoder must search for nearest.
constexpr std::uint8_t kQuant40Unpack[40] = {
      0, 255,  32, 223,  65, 190,  97, 158,
      6, 249,  39, 216,  71, 184, 104, 151,
     13, 242,  45, 210,  78, 177, 110, 145,
     19, 236,  52, 203,  84, 171, 117, 138,
     26, 229,  58, 197,  91, 164, 123, 132,
};

inline std::uint8_t quant40_pack(std::uint8_t v) {
    int best = 0;
    int best_d = std::abs(int(v) - int(kQuant40Unpack[0]));
    for (int k = 1; k < 40; ++k) {
        int d = std::abs(int(v) - int(kQuant40Unpack[k]));
        if (d < best_d) { best_d = d; best = k; }
    }
    return std::uint8_t(best);
}

// integer_of_quints[i2][i1][i0] — ASTC spec §16.6 quint-pack lookup.
// 125 entries, ported verbatim from astcenc.
constexpr std::uint8_t kQuintOf[5][5][5] = {
    {{0, 1, 2, 3, 4},
     {8, 9, 10, 11, 12},
     {16, 17, 18, 19, 20},
     {24, 25, 26, 27, 28},
     {5, 13, 21, 29, 6}},
    {{32, 33, 34, 35, 36},
     {40, 41, 42, 43, 44},
     {48, 49, 50, 51, 52},
     {56, 57, 58, 59, 60},
     {37, 45, 53, 61, 14}},
    {{64, 65, 66, 67, 68},
     {72, 73, 74, 75, 76},
     {80, 81, 82, 83, 84},
     {88, 89, 90, 91, 92},
     {69, 77, 85, 93, 22}},
    {{96, 97, 98, 99, 100},
     {104, 105, 106, 107, 108},
     {112, 113, 114, 115, 116},
     {120, 121, 122, 123, 124},
     {101, 109, 117, 125, 30}},
    {{102, 103, 70, 71, 38},
     {110, 111, 78, 79, 46},
     {118, 119, 86, 87, 54},
     {126, 127, 94, 95, 62},
     {39, 47, 55, 63, 31}},
};

// BISE encode N values × QUANT_40 (3 bits + 1 quint) into output_data,
// starting at bit_offset. 3 chars per quint block (16 bits). 12 chars
// uses 4 full blocks (64 bits) — no partial tail needed for our 2-part
// CEM-8 case but the partial branch is here for completeness.
void encode_ise_q40(const std::uint8_t* input_data,
                    int character_count,
                    std::uint8_t* output_data,
                    int bit_offset) {
    constexpr int bits = 3;
    constexpr int mask = (1 << bits) - 1;
    int i = 0;
    int full_blocks = character_count / 3;

    for (int j = 0; j < full_blocks; ++j) {
        int i0 = input_data[i + 0] >> bits;
        int i1 = input_data[i + 1] >> bits;
        int i2 = input_data[i + 2] >> bits;
        int T = kQuintOf[i2][i1][i0];

        std::uint32_t pack;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 0) & 0x7) << bits));
        write_bits(pack, bits + 3, bit_offset, output_data); bit_offset += bits + 3; ++i;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 3) & 0x3) << bits));
        write_bits(pack, bits + 2, bit_offset, output_data); bit_offset += bits + 2; ++i;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 5) & 0x3) << bits));
        write_bits(pack, bits + 2, bit_offset, output_data); bit_offset += bits + 2; ++i;
    }
    if (i != character_count) {
        int i2 = 0;
        int i1 = (i + 1 >= character_count) ? 0 : (input_data[i + 1] >> bits);
        int i0 = input_data[i + 0] >> bits;
        int T = kQuintOf[i2][i1][i0];
        static const int tbits[2]  = {3, 2};
        static const int tshift[2] = {0, 3};
        for (int j = 0; i < character_count; ++i, ++j) {
            std::uint32_t pack = std::uint32_t(
                (input_data[i] & mask) |
                (((T >> tshift[j]) & ((1 << tbits[j]) - 1)) << bits));
            write_bits(pack, bits + tbits[j], bit_offset, output_data);
            bit_offset += bits + tbits[j];
        }
    }
}

// Per-block partition assignment cache. partition_assign[pidx][texel]
// stores 0/1 for 2-partition 4×4 blocks (small_block=true, z=0).
struct PartitionTable2P {
    std::uint8_t assign[1024][16];
};
const PartitionTable2P& partition_table_2p_4x4() {
    static const PartitionTable2P table = []{
        PartitionTable2P t{};
        for (int pi = 0; pi < 1024; ++pi) {
            for (int dy = 0; dy < kBlockH; ++dy) {
                for (int dx = 0; dx < kBlockW; ++dx) {
                    t.assign[pi][dy * kBlockW + dx] =
                        spec_select_partition(pi, dx, dy, 0, 2, /*small=*/true);
                }
            }
        }
        return t;
    }();
    return table;
}

// PCA seed over a pixel subset (mask = 1 for "include").
void pca_seed_rgb_subset(const Sample16& s, const std::uint8_t mask[16],
                         std::uint8_t e0[3], std::uint8_t e1[3]) {
    int n = 0;
    float mL = 0, mA = 0, mB = 0;
    for (int i = 0; i < 16; ++i) if (mask[i]) {
        mL += s.lab[i].L; mA += s.lab[i].a; mB += s.lab[i].b; ++n;
    }
    if (n == 0) { for (int c = 0; c < 3; ++c) e0[c] = e1[c] = 0; return; }
    float inv_n = 1.f / float(n);
    mL *= inv_n; mA *= inv_n; mB *= inv_n;
    if (n == 1) {
        for (int i = 0; i < 16; ++i) if (mask[i]) {
            e0[0] = e1[0] = s.rgba8[i][0];
            e0[1] = e1[1] = s.rgba8[i][1];
            e0[2] = e1[2] = s.rgba8[i][2];
            return;
        }
    }
    float cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
    for (int i = 0; i < 16; ++i) if (mask[i]) {
        float dx = s.lab[i].L - mL;
        float dy = s.lab[i].a - mA;
        float dz = s.lab[i].b - mB;
        cxx += dx * dx; cxy += dx * dy; cxz += dx * dz;
        cyy += dy * dy; cyz += dy * dz;
        czz += dz * dz;
    }
    if (cxx + cyy + czz < 1e-7f) {
        for (int ch = 0; ch < 3; ++ch) {
            int lo = 255, hi = 0;
            for (int i = 0; i < 16; ++i) if (mask[i]) {
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
        float invm = 1.f / m;
        vx = nx * invm; vy = ny * invm; vz = nz * invm;
    }
    int imin = -1, imax = -1;
    float pmin = std::numeric_limits<float>::infinity();
    float pmax = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < 16; ++i) if (mask[i]) {
        float t = (s.lab[i].L - mL) * vx + (s.lab[i].a - mA) * vy +
                  (s.lab[i].b - mB) * vz;
        if (t < pmin) { pmin = t; imin = i; }
        if (t > pmax) { pmax = t; imax = i; }
    }
    e0[0] = s.rgba8[imin][0]; e0[1] = s.rgba8[imin][1]; e0[2] = s.rgba8[imin][2];
    e1[0] = s.rgba8[imax][0]; e1[1] = s.rgba8[imax][1]; e1[2] = s.rgba8[imax][2];
}

// Joint weight pick across both partitions: each pixel picks the
// QUANT_4 weight (0..3) minimising err against its own partition's
// paint ramp. Returns total error.
template <block_compress::BlockMetric M>
float pick_weights_2p_q4(const Sample16& s,
                         const std::uint8_t assign[16],
                         const std::uint8_t e0[2][3],
                         const std::uint8_t e1[2][3],
                         std::uint8_t out_w[16],
                         std::uint8_t decoded[16][4]) {
    std::uint8_t paint[2][kWeightLevels4][3];
    for (int k = 0; k < 2; ++k) {
        for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
            int w = kWeightToInterp4[w_i];
            int inv = 64 - w;
            for (int ch = 0; ch < 3; ++ch) {
                paint[k][w_i][ch] =
                    std::uint8_t((inv * int(e0[k][ch]) + w * int(e1[k][ch]) + 32) >> 6);
            }
        }
    }
    alignas(16) float paint_L[2][kWeightLevels4],
                      paint_A[2][kWeightLevels4],
                      paint_B[2][kWeightLevels4];
    if constexpr (M == block_compress::BlockMetric::oklab2) {
        for (int k = 0; k < 2; ++k) {
            std::uint8_t rgb4[4][3];
            for (int j = 0; j < 4; ++j) {
                rgb4[j][0] = paint[k][j][0];
                rgb4[j][1] = paint[k][j][1];
                rgb4[j][2] = paint[k][j][2];
            }
            auto labs = color_space::srgb8_to_oklab_batch4(rgb4);
            for (int j = 0; j < 4; ++j) {
                paint_L[k][j] = labs.labs[j].L;
                paint_A[k][j] = labs.labs[j].a;
                paint_B[k][j] = labs.labs[j].b;
            }
        }
    }
    float tot = 0.f;
    for (int p = 0; p < 16; ++p) {
        int k = assign[p];
        int best = 0;
        float best_e = std::numeric_limits<float>::infinity();
        if constexpr (M == block_compress::BlockMetric::srgb_mse) {
            int sr = int(s.rgba8[p][0]);
            int sg = int(s.rgba8[p][1]);
            int sb = int(s.rgba8[p][2]);
            for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
                int dr = sr - int(paint[k][w_i][0]);
                int dg = sg - int(paint[k][w_i][1]);
                int db = sb - int(paint[k][w_i][2]);
                float e = float(dr * dr + dg * dg + db * db);
                if (e < best_e) { best_e = e; best = w_i; }
            }
        } else {
            float sL = s.lab[p].L, sA = s.lab[p].a, sB = s.lab[p].b;
            for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
                float dL = sL - paint_L[k][w_i];
                float dA = sA - paint_A[k][w_i];
                float dB = sB - paint_B[k][w_i];
                float e = dL * dL + dA * dA + dB * dB;
                if (e < best_e) { best_e = e; best = w_i; }
            }
        }
        out_w[p] = std::uint8_t(best);
        decoded[p][0] = paint[k][best][0];
        decoded[p][1] = paint[k][best][1];
        decoded[p][2] = paint[k][best][2];
        decoded[p][3] = 255;
        tot += best_e;
    }
    if constexpr (M == block_compress::BlockMetric::srgb_mse) {
        tot *= (1.f / 65536.f);
    }
    return tot;
}

// LSQ refit endpoints per partition (single channel solved 3×; one 2x2
// system per partition). Returns false if either partition is singular.
bool refit_endpoints_2p(const Sample16& s, const std::uint8_t assign[16],
                        const std::uint8_t w[16],
                        std::uint8_t e0[2][3], std::uint8_t e1[2][3]) {
    int n[2][kWeightLevels4] = {};
    int sum[2][kWeightLevels4][3] = {};
    for (int p = 0; p < 16; ++p) {
        int k = assign[p], q = w[p];
        ++n[k][q];
        sum[k][q][0] += s.rgba8[p][0];
        sum[k][q][1] += s.rgba8[p][1];
        sum[k][q][2] += s.rgba8[p][2];
    }
    for (int k = 0; k < 2; ++k) {
        float A00 = 0, A11 = 0, A01 = 0;
        float B[3] = {0, 0, 0}, Bb[3] = {0, 0, 0};
        for (int q = 0; q < kWeightLevels4; ++q) {
            if (n[k][q] == 0) continue;
            float w1 = float(kWeightToInterp4[q]) * (1.f / 64.f);
            float w0 = 1.f - w1;
            float nk = float(n[k][q]);
            A00 += nk * w0 * w0;
            A11 += nk * w1 * w1;
            A01 += nk * w0 * w1;
            for (int ch = 0; ch < 3; ++ch) {
                B[ch]  += w0 * float(sum[k][q][ch]);
                Bb[ch] += w1 * float(sum[k][q][ch]);
            }
        }
        float det = A00 * A11 - A01 * A01;
        if (std::abs(det) < 1e-6f) return false;
        float inv = 1.f / det;
        for (int ch = 0; ch < 3; ++ch) {
            float c0 = (A11 * B[ch] - A01 * Bb[ch]) * inv;
            float c1 = (-A01 * B[ch] + A00 * Bb[ch]) * inv;
            e0[k][ch] = std::uint8_t(std::clamp(int(std::lround(c0)), 0, 255));
            e1[k][ch] = std::uint8_t(std::clamp(int(std::lround(c1)), 0, 255));
        }
    }
    return true;
}

// Pack a 2-partition CEM-8 4×4 block:
//   - bit 0..10:  block mode = 0x42  (QUANT_4 weights, 4×4 grid)
//   - bit 11..12: partition_count - 1 = 1
//   - bit 13..22: partition_index (10 bits)
//   - bit 23..28: matched-CEM = (CEM_8 << 2) = 32  (6 bits)
//   - bit 29..84: endpoint BISE, 12 × QUANT_24 (3 bits + 1 trit each),
//                 packed in 2 full blocks (5 chars × 23 bits) + 1 partial
//                 (2 chars × 5 bits) = 56 bits
//   - bit 96..127: weight BISE, 16 × 2-bit QUANT_4, top-down bit-reversed
void pack_block_2p(int partition_index,
                   const std::uint8_t e0[2][3], const std::uint8_t e1[2][3],
                   const std::uint8_t weights[16], Block& out) {
    std::uint8_t pcb[16] = {};

    std::uint8_t weightbuf[16] = {};
    for (int i = 0; i < 16; ++i) {
        write_bits(std::uint32_t(weights[i] & 0x3), 2, i * 2, weightbuf);
    }
    for (int i = 0; i < 16; ++i) pcb[i] = bitrev8(weightbuf[15 - i]);

    write_bits(kBlockModeRgba, 11, 0, pcb);
    write_bits(1, 2, 11, pcb);  // partition_count - 1

    // partition_index (10 bits, low 6 at bit 13, high 4 at bit 19).
    write_bits(std::uint32_t(partition_index) & 0x3Fu, 6, 13, pcb);
    write_bits(std::uint32_t(partition_index) >> 6, 4, 19, pcb);

    // matched-CEM 6 bits at bit 23: (CEM_8 << 2) = 0b100000 = 32.
    write_bits(32, 6, 23, pcb);

    // 12 endpoint values × QUANT_40 BISE at bit 29.
    // Order per partition (CEM_8): r0, r1, g0, g1, b0, b1.
    std::uint8_t ep_packed[12];
    int idx = 0;
    for (int k = 0; k < 2; ++k) {
        ep_packed[idx++] = quant40_pack(e0[k][0]);
        ep_packed[idx++] = quant40_pack(e1[k][0]);
        ep_packed[idx++] = quant40_pack(e0[k][1]);
        ep_packed[idx++] = quant40_pack(e1[k][1]);
        ep_packed[idx++] = quant40_pack(e0[k][2]);
        ep_packed[idx++] = quant40_pack(e1[k][2]);
    }
    encode_ise_q40(ep_packed, 12, pcb, 29);
    (void)idx;

    for (std::size_t i = 0; i < kBlockBytes; ++i) out[i] = pcb[i];
}

// Full 2-partition encoder. Brute-forces all 1024 partition indices
// using a cheap PCA+pick scoring inner loop, then performs a full LSQ
// refit on the winning index.
template <block_compress::BlockMetric M>
Candidate encode_block_rgb_2p(const Sample16& s) {
    Candidate best{};
    best.err = std::numeric_limits<float>::infinity();
    const auto& table = partition_table_2p_4x4();

    std::uint8_t mask_a[16], mask_b[16];
    std::uint8_t e0[2][3], e1[2][3];
    std::uint8_t weights[16];
    std::uint8_t decoded[16][4];

    int best_pi = 0;
    float best_pi_err = std::numeric_limits<float>::infinity();

    for (int pi = 0; pi < 1024; ++pi) {
        const std::uint8_t* assign = table.assign[pi];
        int n0 = 0;
        for (int i = 0; i < 16; ++i) {
            mask_a[i] = (assign[i] == 0) ? 1 : 0;
            mask_b[i] = (assign[i] == 1) ? 1 : 0;
            n0 += mask_a[i];
        }
        // Degenerate partition (one cluster empty in this 4×4 block) — skip.
        if (n0 == 0 || n0 == 16) continue;

        pca_seed_rgb_subset(s, mask_a, e0[0], e1[0]);
        pca_seed_rgb_subset(s, mask_b, e0[1], e1[1]);

        std::uint8_t tw[16];
        std::uint8_t td[16][4];
        float err = pick_weights_2p_q4<M>(s, assign, e0, e1, tw, td);
        if (err < best_pi_err) { best_pi_err = err; best_pi = pi; }
    }

    // Full convergence for the winning partition index.
    const std::uint8_t* assign = table.assign[best_pi];
    for (int i = 0; i < 16; ++i) {
        mask_a[i] = (assign[i] == 0) ? 1 : 0;
        mask_b[i] = (assign[i] == 1) ? 1 : 0;
    }
    pca_seed_rgb_subset(s, mask_a, e0[0], e1[0]);
    pca_seed_rgb_subset(s, mask_b, e0[1], e1[1]);
    float err = pick_weights_2p_q4<M>(s, assign, e0, e1, weights, decoded);
    for (int it = 0; it < 4; ++it) {
        std::uint8_t ne0[2][3], ne1[2][3];
        std::memcpy(ne0, e0, sizeof(ne0));
        std::memcpy(ne1, e1, sizeof(ne1));
        if (!refit_endpoints_2p(s, assign, weights, ne0, ne1)) break;
        std::uint8_t new_w[16];
        std::uint8_t new_dec[16][4];
        float new_err = pick_weights_2p_q4<M>(s, assign, ne0, ne1, new_w, new_dec);
        if (new_err >= err - 1e-7f) break;
        err = new_err;
        std::memcpy(e0, ne0, sizeof(e0));
        std::memcpy(e1, ne1, sizeof(e1));
        std::memcpy(weights, new_w, 16);
        std::memcpy(decoded, new_dec, sizeof(new_dec));
    }

    // Quantize endpoints to QUANT_40 (decoder's actual paint endpoints).
    std::uint8_t e0d[2][3], e1d[2][3];
    for (int k = 0; k < 2; ++k) {
        for (int ch = 0; ch < 3; ++ch) {
            e0d[k][ch] = kQuant40Unpack[quant40_pack(e0[k][ch])];
            e1d[k][ch] = kQuant40Unpack[quant40_pack(e1[k][ch])];
        }
    }

    // Per-partition blue-contract sum-normalisation on the QUANTIZED
    // endpoints — the decoder's swap rule fires on the DECODED sums,
    // not the encoder's pre-quant LSQ output. Swap + complement-weights
    // here so the decoder transform is a no-op.
    for (int k = 0; k < 2; ++k) {
        int sa = int(e0d[k][0]) + int(e0d[k][1]) + int(e0d[k][2]);
        int sb = int(e1d[k][0]) + int(e1d[k][1]) + int(e1d[k][2]);
        if (sa > sb) {
            std::swap(e0d[k][0], e1d[k][0]);
            std::swap(e0d[k][1], e1d[k][1]);
            std::swap(e0d[k][2], e1d[k][2]);
            // Also swap the matching pre-quant values used by pack — pack
            // calls quant24_pack on (e0[k], e1[k]) and the bit-stream
            // ordering must match the post-swap endpoints.
            std::swap(e0[k][0], e1[k][0]);
            std::swap(e0[k][1], e1[k][1]);
            std::swap(e0[k][2], e1[k][2]);
            for (int i = 0; i < 16; ++i) {
                if (assign[i] == k)
                    weights[i] = std::uint8_t(3 - int(weights[i]));
            }
        }
    }

    // Re-pick weights against the decoded paint ramp + the (possibly
    // swapped) endpoints so reported err matches the decoder's output.
    std::uint8_t final_w[16];
    std::uint8_t final_dec[16][4];
    float final_err = pick_weights_2p_q4<M>(s, assign, e0d, e1d, final_w, final_dec);

    pack_block_2p(best_pi, e0d, e1d, final_w, best.block);
    std::memcpy(best.decoded, final_dec, sizeof(final_dec));
    best.err = final_err;
    return best;
}

// Per-block dispatch: blocks whose alpha is constant 255 use the
// QUANT_8-weight RGB-direct path (CEM 8, 3-bit weights for sharper
// gradients). Blocks with any non-trivial alpha use the QUANT_4-weight
// RGBA-direct path (CEM 12). For RGB blocks we also try a 2-partition
// fit and keep whichever has lower error.
template <block_compress::BlockMetric M>
Candidate encode_block(const Sample16& s) {
    bool any_alpha = false;
    for (int i = 0; i < 16; ++i) {
        if (s.alpha[i] != 255) { any_alpha = true; break; }
    }
    if (any_alpha) return encode_block_rgba<M>(s);
    Candidate one = encode_block_rgb<M>(s);
    Candidate two = encode_block_rgb_2p<M>(s);
    return (two.err < one.err) ? two : one;
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
