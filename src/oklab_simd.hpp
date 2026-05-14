#pragma once

// SoA OKLab palette + SIMD argmin shared between modules that all need
// the same nearest-color-over-N-palette-entries inner loop. The
// scalar form is the dominant inner cost in quantize::kmeans_refine,
// copper::build_swap_scratch / refresh_swap_scratch, strips' base-
// index assignment, snes_io's tile / pixel palette mapping, and
// genesis's k-means. Each ran a private copy of this pattern; this
// header centralises the SoA layout + AVX2 / WASM SIMD argmin so
// future call sites just include it.
//
// Weighted variants (used by the quantizer's perceptual k-means) keep
// a separate scaffold in quantize.cpp's anonymous namespace because
// the WEIGHT_L/A/B pre-multiply means the SoA stores a different
// representation; we don't want one helper to do both jobs and pay
// the conditional cost at every argmin.

#include "color_space.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define PNG2AMIGA_OKLAB_SIMD_AVX2 0
#define PNG2AMIGA_OKLAB_SIMD_WASM 1
#elif defined(__AVX2__)
#include <immintrin.h>
#define PNG2AMIGA_OKLAB_SIMD_AVX2 1
#define PNG2AMIGA_OKLAB_SIMD_WASM 0
#else
#define PNG2AMIGA_OKLAB_SIMD_AVX2 0
#define PNG2AMIGA_OKLAB_SIMD_WASM 0
#endif

