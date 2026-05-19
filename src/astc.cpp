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
//
// Selection: encoder tries 1p + 2p per RGB block, picks lower OKLab².
//
// The ASTC decoder picks the largest endpoint quant level that fits
// the available color-bits, so the encoder must match that choice
// exactly or the bits decode as garbage. For 2p (12 endpoint values,
// 67 color-bits available), that is QUANT_40 (3 data bits + 1 quint,
// 64 stored bits).

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

// Footprint defaults for the 4x4 case, kept as constants so the
// existing 2-partition / RGBA paths (which are 4x4-specific) read
// the same way they always did.
constexpr int kBlockW = 4;
constexpr int kBlockH = 4;

// Block-mode constants (encode weight grid + weight quant — see ASTC
// §16.5 / astcenc decode_block_mode_2d):
//
//   4x4 RGB  (CEM 8):  0x53  — QUANT_8 weights (3-bit), 16 weights
//   4x4 RGBA (CEM 12): 0x42  — QUANT_4 weights (2-bit), 16 weights
//   5x4 RGB  (CEM 8):  0xD3  — QUANT_8 weights, 20 weights
//
// Power-of-2 endpoint quant levels (QUANT_2/4/8/16/32/64/128/256)
// have identity-monotonic scramble, so no trit/quint BISE needed
// for any of the footprints listed above.
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb4x4  = 0x53;
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgba4x4 = 0x42;
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb5x4  = 0xD3;
// QUANT_4 weight footprints (5x5: 25w×2bpw=50; 6x5: 30w×2bpw=60).
// case 0 of decode_block_mode_2d: x_w = B+4, y_w = A+2; bit 4 = high
// of base_quant; bits 0..1 = low 2 of base_quant (=2 for QUANT_4
// → bits 0..1 = 10, bit 4 = 0).
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb5x5  = 0xE2;
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb6x5  = 0x162;
// QUANT_2 weight footprints (binary, 1bpw). 6x6: 36w×1=36; 8x5: 40w;
// 8x6: 48w. Quality is expected to suffer — block reduces to "snap each
// pixel to one of two endpoints" — but the bit-budget supports
// QUANT_256 endpoints for all three.
//
// Decoded via decode_block_mode_2d:
//   6x6: else branch, case 2, A=0, B=0, (block_mode>>2)&3 = 1 → 0x104
//   8x5: first branch, case 1 (x_w=B+8=8), A=3, B=0          → 0x065
//   8x6: else branch, case 2, A=2 (x_w=8), B=0 (y_w=6)        → 0x144
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb6x6  = 0x104;
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb8x5  = 0x065;
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb8x6  = 0x144;
[[maybe_unused]] constexpr std::uint32_t kBlockModeRgb  = kBlockModeRgb4x4;
constexpr std::uint32_t kBlockModeRgba = kBlockModeRgba4x4;

constexpr int kWeightLevels8 = 8;
constexpr int kWeightLevels4 = 4;
[[maybe_unused]] constexpr int kWeightLevels2 = 2;

