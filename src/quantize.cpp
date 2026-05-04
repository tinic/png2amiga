#include "quantize.hpp"
#include "color_space.hpp"
#include "palette.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <span>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#include <vector>

// ============================================================================
// SIMD backend selection.
//
// Three backends — pick exactly one at compile time:
//   - x86_64 AVX2 + FMA     (256-bit, 8 lanes) — Windows / Linux / Intel Mac
//   - Apple Silicon NEON    (128-bit, 4 lanes) — AArch64 macOS / Linux
//   - WASM SIMD             (128-bit, 4 lanes) — Emscripten
//
// All hot kernels operate on SoA float buffers padded to a multiple of 8
// (LCM of both lane counts) with weight=0 in the tail, so the tail is
// numerically harmless across all three backends. Build flags wired in
// CMakeLists.txt: /arch:AVX2 (MSVC) or -mavx2 -mfma (GCC/Clang) for x86;
// no flag needed for AArch64 NEON; -msimd128 for the WASM target.
// ============================================================================
#if defined(__wasm_simd128__)
    #include <wasm_simd128.h>
    #define PNG2AMIGA_BACKEND_WASM_SIMD 1
    #define PNG2AMIGA_BACKEND_AVX2      0
    #define PNG2AMIGA_BACKEND_NEON      0
#elif defined(__AVX2__)
    #include <immintrin.h>
    #define PNG2AMIGA_BACKEND_AVX2      1
    #define PNG2AMIGA_BACKEND_WASM_SIMD 0
    #define PNG2AMIGA_BACKEND_NEON      0
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define PNG2AMIGA_BACKEND_NEON      1
    #define PNG2AMIGA_BACKEND_AVX2      0
    #define PNG2AMIGA_BACKEND_WASM_SIMD 0
#else
    #error "quantize.cpp requires AVX2 (x86_64), NEON (ARM64), or WASM SIMD \
(Emscripten). For x86 enable /arch:AVX2 (MSVC) or -mavx2 -mfma (GCC/Clang); \
for ARM64 NEON is implicit; for WASM use -msimd128."
#endif