namespace png2amiga::oklab_simd {

// Capacity covers every indexed mode we run through these loops:
// VGA-13h chunky 256, AGA d=8 256, EHB 64 (32 base × 2), HAM bases
// 16–64, SNES Mode 7 256. Fixed-size stack-allocated array — no
// malloc on the hot path.
inline constexpr std::size_t kMaxN = 256;

struct PaletteSoA {
    alignas(32) std::array<float, kMaxN> L{};
    alignas(32) std::array<float, kMaxN> a{};
    alignas(32) std::array<float, kMaxN> b{};
    std::size_t n{};       // real entries
    std::size_t padded{};  // padded up to SIMD width
};

inline void fill(std::span<const png2amiga::color_space::OKLab> pal,
                 const std::vector<bool>& excluded,
                 PaletteSoA& s) noexcept {
#if PNG2AMIGA_OKLAB_SIMD_AVX2
    constexpr std::size_t W = 8;
#else
    constexpr std::size_t W = 4;
#endif
    s.n = pal.size();
    s.padded = std::min(((s.n + W - 1) / W) * W, kMaxN);
    for (std::size_t i = 0; i < s.padded; ++i) {
        if (i < s.n && (excluded.empty() || !excluded[i])) {
            s.L[i] = pal[i].L;
            s.a[i] = pal[i].a;
            s.b[i] = pal[i].b;
        } else {
            // Sentinel — huge L pushes the lane out of contention so
            // padding + excluded slots are never picked.
            s.L[i] = 1e30f;
            s.a[i] = 0.0f;
            s.b[i] = 0.0f;
        }
    }
}

// Convenience overload — no exclusion mask.
inline void fill(std::span<const png2amiga::color_space::OKLab> pal, PaletteSoA& s) noexcept {
    fill(pal, std::vector<bool>{}, s);
}

struct Result {
    std::size_t index;
    float dist_sq;
};

inline Result argmin(png2amiga::color_space::OKLab px, const PaletteSoA& s) noexcept {
    const std::size_t n = s.padded;
    if (n == 0) return {0, 0.0f};
#if PNG2AMIGA_OKLAB_SIMD_AVX2
    const __m256 pL = _mm256_set1_ps(px.L);
    const __m256 pa = _mm256_set1_ps(px.a);
    const __m256 pb = _mm256_set1_ps(px.b);
    __m256 best_d = _mm256_set1_ps(std::numeric_limits<float>::max());
    __m256i best_i = _mm256_setzero_si256();
    const __m256i k01234567 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    for (std::size_t i = 0; i < n; i += 8) {
        __m256 cL = _mm256_load_ps(s.L.data() + i);
        __m256 ca = _mm256_load_ps(s.a.data() + i);
        __m256 cb = _mm256_load_ps(s.b.data() + i);
        __m256 dL = _mm256_sub_ps(pL, cL);
        __m256 da = _mm256_sub_ps(pa, ca);
        __m256 db = _mm256_sub_ps(pb, cb);
        __m256 d = _mm256_fmadd_ps(db, db, _mm256_fmadd_ps(da, da, _mm256_mul_ps(dL, dL)));
        __m256 lt = _mm256_cmp_ps(d, best_d, _CMP_LT_OQ);
        best_d = _mm256_blendv_ps(best_d, d, lt);
        __m256i cur_i = _mm256_add_epi32(k01234567, _mm256_set1_epi32(static_cast<int>(i)));
        best_i = _mm256_castps_si256(
            _mm256_blendv_ps(_mm256_castsi256_ps(best_i), _mm256_castsi256_ps(cur_i), lt));
    }
    alignas(32) float dd[8];
    alignas(32) std::int32_t ii[8];
    _mm256_store_ps(dd, best_d);
    _mm256_store_si256(reinterpret_cast<__m256i*>(ii), best_i);
    int bk = 0;
    float bd = dd[0];
    for (int k = 1; k < 8; ++k)
        if (dd[k] < bd) {
            bd = dd[k];
            bk = k;
        }
    auto bi = static_cast<std::size_t>(ii[bk]);
    if (bi >= s.n) bi = s.n - 1;
    return {bi, bd};
#elif PNG2AMIGA_OKLAB_SIMD_WASM
    const v128_t pL = wasm_f32x4_splat(px.L);
    const v128_t pa = wasm_f32x4_splat(px.a);
    const v128_t pb = wasm_f32x4_splat(px.b);
    v128_t best_d = wasm_f32x4_splat(std::numeric_limits<float>::max());
    v128_t best_i = wasm_i32x4_splat(0);
    const v128_t k0123 = wasm_i32x4_make(0, 1, 2, 3);
    for (std::size_t i = 0; i < n; i += 4) {
        v128_t cL = wasm_v128_load(s.L.data() + i);
        v128_t ca = wasm_v128_load(s.a.data() + i);
        v128_t cb = wasm_v128_load(s.b.data() + i);
        v128_t dL = wasm_f32x4_sub(pL, cL);
        v128_t da = wasm_f32x4_sub(pa, ca);
        v128_t db = wasm_f32x4_sub(pb, cb);
        v128_t d = wasm_f32x4_add(wasm_f32x4_mul(dL, dL),
                                  wasm_f32x4_add(wasm_f32x4_mul(da, da), wasm_f32x4_mul(db, db)));
        v128_t cur_i = wasm_i32x4_add(k0123, wasm_i32x4_splat(static_cast<std::int32_t>(i)));
        v128_t lt = wasm_f32x4_lt(d, best_d);
        best_d = wasm_v128_bitselect(d, best_d, lt);
        best_i = wasm_v128_bitselect(cur_i, best_i, lt);
    }
    alignas(16) float dd[4];
    alignas(16) std::int32_t ii[4];
    wasm_v128_store(dd, best_d);
    wasm_v128_store(ii, best_i);
    int bk = 0;
    float bd = dd[0];
    for (int k = 1; k < 4; ++k)
        if (dd[k] < bd) {
            bd = dd[k];
            bk = k;
        }
    auto bi = static_cast<std::size_t>(ii[bk]);
    if (bi >= s.n) bi = s.n - 1;
    return {bi, bd};
#else
    float bd = std::numeric_limits<float>::max();
    std::size_t bk = 0;
    for (std::size_t i = 0; i < n; ++i) {
        float dL = px.L - s.L[i];
        float da = px.a - s.a[i];
        float db = px.b - s.b[i];
        float d = dL * dL + da * da + db * db;
        if (d < bd) {
            bd = d;
            bk = i;
        }
    }
    if (bk >= s.n) bk = s.n - 1;
    return {bk, bd};
#endif
}

}  // namespace png2amiga::oklab_simd