// Pre-computed weight ramps scaled to the canonical 0..64 range used
// in the ASTC paint formula `((64-w)*e0 + w*e1) / 64`. Per ASTC §16:
//   QUANT_8 = {0, 9, 18, 27, 37, 46, 55, 64}
//   QUANT_4 = {0, 21, 43, 64}
//   QUANT_2 = {0, 64}                  (binary, used for 6x6+ footprints)
constexpr int kWeightToInterp8[kWeightLevels8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kWeightToInterp4[kWeightLevels4] = {0, 21, 43, 64};
constexpr int kWeightToInterp2[kWeightLevels2] = {0, 64};

// (legacy kWeightLevels / kWeightToInterp aliases removed — superseded
// by weight_ramp<WL>() + the kWeightLevels8 / kWeightLevels4 constants.)

// Per-texel sample buffer parameterised on texel count. Same shape as
// bc7.cpp's Sample16. Sample16 = SampleT<16> is kept as a type alias
// so the 4x4-specific 2-partition + RGBA code paths compile unchanged.
template <int N>
struct SampleT {
    std::uint8_t rgba8[N][4];
    color_space::OKLab lab[N];
    std::uint8_t alpha[N];
};
using Sample16 = SampleT<16>;
using Sample20 = SampleT<20>;

template <int W, int H>
void load_sample(SampleT<W * H>& s,
                 const std::vector<std::uint8_t>& padded,
                 std::size_t pad_w,
                 int bx_px,
                 int by_px,
                 color_space::OKLab shift) {
    constexpr int N = W * H;
    for (int dy = 0; dy < H; ++dy) {
        for (int dx = 0; dx < W; ++dx) {
            int i = dy * W + dx;
            std::size_t p = (std::size_t(by_px + dy) * pad_w + std::size_t(bx_px + dx)) * 4u;
            s.rgba8[i][0] = padded[p + 0];
            s.rgba8[i][1] = padded[p + 1];
            s.rgba8[i][2] = padded[p + 2];
            s.rgba8[i][3] = padded[p + 3];
            s.alpha[i] = padded[p + 3];
        }
    }
    // OKLab batches of 4, then scalar tail for N % 4 != 0.
    int g = 0;
    for (; g + 4 <= N; g += 4) {
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
    for (; g < N; ++g) {
        auto l = color_space::srgb8_to_oklab(s.rgba8[g][0], s.rgba8[g][1], s.rgba8[g][2]);
        s.lab[g].L = l.L + shift.L;
        s.lab[g].a = l.a + shift.a;
        s.lab[g].b = l.b + shift.b;
    }
}

// PCA seed in OKLab — pulls extreme sRGB endpoints along the principal
// axis. Same algorithm as bc7.cpp's pca_seed_rgba but RGB-only.
template <int N>
void pca_seed_rgb(const SampleT<N>& s, std::uint8_t e0[3], std::uint8_t e1[3]) {
    float mL = 0, mA = 0, mB = 0;
    for (int i = 0; i < N; ++i) {
        mL += s.lab[i].L; mA += s.lab[i].a; mB += s.lab[i].b;
    }
    constexpr float inv_n = 1.f / float(N);
    mL *= inv_n; mA *= inv_n; mB *= inv_n;
    float cxx = 0, cxy = 0, cxz = 0, cyy = 0, cyz = 0, czz = 0;
    for (int i = 0; i < N; ++i) {
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
            for (int i = 0; i < N; ++i) {
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
    for (int i = 0; i < N; ++i) {
        float t = (s.lab[i].L - mL) * vx + (s.lab[i].a - mA) * vy + (s.lab[i].b - mB) * vz;
        if (t < pmin) { pmin = t; imin = i; }
        if (t > pmax) { pmax = t; imax = i; }
    }
    e0[0] = s.rgba8[imin][0]; e0[1] = s.rgba8[imin][1]; e0[2] = s.rgba8[imin][2];
    e1[0] = s.rgba8[imax][0]; e1[1] = s.rgba8[imax][1]; e1[2] = s.rgba8[imax][2];
}

// Compile-time weight ramp lookup. WL is the weight-quant level
// (number of stored values: 2, 4, 8).
template <int WL>
constexpr const int* weight_ramp() {
    if constexpr (WL == 8) return kWeightToInterp8;
    else if constexpr (WL == 4) return kWeightToInterp4;
    else if constexpr (WL == 2) return kWeightToInterp2;
    else { static_assert(WL == 8 || WL == 4 || WL == 2, "WL must be 2, 4, or 8"); return nullptr; }
}

template <int WL>
constexpr int weight_mask() { return WL - 1; }  // WL is power of 2

// Pick the weight per pixel minimising OKLab² (and return the total
// err). Mirrors pick_selectors_m6 from bc7.cpp. Templated on WL
// (weight levels) so the same kernel covers QUANT_8 + QUANT_4 weights.
template <block_compress::BlockMetric M, int WL, int N>
float pick_weights(const SampleT<N>& s,
                   const std::uint8_t e0[3],
                   const std::uint8_t e1[3],
                   std::uint8_t out_w[N],
                   std::uint8_t decoded[N][4]) {
    constexpr const int* ramp = weight_ramp<WL>();
    std::uint8_t paint[WL][3];
    for (int w_i = 0; w_i < WL; ++w_i) {
        int w = ramp[w_i];
        int inv = 64 - w;
        for (int ch = 0; ch < 3; ++ch) {
            paint[w_i][ch] =
                std::uint8_t((inv * int(e0[ch]) + w * int(e1[ch]) + 32) >> 6);
        }
    }
    alignas(16) float paint_L[WL], paint_A[WL], paint_B[WL];
    if constexpr (M == block_compress::BlockMetric::oklab2) {
        int g = 0;
        for (; g + 4 <= WL; g += 4) {
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
        for (; g < WL; ++g) {
            auto l = color_space::srgb8_to_oklab(paint[g][0], paint[g][1], paint[g][2]);
            paint_L[g] = l.L; paint_A[g] = l.a; paint_B[g] = l.b;
        }
    }
    float tot = 0.f;
    for (int p = 0; p < N; ++p) {
        int best = 0;
        float best_e = std::numeric_limits<float>::infinity();
        if constexpr (M == block_compress::BlockMetric::srgb_mse) {
            int sr = int(s.rgba8[p][0]);
            int sg = int(s.rgba8[p][1]);
            int sb = int(s.rgba8[p][2]);
            for (int w_i = 0; w_i < WL; ++w_i) {
                int dr = sr - int(paint[w_i][0]);
                int dg = sg - int(paint[w_i][1]);
                int db = sb - int(paint[w_i][2]);
                float e = float(dr * dr + dg * dg + db * db);
                if (e < best_e) { best_e = e; best = w_i; }
            }
        } else {
            float sL = s.lab[p].L, sA = s.lab[p].a, sB = s.lab[p].b;
            for (int w_i = 0; w_i < WL; ++w_i) {
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

// LSQ refit endpoints given fixed weights — 2×2 normal-eq per channel.
template <int WL, int N>
bool refit_endpoints(const SampleT<N>& s,
                     const std::uint8_t w[N],
                     std::uint8_t e0[3],
                     std::uint8_t e1[3]) {
    constexpr const int* ramp = weight_ramp<WL>();
    int n[WL] = {};
    int sum[WL][3] = {};
    for (int p = 0; p < N; ++p) {
        int k = w[p];
        ++n[k];
        sum[k][0] += s.rgba8[p][0];
        sum[k][1] += s.rgba8[p][1];
        sum[k][2] += s.rgba8[p][2];
    }
    float A00 = 0, A11 = 0, A01 = 0;
    float B[3] = {0, 0, 0}, Bb[3] = {0, 0, 0};
    for (int k = 0; k < WL; ++k) {
        if (n[k] == 0) continue;
        float w1 = float(ramp[k]) * (1.f / 64.f);
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

// Pack a single-partition CEM-8 LDR-RGB block. Templated on:
//   - N: weight count (= W * H for 1:1 weight grid up to 8x8 = 64)
//   - BlockMode: 11-bit block_mode that encodes (W, H, weight quant)
//   - WL: weight quant level (8 = QUANT_8 / 3-bit; 4 = QUANT_4 / 2-bit)
// QUANT_256 endpoints (8-bit straight) are used unconditionally — the
// bit budget supports them for any 1:1 footprint where WL × N ≤ 63.
template <int N, std::uint32_t BlockMode, int WL>
void pack_block(const std::uint8_t e0[3], const std::uint8_t e1[3],
                const std::uint8_t weights[N], Block& out) {
    constexpr int BPW = (WL == 8) ? 3 : (WL == 4) ? 2 : 1;
    constexpr std::uint32_t WMask = (1u << BPW) - 1u;
    std::uint8_t pcb[16] = {};

    // Weight buffer: N × BPW-bit values LSB-first, then per-byte bit-
    // reversed and placed top-down per ASTC §16.7.
    std::uint8_t weightbuf[16] = {};
    for (int i = 0; i < N; ++i) {
        write_bits(std::uint32_t(weights[i]) & WMask, BPW, i * BPW, weightbuf);
    }
    for (int i = 0; i < 16; ++i) {
        pcb[i] = bitrev8(weightbuf[15 - i]);
    }

    write_bits(BlockMode, 11, 0, pcb);
    write_bits(0, 2, 11, pcb);  // partition_count - 1
    write_bits(8, 4, 13, pcb);  // CEM = LDR RGB direct

    // CEM-8 endpoint order: r0, r1, g0, g1, b0, b1. Straight-8-bit
    // QUANT_256 (identity scramble for power-of-2 quant levels).
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

// Max supported texel count (8x8 = 64; larger footprints need bilinear
// weight interp and aren't on the 1:1 path).
constexpr int kMaxTexels = 64;

struct Candidate {
    Block block{};
    std::uint8_t decoded[kMaxTexels][4]{};  // RGBA — alpha=255 for CEM-8 outputs
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

// ---------------------------------------------------------------------------
// QUANT_40 (quint-pack) + QUANT_48 (trit-pack) endpoint encoding —
// non-power-of-2 quant levels needed by 2-partition CEM-8 and
// dual-plane CEM-12 paths, respectively. The decoder picks the
// largest quant level that fits the per-block bit budget, so the
// encoder MUST match that level exactly.
//
// QUANT_48 (47-stored-values, 4 data bits + 1 trit per char) is
// picked at color_bits=45 (dual-plane CEM-12 1-partition).
// QUANT_40 (39-stored-values, 3 data bits + 1 quint per char) is
// picked at color_bits=67 (2-partition CEM-8).
// ---------------------------------------------------------------------------

// QUANT_48 unquant table (scrambled stored → uquant uint8).
constexpr std::uint8_t kQuant48Unpack[48] = {
      0, 255,  16, 239,  32, 223,  48, 207,  65, 190,  81, 174,  97, 158, 113, 142,
      5, 250,  21, 234,  38, 217,  54, 201,  70, 185,  86, 169, 103, 152, 119, 136,
     11, 244,  27, 228,  43, 212,  59, 196,  76, 179,  92, 163, 108, 147, 124, 131,
};

inline std::uint8_t quant48_pack(std::uint8_t v) {
    int best = 0;
    int best_d = std::abs(int(v) - int(kQuant48Unpack[0]));
    for (int k = 1; k < 48; ++k) {
        int d = std::abs(int(v) - int(kQuant48Unpack[k]));
        if (d < best_d) { best_d = d; best = k; }
    }
    return std::uint8_t(best);
}

// integer_of_trits[i4][i3][i2][i1][i0] — ASTC §16.6 trit-pack lookup,
// 243 entries. Restored from the phase-3 work for QUANT_48 + future
// trit-encoded paths.
constexpr std::uint8_t kTritOf[3][3][3][3][3] = {
    {
        {{{0, 1, 2},     {4, 5, 6},     {8, 9, 10}},
         {{16, 17, 18},  {20, 21, 22},  {24, 25, 26}},
         {{3, 7, 15},    {19, 23, 27},  {12, 13, 14}}},
        {{{32, 33, 34},  {36, 37, 38},  {40, 41, 42}},
         {{48, 49, 50},  {52, 53, 54},  {56, 57, 58}},
         {{35, 39, 47},  {51, 55, 59},  {44, 45, 46}}},
        {{{64, 65, 66},  {68, 69, 70},  {72, 73, 74}},
         {{80, 81, 82},  {84, 85, 86},  {88, 89, 90}},
         {{67, 71, 79},  {83, 87, 91},  {76, 77, 78}}},
    },
    {
        {{{128, 129, 130}, {132, 133, 134}, {136, 137, 138}},
         {{144, 145, 146}, {148, 149, 150}, {152, 153, 154}},
         {{131, 135, 143}, {147, 151, 155}, {140, 141, 142}}},
        {{{160, 161, 162}, {164, 165, 166}, {168, 169, 170}},
         {{176, 177, 178}, {180, 181, 182}, {184, 185, 186}},
         {{163, 167, 175}, {179, 183, 187}, {172, 173, 174}}},
        {{{192, 193, 194}, {196, 197, 198}, {200, 201, 202}},
         {{208, 209, 210}, {212, 213, 214}, {216, 217, 218}},
         {{195, 199, 207}, {211, 215, 219}, {204, 205, 206}}},
    },
    {
        {{{96, 97, 98},     {100, 101, 102}, {104, 105, 106}},
         {{112, 113, 114},  {116, 117, 118}, {120, 121, 122}},
         {{99, 103, 111},   {115, 119, 123}, {108, 109, 110}}},
        {{{224, 225, 226},  {228, 229, 230}, {232, 233, 234}},
         {{240, 241, 242},  {244, 245, 246}, {248, 249, 250}},
         {{227, 231, 239},  {243, 247, 251}, {236, 237, 238}}},
        {{{28, 29, 30},     {60, 61, 62},    {92, 93, 94}},
         {{156, 157, 158},  {188, 189, 190}, {220, 221, 222}},
         {{31, 63, 127},    {159, 191, 255}, {252, 253, 254}}},
    },
};

// BISE encode N values × QUANT_48 (4 bits + 1 trit). 5 chars per trit
// block (23 bits / block: 5×4 data + 8 trit-pack bits, but stored
// interleaved as 4+2 / 4+2 / 4+1 / 4+2 / 4+1 = 23). For 8 endpoints
// (dual-plane CEM-12): 1 full block (5) + partial (3) = 45 bits.
void encode_ise_q48(const std::uint8_t* input_data,
                    int character_count,
                    std::uint8_t* output_data,
                    int bit_offset) {
    constexpr int bits = 4;
    constexpr int mask = (1 << bits) - 1;
    int i = 0;
    int full_blocks = character_count / 5;

    for (int j = 0; j < full_blocks; ++j) {
        int i0 = input_data[i + 0] >> bits;
        int i1 = input_data[i + 1] >> bits;
        int i2 = input_data[i + 2] >> bits;
        int i3 = input_data[i + 3] >> bits;
        int i4 = input_data[i + 4] >> bits;
        int T = kTritOf[i4][i3][i2][i1][i0];

        std::uint32_t pack;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 0) & 0x3) << bits));
        write_bits(pack, bits + 2, bit_offset, output_data); bit_offset += bits + 2; ++i;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 2) & 0x3) << bits));
        write_bits(pack, bits + 2, bit_offset, output_data); bit_offset += bits + 2; ++i;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 4) & 0x1) << bits));
        write_bits(pack, bits + 1, bit_offset, output_data); bit_offset += bits + 1; ++i;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 5) & 0x3) << bits));
        write_bits(pack, bits + 2, bit_offset, output_data); bit_offset += bits + 2; ++i;
        pack = std::uint32_t((input_data[i] & mask) | (((T >> 7) & 0x1) << bits));
        write_bits(pack, bits + 1, bit_offset, output_data); bit_offset += bits + 1; ++i;
    }
    if (i != character_count) {
        int i4 = 0;
        int i3 = (i + 3 >= character_count) ? 0 : (input_data[i + 3] >> bits);
        int i2 = (i + 2 >= character_count) ? 0 : (input_data[i + 2] >> bits);
        int i1 = (i + 1 >= character_count) ? 0 : (input_data[i + 1] >> bits);
        int i0 = input_data[i + 0] >> bits;
        int T = kTritOf[i4][i3][i2][i1][i0];
        static const int tbits[4]  = {2, 2, 1, 2};
        static const int tshift[4] = {0, 2, 4, 5};
        for (int j = 0; i < character_count; ++i, ++j) {
            std::uint32_t pack = std::uint32_t(
                (input_data[i] & mask) |
                (((T >> tshift[j]) & ((1 << tbits[j]) - 1)) << bits));
            write_bits(pack, bits + tbits[j], bit_offset, output_data);
            bit_offset += bits + tbits[j];
        }
    }
}