namespace {
using OKLab = png2amiga::color_space::OKLab;

inline float oklab_dist_sq(OKLab a, OKLab b) noexcept {
    float dL = (a.L - b.L) * png2amiga::color_space::WEIGHT_L;
    float da = (a.a - b.a) * png2amiga::color_space::WEIGHT_A;
    float db = (a.b - b.b) * png2amiga::color_space::WEIGHT_B;
    return dL * dL + da * da + db * db;
}

// Pad an entry count up to the SIMD-friendly multiple. We pick 8 so AVX2
// (8 lanes) consumes one block per iteration and WASM SIMD (4 lanes) two.
constexpr std::size_t kSimdPadGroup = 8;
constexpr std::size_t simd_pad(std::size_t n) noexcept {
    return (n + kSimdPadGroup - 1) & ~(kSimdPadGroup - 1);
}

// Pre-weighted SoA OKLab samples. Each component is multiplied by the
// per-channel perceptual weight at fill time so the distance kernel reduces
// to plain (eL - cL)^2 + (ea - ca)^2 + (eb - cb)^2.
struct SoaLab {
    std::vector<float> L, a, b;
    std::size_t valid_n{0};
    std::size_t padded_n{0};
};

inline SoaLab build_soa_lab(std::span<const OKLab> in) {
    SoaLab out;
    out.valid_n  = in.size();
    out.padded_n = simd_pad(in.size());
    out.L.assign(out.padded_n, 0.0f);
    out.a.assign(out.padded_n, 0.0f);
    out.b.assign(out.padded_n, 0.0f);
    for (std::size_t i = 0; i < in.size(); ++i) {
        out.L[i] = in[i].L * png2amiga::color_space::WEIGHT_L;
        out.a[i] = in[i].a * png2amiga::color_space::WEIGHT_A;
        out.b[i] = in[i].b * png2amiga::color_space::WEIGHT_B;
    }
    return out;
}

// Kernel 1 — sum_i min(d_i, best[i]) * weight[i] for a single candidate.
// Used by ocs_bruteforce_quantize's per-candidate scoring loop. Tail
// entries have weight=0 so they contribute nothing regardless of d / best.
inline float simd_clamped_weighted_sse(
    const float* sL, const float* sA, const float* sB, const float* sW,
    const float* best_dist, std::size_t n_padded,
    float cl_L, float cl_a, float cl_b) noexcept
{
#if PNG2AMIGA_BACKEND_AVX2
    __m256 cL = _mm256_set1_ps(cl_L);
    __m256 cA = _mm256_set1_ps(cl_a);
    __m256 cB = _mm256_set1_ps(cl_b);
    __m256 acc = _mm256_setzero_ps();
    for (std::size_t i = 0; i < n_padded; i += 8) {
        __m256 eL = _mm256_loadu_ps(sL + i);
        __m256 eA = _mm256_loadu_ps(sA + i);
        __m256 eB = _mm256_loadu_ps(sB + i);
        __m256 dL = _mm256_sub_ps(eL, cL);
        __m256 dA = _mm256_sub_ps(eA, cA);
        __m256 dB = _mm256_sub_ps(eB, cB);
        __m256 d  = _mm256_fmadd_ps(dL, dL,
                        _mm256_fmadd_ps(dA, dA,
                            _mm256_mul_ps(dB, dB)));
        __m256 bd = _mm256_loadu_ps(best_dist + i);
        __m256 ef = _mm256_min_ps(d, bd);
        __m256 w  = _mm256_loadu_ps(sW + i);
        acc = _mm256_fmadd_ps(ef, w, acc);
    }
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 s  = _mm_add_ps(lo, hi);
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
#elif PNG2AMIGA_BACKEND_NEON
    float32x4_t cL = vdupq_n_f32(cl_L);
    float32x4_t cA = vdupq_n_f32(cl_a);
    float32x4_t cB = vdupq_n_f32(cl_b);
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (std::size_t i = 0; i < n_padded; i += 4) {
        float32x4_t eL = vld1q_f32(sL + i);
        float32x4_t eA = vld1q_f32(sA + i);
        float32x4_t eB = vld1q_f32(sB + i);
        float32x4_t dL = vsubq_f32(eL, cL);
        float32x4_t dA = vsubq_f32(eA, cA);
        float32x4_t dB = vsubq_f32(eB, cB);
        // d = dB*dB + dA*dA + dL*dL via two FMAs.
        float32x4_t d  = vfmaq_f32(
                            vfmaq_f32(vmulq_f32(dB, dB), dA, dA),
                            dL, dL);
        float32x4_t bd = vld1q_f32(best_dist + i);
        float32x4_t ef = vminq_f32(d, bd);
        float32x4_t w  = vld1q_f32(sW + i);
        acc = vfmaq_f32(acc, ef, w);
    }
    return vaddvq_f32(acc);  // AArch64 horizontal add of 4 floats
#else  // WASM SIMD
    v128_t cL = wasm_f32x4_splat(cl_L);
    v128_t cA = wasm_f32x4_splat(cl_a);
    v128_t cB = wasm_f32x4_splat(cl_b);
    v128_t acc = wasm_f32x4_const_splat(0.0f);
    for (std::size_t i = 0; i < n_padded; i += 4) {
        v128_t eL = wasm_v128_load(sL + i);
        v128_t eA = wasm_v128_load(sA + i);
        v128_t eB = wasm_v128_load(sB + i);
        v128_t dL = wasm_f32x4_sub(eL, cL);
        v128_t dA = wasm_f32x4_sub(eA, cA);
        v128_t dB = wasm_f32x4_sub(eB, cB);
        v128_t d  = wasm_f32x4_add(
                        wasm_f32x4_mul(dL, dL),
                        wasm_f32x4_add(
                            wasm_f32x4_mul(dA, dA),
                            wasm_f32x4_mul(dB, dB)));
        v128_t bd = wasm_v128_load(best_dist + i);
        v128_t ef = wasm_f32x4_min(d, bd);
        v128_t w  = wasm_v128_load(sW + i);
        acc = wasm_f32x4_add(acc, wasm_f32x4_mul(ef, w));
    }
    alignas(16) float buf[4];
    wasm_v128_store(buf, acc);
    return buf[0] + buf[1] + buf[2] + buf[3];
#endif
}

// Kernel 2 — best_dist[i] = min(best_dist[i], d_i). Used after the greedy
// picks a new palette colour to fold its distances into the running best.
inline void simd_update_min_dist(
    const float* sL, const float* sA, const float* sB,
    float* best_dist, std::size_t n_padded,
    float cl_L, float cl_a, float cl_b) noexcept
{
#if PNG2AMIGA_BACKEND_AVX2
    __m256 cL = _mm256_set1_ps(cl_L);
    __m256 cA = _mm256_set1_ps(cl_a);
    __m256 cB = _mm256_set1_ps(cl_b);
    for (std::size_t i = 0; i < n_padded; i += 8) {
        __m256 eL = _mm256_loadu_ps(sL + i);
        __m256 eA = _mm256_loadu_ps(sA + i);
        __m256 eB = _mm256_loadu_ps(sB + i);
        __m256 dL = _mm256_sub_ps(eL, cL);
        __m256 dA = _mm256_sub_ps(eA, cA);
        __m256 dB = _mm256_sub_ps(eB, cB);
        __m256 d  = _mm256_fmadd_ps(dL, dL,
                        _mm256_fmadd_ps(dA, dA,
                            _mm256_mul_ps(dB, dB)));
        __m256 bd = _mm256_loadu_ps(best_dist + i);
        __m256 mn = _mm256_min_ps(bd, d);
        _mm256_storeu_ps(best_dist + i, mn);
    }
#elif PNG2AMIGA_BACKEND_NEON
    float32x4_t cL = vdupq_n_f32(cl_L);
    float32x4_t cA = vdupq_n_f32(cl_a);
    float32x4_t cB = vdupq_n_f32(cl_b);
    for (std::size_t i = 0; i < n_padded; i += 4) {
        float32x4_t eL = vld1q_f32(sL + i);
        float32x4_t eA = vld1q_f32(sA + i);
        float32x4_t eB = vld1q_f32(sB + i);
        float32x4_t dL = vsubq_f32(eL, cL);
        float32x4_t dA = vsubq_f32(eA, cA);
        float32x4_t dB = vsubq_f32(eB, cB);
        float32x4_t d  = vfmaq_f32(
                            vfmaq_f32(vmulq_f32(dB, dB), dA, dA),
                            dL, dL);
        float32x4_t bd = vld1q_f32(best_dist + i);
        float32x4_t mn = vminq_f32(bd, d);
        vst1q_f32(best_dist + i, mn);
    }
#else  // WASM SIMD
    v128_t cL = wasm_f32x4_splat(cl_L);
    v128_t cA = wasm_f32x4_splat(cl_a);
    v128_t cB = wasm_f32x4_splat(cl_b);
    for (std::size_t i = 0; i < n_padded; i += 4) {
        v128_t eL = wasm_v128_load(sL + i);
        v128_t eA = wasm_v128_load(sA + i);
        v128_t eB = wasm_v128_load(sB + i);
        v128_t dL = wasm_f32x4_sub(eL, cL);
        v128_t dA = wasm_f32x4_sub(eA, cA);
        v128_t dB = wasm_f32x4_sub(eB, cB);
        v128_t d  = wasm_f32x4_add(
                        wasm_f32x4_mul(dL, dL),
                        wasm_f32x4_add(
                            wasm_f32x4_mul(dA, dA),
                            wasm_f32x4_mul(dB, dB)));
        v128_t bd = wasm_v128_load(best_dist + i);
        v128_t mn = wasm_f32x4_min(bd, d);
        wasm_v128_store(best_dist + i, mn);
    }
#endif
}

// Kernel 3 — k-means assignment. For each sample, find the nearest centroid
// (argmin over k) and write best_d[i] / best_k[i]. Outer loop iterates over
// centroids so the SoA sample buffers stream cache-linearly. Centroids are
// expected pre-weighted (multiplied by WEIGHT_L/A/B at fill time).
//
// best_d / best_k must already be initialized — best_d to FLT_MAX, best_k
// to 0 — so the first centroid pass populates them via the "d < best_d"
// blend. Padded sample lanes write into best_k tail entries that the caller
// will discard.
inline void simd_assign_nearest(
    const float* sL, const float* sA, const float* sB,
    std::size_t n_samples_padded,
    const float* cL, const float* cA, const float* cB,
    std::size_t n_centroids,
    float* best_d, std::uint32_t* best_k) noexcept
{
#if PNG2AMIGA_BACKEND_AVX2
    for (std::size_t k = 0; k < n_centroids; ++k) {
        __m256 ck   = _mm256_set1_ps(cL[k]);
        __m256 cak  = _mm256_set1_ps(cA[k]);
        __m256 cbk  = _mm256_set1_ps(cB[k]);
        // Broadcast centroid index k as a float bit pattern (uint32 → ps
        // reinterpretation). We use _mm256_blendv_ps to select between the
        // existing best_k vector and the new k, treating both as raw bits.
        // Bit-preserving so the integer pattern survives.
        union { std::uint32_t u; float f; } kbits{static_cast<std::uint32_t>(k)};
        __m256 kvec = _mm256_set1_ps(kbits.f);
        for (std::size_t i = 0; i < n_samples_padded; i += 8) {
            __m256 sLv = _mm256_loadu_ps(sL + i);
            __m256 sAv = _mm256_loadu_ps(sA + i);
            __m256 sBv = _mm256_loadu_ps(sB + i);
            __m256 dL  = _mm256_sub_ps(sLv, ck);
            __m256 dA  = _mm256_sub_ps(sAv, cak);
            __m256 dB  = _mm256_sub_ps(sBv, cbk);
            __m256 d   = _mm256_fmadd_ps(dL, dL,
                            _mm256_fmadd_ps(dA, dA,
                                _mm256_mul_ps(dB, dB)));
            __m256 bd  = _mm256_loadu_ps(best_d + i);
            __m256 cmp = _mm256_cmp_ps(d, bd, _CMP_LT_OQ);  // d < bd
            __m256 nd  = _mm256_min_ps(d, bd);
            _mm256_storeu_ps(best_d + i, nd);
            // best_k as 32-bit bit pattern: blend uses the float form; the
            // union round-trip keeps the integer encoding intact through the
            // _ps register.
            __m256 bk_f = _mm256_loadu_ps(reinterpret_cast<const float*>(best_k + i));
            __m256 nbk  = _mm256_blendv_ps(bk_f, kvec, cmp);
            _mm256_storeu_ps(reinterpret_cast<float*>(best_k + i), nbk);
        }
    }
#elif PNG2AMIGA_BACKEND_NEON
    for (std::size_t k = 0; k < n_centroids; ++k) {
        float32x4_t ck   = vdupq_n_f32(cL[k]);
        float32x4_t cak  = vdupq_n_f32(cA[k]);
        float32x4_t cbk  = vdupq_n_f32(cB[k]);
        uint32x4_t  kvec = vdupq_n_u32(static_cast<std::uint32_t>(k));
        for (std::size_t i = 0; i < n_samples_padded; i += 4) {
            float32x4_t sLv = vld1q_f32(sL + i);
            float32x4_t sAv = vld1q_f32(sA + i);
            float32x4_t sBv = vld1q_f32(sB + i);
            float32x4_t dL  = vsubq_f32(sLv, ck);
            float32x4_t dA  = vsubq_f32(sAv, cak);
            float32x4_t dB  = vsubq_f32(sBv, cbk);
            float32x4_t d   = vfmaq_f32(
                                vfmaq_f32(vmulq_f32(dB, dB), dA, dA),
                                dL, dL);
            float32x4_t bd  = vld1q_f32(best_d + i);
            uint32x4_t  cmp = vcltq_f32(d, bd);   // d < bd → all-1s mask
            float32x4_t nd  = vminq_f32(d, bd);
            vst1q_f32(best_d + i, nd);
            uint32x4_t  bk  = vld1q_u32(best_k + i);
            uint32x4_t  nbk = vbslq_u32(cmp, kvec, bk);  // cmp ? kvec : bk
            vst1q_u32(best_k + i, nbk);
        }
    }
#else  // WASM SIMD
    for (std::size_t k = 0; k < n_centroids; ++k) {
        v128_t ck  = wasm_f32x4_splat(cL[k]);
        v128_t cak = wasm_f32x4_splat(cA[k]);
        v128_t cbk = wasm_f32x4_splat(cB[k]);
        v128_t kvec = wasm_u32x4_splat(static_cast<std::uint32_t>(k));
        for (std::size_t i = 0; i < n_samples_padded; i += 4) {
            v128_t sLv = wasm_v128_load(sL + i);
            v128_t sAv = wasm_v128_load(sA + i);
            v128_t sBv = wasm_v128_load(sB + i);
            v128_t dL  = wasm_f32x4_sub(sLv, ck);
            v128_t dA  = wasm_f32x4_sub(sAv, cak);
            v128_t dB  = wasm_f32x4_sub(sBv, cbk);
            v128_t d   = wasm_f32x4_add(
                            wasm_f32x4_mul(dL, dL),
                            wasm_f32x4_add(
                                wasm_f32x4_mul(dA, dA),
                                wasm_f32x4_mul(dB, dB)));
            v128_t bd  = wasm_v128_load(best_d + i);
            v128_t cmp = wasm_f32x4_lt(d, bd);   // mask, all-1s where true
            v128_t nd  = wasm_f32x4_min(d, bd);
            wasm_v128_store(best_d + i, nd);
            v128_t bk  = wasm_v128_load(best_k + i);
            v128_t nbk = wasm_v128_bitselect(kvec, bk, cmp);  // cmp ? kvec : bk
            wasm_v128_store(best_k + i, nbk);
        }
    }
#endif
}
} // namespace

namespace png2amiga::quantize {

namespace {

// ===========================================================================
// Median-cut algorithm
//
// 1. Start with all pixels in one "box" in linear RGB space.
// 2. Find the box with the largest range along any channel.
// 3. Split that box at the median of its widest channel.
// 4. Repeat until we have max_colors boxes.
// 5. The centroid of each box is one palette color.
//
// We work in linear RGB because the palette values are stored in linear
// RGB and the color space conversion happens at the boundary. The split
// decisions could alternatively use OKLab for more perceptual uniformity,
// but linear RGB gives good results and is much faster (no cbrt per pixel).
// ===========================================================================

// (ColorBox / box_centroid removed — Wu's quantization replaces median-cut)

// ===========================================================================
// OCS brute-force quantizer
//
// Since OCS only has 4096 possible colors (12-bit, 4 bits/channel),
// we can find the optimal N-color palette by:
//
// 1. Build a histogram: map every pixel to its nearest OCS color.
// 2. Weighted median-cut on the histogram to get initial N colors.
// 3. K-means refinement: for each cluster, brute-force search all 4096
//    OCS colors to find the one minimizing total perceptual error.
//    Each cluster is refined independently → parallelizable.
//
// This produces genuinely optimal OCS palettes, avoiding the lossy
// "median-cut in continuous space, then snap to 12-bit" approach.
// ===========================================================================

// Precomputed OCS color tables (computed once, shared across calls)
struct OcsLut {
    std::array<Color3f, 4096> linear;
    std::array<color_space::OKLab, 4096> oklab;

    OcsLut() {
        for (std::uint16_t i = 0; i < 4096; ++i) {
            linear[i] = palette::ocs_to_linear(i);
            oklab[i] = color_space::linear_to_oklab(linear[i]);
        }
    }
};

static const OcsLut& ocs_lut() {
    static const OcsLut lut;
    return lut;
}

// Weighted color entry for histogram-based quantization
struct WeightedOcs {
    std::uint16_t ocs_index;    // 0-4095
    std::uint32_t weight;       // pixel count
};

Palette ocs_bruteforce_quantize(std::span<const Color3f> pixels,
                                std::size_t max_colors,
                                int palette_diversity = 0);

// Forward declaration — the diversity pass is defined after median_cut.
// (Also exposed publicly as quantize::diversify_palette.)
void apply_palette_diversity(Palette& palette,
                             std::span<const Color3f> pixels,
                             int diversity_level,
                             bool snap_to_ocs);

Palette ocs_bruteforce_quantize(std::span<const Color3f> pixels,
                                std::size_t max_colors,
                                int palette_diversity) {
    if (max_colors == 0) max_colors = 1;
    if (pixels.empty()) {
        return Palette{"ocs-optimal", {Color3f{0.0f, 0.0f, 0.0f}}};
    }

    auto& lut = ocs_lut();

    // Step 1: Build histogram of OCS colors
    std::array<std::uint32_t, 4096> histogram{};
    for (auto& pixel : pixels) {
        auto ocs = palette::linear_to_ocs(pixel);
        histogram[ocs]++;
    }

    // Collect non-zero entries
    std::vector<WeightedOcs> entries;
    entries.reserve(4096);
    for (std::uint16_t i = 0; i < 4096; ++i) {
        if (histogram[i] > 0) {
            entries.push_back({i, histogram[i]});
        }
    }

    // If we have fewer unique colors than max_colors, just use them all
    if (entries.size() <= max_colors) {
        Palette result;
        result.name = "ocs-optimal";
        for (auto& e : entries) {
            result.colors.push_back(lut.linear[e.ocs_index]);
        }
        // Sort by luminance
        std::sort(result.colors.begin(), result.colors.end(),
                  [](const Color3f& a, const Color3f& b) {
                      return color_space::linear_to_oklab(a).L <
                             color_space::linear_to_oklab(b).L;
                  });
        return result;
    }

    // Step 2: Greedy sequential palette construction.
    // Add one color at a time, each time picking the OCS color that
    // minimizes the total weighted error across all histogram entries.
    // Same approach as abc (AmigAtari Bitmap Converter).
    std::vector<std::uint16_t> palette_ocs(max_colors);

    // SoA, pre-weighted entry buffers for the AVX2/WASM-SIMD inner loops.
    // Padded to a multiple of 8 (kSimdPadGroup); padded entries carry
    // weight=0 so they contribute nothing to the running total regardless
    // of the distance they produce against any candidate.
    std::size_t n_entries = entries.size();
    std::size_t n_padded  = simd_pad(n_entries);
    std::vector<float> e_L(n_padded, 0.0f);
    std::vector<float> e_a(n_padded, 0.0f);
    std::vector<float> e_b(n_padded, 0.0f);
    std::vector<float> e_w(n_padded, 0.0f);
    for (std::size_t i = 0; i < n_entries; ++i) {
        auto& lab = lut.oklab[entries[i].ocs_index];
        e_L[i] = lab.L * color_space::WEIGHT_L;
        e_a[i] = lab.a * color_space::WEIGHT_A;
        e_b[i] = lab.b * color_space::WEIGHT_B;
        e_w[i] = static_cast<float>(entries[i].weight);
    }

    // Pre-weighted candidate OKLab table. Computing this once amortises
    // the WEIGHT_* multiplies across max_colors × 4096 candidate visits.
    std::array<float, 4096> cand_L, cand_a, cand_b;
    for (std::uint16_t c = 0; c < 4096; ++c) {
        cand_L[c] = lut.oklab[c].L * color_space::WEIGHT_L;
        cand_a[c] = lut.oklab[c].a * color_space::WEIGHT_A;
        cand_b[c] = lut.oklab[c].b * color_space::WEIGHT_B;
    }

    // Per-entry cache: current minimum distance to any palette colour
    // already chosen. Padded tail set to FLT_MAX so the min in the SIMD
    // kernel is a no-op, while the matching weight=0 ensures no
    // contribution to the total either.
    std::vector<float> best_dist(n_padded,
                                 std::numeric_limits<float>::max());

    // Track which OCS codes are already in the palette so the greedy
    // can skip duplicates (slot 0 + slot 1 both landing on 0x000 was
    // a real symptom on low-chroma sources).
    std::array<bool, 4096> picked{};

    auto is_gray_code = [](std::uint16_t c) {
        int r = (c >> 8) & 0xF, g = (c >> 4) & 0xF, b = c & 0xF;
        return r == g && g == b;
    };

    for (std::size_t k = 0; k < max_colors; ++k) {
        // Try all unpicked 4096 OCS colors. Tie-break: when two
        // candidates yield equal total error, prefer the gray-axis
        // code (R=G=B). Without this, ties on near-gray content are
        // resolved by the 0..4095 traversal order, which surfaces
        // chromatic codes like 0x001 = (0,0,17) ahead of grays — the
        // pixels aren't tinted but the palette becomes a striped
        // mess of one-nibble-off grays.
        float best_total = std::numeric_limits<float>::max();
        std::uint16_t best_ocs = 0;
        bool best_is_gray = false;

        for (std::uint16_t candidate = 0; candidate < 4096; ++candidate) {
            if (picked[candidate]) continue;
            float total = simd_clamped_weighted_sse(
                e_L.data(), e_a.data(), e_b.data(), e_w.data(),
                best_dist.data(), n_padded,
                cand_L[candidate], cand_a[candidate], cand_b[candidate]);

            bool cand_gray = is_gray_code(candidate);
            constexpr float kTieEps = 1e-6f;
            bool strictly_better = total < best_total - kTieEps;
            bool tied_and_gray = !strictly_better &&
                                 total < best_total + kTieEps &&
                                 cand_gray && !best_is_gray;
            if (strictly_better || tied_and_gray) {
                best_total = total;
                best_ocs = candidate;
                best_is_gray = cand_gray;
            }
        }

        // Stop adding fresh codes once the palette already covers
        // the histogram exactly. Otherwise the greedy keeps picking
        // arbitrary unpicked OCS codes (in 0..4095 order) for no
        // benefit, surfacing chromatic phantoms like (0,0,17) on
        // gray-only sources. Pad remaining slots with duplicates of
        // the last-picked colour — the encoder's dither sees only
        // the unique entries either way.
        constexpr float kSaturatedEps = 1e-9f;
        if (best_total < kSaturatedEps && k > 0) {
            for (std::size_t kk = k; kk < max_colors; ++kk) {
                palette_ocs[kk] = palette_ocs[k - 1];
            }
            break;
        }

        palette_ocs[k] = best_ocs;
        picked[best_ocs] = true;

        // Fold the newly-added colour's distances into the running best.
        simd_update_min_dist(
            e_L.data(), e_a.data(), e_b.data(),
            best_dist.data(), n_padded,
            cand_L[best_ocs], cand_a[best_ocs], cand_b[best_ocs]);
    }

    // (k-means refinement removed — greedy sequential is sufficient)

    // Build result palette
    Palette result;
    result.name = "ocs-optimal";
    result.colors.reserve(max_colors);
    for (auto ocs : palette_ocs) {
        result.colors.push_back(lut.linear[ocs]);
    }

    // Optional palette diversity pass (remove near-duplicates, re-seed from
    // worst-served pixels). Snap back to OCS precision after each move.
    if (palette_diversity > 0) {
        apply_palette_diversity(result, pixels, palette_diversity,
                                /*snap_to_ocs=*/true);
    }

    // Sort by perceptual luminance
    std::sort(result.colors.begin(), result.colors.end(),
              [](const Color3f& a, const Color3f& b) {
                  return color_space::linear_to_oklab(a).L <
                         color_space::linear_to_oklab(b).L;
              });

    return result;
}

} // namespace

// ===========================================================================
// Median-cut implementation
// ===========================================================================

Palette median_cut(std::span<const Color3f> colors,
                   std::size_t max_colors,
                   int palette_diversity) {
    if (max_colors == 0) max_colors = 1;
    if (colors.empty()) {
        return Palette{"quantized", {Color3f{0.0f, 0.0f, 0.0f}}};
    }

    // Subsample large images for k-means refinement
    std::vector<Color3f> work(colors.begin(), colors.end());
    constexpr std::size_t max_samples = 262144;
    if (work.size() > max_samples) {
        auto stride = work.size() / max_samples;
        std::vector<Color3f> sampled;
        sampled.reserve(max_samples);
        for (std::size_t i = 0; i < work.size(); i += stride) {
            sampled.push_back(work[i]);
        }
        work = std::move(sampled);
    }

    // ---------------------------------------------------------------
    // Median-cut → k-means refinement in OKLab perceptual space
    //
    // Median-cut gives stable initial centroids. K-means in OKLab
    // (perceptually uniform) refines them with empty cluster handling.
    // ---------------------------------------------------------------

    // Run median-cut in linear RGB for initial centroids
    // (reuse the subsampled 'work' array)
    auto mc_result = [&]() {
        std::vector<Color3f> mc_work = work;  // median-cut sorts in-place
        struct Box {
            std::size_t start, count;
            float vol{};
            int axis{};
            void compute(std::span<const Color3f> c) {
                if (count == 0) { vol = 0; return; }
                float minr=1e9f,maxr=-1e9f,ming=1e9f,maxg=-1e9f,minb=1e9f,maxb=-1e9f;
                for (std::size_t i = start; i < start+count; ++i) {
                    minr=std::min(minr,c[i].r); maxr=std::max(maxr,c[i].r);
                    ming=std::min(ming,c[i].g); maxg=std::max(maxg,c[i].g);
                    minb=std::min(minb,c[i].b); maxb=std::max(maxb,c[i].b);
                }
                float rr=maxr-minr, rg=maxg-ming, rb=maxb-minb;
                vol = std::max({rr,rg,rb});
                axis = (rr>=rg && rr>=rb) ? 0 : (rg>=rb) ? 1 : 2;
            }
        };
        std::vector<Box> boxes;
        boxes.reserve(max_colors);

        // libimagequant-inspired sub-box init (16 corner buckets via
        // 1 high bit per RGB channel + 1 high bit per "lightness").
        // Without this, median-cut starts with one giant box: the
        // first split direction is dominated by whatever channel has
        // the largest range across the whole image, and tiny minority
        // colours in extreme hue corners can get buried for the first
        // few splits. Pre-bucketing guarantees an extreme-corner
        // colour gets an initial slot when present in the source —
        // small bucket → contributes one centroid to the starting set,
        // then normal median-cut takes over for the remainder.
        //
        // Only applied when target_colors > 2 × n_nonempty_buckets so
        // we don't burn half the palette on corners alone — that
        // guard mirrors libimagequant's `LIQ_MAXCLUSTER`-vs-target
        // gate (`mediancut.rs:240-254`).
        {
            constexpr std::size_t kCornerBuckets = 16;
            // Use perceptual lightness as the 4th axis: max(R, G, B) as
            // a cheap stand-in for OKLab L. Keeps the 16-bucket
            // partition meaningful without paying for a full conversion.
            auto bucket_id = [](const Color3f& c) -> std::size_t {
                std::size_t r = c.r >= 0.5f ? 1 : 0;
                std::size_t g = c.g >= 0.5f ? 1 : 0;
                std::size_t b = c.b >= 0.5f ? 1 : 0;
                std::size_t l = std::max({c.r, c.g, c.b}) >= 0.5f ? 1 : 0;
                return (r << 3) | (g << 2) | (b << 1) | l;
            };
            std::array<std::vector<std::size_t>, kCornerBuckets> bucket_idx;
            for (std::size_t i = 0; i < mc_work.size(); ++i)
                bucket_idx[bucket_id(mc_work[i])].push_back(i);
            std::size_t n_nonempty = 0;
            for (auto& b : bucket_idx) if (!b.empty()) ++n_nonempty;

            if (n_nonempty >= 2 && max_colors > 2 * n_nonempty) {
                // Compact mc_work into bucket-contiguous order so each
                // non-empty corner becomes one initial Box{start, count}.
                std::vector<Color3f> compacted;
                compacted.reserve(mc_work.size());
                std::size_t off = 0;
                for (auto& bi : bucket_idx) {
                    if (bi.empty()) continue;
                    Box bx{off, bi.size()};
                    for (auto idx : bi) compacted.push_back(mc_work[idx]);
                    bx.compute(compacted);
                    boxes.push_back(bx);
                    off += bi.size();
                }
                mc_work = std::move(compacted);
            } else {
                boxes.push_back({0, mc_work.size()});
                boxes[0].compute(mc_work);
            }
        }

        while (boxes.size() < max_colors) {
            std::size_t bi = 0; float bv = -1;
            for (std::size_t i = 0; i < boxes.size(); ++i)
                if (boxes[i].count >= 2 && boxes[i].vol > bv) { bv = boxes[i].vol; bi = i; }
            if (bv <= 0) break;
            auto& b = boxes[bi];
            auto bb = mc_work.begin()+static_cast<std::ptrdiff_t>(b.start);
            auto be = bb+static_cast<std::ptrdiff_t>(b.count);
            switch (b.axis) {
            case 0: std::sort(bb,be,[](auto&p,auto&q){return p.r<q.r;}); break;
            case 1: std::sort(bb,be,[](auto&p,auto&q){return p.g<q.g;}); break;
            case 2: std::sort(bb,be,[](auto&p,auto&q){return p.b<q.b;}); break;
            }
            auto m = b.count/2;
            Box a{b.start,m}, c{b.start+m,b.count-m};
            a.compute(mc_work); c.compute(mc_work);
            boxes[bi] = a; boxes.push_back(c);
        }
        std::vector<Color3f> centroids;
        for (auto& b : boxes) {
            double sr=0,sg=0,sb=0;
            for (std::size_t i=b.start; i<b.start+b.count; ++i) {
                sr+=static_cast<double>(mc_work[i].r);
                sg+=static_cast<double>(mc_work[i].g);
                sb+=static_cast<double>(mc_work[i].b);
            }
            auto n=static_cast<double>(b.count);
            centroids.push_back(Color3f{
                static_cast<float>(sr/n),
                static_cast<float>(sg/n),
                static_cast<float>(sb/n)}.clamped());
        }
        return centroids;
    }();

    auto n_colors = mc_result.size();

    Palette result;
    result.name = "quantized";
    result.colors = mc_result;

    constexpr int kmeans_max_iter = 40;

    // Pre-weighted SoA samples. Pad to a multiple of 8 with zeros so the
    // SIMD assignment kernel can stream evenly without a tail loop. The
    // padded sample lanes deterministically resolve to centroid 0 (or
    // whichever lies closest to the origin in pre-weighted OKLab space);
    // we ignore those tail entries when accumulating cluster stats.
    std::size_t n_samples = work.size();
    std::size_t n_pad     = simd_pad(n_samples);
    std::vector<float> sLv(n_pad, 0.0f), sAv(n_pad, 0.0f), sBv(n_pad, 0.0f);
    for (std::size_t i = 0; i < n_samples; ++i) {
        auto lab = color_space::linear_to_oklab(work[i]);
        sLv[i] = lab.L * color_space::WEIGHT_L;
        sAv[i] = lab.a * color_space::WEIGHT_A;
        sBv[i] = lab.b * color_space::WEIGHT_B;
    }

    // Centroids: kept in pre-weighted SoA for the SIMD kernel, plus
    // unweighted OKLab so we can read them back at the end and compute
    // raw deltas. (The k-means convergence check needs raw OKLab.)
    std::vector<float> cLv(n_colors), cAv(n_colors), cBv(n_colors);
    std::vector<color_space::OKLab> centroids(n_colors);
    for (std::size_t i = 0; i < n_colors; ++i) {
        centroids[i] = color_space::linear_to_oklab(result.colors[i]);
        cLv[i] = centroids[i].L * color_space::WEIGHT_L;
        cAv[i] = centroids[i].a * color_space::WEIGHT_A;
        cBv[i] = centroids[i].b * color_space::WEIGHT_B;
    }

    // Per-sample assignment + per-sample squared distance to its assigned
    // centroid. Both padded to n_pad. uint32 for assignments so the SIMD
    // blend can write 32-bit lanes.
    std::vector<std::uint32_t> assignments(n_pad, 0);
    std::vector<float>         pixel_errors(n_pad, std::numeric_limits<float>::max());

    for (int iter = 0; iter < kmeans_max_iter; ++iter) {
        // Reset to FLT_MAX before each pass; simd_assign_nearest folds
        // the new minimum in via cmplt+blend.
        std::fill(pixel_errors.begin(), pixel_errors.end(),
                  std::numeric_limits<float>::max());
        std::fill(assignments.begin(), assignments.end(), 0u);
        simd_assign_nearest(
            sLv.data(), sAv.data(), sBv.data(), n_pad,
            cLv.data(), cAv.data(), cBv.data(), n_colors,
            pixel_errors.data(), assignments.data());

        // Accumulate per-cluster stats. Only the first n_samples lanes are
        // real input; padded tail is excluded.
        struct Acc { double L{}, a{}, b{}; std::size_t n{}; double total_err{}; };
        std::vector<Acc> acc(n_colors);
        for (std::size_t i = 0; i < n_samples; ++i) {
            auto k = assignments[i];
            acc[k].L += static_cast<double>(sLv[i]) / color_space::WEIGHT_L;
            acc[k].a += static_cast<double>(sAv[i]) / color_space::WEIGHT_A;
            acc[k].b += static_cast<double>(sBv[i]) / color_space::WEIGHT_B;
            acc[k].n++;
            acc[k].total_err += static_cast<double>(pixel_errors[i]);
        }

        // Recompute centroids; handle empty clusters
        bool changed = false;
        for (std::size_t k = 0; k < n_colors; ++k) {
            if (acc[k].n == 0) {
                // Re-seed from the farthest pixel of the worst cluster.
                std::size_t worst_cluster = 0;
                double worst_err = -1.0;
                for (std::size_t j = 0; j < n_colors; ++j) {
                    if (acc[j].total_err > worst_err) {
                        worst_err = acc[j].total_err;
                        worst_cluster = j;
                    }
                }
                float farthest_d = -1.0f;
                std::size_t farthest_idx = 0;
                for (std::size_t i = 0; i < n_samples; ++i) {
                    if (assignments[i] == worst_cluster &&
                        pixel_errors[i] > farthest_d) {
                        farthest_d = pixel_errors[i];
                        farthest_idx = i;
                    }
                }
                centroids[k] = color_space::OKLab{
                    sLv[farthest_idx] / color_space::WEIGHT_L,
                    sAv[farthest_idx] / color_space::WEIGHT_A,
                    sBv[farthest_idx] / color_space::WEIGHT_B,
                };
                cLv[k] = sLv[farthest_idx];
                cAv[k] = sAv[farthest_idx];
                cBv[k] = sBv[farthest_idx];
                changed = true;
                continue;
            }

            auto dn = static_cast<double>(acc[k].n);
            color_space::OKLab nlab{
                static_cast<float>(acc[k].L / dn),
                static_cast<float>(acc[k].a / dn),
                static_cast<float>(acc[k].b / dn),
            };
            if (oklab_dist_sq(nlab, centroids[k]) > 1e-12f) {
                changed = true;
                centroids[k] = nlab;
                cLv[k] = nlab.L * color_space::WEIGHT_L;
                cAv[k] = nlab.a * color_space::WEIGHT_A;
                cBv[k] = nlab.b * color_space::WEIGHT_B;
            }
        }
        if (!changed) break;
    }

    // Write refined centroids back
    for (std::size_t i = 0; i < n_colors; ++i) {
        result.colors[i] = color_space::oklab_to_linear(centroids[i]).clamped();
    }

    // Optional palette diversity pass: remove near-duplicate entries and
    // re-seed them from image regions that are poorly served by the current
    // palette. Operates on the full (non-subsampled) pixel array.
    if (palette_diversity > 0) {
        apply_palette_diversity(result, colors, palette_diversity,
                                /*snap_to_ocs=*/false);
    }

    // Sort palette by perceptual luminance (OKLab L) for consistent ordering
    std::sort(result.colors.begin(), result.colors.end(),
              [](const Color3f& a, const Color3f& b) {
                  return color_space::linear_to_oklab(a).L <
                         color_space::linear_to_oklab(b).L;
              });

    return result;
}

// ===========================================================================
// Pairwise Nearest Neighbor quantization (Equitz 1989, Kurz/Thyssen)
//
// Agglomerative clustering with Ward-linkage merge cost in OKLab:
//
//     cost(i, j) = (w_i * w_j / (w_i + w_j)) * ||c_i - c_j||²
//
// Starting from a histogram of the input image, we repeatedly merge the
// pair with lowest merge cost until `max_colors` clusters remain.
// Published results have PNN beating median-cut at low palette counts
// (≤32) at the cost of higher runtime — but since our typical N is
// 16–64 and histogram bins top out at ~4096 (OCS) or 32768 (coarse AGA
// grid), the O(B²) init is fast enough.
//
// Nearest-neighbor pointers plus lazy heap validation give O(B²) init
// and O((B-N)·B) merges in the worst case.
// ===========================================================================

Palette pnn_quantize(std::span<const Color3f> colors,
                     std::size_t max_colors,
                     int palette_diversity,
                     bool snap_to_ocs) {

    if (max_colors == 0) max_colors = 1;
    if (colors.empty()) {
        return Palette{"pnn", {Color3f{0.0f, 0.0f, 0.0f}}};
    }

    // Each cluster stores a weighted centroid in OKLab AND a "representative"
    // that the cost function uses. For continuous PNN these are identical.
    // For OCS PNN the representative is the OCS color nearest the weighted
    // centroid, so every merge cost and the final palette respect the
    // discrete 12-bit gamut.
    struct Cluster {
        // Weighted sums (in OKLab); centroid = sum / weight
        double sum_L{}, sum_a{}, sum_b{};
        float weight{};
        OKLab rep{};          // representative: either centroid or OCS-snapped
        std::size_t nn{};
        float nn_cost{};
        bool alive{};
    };
    std::vector<Cluster> clusters;

    auto refresh_rep = [&](Cluster& c) {
        auto w = static_cast<double>(c.weight);
        OKLab centroid{
            static_cast<float>(c.sum_L / w),
            static_cast<float>(c.sum_a / w),
            static_cast<float>(c.sum_b / w),
        };
        if (snap_to_ocs) {
            auto rgb = color_space::oklab_to_linear(centroid).clamped();
            rgb = palette::quantize_to_ocs(rgb);
            c.rep = color_space::linear_to_oklab(rgb);
        } else {
            c.rep = centroid;
        }
    };

    if (snap_to_ocs) {
        // Initialize one cluster per distinct OCS color that the image uses.
        // Max 4096 clusters, cheap O(B²).
        auto& lut = ocs_lut();
        std::array<std::uint32_t, 4096> hist{};
        std::array<double, 4096> sum_L{}, sum_a{}, sum_b{};
        for (auto& pixel : colors) {
            auto ocs = palette::linear_to_ocs(pixel);
            auto lab = color_space::linear_to_oklab(pixel);
            ++hist[ocs];
            sum_L[ocs] += static_cast<double>(lab.L);
            sum_a[ocs] += static_cast<double>(lab.a);
            sum_b[ocs] += static_cast<double>(lab.b);
        }
        clusters.reserve(4096);
        for (std::uint16_t i = 0; i < 4096; ++i) {
            if (hist[i] == 0) continue;
            Cluster c{};
            c.sum_L  = sum_L[i];
            c.sum_a  = sum_a[i];
            c.sum_b  = sum_b[i];
            c.weight = static_cast<float>(hist[i]);
            c.rep    = lut.oklab[i];  // OCS color exactly
            c.alive  = true;
            c.nn_cost = std::numeric_limits<float>::max();
            clusters.push_back(c);
        }
    } else {
        // Continuous-space histogram: 24³ = 13824 bins in OKLab.
        constexpr std::size_t BINS = 24;
        constexpr float L_min = 0.0f, L_max = 1.0f;
        constexpr float a_min = -0.5f, a_max = 0.5f;
        constexpr float b_min = -0.5f, b_max = 0.5f;
        constexpr float L_scale = static_cast<float>(BINS) / (L_max - L_min);
        constexpr float a_scale = static_cast<float>(BINS) / (a_max - a_min);
        constexpr float b_scale = static_cast<float>(BINS) / (b_max - b_min);

        struct Bin {
            double L{}, a{}, b{};
            std::uint32_t weight{};
        };
        std::vector<Bin> hist(BINS * BINS * BINS);
        for (auto& pixel : colors) {
            auto lab = color_space::linear_to_oklab(pixel);
            auto iL = static_cast<std::size_t>(
                std::clamp((lab.L - L_min) * L_scale, 0.0f,
                           static_cast<float>(BINS - 1)));
            auto ia = static_cast<std::size_t>(
                std::clamp((lab.a - a_min) * a_scale, 0.0f,
                           static_cast<float>(BINS - 1)));
            auto ib = static_cast<std::size_t>(
                std::clamp((lab.b - b_min) * b_scale, 0.0f,
                           static_cast<float>(BINS - 1)));
            auto& bin = hist[(iL * BINS + ia) * BINS + ib];
            bin.L += static_cast<double>(lab.L);
            bin.a += static_cast<double>(lab.a);
            bin.b += static_cast<double>(lab.b);
            ++bin.weight;
        }
        clusters.reserve(1024);
        for (auto& bin : hist) {
            if (bin.weight == 0) continue;
            Cluster c{};
            c.sum_L  = bin.L;
            c.sum_a  = bin.a;
            c.sum_b  = bin.b;
            c.weight = static_cast<float>(bin.weight);
            c.rep    = OKLab{
                static_cast<float>(bin.L / bin.weight),
                static_cast<float>(bin.a / bin.weight),
                static_cast<float>(bin.b / bin.weight),
            };
            c.alive  = true;
            c.nn_cost = std::numeric_limits<float>::max();
            clusters.push_back(c);
        }
    }

    auto merge_cost = [](const Cluster& a, const Cluster& b) {
        float d = oklab_dist_sq(a.rep, b.rep);
        float w = (a.weight * b.weight) / (a.weight + b.weight);
        return w * d;
    };

    // If we already have few enough clusters, return them directly
    if (clusters.size() <= max_colors) {
        Palette result;
        result.name = "pnn";
        for (auto& c : clusters) {
            result.colors.push_back(
                color_space::oklab_to_linear(c.rep).clamped());
        }
        std::sort(result.colors.begin(), result.colors.end(),
                  [](const Color3f& a, const Color3f& b) {
                      return color_space::linear_to_oklab(a).L <
                             color_space::linear_to_oklab(b).L;
                  });
        if (palette_diversity > 0)
            apply_palette_diversity(result, colors, palette_diversity, false);
        return result;
    }

    // Initialize each cluster's nearest neighbor (O(B²) one-time).
    auto update_nn = [&](std::size_t i) {
        float best = std::numeric_limits<float>::max();
        std::size_t best_j = i;
        for (std::size_t j = 0; j < clusters.size(); ++j) {
            if (j == i || !clusters[j].alive) continue;
            float c = merge_cost(clusters[i], clusters[j]);
            if (c < best) { best = c; best_j = j; }
        }
        clusters[i].nn = best_j;
        clusters[i].nn_cost = best;
    };

    for (std::size_t i = 0; i < clusters.size(); ++i) update_nn(i);

    std::size_t alive_count = clusters.size();
    while (alive_count > max_colors) {
        // Find alive cluster with lowest merge cost
        std::size_t best_i = 0;
        float best_c = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            if (!clusters[i].alive) continue;
            if (clusters[i].nn_cost < best_c) {
                best_c = clusters[i].nn_cost;
                best_i = i;
            }
        }
        auto j = clusters[best_i].nn;
        if (!clusters[j].alive || j == best_i) {
            // Stale pointer; refresh and retry
            update_nn(best_i);
            continue;
        }

        // Merge j into best_i. Accumulate weighted sums (not weighted
        // reps — reps are snapped discrete values and don't compose).
        auto& a = clusters[best_i];
        auto& b = clusters[j];
        a.sum_L  += b.sum_L;
        a.sum_a  += b.sum_a;
        a.sum_b  += b.sum_b;
        a.weight += b.weight;
        refresh_rep(a);
        b.alive = false;
        --alive_count;

        // Any cluster whose NN was best_i or j needs its NN recomputed.
        // best_i itself also needs a new NN.
        update_nn(best_i);
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            if (!clusters[i].alive || i == best_i) continue;
            if (clusters[i].nn == best_i || clusters[i].nn == j) {
                update_nn(i);
            } else {
                // Check if best_i (with its updated centroid) is now closer
                // than its current nn.
                float c = merge_cost(clusters[i], clusters[best_i]);
                if (c < clusters[i].nn_cost) {
                    clusters[i].nn = best_i;
                    clusters[i].nn_cost = c;
                }
            }
        }
    }

    // Collect surviving clusters
    Palette result;
    result.name = "pnn";
    for (auto& c : clusters) {
        if (!c.alive) continue;
        result.colors.push_back(
            color_space::oklab_to_linear(c.rep).clamped());
    }

    // Sort by perceptual luminance for consistent ordering
    std::sort(result.colors.begin(), result.colors.end(),
              [](const Color3f& a, const Color3f& b) {
                  return color_space::linear_to_oklab(a).L <
                         color_space::linear_to_oklab(b).L;
              });

    if (palette_diversity > 0)
        apply_palette_diversity(result, colors, palette_diversity, false);

    return result;
}