// ---------------------------------------------------------------------------
// QUANT_40 endpoint encoding (used by 2-partition CEM-8).
// ---------------------------------------------------------------------------

// QUANT_40 unquant table (decoded uint8 levels for 40 stored values).
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

// integer_of_quints[i2][i1][i0] — ASTC §16.6 quint-pack lookup (ported
// from astcenc). 125 entries.
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

// BISE encode N values × QUANT_40 (3 bits + 1 quint) into output_data
// starting at bit_offset. 3 chars per quint block (16 bits); 8 chars
// (dual-plane RGBA) = 2 full blocks + 2-char partial = 43 bits total.
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

// ---------------------------------------------------------------------------
// Dual-plane RGBA path (4x4, CEM 12 + D=1):
//   - block_mode 0x442 (4x4 QUANT_4 weights + dual-plane bit set)
//   - plane2_component = 3 (alpha)
//   - 16 plane-1 weights drive RGB; 16 plane-2 weights drive alpha
//   - QUANT_40 endpoints (BISE quint, 43 bits for 8 values — same quint
//     pack the 2-partition path uses)
// Decoupling alpha from the RGB ramp helps when alpha is poorly
// correlated with colour (e.g., a particle texture with a soft mask).
// ---------------------------------------------------------------------------

// Pick QUANT_4 weights along a single 1D channel (used for the alpha
// plane). Picks per-pixel w in 0..3 minimising err of the per-pixel
// reconstruction against e0_alpha / e1_alpha.
float pick_weights_q4_alpha(const Sample16& s,
                            std::uint8_t e0_a, std::uint8_t e1_a,
                            std::uint8_t out_w[16],
                            std::uint8_t decoded_a[16]) {
    std::uint8_t paint[kWeightLevels4];
    for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
        int w = kWeightToInterp4[w_i];
        int inv = 64 - w;
        paint[w_i] = std::uint8_t((inv * int(e0_a) + w * int(e1_a) + 32) >> 6);
    }
    float tot = 0.f;
    for (int p = 0; p < 16; ++p) {
        int sa = int(s.alpha[p]);
        int best = 0;
        int best_e = 1 << 30;
        for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
            int d = sa - int(paint[w_i]);
            int e = d * d;
            if (e < best_e) { best_e = e; best = w_i; }
        }
        out_w[p] = std::uint8_t(best);
        decoded_a[p] = paint[best];
        tot += float(best_e) * (1.f / 65536.f);
    }
    return tot;
}

// Pack a dual-plane CEM-12 4x4 block. Block mode 0x442 = 0x42 | (D<<10).
void pack_block_rgba_dual(const std::uint8_t e0[4], const std::uint8_t e1[4],
                          const std::uint8_t weights_rgb[16],
                          const std::uint8_t weights_a[16],
                          Block& out) {
    std::uint8_t pcb[16] = {};

    // Interleaved weight stream: [w_rgb[0], w_a[0], w_rgb[1], w_a[1], ...]
    // 32 values × 2 bits = 64 weight bits, top-down with per-byte bit reverse.
    std::uint8_t weightbuf[16] = {};
    for (int i = 0; i < 16; ++i) {
        write_bits(std::uint32_t(weights_rgb[i] & 0x3), 2, (i * 2) * 2 + 0, weightbuf);
        write_bits(std::uint32_t(weights_a[i]   & 0x3), 2, (i * 2) * 2 + 2, weightbuf);
    }
    for (int i = 0; i < 16; ++i) {
        pcb[i] = bitrev8(weightbuf[15 - i]);
    }

    constexpr std::uint32_t kBlockModeRgbaDual = 0x42 | (1u << 10);  // D=1
    write_bits(kBlockModeRgbaDual, 11, 0, pcb);
    write_bits(0, 2, 11, pcb);                 // partition_count - 1
    write_bits(12, 4, 13, pcb);                // CEM = LDR RGBA direct

    // Plane-2 component selector at (128 - 64 - 2) = bit 62.
    write_bits(3, 2, 62, pcb);                 // 3 = alpha

    // 8 endpoint values × QUANT_40 BISE (43 bits) at bit 17.
    // Order: r0, r1, g0, g1, b0, b1, a0, a1.
    std::uint8_t ep_packed[8];
    ep_packed[0] = quant48_pack(e0[0]); ep_packed[1] = quant48_pack(e1[0]);
    ep_packed[2] = quant48_pack(e0[1]); ep_packed[3] = quant48_pack(e1[1]);
    ep_packed[4] = quant48_pack(e0[2]); ep_packed[5] = quant48_pack(e1[2]);
    ep_packed[6] = quant48_pack(e0[3]); ep_packed[7] = quant48_pack(e1[3]);
    encode_ise_q48(ep_packed, 8, pcb, 17);

    for (std::size_t i = 0; i < kBlockBytes; ++i) out[i] = pcb[i];
}