// ===========================================================================
// Palette diversity pass — inspired by ham_convert's diversity option.
//
// Iteratively replaces the two closest palette entries (in OKLab) with the
// image pixel that is currently worst-served by the palette (the pixel with
// the largest min-distance to any entry). After each swap, run a few
// k-means iterations restricted to a small neighborhood around the swapped
// entry so the rest of the palette stays stable.
//
// Level 0: skip. Level N runs up to N swap rounds. The pass is conservative:
// a swap is only committed if the candidate pixel is more distant from the
// existing palette than the closest-pair distance — otherwise the swap
// would create a new near-duplicate.
// ===========================================================================

namespace {

std::pair<std::size_t, std::size_t> find_closest_pair(
    std::span<const color_space::OKLab> pal_lab, float& out_dist) {

    float best = std::numeric_limits<float>::max();
    std::size_t ia = 0, ib = 1;
    for (std::size_t i = 0; i < pal_lab.size(); ++i) {
        for (std::size_t j = i + 1; j < pal_lab.size(); ++j) {
            float d = oklab_dist_sq(pal_lab[i], pal_lab[j]);
            if (d < best) { best = d; ia = i; ib = j; }
        }
    }
    out_dist = best;
    return {ia, ib};
}

// Compute total weighted SSE (sum over samples of squared distance to nearest
// palette entry). Used as the objective for greedy swap acceptance.
//
// SoA conversion happens inside: each call is O(n + n*k); the SoA build
// is dwarfed by the SIMD assignment kernel except at trivially small
// sample counts (where the function isn't hot anyway).
float palette_total_sse(
    std::span<const color_space::OKLab> samples_lab,
    std::span<const color_space::OKLab> pal_lab) {

    auto soa = build_soa_lab(samples_lab);
    std::size_t k_n = pal_lab.size();
    std::vector<float> cL(k_n), cA(k_n), cB(k_n);
    for (std::size_t k = 0; k < k_n; ++k) {
        cL[k] = pal_lab[k].L * color_space::WEIGHT_L;
        cA[k] = pal_lab[k].a * color_space::WEIGHT_A;
        cB[k] = pal_lab[k].b * color_space::WEIGHT_B;
    }
    std::vector<float> best_d(soa.padded_n,
                              std::numeric_limits<float>::max());
    std::vector<std::uint32_t> best_k(soa.padded_n, 0u);
    simd_assign_nearest(soa.L.data(), soa.a.data(), soa.b.data(), soa.padded_n,
                        cL.data(), cA.data(), cB.data(), k_n,
                        best_d.data(), best_k.data());

    float total = 0.0f;
    for (std::size_t i = 0; i < soa.valid_n; ++i) total += best_d[i];
    return total;
}

// Run a few k-means iterations in OKLab, keeping entries mutable. Returns
// the refined palette (copy).
std::vector<color_space::OKLab> kmeans_refine(
    std::span<const color_space::OKLab> samples_lab,
    std::vector<color_space::OKLab> centroids,
    int iterations) {

    auto soa = build_soa_lab(samples_lab);
    auto n = centroids.size();
    std::vector<float> cL(n), cA(n), cB(n);
    for (std::size_t k = 0; k < n; ++k) {
        cL[k] = centroids[k].L * color_space::WEIGHT_L;
        cA[k] = centroids[k].a * color_space::WEIGHT_A;
        cB[k] = centroids[k].b * color_space::WEIGHT_B;
    }
    std::vector<std::uint32_t> assignments(soa.padded_n, 0u);
    std::vector<float>         pixel_errors(soa.padded_n,
                                            std::numeric_limits<float>::max());

    for (int iter = 0; iter < iterations; ++iter) {
        std::fill(pixel_errors.begin(), pixel_errors.end(),
                  std::numeric_limits<float>::max());
        std::fill(assignments.begin(), assignments.end(), 0u);
        simd_assign_nearest(soa.L.data(), soa.a.data(), soa.b.data(),
                            soa.padded_n,
                            cL.data(), cA.data(), cB.data(), n,
                            pixel_errors.data(), assignments.data());

        struct Acc { double L{}, a{}, b{}; std::size_t n{}; };
        std::vector<Acc> acc(n);
        for (std::size_t i = 0; i < soa.valid_n; ++i) {
            auto& A = acc[assignments[i]];
            A.L += static_cast<double>(soa.L[i]) / color_space::WEIGHT_L;
            A.a += static_cast<double>(soa.a[i]) / color_space::WEIGHT_A;
            A.b += static_cast<double>(soa.b[i]) / color_space::WEIGHT_B;
            ++A.n;
        }
        bool changed = false;
        for (std::size_t k = 0; k < n; ++k) {
            if (acc[k].n == 0) continue;
            auto dn = static_cast<double>(acc[k].n);
            color_space::OKLab nlab{
                static_cast<float>(acc[k].L / dn),
                static_cast<float>(acc[k].a / dn),
                static_cast<float>(acc[k].b / dn),
            };
            if (oklab_dist_sq(nlab, centroids[k]) > 1e-12f) {
                changed = true;
                centroids[k] = nlab;
                cL[k] = nlab.L * color_space::WEIGHT_L;
                cA[k] = nlab.a * color_space::WEIGHT_A;
                cB[k] = nlab.b * color_space::WEIGHT_B;
            }
        }
        if (!changed) break;
    }
    return centroids;
}

void apply_palette_diversity(Palette& palette,
                             std::span<const Color3f> pixels,
                             int diversity_level,
                             bool snap_to_ocs) {

    if (diversity_level <= 0 || palette.colors.size() < 3 || pixels.empty())
        return;

    // Subsample large images for responsiveness
    std::vector<Color3f> work(pixels.begin(), pixels.end());
    constexpr std::size_t max_samples = 131072;
    if (work.size() > max_samples) {
        auto stride = work.size() / max_samples;
        std::vector<Color3f> sampled;
        sampled.reserve(max_samples);
        for (std::size_t i = 0; i < work.size(); i += stride)
            sampled.push_back(work[i]);
        work = std::move(sampled);
    }

    std::vector<color_space::OKLab> samples_lab(work.size());
    for (std::size_t i = 0; i < work.size(); ++i)
        samples_lab[i] = color_space::linear_to_oklab(work[i]);

    // SoA samples shared across all hot loops in this function. Built once
    // (the AOS samples_lab is kept around for the AOS-out push at the end).
    auto soa_samples = build_soa_lab(samples_lab);

    // Working palette in OKLab
    std::vector<color_space::OKLab> pal_lab(palette.colors.size());
    for (std::size_t i = 0; i < palette.colors.size(); ++i)
        pal_lab[i] = color_space::linear_to_oklab(palette.colors[i]);

    // Current best SSE
    float best_sse = palette_total_sse(samples_lab, pal_lab);

    // Cluster-assignment error accumulation: find the cluster with highest
    // total SSE — that's where we should reseed a centroid. One SIMD assign
    // pass populates best_d / best_k for every sample; both the cluster-
    // error accumulation and the farthest-sample scan reuse those buffers.
    auto find_worst_cluster_centroid = [&](std::vector<color_space::OKLab>& out_centroid) -> bool {
        std::size_t k_n = pal_lab.size();
        std::vector<float> pcL(k_n), pcA(k_n), pcB(k_n);
        for (std::size_t k = 0; k < k_n; ++k) {
            pcL[k] = pal_lab[k].L * color_space::WEIGHT_L;
            pcA[k] = pal_lab[k].a * color_space::WEIGHT_A;
            pcB[k] = pal_lab[k].b * color_space::WEIGHT_B;
        }
        std::vector<float> best_d(soa_samples.padded_n,
                                  std::numeric_limits<float>::max());
        std::vector<std::uint32_t> best_k(soa_samples.padded_n, 0u);
        simd_assign_nearest(
            soa_samples.L.data(), soa_samples.a.data(), soa_samples.b.data(),
            soa_samples.padded_n,
            pcL.data(), pcA.data(), pcB.data(), k_n,
            best_d.data(), best_k.data());

        struct Acc { double L{}, a{}, b{}, err{}; std::size_t n{}; };
        std::vector<Acc> acc(k_n);
        for (std::size_t i = 0; i < soa_samples.valid_n; ++i) {
            auto& A = acc[best_k[i]];
            A.L += static_cast<double>(samples_lab[i].L);
            A.a += static_cast<double>(samples_lab[i].a);
            A.b += static_cast<double>(samples_lab[i].b);
            A.err += static_cast<double>(best_d[i]);
            ++A.n;
        }
        // Pick cluster with largest total error, then split it: new centroid
        // at the pixel farthest from the cluster's current centroid.
        std::size_t worst_k = 0; double worst_err = -1.0;
        for (std::size_t k = 0; k < acc.size(); ++k) {
            if (acc[k].err > worst_err) { worst_err = acc[k].err; worst_k = k; }
        }
        if (acc[worst_k].n < 2) return false;
        float fd = -1.0f; std::size_t fi = 0;
        for (std::size_t i = 0; i < soa_samples.valid_n; ++i) {
            if (best_k[i] == worst_k && best_d[i] > fd) {
                fd = best_d[i]; fi = i;
            }
        }
        out_centroid.clear();
        out_centroid.push_back(samples_lab[fi]);
        return true;
    };

    // Diversity threshold: for each level, allow closer pairs to be merged.
    // Level 1 = very conservative (only merge near-identical pairs), level 5
    // = aggressive (merge pairs up to 2x the average nearest-neighbor dist).
    float avg_nn = 0.0f;
    {
        for (std::size_t i = 0; i < pal_lab.size(); ++i) {
            float best = std::numeric_limits<float>::max();
            for (std::size_t j = 0; j < pal_lab.size(); ++j) {
                if (i == j) continue;
                float d = oklab_dist_sq(pal_lab[i], pal_lab[j]);
                if (d < best) best = d;
            }
            avg_nn += std::sqrt(best);
        }
        avg_nn /= static_cast<float>(pal_lab.size());
    }
    // Threshold: allow closer pairs to be merged at higher diversity levels.
    //   level 1  →  0.35 * avg_nn  (very conservative: only near-duplicates)
    //   level 5  →  0.95 * avg_nn  (merge pairs up to the average)
    //   level 9  →  1.55 * avg_nn  (merge pairs 50% wider than average)
    float merge_threshold_dist =
        avg_nn * (0.2f + 0.15f * static_cast<float>(diversity_level));
    float merge_threshold_sq = merge_threshold_dist * merge_threshold_dist;

    // Swap budget scales with level so high settings actually get to try
    // more merges (low levels cap out quickly once the threshold is met).
    //   level 1  →  ~N swaps
    //   level 5  →  ~3N
    //   level 9  →  ~5N
    std::size_t max_swaps =
        (palette.colors.size() *
         (1 + static_cast<std::size_t>(std::max(0, diversity_level - 1)) / 2)) * 2;
    std::size_t committed = 0;

    for (std::size_t attempt = 0; attempt < max_swaps; ++attempt) {
        float pair_dist_sq = 0.0f;
        auto [ia, ib] = find_closest_pair(pal_lab, pair_dist_sq);
        if (pair_dist_sq > merge_threshold_sq) break;  // nothing close enough

        // Candidate palette: merge ia and ib into their midpoint, reseed ib
        // from the worst-served cluster centroid.
        auto candidate = pal_lab;
        candidate[ia] = color_space::OKLab{
            (pal_lab[ia].L + pal_lab[ib].L) * 0.5f,
            (pal_lab[ia].a + pal_lab[ib].a) * 0.5f,
            (pal_lab[ia].b + pal_lab[ib].b) * 0.5f,
        };
        std::vector<color_space::OKLab> reseed;
        if (!find_worst_cluster_centroid(reseed)) break;
        candidate[ib] = reseed[0];

        // Re-converge with k-means (5 iterations is plenty when starting
        // from a near-optimal palette).
        auto refined = kmeans_refine(samples_lab, candidate, 5);

        // Optionally snap each entry to OCS precision for OCS modes, so the
        // SSE we measure reflects what the hardware will actually display.
        if (snap_to_ocs) {
            for (auto& c : refined) {
                auto rgb = color_space::oklab_to_linear(c).clamped();
                rgb = palette::quantize_to_ocs(rgb);
                c = color_space::linear_to_oklab(rgb);
            }
        }

        // Guard against OCS collisions: if the refined palette ends up with
        // fewer unique colors than we started with, the "improvement" came
        // from losing a palette slot, which hurts the final dithered output.
        auto count_unique_ocs =
            [](std::span<const color_space::OKLab> p) {
                std::vector<std::uint16_t> ocs;
                ocs.reserve(p.size());
                for (auto& c : p) {
                    auto rgb = color_space::oklab_to_linear(c).clamped();
                    ocs.push_back(palette::linear_to_ocs(rgb));
                }
                std::sort(ocs.begin(), ocs.end());
                ocs.erase(std::unique(ocs.begin(), ocs.end()), ocs.end());
                return ocs.size();
            };

        if (snap_to_ocs &&
            count_unique_ocs(refined) < count_unique_ocs(pal_lab)) {
            // Refuse the swap: it would collapse colors.
            break;
        }

        float new_sse = palette_total_sse(samples_lab, refined);
        if (new_sse < best_sse) {
            best_sse = new_sse;
            pal_lab = std::move(refined);
            ++committed;
        } else {
            // The merge didn't help; stop — further swaps are unlikely to help.
            break;
        }
    }

    if (committed == 0) return;

    // Write back the refined palette
    for (std::size_t i = 0; i < palette.colors.size(); ++i) {
        auto rgb = color_space::oklab_to_linear(pal_lab[i]).clamped();
        if (snap_to_ocs) rgb = palette::quantize_to_ocs(rgb);
        palette.colors[i] = rgb;
    }
}

} // namespace

// ===========================================================================
// quantize() entry point
// ===========================================================================

Result<Palette> quantize(const Image& image, std::size_t max_colors,
                         Algorithm algo, int palette_diversity) {
    if (max_colors == 0 || max_colors > 256) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("Palette size must be 1-256, got {}", max_colors),
        }};
    }

    if (image.width() == 0 || image.height() == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Image dimensions must be non-zero",
        }};
    }

    switch (algo) {
    case Algorithm::median_cut:
        return median_cut(image.pixels(), max_colors, palette_diversity);
    case Algorithm::ocs_bruteforce:
        return ocs_bruteforce_quantize(image.pixels(), max_colors,
                                       palette_diversity);
    case Algorithm::pnn:
        // Continuous-space PNN for AGA, OCS-snapped PNN for OCS/STF.
        // Detection via an OCS snap test isn't available here, so the
        // caller selects snap via the helper overload. Default: continuous.
        return pnn_quantize(image.pixels(), max_colors, palette_diversity,
                            /*snap_to_ocs=*/false);
    }

    return median_cut(image.pixels(), max_colors, palette_diversity);
}

// ===========================================================================
// Public wrapper for the diversity pass
// ===========================================================================

void diversify_palette(Palette& palette,
                       std::span<const Color3f> pixels,
                       int diversity_level,
                       bool snap_to_ocs) {
    apply_palette_diversity(palette, pixels, diversity_level, snap_to_ocs);
}