template <block_compress::BlockMetric M>
Candidate encode_block_rgba_dual(const Sample16& s) {
    Candidate out{};

    // Endpoint seed: PCA on RGB; alpha range min/max.
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

    // Initial weight pick — RGB plane uses 3-channel ramp, alpha plane is 1D.
    std::uint8_t w_rgb[16], w_a[16];
    std::uint8_t dec_rgb[16][4];
    std::uint8_t dec_a[16];
    float err_rgb = pick_weights<M, 4>(s, e0, e1, w_rgb, dec_rgb);
    float err_a   = pick_weights_q4_alpha(s, e0[3], e1[3], w_a, dec_a);

    // LSQ refit endpoints — separate 2x2 systems for RGB (uses w_rgb)
    // and alpha (uses w_a). Reuse refit_endpoints<4> for RGB by ignoring
    // alpha column; alpha solved inline below.
    for (int it = 0; it < 4; ++it) {
        std::uint8_t ne0[3], ne1[3];
        bool ok_rgb = refit_endpoints<4>(s, w_rgb, ne0, ne1);
        std::uint8_t ne0_a = e0[3], ne1_a = e1[3];
        // Alpha LSQ refit (1D variant of refit_endpoints).
        {
            int n[kWeightLevels4] = {};
            int sum[kWeightLevels4] = {};
            for (int p = 0; p < 16; ++p) {
                int k = w_a[p];
                ++n[k];
                sum[k] += int(s.alpha[p]);
            }
            float A00 = 0, A11 = 0, A01 = 0;
            float B = 0, Bb = 0;
            for (int k = 0; k < kWeightLevels4; ++k) {
                if (n[k] == 0) continue;
                float w1 = float(kWeightToInterp4[k]) * (1.f / 64.f);
                float w0 = 1.f - w1;
                float nk = float(n[k]);
                A00 += nk * w0 * w0;
                A11 += nk * w1 * w1;
                A01 += nk * w0 * w1;
                B  += w0 * float(sum[k]);
                Bb += w1 * float(sum[k]);
            }
            float det = A00 * A11 - A01 * A01;
            if (std::abs(det) >= 1e-6f) {
                float inv = 1.f / det;
                float c0 = (A11 * B - A01 * Bb) * inv;
                float c1 = (-A01 * B + A00 * Bb) * inv;
                ne0_a = std::uint8_t(std::clamp(int(std::lround(c0)), 0, 255));
                ne1_a = std::uint8_t(std::clamp(int(std::lround(c1)), 0, 255));
            }
        }
        if (!ok_rgb) break;
        std::uint8_t new_w_rgb[16], new_w_a[16];
        std::uint8_t new_dec_rgb[16][4], new_dec_a[16];
        std::uint8_t ne[4] = {ne0[0], ne0[1], ne0[2], ne0_a};
        std::uint8_t ne_hi[4] = {ne1[0], ne1[1], ne1[2], ne1_a};
        float new_err_rgb = pick_weights<M, 4>(s, ne, ne_hi, new_w_rgb, new_dec_rgb);
        float new_err_a   = pick_weights_q4_alpha(s, ne_hi[3], ne[3] == ne_hi[3] ? ne_hi[3] : ne_hi[3],
                                                  new_w_a, new_dec_a);
        // Use the simpler call without that swap (typo above):
        new_err_a = pick_weights_q4_alpha(s, ne0_a, ne1_a, new_w_a, new_dec_a);
        if (new_err_rgb + new_err_a >= err_rgb + err_a - 1e-7f) break;
        err_rgb = new_err_rgb;
        err_a   = new_err_a;
        std::memcpy(e0, ne,    4);
        std::memcpy(e1, ne_hi, 4);
        std::memcpy(w_rgb, new_w_rgb, 16);
        std::memcpy(w_a,   new_w_a,   16);
        std::memcpy(dec_rgb, new_dec_rgb, sizeof(new_dec_rgb));
        std::memcpy(dec_a,   new_dec_a,   sizeof(new_dec_a));
    }

    // Blue-contract on RGB (alpha is independent so left untouched).
    int sum0 = int(e0[0]) + int(e0[1]) + int(e0[2]);
    int sum1 = int(e1[0]) + int(e1[1]) + int(e1[2]);
    if (sum0 > sum1) {
        std::swap(e0[0], e1[0]);
        std::swap(e0[1], e1[1]);
        std::swap(e0[2], e1[2]);
        for (int i = 0; i < 16; ++i) w_rgb[i] = std::uint8_t(3 - int(w_rgb[i]));
    }

    // Quantize endpoints to QUANT_40 then re-pick weights against the
    // decoded paint ramps so reported err matches reconstruction.
    std::uint8_t e0d[4], e1d[4];
    for (int ch = 0; ch < 4; ++ch) {
        e0d[ch] = kQuant40Unpack[quant40_pack(e0[ch])];
        e1d[ch] = kQuant40Unpack[quant40_pack(e1[ch])];
    }
    std::uint8_t final_w_rgb[16], final_w_a[16];
    std::uint8_t final_dec_rgb[16][4], final_dec_a[16];
    float fr = pick_weights<M, 4>(s, e0d, e1d, final_w_rgb, final_dec_rgb);
    float fa = pick_weights_q4_alpha(s, e0d[3], e1d[3], final_w_a, final_dec_a);

    // Pack final decoded[] = RGB from plane 1, A from plane 2.
    for (int i = 0; i < 16; ++i) {
        out.decoded[i][0] = final_dec_rgb[i][0];
        out.decoded[i][1] = final_dec_rgb[i][1];
        out.decoded[i][2] = final_dec_rgb[i][2];
        out.decoded[i][3] = final_dec_a[i];
    }
    pack_block_rgba_dual(e0d, e1d, final_w_rgb, final_w_a, out.block);
    out.err = fr + fa;
    return out;
}

// Single-partition CEM-8 RGB encoder, templated on (W, H, BlockMode, WL).
// WL = weight-quant levels (8 or 4). QUANT_256 endpoints unconditional.
template <int W, int H, std::uint32_t BlockMode, int WL, block_compress::BlockMetric M>
Candidate encode_block_rgb(const SampleT<W * H>& s) {
    constexpr int N = W * H;
    Candidate out{};

    std::uint8_t e0[3], e1[3];
    pca_seed_rgb(s, e0, e1);

    std::uint8_t weights[N];
    float err = pick_weights<M, WL>(s, e0, e1, weights, out.decoded);

    // 4-iter LSQ-refit + re-pick loop mirrors BC7 Mode 6.
    for (int it = 0; it < 4; ++it) {
        std::uint8_t ne0[3], ne1[3];
        if (!refit_endpoints<WL>(s, weights, ne0, ne1)) break;
        std::uint8_t new_w[N];
        std::uint8_t new_dec[N][4];
        float new_err = pick_weights<M, WL>(s, ne0, ne1, new_w, new_dec);
        if (new_err >= err - 1e-7f) break;
        err = new_err;
        std::memcpy(e0, ne0, 3);
        std::memcpy(e1, ne1, 3);
        std::memcpy(weights, new_w, N);
        std::memcpy(out.decoded, new_dec, sizeof(new_dec));
    }

    // Blue-contract sum normalisation. Decoder swaps endpoint pair +
    // uncontracts when sum(e0_rgb) > sum(e1_rgb), so pre-swap +
    // complement weights to make that path a no-op. w → (WL-1) - w
    // is a decode-preserving identity since kWeightToInterp[s] +
    // kWeightToInterp[(WL-1)-s] == 64 for both ramps.
    int s0 = int(e0[0]) + int(e0[1]) + int(e0[2]);
    int s1 = int(e1[0]) + int(e1[1]) + int(e1[2]);
    if (s0 > s1) {
        std::swap(e0[0], e1[0]);
        std::swap(e0[1], e1[1]);
        std::swap(e0[2], e1[2]);
        for (int i = 0; i < N; ++i) weights[i] = std::uint8_t((WL - 1) - int(weights[i]));
    }

    pack_block<N, BlockMode, WL>(e0, e1, weights, out.block);
    out.err = err;
    return out;
}