// ===========================================================================
// Dither-aware palette refinement
// ===========================================================================

Result<Palette> refine_with_dither(
    const Image& image,
    const Palette& initial_palette,
    const dither::Settings& dither_settings,
    amiga::Chipset chipset,
    amiga::Mode mode,
    std::size_t max_iterations,
    const std::vector<bool>& locked) {

    if (dither_settings.method == dither::Method::none)
        return initial_palette;  // nothing to refine against

    // STF 9-bit gamut (512 colors) is too coarse for centroid-based
    // refinement — the brute-force quantizer already finds the optimal
    // discrete palette, and snapping continuous centroids to 9-bit can
    // collapse nearby colors to the same value.
    if (amiga::is_stf(mode))
        return initial_palette;

    auto pal = initial_palette;
    auto num_colors = pal.colors.size();
    if (num_colors < 2) return pal;

    bool is_stf = amiga::is_stf(mode);
    bool is_vga_mode = amiga::is_vga(mode);
    bool is_ega_mode = amiga::is_ega(mode);
    bool snap_to_discrete = (chipset != amiga::Chipset::aga) || is_stf ||
                            is_vga_mode || is_ega_mode;

    auto w = image.width();
    auto h = image.height();

    // Precompute palette in OKLab once per iteration (rebuilt below).
    std::vector<color_space::OKLab> pal_lab(num_colors);

    // Per-pixel nearest-palette assignment buffer (nearest-color, NOT
    // dither-driven). The previous implementation used the dither's
    // index buffer here, which has pixels rerouted across cluster
    // boundaries by neighbour error — that breaks K-means' convergence
    // guarantee (assignment metric ≠ centroid metric) and pollutes
    // each cluster's centroid with pixels that don't perceptually
    // belong. Nearest-match is the K-means assignment libimagequant
    // uses (kmeans.rs:92-108); dither-awareness comes from the
    // neighbour-coherence weight below, not from re-routing pixels.
    std::vector<std::uint8_t> nearest(w * h);
    std::vector<color_space::OKLab> img_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            img_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    // Per-pixel feedback weight (libimagequant `adjusted_weight`,
    // kmeans.rs:97-108). Starts at 1.0; each iteration's residual
    // perceptual error rescales it so persistent high-error pixels
    // pull progressively harder on their cluster centroid in
    // subsequent iterations. Capped to keep tail outliers from
    // dominating.
    std::vector<float> feedback(w * h, 1.0f);
    std::vector<float> pixel_err(w * h, 0.0f);

    for (std::size_t iter = 0; iter < max_iterations; ++iter) {
        for (std::size_t i = 0; i < num_colors; ++i)
            pal_lab[i] = color_space::linear_to_oklab(pal.colors[i]);

        // Per-pixel nearest-palette assignment in OKLab; cache the
        // residual error for the feedback rule below.
        for (std::size_t i = 0; i < w * h; ++i) {
            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            for (std::size_t k = 0; k < num_colors; ++k) {
                float d = oklab_dist_sq(img_lab[i], pal_lab[k]);
                if (d < best_d) { best_d = d; best_k = k; }
            }
            nearest[i] = static_cast<std::uint8_t>(best_k);
            pixel_err[i] = best_d;
        }

        // Spatial Color Quantization: weight each pixel's contribution to
        // its cluster centroid by how many of its 4-neighbors share the
        // same nearest-palette assignment. Pixels in contiguous same-
        // colour regions pull harder on the centroid; isolated single-
        // pixel "islands" pull less.
        //   0 matching neighbours → weight 1.0  (isolated pixel)
        //   4 matching neighbours → weight 3.0  (interior of a region)
        struct Acc { double L{}, a{}, b{}, w{}; };
        std::vector<Acc> acc(num_colors);

        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto idx = nearest[y * w + x];
                if (idx >= num_colors) continue;

                int neighbors = 0;
                if (x > 0     && nearest[y * w + (x - 1)] == idx) ++neighbors;
                if (x + 1 < w && nearest[y * w + (x + 1)] == idx) ++neighbors;
                if (y > 0     && nearest[(y - 1) * w + x] == idx) ++neighbors;
                if (y + 1 < h && nearest[(y + 1) * w + x] == idx) ++neighbors;

                double base = 1.0 + static_cast<double>(neighbors) * 0.5;
                double weight = base * static_cast<double>(feedback[y * w + x]);
                const auto& lab = img_lab[y * w + x];
                acc[idx].L += static_cast<double>(lab.L) * weight;
                acc[idx].a += static_cast<double>(lab.a) * weight;
                acc[idx].b += static_cast<double>(lab.b) * weight;
                acc[idx].w += weight;
            }
        }
        // dither_settings is consulted via the API but the assignment
        // pass uses nearest-match (not dither) — the aware-ness comes
        // from the feedback weight scaling above. Suppress unused-arg
        // warnings while keeping the parameter in the public API.
        (void)dither_settings;

        // Update palette: move each unlocked slot to its weighted centroid
        bool changed = false;
        for (std::size_t k = 0; k < num_colors; ++k) {
            if (!locked.empty() && k < locked.size() && locked[k]) continue;
            if (acc[k].w < 1e-9) continue;

            auto dn = acc[k].w;
            auto lab = color_space::OKLab{
                static_cast<float>(acc[k].L / dn),
                static_cast<float>(acc[k].a / dn),
                static_cast<float>(acc[k].b / dn),
            };
            auto new_color = color_space::oklab_to_linear(lab).clamped();
            if (is_stf) new_color = palette::quantize_to_stf(new_color);
            else if (is_vga_mode) new_color = palette::quantize_to_vga(new_color);
            else if (is_ega_mode) new_color = palette::quantize_to_ega(new_color);
            else if (snap_to_discrete) new_color = palette::quantize_to_ocs(new_color);

            // Check convergence (discrete: exact comparison; AGA: epsilon)
            auto& old_color = pal.colors[k];
            if (snap_to_discrete) {
                auto oh = is_stf ? palette::linear_to_stf(old_color)
                                 : is_vga_mode ? palette::linear_to_vga(old_color)
                                 : is_ega_mode ? std::uint32_t{palette::linear_to_ega(old_color)}
                                 : palette::linear_to_ocs(old_color);
                auto nh = is_stf ? palette::linear_to_stf(new_color)
                                 : is_vga_mode ? palette::linear_to_vga(new_color)
                                 : is_ega_mode ? std::uint32_t{palette::linear_to_ega(new_color)}
                                 : palette::linear_to_ocs(new_color);
                if (oh != nh) { old_color = new_color; changed = true; }
            } else {
                auto d = oklab_dist_sq(
                    color_space::linear_to_oklab(old_color),
                    color_space::linear_to_oklab(new_color));
                if (d > 1e-10f) { old_color = new_color; changed = true; }
            }
        }

        if (!changed) break;

        // Update per-pixel feedback weights (libimagequant's
        // adjust_weight rule, normalised against the per-iteration p99
        // error so typical pixels stay near weight 1 and only the
        // long-tail outliers grow). Capped at 8 to keep extreme
        // pixels from monopolising the centroid.
        if (iter + 1 < max_iterations) {
            std::vector<float> sorted_err = pixel_err;
            auto p99_idx = std::min(
                sorted_err.size() - 1,
                sorted_err.size() * 99 / 100);
            std::nth_element(sorted_err.begin(),
                             sorted_err.begin() +
                                 static_cast<std::ptrdiff_t>(p99_idx),
                             sorted_err.end());
            float p99 = std::max(sorted_err[p99_idx], 1e-6f);
            for (std::size_t i = 0; i < w * h; ++i) {
                float d_norm = std::min(pixel_err[i] / p99, 1.5f);
                float target = 1.0f + 4.0f * d_norm;
                feedback[i] = std::min(
                    0.5f * feedback[i] + 0.5f * target, 8.0f);
            }
        }
    }

    return pal;
}

// ---------------------------------------------------------------------------
// EGA histogram quantizer — see quantize.hpp for rationale.
// ---------------------------------------------------------------------------
Palette ega_histogram(const Image& image, std::size_t K) {
    std::array<std::uint64_t, 64> hist{};
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            auto e = palette::linear_to_ega(image[x, y]);
            hist[e]++;
        }
    }
    std::array<color_space::OKLab, 64> gamut_lab;
    std::array<Color3f, 64> gamut_rgb;
    for (std::size_t i = 0; i < 64; ++i) {
        gamut_rgb[i] = palette::ega_to_linear(static_cast<std::uint8_t>(i));
        gamut_lab[i] = color_space::linear_to_oklab(gamut_rgb[i]);
    }

    std::vector<std::uint8_t> picked;
    picked.reserve(K);

    // Seed 1: highest-frequency non-zero bucket.
    {
        std::uint64_t best_count = 0; std::uint8_t best = 0;
        for (std::size_t i = 0; i < 64; ++i) if (hist[i] > best_count) {
            best_count = hist[i]; best = static_cast<std::uint8_t>(i);
        }
        picked.push_back(best);
    }

    // Seed 2..K: weighted by (count × min_d²_to_existing_picks).
    while (picked.size() < K) {
        std::array<double, 64> score{};
        double total = 0;
        for (std::size_t i = 0; i < 64; ++i) {
            if (hist[i] == 0) continue;
            double min_d = std::numeric_limits<double>::infinity();
            for (auto p : picked) {
                if (p == i) { min_d = 0; break; }
                auto& a = gamut_lab[i]; auto& b = gamut_lab[p];
                double dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                double d = dL*dL + da*da + db*db;
                if (d < min_d) min_d = d;
            }
            score[i] = static_cast<double>(hist[i]) * min_d;
            total += score[i];
        }
        if (total <= 0) break;
        std::uint8_t best = 0; double best_s = -1;
        for (std::size_t i = 0; i < 64; ++i) if (score[i] > best_s) {
            best_s = score[i]; best = static_cast<std::uint8_t>(i);
        }
        picked.push_back(best);
    }

    // Lloyd refinement in EGA space.
    constexpr int kMaxIters = 16;
    for (int iter = 0; iter < kMaxIters; ++iter) {
        struct Acc { double L{}, a{}, b{}; double w{}; };
        std::vector<Acc> acc(picked.size());
        for (std::size_t i = 0; i < 64; ++i) {
            if (hist[i] == 0) continue;
            float best_d = std::numeric_limits<float>::infinity();
            std::size_t best_k = 0;
            for (std::size_t k = 0; k < picked.size(); ++k) {
                auto& a = gamut_lab[i]; auto& b = gamut_lab[picked[k]];
                float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                float d = dL*dL + da*da + db*db;
                if (d < best_d) { best_d = d; best_k = k; }
            }
            auto w = static_cast<double>(hist[i]);
            acc[best_k].L += static_cast<double>(gamut_lab[i].L) * w;
            acc[best_k].a += static_cast<double>(gamut_lab[i].a) * w;
            acc[best_k].b += static_cast<double>(gamut_lab[i].b) * w;
            acc[best_k].w += w;
        }
        std::vector<std::uint8_t> new_picked(picked.size());
        std::array<bool, 64> taken{};
        bool changed = false;
        std::vector<std::size_t> order(picked.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](auto a, auto b) { return acc[a].w > acc[b].w; });
        for (auto k : order) {
            if (acc[k].w == 0) {
                if (!taken[picked[k]]) {
                    new_picked[k] = picked[k]; taken[picked[k]] = true;
                    continue;
                }
            }
            auto cent = (acc[k].w > 0)
                ? color_space::OKLab{
                      static_cast<float>(acc[k].L / acc[k].w),
                      static_cast<float>(acc[k].a / acc[k].w),
                      static_cast<float>(acc[k].b / acc[k].w)}
                : gamut_lab[picked[k]];
            std::uint8_t best = 0; float best_d = std::numeric_limits<float>::infinity();
            for (std::size_t g = 0; g < 64; ++g) {
                if (taken[g]) continue;
                auto& gl = gamut_lab[g];
                float dL = cent.L - gl.L, da = cent.a - gl.a, db = cent.b - gl.b;
                float d = dL*dL + da*da + db*db;
                if (d < best_d) { best_d = d; best = static_cast<std::uint8_t>(g); }
            }
            new_picked[k] = best;
            taken[best] = true;
            if (best != picked[k]) changed = true;
        }
        picked = new_picked;
        if (!changed) break;
    }

    Palette pal;
    pal.name = "ega";
    pal.colors.reserve(picked.size());
    for (auto p : picked) pal.colors.push_back(gamut_rgb[p]);
    return pal;
}

} // namespace png2amiga::quantize