// ---------------------------------------------------------------------------
// Bilinear-decimation path — texel grid bigger than weight grid.
// ASTC §16.3 maps each texel (tx, ty) → fractional position in weight
// space; the per-texel weight is the bilinear blend of 4 corner grid
// weights. The encoder reverses this: pick grid weights such that the
// blended per-texel weight tracks the ideal continuous weight (projection
// of each texel onto the e0..e1 axis).
//
// First-cut heuristic (no LSQ on the sparse bilinear system yet):
//   1. PCA on the full texel set → e0, e1
//   2. For each texel: ideal w_j = (texel - e0) · (e1 - e0) / |e1 - e0|^2
//   3. Each grid weight = bilinear-coefficient-weighted average of the
//      texel ideal weights that include it
//   4. Quantize each grid weight to WL levels
//   5. Bilinear-infill decoded[] back to full texel resolution so the
//      block-grid ED residual loop has real reconstructed data
// ---------------------------------------------------------------------------

// Compile-time bilinear contribution map: each texel records the 4
// grid neighbours that contribute to it plus a 4-bit (0..16, sum=16)
// truncated-precision bilinear coefficient per neighbour. The math
// matches astcenc_block_sizes.cpp::assign_kmeans_texels exactly so
// the texel weights the encoder produces match what the decoder
// reconstructs bit-for-bit.
template <int TexW, int TexH, int GridW, int GridH>
struct BilinearMap {
    struct TexelContrib {
        std::uint8_t grid[4];   // qweight indices into the GridW × GridH array
        std::uint8_t weight[4]; // 0..16 each; sum = 16
    };
    TexelContrib texels[TexW * TexH];

    constexpr BilinearMap() : texels{} {
        constexpr int sx = (TexW > 1) ? ((1024 + TexW / 2) / (TexW - 1)) : 0;
        constexpr int sy = (TexH > 1) ? ((1024 + TexH / 2) / (TexH - 1)) : 0;
        for (int y = 0; y < TexH; ++y) {
            for (int x = 0; x < TexW; ++x) {
                int xw = (sx * x * (GridW - 1) + 32) >> 6;
                int yw = (sy * y * (GridH - 1) + 32) >> 6;
                int xfrac = xw & 0xF;
                int yfrac = yw & 0xF;
                int xint  = xw >> 4;
                int yint  = yw >> 4;

                int q0 = yint * GridW + xint;
                int q1 = q0 + 1;
                int q2 = q0 + GridW;
                int q3 = q2 + 1;
                // Right-/bottom-edge texels have xfrac/yfrac == 0, so
                // their contributions to q1/q2/q3 are zero — but we
                // still need a valid index. Clamp out-of-grid.
                if (xint >= GridW - 1) { q1 = q0; q3 = q2; }
                if (yint >= GridH - 1) { q2 = q0; q3 = q1; }

                int prod = xfrac * yfrac;
                int w3 = (prod + 8) >> 4;
                int w1 = xfrac - w3;
                int w2 = yfrac - w3;
                int w0 = 16 - xfrac - yfrac + w3;

                auto& t = texels[y * TexW + x];
                t.grid[0]   = std::uint8_t(q0);
                t.grid[1]   = std::uint8_t(q1);
                t.grid[2]   = std::uint8_t(q2);
                t.grid[3]   = std::uint8_t(q3);
                t.weight[0] = std::uint8_t(w0);
                t.weight[1] = std::uint8_t(w1);
                t.weight[2] = std::uint8_t(w2);
                t.weight[3] = std::uint8_t(w3);
            }
        }
    }
};

template <int TexW, int TexH, int GridW, int GridH,
          std::uint32_t BlockMode, int WL, block_compress::BlockMetric M>
Candidate encode_block_rgb_decim(const SampleT<TexW * TexH>& s) {
    constexpr int TN = TexW * TexH;
    constexpr int GN = GridW * GridH;
    static constexpr BilinearMap<TexW, TexH, GridW, GridH> bm{};
    constexpr const int* ramp = weight_ramp<WL>();

    Candidate out{};

    // 1. PCA on full texel set.
    std::uint8_t e0[3], e1[3];
    pca_seed_rgb(s, e0, e1);

    // 2-4. Alternating LSQ:
    //   a. Compute ideal continuous weight per texel given current e0/e1
    //   b. LSQ grid weights against bilinear infill (Gauss-Jordan on GN×GN)
    //   c. Refit endpoints given the quantized grid weights
    // Three iterations is enough for natural images (most movement happens
    // in pass 1; passes 2-3 polish endpoints once grid weights stabilise).
    std::uint8_t grid_weights[GN] = {};
    for (int iter = 0; iter < 3; ++iter) {
        // 2. Ideal continuous weight per texel = projection onto e0..e1 axis.
        float dr = float(e1[0]) - float(e0[0]);
        float dg = float(e1[1]) - float(e0[1]);
        float db = float(e1[2]) - float(e0[2]);
        float ramp_sq = dr * dr + dg * dg + db * db;
        float inv_ramp_sq = (ramp_sq > 1e-9f) ? (1.f / ramp_sq) : 0.f;
        float w_ideal[TN];
        for (int j = 0; j < TN; ++j) {
            float pr = float(s.rgba8[j][0]) - float(e0[0]);
            float pg = float(s.rgba8[j][1]) - float(e0[1]);
            float pb = float(s.rgba8[j][2]) - float(e0[2]);
            float w = (pr * dr + pg * dg + pb * db) * inv_ramp_sq;
            w_ideal[j] = std::clamp(w, 0.f, 1.f);
        }

        // 3. LSQ on the sparse bilinear contribution system.
        //   paint_j = sum_i (c[i][j] / 16) * v_i,  v_i in [0..64].
        //   Closed-form via normal equations A^T A v = A^T T, solved by
        //   Gauss-Jordan (GN ≤ 36, ~50k flops/iter — negligible).
        //   Small Tikhonov ridge regularises uncovered grid points.
        float ata[GN][GN] = {};
        float atb[GN] = {};
        for (int j = 0; j < TN; ++j) {
            const auto& t = bm.texels[j];
            float T_j = w_ideal[j] * 64.f;
            for (int k = 0; k < 4; ++k) {
                float ck = float(t.weight[k]) * (1.f / 16.f);
                if (ck <= 0.f) continue;
                int gi = t.grid[k];
                atb[gi] += ck * T_j;
                for (int kk = 0; kk < 4; ++kk) {
                    float cc = float(t.weight[kk]) * (1.f / 16.f);
                    if (cc <= 0.f) continue;
                    ata[gi][t.grid[kk]] += ck * cc;
                }
            }
        }
        constexpr float kRidge = 1e-3f;
        for (int i = 0; i < GN; ++i) ata[i][i] += kRidge;

        for (int i = 0; i < GN; ++i) {
            int piv = i;
            float bestmag = std::abs(ata[i][i]);
            for (int r = i + 1; r < GN; ++r) {
                float m = std::abs(ata[r][i]);
                if (m > bestmag) { bestmag = m; piv = r; }
            }
            if (bestmag < 1e-9f) continue;
            if (piv != i) {
                for (int c = 0; c < GN; ++c) std::swap(ata[i][c], ata[piv][c]);
                std::swap(atb[i], atb[piv]);
            }
            float pinv = 1.f / ata[i][i];
            for (int c = 0; c < GN; ++c) ata[i][c] *= pinv;
            atb[i] *= pinv;
            for (int r = 0; r < GN; ++r) {
                if (r == i) continue;
                float f = ata[r][i];
                if (f == 0.f) continue;
                for (int c = 0; c < GN; ++c) ata[r][c] -= f * ata[i][c];
                atb[r] -= f * atb[i];
            }
        }

        // Quantize each continuous v_i to nearest ramp[WL] level.
        for (int i = 0; i < GN; ++i) {
            float v = std::clamp(atb[i], 0.f, 64.f);
            int best = 0;
            int best_d = std::abs(int(v + 0.5f) - ramp[0]);
            for (int k = 1; k < WL; ++k) {
                int d = std::abs(int(v + 0.5f) - ramp[k]);
                if (d < best_d) { best_d = d; best = k; }
            }
            grid_weights[i] = std::uint8_t(best);
        }

        // 4. LSQ refit endpoints given the quantized grid weights.
        float A00 = 0, A11 = 0, A01 = 0;
        float B[3] = {}, Bb[3] = {};
        for (int j = 0; j < TN; ++j) {
            const auto& t = bm.texels[j];
            int w_sum = 0;
            for (int k = 0; k < 4; ++k) {
                w_sum += int(t.weight[k]) * ramp[grid_weights[t.grid[k]]];
            }
            float w1 = float(w_sum) * (1.f / (16.f * 64.f));
            float w0 = 1.f - w1;
            A00 += w0 * w0;
            A11 += w1 * w1;
            A01 += w0 * w1;
            for (int ch = 0; ch < 3; ++ch) {
                B[ch]  += w0 * float(s.rgba8[j][ch]);
                Bb[ch] += w1 * float(s.rgba8[j][ch]);
            }
        }
        float det = A00 * A11 - A01 * A01;
        if (std::abs(det) >= 1e-6f) {
            float einv = 1.f / det;
            for (int ch = 0; ch < 3; ++ch) {
                float c0 = (A11 * B[ch] - A01 * Bb[ch]) * einv;
                float c1 = (-A01 * B[ch] + A00 * Bb[ch]) * einv;
                e0[ch] = std::uint8_t(std::clamp(int(std::lround(c0)), 0, 255));
                e1[ch] = std::uint8_t(std::clamp(int(std::lround(c1)), 0, 255));
            }
        }
    }

    // 5. Blue-contract sum normalisation on the refitted endpoints.
    int s0 = int(e0[0]) + int(e0[1]) + int(e0[2]);
    int s1 = int(e1[0]) + int(e1[1]) + int(e1[2]);
    if (s0 > s1) {
        std::swap(e0[0], e1[0]);
        std::swap(e0[1], e1[1]);
        std::swap(e0[2], e1[2]);
        for (int i = 0; i < GN; ++i)
            grid_weights[i] = std::uint8_t((WL - 1) - int(grid_weights[i]));
    }

    // 6. Bilinear-infill decoded[] using the same truncated-precision
    //    formula the decoder applies. Final texel weight is sum_k of
    //    (coeff[k] * grid_weight_ramp[k]) >> 4.
    float total_err = 0.f;
    for (int j = 0; j < TN; ++j) {
        const auto& t = bm.texels[j];
        int w_sum = 0;
        for (int k = 0; k < 4; ++k) {
            w_sum += int(t.weight[k]) * ramp[grid_weights[t.grid[k]]];
        }
        int w_int = (w_sum + 8) >> 4;  // div by 16
        w_int = std::clamp(w_int, 0, 64);
        int inv = 64 - w_int;
        out.decoded[j][0] = std::uint8_t((inv * int(e0[0]) + w_int * int(e1[0]) + 32) >> 6);
        out.decoded[j][1] = std::uint8_t((inv * int(e0[1]) + w_int * int(e1[1]) + 32) >> 6);
        out.decoded[j][2] = std::uint8_t((inv * int(e0[2]) + w_int * int(e1[2]) + 32) >> 6);
        out.decoded[j][3] = 255;
        if constexpr (M == block_compress::BlockMetric::oklab2) {
            auto l = color_space::srgb8_to_oklab(
                out.decoded[j][0], out.decoded[j][1], out.decoded[j][2]);
            float dL = s.lab[j].L - l.L;
            float dA = s.lab[j].a - l.a;
            float dB = s.lab[j].b - l.b;
            total_err += dL * dL + dA * dA + dB * dB;
        } else {
            int er = int(s.rgba8[j][0]) - int(out.decoded[j][0]);
            int eg = int(s.rgba8[j][1]) - int(out.decoded[j][1]);
            int eb = int(s.rgba8[j][2]) - int(out.decoded[j][2]);
            total_err += float(er * er + eg * eg + eb * eb) * (1.f / 65536.f);
        }
    }

    pack_block<GN, BlockMode, WL>(e0, e1, grid_weights, out.block);
    out.err = total_err;
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

// (QUANT_40 helpers + encode_ise_q40 are defined earlier so the dual-
// plane RGBA path + the 2-partition CEM-8 path can share them.)

// Per-block partition assignment cache. assign[pidx][texel] stores
// 0..(N-1) for N-partition 4×4 blocks (small_block=true, z=0).
template <int N>
struct PartitionTableNP {
    std::uint8_t assign[1024][16];
};
template <int N>
const PartitionTableNP<N>& partition_table_np_4x4() {
    static const PartitionTableNP<N> table = []{
        PartitionTableNP<N> t{};
        for (int pi = 0; pi < 1024; ++pi) {
            for (int dy = 0; dy < kBlockH; ++dy) {
                for (int dx = 0; dx < kBlockW; ++dx) {
                    t.assign[pi][dy * kBlockW + dx] =
                        spec_select_partition(pi, dx, dy, 0, N, /*small=*/true);
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

// Joint weight pick across N partitions: each pixel picks the QUANT_4
// weight (0..3) minimising err against its own partition's paint ramp.
template <int N, block_compress::BlockMetric M>
float pick_weights_np_q4(const Sample16& s,
                         const std::uint8_t assign[16],
                         const std::uint8_t e0[N][3],
                         const std::uint8_t e1[N][3],
                         std::uint8_t out_w[16],
                         std::uint8_t decoded[16][4]) {
    std::uint8_t paint[N][kWeightLevels4][3];
    for (int k = 0; k < N; ++k) {
        for (int w_i = 0; w_i < kWeightLevels4; ++w_i) {
            int w = kWeightToInterp4[w_i];
            int inv = 64 - w;
            for (int ch = 0; ch < 3; ++ch) {
                paint[k][w_i][ch] =
                    std::uint8_t((inv * int(e0[k][ch]) + w * int(e1[k][ch]) + 32) >> 6);
            }
        }
    }
    alignas(16) float paint_L[N][kWeightLevels4],
                      paint_A[N][kWeightLevels4],
                      paint_B[N][kWeightLevels4];
    if constexpr (M == block_compress::BlockMetric::oklab2) {
        for (int k = 0; k < N; ++k) {
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

// LSQ refit endpoints per partition (one 2×2 system per partition).
// Returns false if ANY partition is singular (caller falls back).
template <int N>
bool refit_endpoints_np(const Sample16& s, const std::uint8_t assign[16],
                        const std::uint8_t w[16],
                        std::uint8_t e0[N][3], std::uint8_t e1[N][3]) {
    int n[N][kWeightLevels4] = {};
    int sum[N][kWeightLevels4][3] = {};
    for (int p = 0; p < 16; ++p) {
        int k = assign[p], q = w[p];
        ++n[k][q];
        sum[k][q][0] += s.rgba8[p][0];
        sum[k][q][1] += s.rgba8[p][1];
        sum[k][q][2] += s.rgba8[p][2];
    }
    for (int k = 0; k < N; ++k) {
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

// Pack a 2-partition CEM-8 4×4 block. block_mode 0x42 (QUANT_4 weights,
// 4×4 grid). Endpoint quant is QUANT_40 — the largest quant level
// that fits ASTC's 67 color-bit budget for (12 vals, matched CEM,
// 32 weight bits).
//
// Bit layout:
//   - bit 0..10:  block mode = 0x42
//   - bit 11..12: partition_count - 1 = 1
//   - bit 13..22: partition_index (10 bits)
//   - bit 23..28: matched-CEM = (CEM_8 << 2) = 32  (6 bits)
//   - bit 29..92: endpoint BISE, 12 × QUANT_40 (4 quint-blocks × 16 bits)
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
    write_bits(1, 2, 11, pcb);

    write_bits(std::uint32_t(partition_index) & 0x3Fu, 6, 13, pcb);
    write_bits(std::uint32_t(partition_index) >> 6, 4, 19, pcb);

    write_bits(32, 6, 23, pcb);  // matched-CEM = CEM_8 << 2

    std::uint8_t ep_packed[12];
    int idx = 0;
    for (int k = 0; k < 2; ++k) {
        for (int ch = 0; ch < 3; ++ch) {
            ep_packed[idx++] = quant40_pack(e0[k][ch]);
            ep_packed[idx++] = quant40_pack(e1[k][ch]);
        }
    }
    encode_ise_q40(ep_packed, 12, pcb, 29);

    for (std::size_t i = 0; i < kBlockBytes; ++i) out[i] = pcb[i];
}

// Full 2-partition encoder. Brute-forces all 1024 partition indices
// using a cheap PCA+pick scoring inner loop, then performs full LSQ
// convergence on the winning index. Per-partition blue-contract sum-
// normalisation matches the ASTC decoder's swap rule.
template <block_compress::BlockMetric M>
Candidate encode_block_rgb_2p(const Sample16& s) {
    Candidate best{};
    best.err = std::numeric_limits<float>::infinity();
    const auto& table = partition_table_np_4x4<2>();

    std::uint8_t mask[2][16];
    std::uint8_t e0[2][3], e1[2][3];
    std::uint8_t weights[16];
    std::uint8_t decoded[16][4];

    int best_pi = 0;
    float best_pi_err = std::numeric_limits<float>::infinity();

    for (int pi = 0; pi < 1024; ++pi) {
        const std::uint8_t* assign = table.assign[pi];
        int counts[2] = {};
        for (int i = 0; i < 16; ++i) {
            for (int k = 0; k < 2; ++k) mask[k][i] = (assign[i] == k) ? 1 : 0;
            ++counts[assign[i]];
        }
        if (counts[0] == 0 || counts[1] == 0) continue;

        for (int k = 0; k < 2; ++k) pca_seed_rgb_subset(s, mask[k], e0[k], e1[k]);

        std::uint8_t tw[16];
        std::uint8_t td[16][4];
        float err = pick_weights_np_q4<2, M>(s, assign, e0, e1, tw, td);
        if (err < best_pi_err) { best_pi_err = err; best_pi = pi; }
    }

    // Full convergence for the winning partition index.
    const std::uint8_t* assign = table.assign[best_pi];
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 2; ++k) mask[k][i] = (assign[i] == k) ? 1 : 0;
    }
    for (int k = 0; k < 2; ++k) pca_seed_rgb_subset(s, mask[k], e0[k], e1[k]);
    float err = pick_weights_np_q4<2, M>(s, assign, e0, e1, weights, decoded);
    for (int it = 0; it < 4; ++it) {
        std::uint8_t ne0[2][3], ne1[2][3];
        std::memcpy(ne0, e0, sizeof(ne0));
        std::memcpy(ne1, e1, sizeof(ne1));
        if (!refit_endpoints_np<2>(s, assign, weights, ne0, ne1)) break;
        std::uint8_t new_w[16];
        std::uint8_t new_dec[16][4];
        float new_err = pick_weights_np_q4<2, M>(s, assign, ne0, ne1, new_w, new_dec);
        if (new_err >= err - 1e-7f) break;
        err = new_err;
        std::memcpy(e0, ne0, sizeof(e0));
        std::memcpy(e1, ne1, sizeof(e1));
        std::memcpy(weights, new_w, 16);
        std::memcpy(decoded, new_dec, sizeof(new_dec));
    }

    // Quantize endpoints to QUANT_40 (the decoder's actual paint endpoints).
    std::uint8_t e0d[2][3], e1d[2][3];
    for (int k = 0; k < 2; ++k) {
        for (int ch = 0; ch < 3; ++ch) {
            e0d[k][ch] = kQuant40Unpack[quant40_pack(e0[k][ch])];
            e1d[k][ch] = kQuant40Unpack[quant40_pack(e1[k][ch])];
        }
    }

    // Per-partition blue-contract sum-normalisation on QUANTIZED endpoints.
    for (int k = 0; k < 2; ++k) {
        int sa = int(e0d[k][0]) + int(e0d[k][1]) + int(e0d[k][2]);
        int sb = int(e1d[k][0]) + int(e1d[k][1]) + int(e1d[k][2]);
        if (sa > sb) {
            for (int ch = 0; ch < 3; ++ch) std::swap(e0d[k][ch], e1d[k][ch]);
            for (int i = 0; i < 16; ++i) {
                if (assign[i] == k)
                    weights[i] = std::uint8_t(3 - int(weights[i]));
            }
        }
    }

    // Re-pick weights against the decoded paint ramp + swapped endpoints
    // so reported err matches the decoder's output.
    std::uint8_t final_w[16];
    std::uint8_t final_dec[16][4];
    float final_err = pick_weights_np_q4<2, M>(s, assign, e0d, e1d, final_w, final_dec);

    pack_block_2p(best_pi, e0d, e1d, final_w, best.block);
    std::memcpy(best.decoded, final_dec, sizeof(final_dec));
    best.err = final_err;
    return best;
}

// Per-block dispatch: alpha blocks → CEM-12 (1-partition); RGB blocks
// try 1p (CEM-8 QUANT_8 weights + QUANT_256 endpoints) and 2p (CEM-8
// QUANT_4 weights + QUANT_40 endpoints), pick lower-OKLab².
//
// 3-partition was prototyped and dropped: 10-image DIV2K sweep showed
// adding 3p (QUANT_12 endpoints — only 12 levels per channel) regressed
// mean S2 by 0.17 vs 1p+2p. Encoder's OKLab² favoured 3p for some
// blocks but perceptual S2 preferred 2p's smoother gradients on
// natural-image content. Revisit if QUANT_12 coarseness can be
// mitigated (e.g., RGBA gate, content-classifier).
//
// 4-partition CEM-8 is INFEASIBLE — ASTC color_integer_count caps at
// 18 vs 4 × 6 = 24 — would require CEM-6 (base+scale).
// 4x4 dispatch — alpha + 2-partition specialisations all live in the
// 4x4 path because their partition tables / endpoint quants are sized
// to the 16-texel footprint.
template <block_compress::BlockMetric M>
Candidate encode_block_4x4(const Sample16& s) {
    bool any_alpha = false;
    for (int i = 0; i < 16; ++i) {
        if (s.alpha[i] != 255) { any_alpha = true; break; }
    }
    if (any_alpha) {
        Candidate sp = encode_block_rgba<M>(s);
        Candidate dp = encode_block_rgba_dual<M>(s);
        return (dp.err < sp.err) ? dp : sp;
    }
    Candidate one = encode_block_rgb<4, 4, kBlockModeRgb4x4, 8, M>(s);
    Candidate two = encode_block_rgb_2p<M>(s);
    return (two.err < one.err) ? two : one;
}

// Generic non-4x4 dispatch — 1-partition CEM-8 RGB only. Alpha gets
// the same RGB encoding (alpha is dropped from the encode but preserved
// as 255 in the decoder, which is correct for opaque source images).
template <int W, int H, std::uint32_t BlockMode, int WL, block_compress::BlockMetric M>
Candidate encode_block_wh(const SampleT<W * H>& s) {
    return encode_block_rgb<W, H, BlockMode, WL, M>(s);
}

}  // namespace

// Shared block-loop kernel — templated on the per-block encoder. The
// 4x4 path passes encode_block_4x4 (which itself tries 1p + 2p +
// RGBA); other footprints pass encode_block_wh<W,H,Mode>.
namespace {

template <int W, int H, typename EncodeBlockFn>
EncodeResult encode_image_impl(std::span<const std::uint8_t> rgba_srgb8,
                               int image_w,
                               int image_h,
                               const Options& options,
                               EncodeBlockFn encode_block_fn) {
    constexpr int N = W * H;
    EncodeResult res;

    res.block_cols = (image_w + W - 1) / W;
    res.block_rows = (image_h + H - 1) / H;
    const auto bcols = static_cast<std::size_t>(res.block_cols);
    res.blocks.assign(bcols * static_cast<std::size_t>(res.block_rows), Block{});

    int pad_w = res.block_cols * W;
    int pad_h = res.block_rows * H;
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

    pipeline::parallel_for(std::size_t(n_strips), [&](std::size_t strip) {
        const int by_lo = int(strip) * rows_per_strip;
        const int by_hi = std::min(by_lo + rows_per_strip, res.block_rows);
        if (by_lo >= by_hi) return;

        block_compress::BlockGrid<color_space::OKLab> err_carry(
            res.block_cols, by_hi - by_lo);
        for (auto& v : err_carry.as_span()) v = {0.f, 0.f, 0.f};

        SampleT<N> s{};
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
                load_sample<W, H>(s, padded, pw, bx * W, by * H, shift);

                Candidate c = encode_block_fn(s);

                if (use_block_ed && !ed_kernel.empty()) {
                    float tL = 0.f, ta = 0.f, tb = 0.f;
                    float dL = 0.f, da = 0.f, db = 0.f;
                    for (int i = 0; i < N; ++i) {
                        tL += s.lab[i].L; ta += s.lab[i].a; tb += s.lab[i].b;
                        auto dec_lab = color_space::srgb8_to_oklab(
                            c.decoded[i][0], c.decoded[i][1], c.decoded[i][2]);
                        dL += dec_lab.L; da += dec_lab.a; db += dec_lab.b;
                    }
                    const float inv_n = 1.f / float(N);
                    color_space::OKLab residual{(tL - dL) * inv_n,
                                                (ta - da) * inv_n,
                                                (tb - db) * inv_n};
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
        float next;
        do {
            next = prev + strip_err;
        } while (!total_err_atom.compare_exchange_weak(prev, next,
                                                        std::memory_order_relaxed));
    });
    res.total_oklab2_error = total_err_atom.load();
    return res;
}

}  // namespace

namespace {

// Footprint dispatch table — single source of truth for (W, H) to
// (block_mode, WL) mapping. Adding a new 1:1 footprint = one row here
// + one entry in encode_image's switch + one Mode enum + one CLI parse.
struct FootprintSpec {
    int w, h;
    std::uint32_t block_mode;
    int wl;  // 8 (QUANT_8 weights) or 4 (QUANT_4)
};

// Compile-time encode-block dispatcher used by encode_image_impl. The
// fold-style helpers below instantiate the right encode_block_wh<>
// template for the requested footprint.
template <int W2, int H2, std::uint32_t BM2, int WL2, block_compress::BlockMetric M>
auto make_encode_fn() {
    return [](const SampleT<W2 * H2>& s) { return encode_block_wh<W2, H2, BM2, WL2, M>(s); };
}

template <block_compress::BlockMetric M>
auto make_encode_fn_4x4() {
    return [](const Sample16& s) { return encode_block_4x4<M>(s); };
}

// Bilinear-decimation encode lambda factory. Block dim is TexW × TexH;
// weights live on a smaller GridW × GridH grid.
template <int TexW, int TexH, int GridW, int GridH,
          std::uint32_t BM, int WL, block_compress::BlockMetric M>
auto make_encode_fn_decim() {
    return [](const SampleT<TexW * TexH>& s) {
        return encode_block_rgb_decim<TexW, TexH, GridW, GridH, BM, WL, M>(s);
    };
}

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    const int W = options.block_w;
    const int H = options.block_h;

    auto run = [&]<block_compress::BlockMetric M>() -> EncodeResult {
        if (W == 4 && H == 4)
            return encode_image_impl<4, 4>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn_4x4<M>());
        if (W == 5 && H == 4)
            return encode_image_impl<5, 4>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn<5, 4, kBlockModeRgb5x4, 8, M>());
        // 5x5 / 6x5 keep 1:1 QUANT_4 — having a weight grid point per
        // texel (25 / 30 weights) beats coarser 4×4-grid bilinear-infill
        // here, even though each weight only spans 4 levels. Tested both
        // empirically on DIV2K 0001 @ 256w: 1:1 wins by +4 / +7 S2.
        if (W == 5 && H == 5)
            return encode_image_impl<5, 5>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn<5, 5, kBlockModeRgb5x5, 4, M>());
        if (W == 6 && H == 5)
            return encode_image_impl<6, 5>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn<6, 5, kBlockModeRgb6x5, 4, M>());
        // 6x6 / 8x5 / 8x6 were previously 1:1 QUANT_2 (binary weights —
        // S2 ~50 on natural images). Switching to 4×4 weight grid +
        // QUANT_8 weights with bilinear infill recovers ~30 S2 because
        // the LSQ grid-weight refit can spread per-texel error across
        // a smooth 8-level ramp.
        if (W == 6 && H == 6)
            return encode_image_impl<6, 6>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn_decim<6, 6, 4, 4, kBlockModeRgb4x4, 8, M>());
        if (W == 8 && H == 5)
            return encode_image_impl<8, 5>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn_decim<8, 5, 4, 4, kBlockModeRgb4x4, 8, M>());
        if (W == 8 && H == 6)
            return encode_image_impl<8, 6>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn_decim<8, 6, 4, 4, kBlockModeRgb4x4, 8, M>());
        // Bilinear-decimated footprints (texel dim > weight dim).
        // Block-mode constant is the same one used by the 1:1 weight
        // grid for that size — the texel dim comes from KTX format,
        // and the bit budget for endpoints+weights is unchanged.
        if (W == 8 && H == 8)
            return encode_image_impl<8, 8>(rgba_srgb8, image_w, image_h, options,
                                           make_encode_fn_decim<8, 8, 5, 5, kBlockModeRgb5x5, 4, M>());
        if (W == 10 && H == 5)
            return encode_image_impl<10, 5>(rgba_srgb8, image_w, image_h, options,
                                            make_encode_fn_decim<10, 5, 5, 4, kBlockModeRgb5x4, 8, M>());
        if (W == 10 && H == 6)
            return encode_image_impl<10, 6>(rgba_srgb8, image_w, image_h, options,
                                            make_encode_fn_decim<10, 6, 5, 5, kBlockModeRgb5x5, 4, M>());
        if (W == 10 && H == 8)
            return encode_image_impl<10, 8>(rgba_srgb8, image_w, image_h, options,
                                            make_encode_fn_decim<10, 8, 5, 5, kBlockModeRgb5x5, 4, M>());
        if (W == 10 && H == 10)
            return encode_image_impl<10, 10>(rgba_srgb8, image_w, image_h, options,
                                             make_encode_fn_decim<10, 10, 5, 5, kBlockModeRgb5x5, 4, M>());
        if (W == 12 && H == 10)
            return encode_image_impl<12, 10>(rgba_srgb8, image_w, image_h, options,
                                             make_encode_fn_decim<12, 10, 6, 5, kBlockModeRgb6x5, 4, M>());
        if (W == 12 && H == 12)
            return encode_image_impl<12, 12>(rgba_srgb8, image_w, image_h, options,
                                             make_encode_fn_decim<12, 12, 6, 5, kBlockModeRgb6x5, 4, M>());
        return EncodeResult{};  // unsupported footprint
    };

    if (options.metric == block_compress::BlockMetric::srgb_mse)
        return run.template operator()<block_compress::BlockMetric::srgb_mse>();
    return run.template operator()<block_compress::BlockMetric::oklab2>();
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
