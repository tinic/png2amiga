#include "strips.hpp"

#include "amiga.hpp"
#include "bitplane.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "ham.hpp"
#include "oklab_simd.hpp"
#include "palette.hpp"
#include "palette_locks.hpp"
#include "pipeline.hpp"
#include "quantize.hpp"
#include "types.hpp"

// scap.cpp delegates parallel sweep machinery to pipeline::best_sweep
// / pipeline::parallel_for. <atomic> is needed for HAM6 strips's per-row
// parallel planner (HAM-DP-aware swap evaluation is heavy, so the
// per-row planning loop runs in parallel and accumulates totals via
// atomics).
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <cmath>
#include <cstdint>
#include <limits>
#include <format>
#include <mutex>
#include <numeric>
#include <span>
#include <vector>

// SIMD backend selection — mirrors quantize.cpp / ssimulacra2.cpp.
// AVX2 (256-bit, 8 lanes) wins for x86_64 release builds; NEON (128-bit,
// 4 lanes) and WASM SIMD (128-bit, 4 lanes, no native FMA) cover Apple
// Silicon and the web build. Scalar fallback covers everything else.
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD 1
#define PNG2AMIGA_STRIPS_BACKEND_AVX2 0
#define PNG2AMIGA_STRIPS_BACKEND_NEON 0
#elif defined(__AVX2__)
#include <immintrin.h>
#define PNG2AMIGA_STRIPS_BACKEND_AVX2 1
#define PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD 0
#define PNG2AMIGA_STRIPS_BACKEND_NEON 0
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define PNG2AMIGA_STRIPS_BACKEND_NEON 1
#define PNG2AMIGA_STRIPS_BACKEND_AVX2 0
#define PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD 0
#else
#define PNG2AMIGA_STRIPS_BACKEND_AVX2 0
#define PNG2AMIGA_STRIPS_BACKEND_NEON 0
#define PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD 0
#endif

namespace png2amiga::strips {

// ===========================================================================
// SoA strip-pixel distance helpers — used by both DPF and EHB strip
// scorers. The hot loops compute OKLab² distances between strip pixels
// (~16 per strip, AoS Color3f-style) and palette entries. AoS forces
// scalar `vsubss`/`vmulss`/`vaddss` per channel; SoA + AVX2 packs 8
// pixels into one `vsubps`/`vfmadd*ps`/`vminps` step.
//
// Padding contract: SoA L/a/b are padded to a multiple of 8 with
// FLT_MAX. For pre-pass (min-dist update) and reduction (sum) callers
// keep pixel_min[0..valid_n) at FLT_MAX init and pixel_min[valid_n..
// padded_n) at 0 — that way the per-lane min stays 0 in the tail (since
// min(0, huge) = 0) and the sum naturally drops the tail without a
// scalar epilogue.
// ===========================================================================
struct StripPixelsSoA {
    std::vector<float> L, a, b;
    std::size_t valid_n{};
    std::size_t padded_n{};
};

inline void build_strip_soa(StripPixelsSoA& soa, std::span<const color_space::OKLab> pixels) {
    constexpr float kInf = std::numeric_limits<float>::max();
    soa.valid_n = pixels.size();
    soa.padded_n = (pixels.size() + 7u) & ~std::size_t{7};
    soa.L.assign(soa.padded_n, kInf);
    soa.a.assign(soa.padded_n, kInf);
    soa.b.assign(soa.padded_n, kInf);
    for (std::size_t i = 0; i < soa.valid_n; ++i) {
        soa.L[i] = pixels[i].L;
        soa.a[i] = pixels[i].a;
        soa.b[i] = pixels[i].b;
    }
}

// pixel_min must be sized to padded_n. Tail [valid_n..padded_n) should
// be 0; in-range [0..valid_n) should be FLT_MAX initially, then updated
// over multiple calls.
[[gnu::always_inline]]
inline void min_dist_update(
    const StripPixelsSoA& soa, float cL, float ca, float cb, float* pixel_min) noexcept {
#if PNG2AMIGA_STRIPS_BACKEND_AVX2
    __m256 vcL = _mm256_set1_ps(cL);
    __m256 vca = _mm256_set1_ps(ca);
    __m256 vcb = _mm256_set1_ps(cb);
    for (std::size_t i = 0; i < soa.padded_n; i += 8) {
        __m256 dL = _mm256_sub_ps(_mm256_loadu_ps(&soa.L[i]), vcL);
        __m256 da = _mm256_sub_ps(_mm256_loadu_ps(&soa.a[i]), vca);
        __m256 db = _mm256_sub_ps(_mm256_loadu_ps(&soa.b[i]), vcb);
        __m256 d = _mm256_fmadd_ps(dL, dL, _mm256_fmadd_ps(da, da, _mm256_mul_ps(db, db)));
        __m256 cur = _mm256_loadu_ps(pixel_min + i);
        _mm256_storeu_ps(pixel_min + i, _mm256_min_ps(cur, d));
    }
#elif PNG2AMIGA_STRIPS_BACKEND_NEON
    float32x4_t vcL = vdupq_n_f32(cL);
    float32x4_t vca = vdupq_n_f32(ca);
    float32x4_t vcb = vdupq_n_f32(cb);
    // padded_n is a multiple of 8 → also a multiple of 4, so the 4-lane
    // step is exact. FMA chain order matches the AVX2 path.
    for (std::size_t i = 0; i < soa.padded_n; i += 4) {
        float32x4_t dL = vsubq_f32(vld1q_f32(&soa.L[i]), vcL);
        float32x4_t da = vsubq_f32(vld1q_f32(&soa.a[i]), vca);
        float32x4_t db = vsubq_f32(vld1q_f32(&soa.b[i]), vcb);
        float32x4_t d = vmulq_f32(db, db);
        d = vfmaq_f32(d, da, da);
        d = vfmaq_f32(d, dL, dL);
        float32x4_t cur = vld1q_f32(pixel_min + i);
        vst1q_f32(pixel_min + i, vminq_f32(cur, d));
    }
#elif PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD
    v128_t vcL = wasm_f32x4_splat(cL);
    v128_t vca = wasm_f32x4_splat(ca);
    v128_t vcb = wasm_f32x4_splat(cb);
    for (std::size_t i = 0; i < soa.padded_n; i += 4) {
        v128_t dL = wasm_f32x4_sub(wasm_v128_load(&soa.L[i]), vcL);
        v128_t da = wasm_f32x4_sub(wasm_v128_load(&soa.a[i]), vca);
        v128_t db = wasm_f32x4_sub(wasm_v128_load(&soa.b[i]), vcb);
        // No native FMA in wasm-simd128 core spec — separate mul+add.
        v128_t d = wasm_f32x4_add(wasm_f32x4_mul(dL, dL),
                                  wasm_f32x4_add(wasm_f32x4_mul(da, da), wasm_f32x4_mul(db, db)));
        v128_t cur = wasm_v128_load(pixel_min + i);
        wasm_v128_store(pixel_min + i, wasm_f32x4_min(cur, d));
    }
#else
    for (std::size_t i = 0; i < soa.valid_n; ++i) {
        float dL = soa.L[i] - cL;
        float da = soa.a[i] - ca;
        float db = soa.b[i] - cb;
        float d = PNG2AMIGA_FMA(dL, dL, PNG2AMIGA_FMA(da, da, db * db));
        if (d < pixel_min[i]) pixel_min[i] = d;
    }
#endif
}

// Returns sum over i<valid_n of min(pixel_min_excl[i], dist²(b), dist²(h)).
// Tail of pixel_min_excl must be 0 so it contributes nothing to the sum.
[[gnu::always_inline]]
inline double dist_min2_sum(const StripPixelsSoA& soa,
                            const float* pixel_min_excl,
                            float bL,
                            float ba,
                            float bb,
                            float hL,
                            float ha,
                            float hb) noexcept {
#if PNG2AMIGA_STRIPS_BACKEND_AVX2
    __m256 vbL = _mm256_set1_ps(bL);
    __m256 vba = _mm256_set1_ps(ba);
    __m256 vbb = _mm256_set1_ps(bb);
    __m256 vhL = _mm256_set1_ps(hL);
    __m256 vha = _mm256_set1_ps(ha);
    __m256 vhb = _mm256_set1_ps(hb);
    __m256 acc = _mm256_setzero_ps();
    for (std::size_t i = 0; i < soa.padded_n; i += 8) {
        __m256 pL = _mm256_loadu_ps(&soa.L[i]);
        __m256 pa = _mm256_loadu_ps(&soa.a[i]);
        __m256 pb = _mm256_loadu_ps(&soa.b[i]);
        __m256 cur = _mm256_loadu_ps(pixel_min_excl + i);
        __m256 dL = _mm256_sub_ps(pL, vbL);
        __m256 da = _mm256_sub_ps(pa, vba);
        __m256 db = _mm256_sub_ps(pb, vbb);
        __m256 d = _mm256_fmadd_ps(dL, dL, _mm256_fmadd_ps(da, da, _mm256_mul_ps(db, db)));
        cur = _mm256_min_ps(cur, d);
        dL = _mm256_sub_ps(pL, vhL);
        da = _mm256_sub_ps(pa, vha);
        db = _mm256_sub_ps(pb, vhb);
        d = _mm256_fmadd_ps(dL, dL, _mm256_fmadd_ps(da, da, _mm256_mul_ps(db, db)));
        cur = _mm256_min_ps(cur, d);
        acc = _mm256_add_ps(acc, cur);
    }
    // Horizontal sum of 8 lanes.
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return static_cast<double>(_mm_cvtss_f32(sum));
#elif PNG2AMIGA_STRIPS_BACKEND_NEON
    float32x4_t vbL = vdupq_n_f32(bL);
    float32x4_t vba = vdupq_n_f32(ba);
    float32x4_t vbb = vdupq_n_f32(bb);
    float32x4_t vhL = vdupq_n_f32(hL);
    float32x4_t vha = vdupq_n_f32(ha);
    float32x4_t vhb = vdupq_n_f32(hb);
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (std::size_t i = 0; i < soa.padded_n; i += 4) {
        float32x4_t pL = vld1q_f32(&soa.L[i]);
        float32x4_t pa = vld1q_f32(&soa.a[i]);
        float32x4_t pb = vld1q_f32(&soa.b[i]);
        float32x4_t cur = vld1q_f32(pixel_min_excl + i);
        // base candidate
        float32x4_t dL = vsubq_f32(pL, vbL);
        float32x4_t da = vsubq_f32(pa, vba);
        float32x4_t db = vsubq_f32(pb, vbb);
        float32x4_t d = vmulq_f32(db, db);
        d = vfmaq_f32(d, da, da);
        d = vfmaq_f32(d, dL, dL);
        cur = vminq_f32(cur, d);
        // halfbrite mirror
        dL = vsubq_f32(pL, vhL);
        da = vsubq_f32(pa, vha);
        db = vsubq_f32(pb, vhb);
        d = vmulq_f32(db, db);
        d = vfmaq_f32(d, da, da);
        d = vfmaq_f32(d, dL, dL);
        cur = vminq_f32(cur, d);
        acc = vaddq_f32(acc, cur);
    }
    return static_cast<double>(vaddvq_f32(acc));
#elif PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD
    v128_t vbL = wasm_f32x4_splat(bL);
    v128_t vba = wasm_f32x4_splat(ba);
    v128_t vbb = wasm_f32x4_splat(bb);
    v128_t vhL = wasm_f32x4_splat(hL);
    v128_t vha = wasm_f32x4_splat(ha);
    v128_t vhb = wasm_f32x4_splat(hb);
    v128_t acc = wasm_f32x4_const_splat(0.0f);
    for (std::size_t i = 0; i < soa.padded_n; i += 4) {
        v128_t pL = wasm_v128_load(&soa.L[i]);
        v128_t pa = wasm_v128_load(&soa.a[i]);
        v128_t pb = wasm_v128_load(&soa.b[i]);
        v128_t cur = wasm_v128_load(pixel_min_excl + i);
        v128_t dL = wasm_f32x4_sub(pL, vbL);
        v128_t da = wasm_f32x4_sub(pa, vba);
        v128_t db = wasm_f32x4_sub(pb, vbb);
        v128_t d = wasm_f32x4_add(wasm_f32x4_mul(dL, dL),
                                  wasm_f32x4_add(wasm_f32x4_mul(da, da), wasm_f32x4_mul(db, db)));
        cur = wasm_f32x4_min(cur, d);
        dL = wasm_f32x4_sub(pL, vhL);
        da = wasm_f32x4_sub(pa, vha);
        db = wasm_f32x4_sub(pb, vhb);
        d = wasm_f32x4_add(wasm_f32x4_mul(dL, dL),
                           wasm_f32x4_add(wasm_f32x4_mul(da, da), wasm_f32x4_mul(db, db)));
        cur = wasm_f32x4_min(cur, d);
        acc = wasm_f32x4_add(acc, cur);
    }
    float h = wasm_f32x4_extract_lane(acc, 0) + wasm_f32x4_extract_lane(acc, 1) +
              wasm_f32x4_extract_lane(acc, 2) + wasm_f32x4_extract_lane(acc, 3);
    return static_cast<double>(h);
#else
    double e = 0;
    for (std::size_t i = 0; i < soa.valid_n; ++i) {
        float pL = soa.L[i], pa = soa.a[i], pb = soa.b[i];
        float best = pixel_min_excl[i];
        float dL = pL - bL, da = pa - ba, db = pb - bb;
        float d = PNG2AMIGA_FMA(dL, dL, PNG2AMIGA_FMA(da, da, db * db));
        if (d < best) best = d;
        dL = pL - hL;
        da = pa - ha;
        db = pb - hb;
        d = PNG2AMIGA_FMA(dL, dL, PNG2AMIGA_FMA(da, da, db * db));
        if (d < best) best = d;
        e += static_cast<double>(best);
    }
    return e;
#endif
}

// Single-cand variant for DPF (no halfbrite mirror).
[[gnu::always_inline]]
inline double dist_min1_sum(
    const StripPixelsSoA& soa, const float* pixel_min_excl, float cL, float ca, float cb) noexcept {
#if PNG2AMIGA_STRIPS_BACKEND_AVX2
    __m256 vcL = _mm256_set1_ps(cL);
    __m256 vca = _mm256_set1_ps(ca);
    __m256 vcb = _mm256_set1_ps(cb);
    __m256 acc = _mm256_setzero_ps();
    for (std::size_t i = 0; i < soa.padded_n; i += 8) {
        __m256 dL = _mm256_sub_ps(_mm256_loadu_ps(&soa.L[i]), vcL);
        __m256 da = _mm256_sub_ps(_mm256_loadu_ps(&soa.a[i]), vca);
        __m256 db = _mm256_sub_ps(_mm256_loadu_ps(&soa.b[i]), vcb);
        __m256 d = _mm256_fmadd_ps(dL, dL, _mm256_fmadd_ps(da, da, _mm256_mul_ps(db, db)));
        __m256 cur = _mm256_loadu_ps(pixel_min_excl + i);
        acc = _mm256_add_ps(acc, _mm256_min_ps(cur, d));
    }
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return static_cast<double>(_mm_cvtss_f32(sum));
#elif PNG2AMIGA_STRIPS_BACKEND_NEON
    float32x4_t vcL = vdupq_n_f32(cL);
    float32x4_t vca = vdupq_n_f32(ca);
    float32x4_t vcb = vdupq_n_f32(cb);
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (std::size_t i = 0; i < soa.padded_n; i += 4) {
        float32x4_t dL = vsubq_f32(vld1q_f32(&soa.L[i]), vcL);
        float32x4_t da = vsubq_f32(vld1q_f32(&soa.a[i]), vca);
        float32x4_t db = vsubq_f32(vld1q_f32(&soa.b[i]), vcb);
        float32x4_t d = vmulq_f32(db, db);
        d = vfmaq_f32(d, da, da);
        d = vfmaq_f32(d, dL, dL);
        float32x4_t cur = vld1q_f32(pixel_min_excl + i);
        acc = vaddq_f32(acc, vminq_f32(cur, d));
    }
    return static_cast<double>(vaddvq_f32(acc));
#elif PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD
    v128_t vcL = wasm_f32x4_splat(cL);
    v128_t vca = wasm_f32x4_splat(ca);
    v128_t vcb = wasm_f32x4_splat(cb);
    v128_t acc = wasm_f32x4_const_splat(0.0f);
    for (std::size_t i = 0; i < soa.padded_n; i += 4) {
        v128_t dL = wasm_f32x4_sub(wasm_v128_load(&soa.L[i]), vcL);
        v128_t da = wasm_f32x4_sub(wasm_v128_load(&soa.a[i]), vca);
        v128_t db = wasm_f32x4_sub(wasm_v128_load(&soa.b[i]), vcb);
        v128_t d = wasm_f32x4_add(wasm_f32x4_mul(dL, dL),
                                  wasm_f32x4_add(wasm_f32x4_mul(da, da), wasm_f32x4_mul(db, db)));
        v128_t cur = wasm_v128_load(pixel_min_excl + i);
        acc = wasm_f32x4_add(acc, wasm_f32x4_min(cur, d));
    }
    float h = wasm_f32x4_extract_lane(acc, 0) + wasm_f32x4_extract_lane(acc, 1) +
              wasm_f32x4_extract_lane(acc, 2) + wasm_f32x4_extract_lane(acc, 3);
    return static_cast<double>(h);
#else
    double e = 0;
    for (std::size_t i = 0; i < soa.valid_n; ++i) {
        float dL = soa.L[i] - cL, da = soa.a[i] - ca, db = soa.b[i] - cb;
        float d = PNG2AMIGA_FMA(dL, dL, PNG2AMIGA_FMA(da, da, db * db));
        float best = pixel_min_excl[i];
        if (d < best) best = d;
        e += static_cast<double>(best);
    }
    return e;
#endif
}

// Sum of pixel_min[0..valid_n). Tail (valid_n..padded_n) assumed 0,
// allowing a clean SIMD reduction over padded_n.
[[gnu::always_inline]]
inline double sum_pixel_min(const float* pixel_min, std::size_t padded_n) noexcept {
#if PNG2AMIGA_STRIPS_BACKEND_AVX2
    __m256 acc = _mm256_setzero_ps();
    for (std::size_t i = 0; i < padded_n; i += 8) {
        acc = _mm256_add_ps(acc, _mm256_loadu_ps(pixel_min + i));
    }
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return static_cast<double>(_mm_cvtss_f32(sum));
#elif PNG2AMIGA_STRIPS_BACKEND_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (std::size_t i = 0; i < padded_n; i += 4) {
        acc = vaddq_f32(acc, vld1q_f32(pixel_min + i));
    }
    return static_cast<double>(vaddvq_f32(acc));
#elif PNG2AMIGA_STRIPS_BACKEND_WASM_SIMD
    v128_t acc = wasm_f32x4_const_splat(0.0f);
    for (std::size_t i = 0; i < padded_n; i += 4) {
        acc = wasm_f32x4_add(acc, wasm_v128_load(pixel_min + i));
    }
    float h = wasm_f32x4_extract_lane(acc, 0) + wasm_f32x4_extract_lane(acc, 1) +
              wasm_f32x4_extract_lane(acc, 2) + wasm_f32x4_extract_lane(acc, 3);
    return static_cast<double>(h);
#else
    double e = 0;
    for (std::size_t i = 0; i < padded_n; ++i)
        e += static_cast<double>(pixel_min[i]);
    return e;
#endif
}

// Reset pixel_min for a fresh pre-pass: in-range = FLT_MAX, tail = 0.
inline void reset_pixel_min(std::vector<float>& pixel_min, const StripPixelsSoA& soa) {
    pixel_min.assign(soa.padded_n, 0.0f);
    constexpr float kInf = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < soa.valid_n; ++i)
        pixel_min[i] = kInf;
}

namespace {

// Build a 6-plane DPF frame where every pixel = PF2 index 1, plus a
// position-marker grid baked into PF1 (foreground) for readability.
//
// PF2 LSB sits at 0-indexed plane 1 (= hardware BPL2). Setting all of
// plane 1 to 0xFF gives PF2 index 1 across the entire frame; PF2 maps
// that to color register 9 (OCS) / 17 (AGA) via PF2OF.
//
// PF1 markers — drawn in front of PF2 (PF2PRI=0). PF1 has 3 planes at
// 0-indexed positions 0, 2, 4 (PF1 LSB / mid / MSB):
//   * minor tick (1 px wide, every 16 px): PF1 plane 0 bit set
//     -> PF1 index 1 -> color register 1
//   * major tick (1 px wide, every 64 px): PF1 planes 0 + 2 bit set
//     (every 64 px is also a multiple of 16, so plane 0 is on too)
//     -> PF1 index 1|2 = 3 -> color register 3
// The probe's frame-start palette assigns reg 1 = bright yellow and
// reg 3 = bright red, so the pixel where the strips MOVE fires can be
// read off against the embedded ruler.
Result<bitplane::BitplaneData> make_dpf_pf2_index1_planes(std::size_t width,
                                                          std::size_t height,
                                                          std::size_t total_planes,
                                                          bool add_position_grid) {
    if (total_planes != 6 && total_planes != 8) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("Strips probe: expected 6 (OCS) or 8 (AGA) planes, got {}", total_planes),
        }};
    }
    if (width == 0 || height == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Strips probe: zero dimensions",
        }};
    }

    auto aligned_width = (width + 15u) & ~std::size_t{15};
    auto bpr = aligned_width / 8;

    bitplane::BitplaneData planes;
    planes.width = width;
    planes.height = height;
    planes.depth = total_planes;
    planes.bytes_per_row = bpr;
    planes.layout = bitplane::Layout::interleaved;
    planes.data.assign(planes.total_bytes(), 0);

    // Set every byte of plane 1 (PF2 LSB) to 0xFF -> PF2 index 1 everywhere.
    for (std::size_t y = 0; y < height; ++y) {
        auto off = planes.plane_row_offset(/*plane=*/1, y);
        std::fill_n(planes.data.data() + off, bpr, static_cast<std::uint8_t>(0xFF));
    }

    if (add_position_grid) {
        // Set bit at column x in plane p, row y: byte = x/8, bit = 7 - (x%8).
        auto set_pixel = [&](std::size_t plane, std::size_t y, std::size_t x) {
            auto off = planes.plane_row_offset(plane, y);
            planes.data[off + x / 8] |= static_cast<std::uint8_t>(1u << (7 - (x % 8)));
        };
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; x += 16) {
                set_pixel(/*plane=*/0, y, x);                   // minor tick
                if (x % 64 == 0) set_pixel(/*plane=*/2, y, x);  // major tick
            }
        }
    }
    return planes;
}

// HPOS sweep across the per-line copper budget [0x00, 0xE3]. 0xE3 is the
// HPOS used by the existing sliced encoder for interlace per-line WAITs —
// past that there's no DMA-allowed window before the next horizontal
// blank ends.
constexpr int kHpMin = 0x00;
constexpr int kHpMax = 0xE3;

int hp_for_y(int y, int height) {
    if (height <= 1) return kHpMin;
    long num = static_cast<long>(y) * (kHpMax - kHpMin);
    long den = static_cast<long>(height - 1);
    int hp = static_cast<int>(num / den) + kHpMin;
    return std::clamp(hp, kHpMin, kHpMax);
}

ScapMove make_wait(std::uint8_t hpos, std::uint8_t vpos, int slot_index = -1) {
    ScapMove w{};
    w.kind = ScapOpKind::kWait;
    w.hpos = hpos;
    w.vpos = vpos;
    w.slot_index = slot_index;
    return w;
}

ScapMove make_move(std::uint8_t reg, std::uint16_t rgb_ocs, int slot_index = -1) {
    ScapMove m{};
    m.kind = ScapOpKind::kMove;
    m.reg = reg;
    m.rgb_ocs = rgb_ocs;
    m.slot_index = slot_index;
    return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Planner — OCS DPF, 6-plane lores 320 px.
//
// Two-stage approach to keep F-S texture flowing across the whole frame
// while still letting each strip pick a palette tuned to its content:
//
//   Stage 1 (planning) — global F-S vs the 8-color base palette
//   produces a per-pixel base_index whose distribution per strip drives
//   the per-line MOVE planner. Each strip's swap is the OKLab centroid
//   of the strip pixels currently assigned to one of its registers,
//   chosen for biggest error reduction.
//
//   Stage 2 (rendering) — runs F-S a second time, but this time against
//   the EVOLVING per-strip palette: for each pixel the nearest-color
//   lookup uses strip_palettes[strip(x)], and residuals propagate
//   through the standard F-S kernel across strip boundaries. Errors
//   are in linear RGB so they discharge in whichever palette is active
//   downstream — F-S texture is uniform; only the color rendition
//   shifts at strip boundaries (and only by the amount the palette
//   actually changed).
//
//   This combines:
//     * uniform dither texture across the frame (no per-strip "blocks")
//     * per-strip palette specialisation (good color fidelity)
// ---------------------------------------------------------------------------
Result<ScapResult> encode_strips_dpf_ocs(
    const Image& image,
    int width_arg,
    int height_arg,
    bool lock_color0,
    const dither::Settings& dither_settings,
    bool debug_overlay,
    std::size_t copper_changes_override,
    int palette_diversity,
    std::function<void(float, std::string_view)> on_progress,
    bool enable_best,
    int sliced_spread_radius,
    float sliced_spread_decay,
    bool sliced_vertical_dither,
    std::span<const Color3f> external_palette,
    const std::vector<std::pair<std::size_t, Color3f>>& reserved_slots,
    const std::vector<std::pair<std::size_t, Color3f>>& locked_slots,
    bool sliced_beam) {
    // --best: multi-restart with varied palette_diversity + dither
    // strength + beam (forward-look scavenge in the sliced base pass).
    // The strips planner is deterministic for a given input, so varying
    // these knobs is the only way to sample different optimisation
    // landscapes. Each restart is a full encode (~100 ms); user OK'd
    // unbounded compute. Keep the highest-S2 result across both beam
    // states.
    if (enable_best) {
        // DPF: 24 jitter seeds — the 8-color PF2 palette is highly
        // sensitive to which colors win the median-cut, so heavy jitter
        // sampling buys more here than for wider palettes (EHB stays at
        // 8). Total 5×4×24 + 1 = 481 trials, ~2–3 min on 8 cores.
        //
        // The beam (forward-look residual fill in encode_copper's sliced
        // base pass) was measured at +0.49 S2 on ocs_4096 and 0.0 S2
        // everywhere else — strips' own mid-line MOVE planner already
        // diversifies the per-row palette enough that the beam's
        // empty-slot gate never fires productively. So we don't sweep
        // beam as an axis here; just honour the caller's sliced_beam
        // flag verbatim (still works via --sliced-beam for force-enable).
        auto best = pipeline::best_sweep<ScapResult>(
            image,
            dither_settings,
            palette_diversity,
            /*jitter_count=*/24,
            [&](const Image& jittered_in, const dither::Settings& d, int div) {
                return encode_strips_dpf_ocs(jittered_in,
                                             width_arg,
                                             height_arg,
                                             lock_color0,
                                             d,
                                             debug_overlay,
                                             copper_changes_override,
                                             div,
                                             /*on_progress=*/{},
                                             /*enable_best=*/false,
                                             sliced_spread_radius,
                                             sliced_spread_decay,
                                             sliced_vertical_dither,
                                             external_palette,
                                             reserved_slots,
                                             locked_slots,
                                             sliced_beam);
            },
            [](const ScapResult& r) -> const Image& { return r.rendered; },
            on_progress,
            /*jitter_amplitude=*/1.0f);
        if (best.has_value()) return std::move(*best);
        // Fall through to the single-pass path if every restart failed
        // (shouldn't happen with valid input, but degrade gracefully).
    }

    auto& table = strips_table_for(6);
    if (table.slots.empty()) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "Strips planner: kStrips6bplOcs slot table is empty",
        }};
    }

    auto width = (width_arg > 0) ? static_cast<std::size_t>(width_arg) : image.width();
    auto height = (height_arg > 0) ? static_cast<std::size_t>(height_arg) : image.height();
    if (image.width() != width || image.height() != height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Strips planner: image is {}x{} but caller asked for "
                        "{}x{} — resize before calling",
                        image.width(),
                        image.height(),
                        width,
                        height),
        }};
    }

    // ---- 0. Optional debug source: synthetic 4-ramp test pattern --------
    // When debug_overlay is on, replace the input image with the same
    // 4-ramps-per-line test pattern as examples/ramps.png (black->green,
    // black->red, black->blue, black->white; 16 steps × 5 lores px each).
    // The whole debug bundle (this ramp + black base palette + yellow
    // PF1 rulers) is the canonical visual test case for slot-tuning.
    Image ramps_holder;
    const Image* src_image = &image;
    if (debug_overlay) {
        ramps_holder = Image(width, height);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                std::size_t band = (x * 4) / std::max<std::size_t>(width, 1);
                std::size_t band_w = std::max<std::size_t>(width / 4, 1);
                std::size_t band_x = x - band * band_w;
                std::size_t step = (band_x * 16) / std::max<std::size_t>(band_w, 1);
                if (step > 15) step = 15;
                float v = static_cast<float>(step) / 15.0f;  // 0..1 sRGB
                // Match examples/ramps.png: bands are sRGB ramps, so we
                // need to convert to linear before storing in the Image.
                float lin = color_space::srgb_to_linear(v);
                Color3f c{0.0f, 0.0f, 0.0f};
                if (band == 0)
                    c.g = lin;
                else if (band == 1)
                    c.r = lin;
                else if (band == 2)
                    c.b = lin;
                else
                    c = Color3f{lin, lin, lin};
                ramps_holder[x, y] = c;
            }
        }
        src_image = &ramps_holder;
    }
    auto& src = *src_image;

    // ---- 1. sliced first: per-line 8-color palette evolution.
    // strips layers on top of sliced. With only 8 PF2 colors the per-line
    // search space is tiny, but sliced still finds useful palette diffs
    // against neighbor scanlines.
    constexpr int kBaseColors = 8;  // PF2 width = 3 bitplanes
    // PF2 index → COLOR-register mapping:
    //   * OCS DPF combiner rule: PF2 index 0 falls through to COLOR00
    //     (the "both PFs zero → background" case). Indices 1..7 use the
    //     implicit +8 offset → COLOR09..15. COLOR08 is unused on OCS.
    //   * AGA DPF with BPLCON3 PF2OF=011: index 0 → COLOR08, indices
    //     1..7 → COLOR09..15.
    // The cpp viewer needs to work on both chipsets, so we write index
    // 0 to BOTH COLOR00 and COLOR08 (one of the two is always the live
    // register depending on chipset). pf2_writes(k) returns the list
    // of registers that must be written for PF2 index k.
    auto pf2_writes = [](std::size_t k) -> std::array<int, 2> {
        return (k == 0) ? std::array<int, 2>{0, 8}
                        : std::array<int, 2>{static_cast<int>(8 + k), -1};
    };
    // Hblank load is fixed at ~9 MOVEs (8 PF2 indices, k=0 dual-writes
    // COLOR00+COLOR08) re-emitted unconditionally every line, so the sliced
    // share is whatever the user asked for — bounded by the 14-MOVE OCS
    // hblank budget. strips swaps live in the visible region and don't
    // contend for hblank, so no strips share split is needed.
    constexpr std::size_t kMaxCombined = copper::max_changes_per_line(
        /*depth=*/3, false, false, amiga::Chipset::ocs, false);
    std::size_t total_budget = (copper_changes_override > 0)
                                   ? std::min<std::size_t>(copper_changes_override, kMaxCombined)
                                   : kMaxCombined;
    std::size_t sliced_share = std::min<std::size_t>(total_budget, 2u);
    // External-palette plumbing for DPF: --palette gets forwarded as
    // encode_copper's user_palette so the sliced base palette is locked
    // to the user's choice (trimmed to 8 PF2 colors).
    std::vector<Color3f> dpf_user_pal;
    if (!external_palette.empty()) {
        dpf_user_pal.assign(external_palette.begin(), external_palette.end());
        if (dpf_user_pal.size() > 8) dpf_user_pal.resize(8);
    }
    // --reserve-range + --lock-index plumbing: both pin a PF2 slot's
    // color. Differences:
    //   * reserves are dither_excluded — image pixels never route there.
    //   * locks are NOT excluded — dither picks them normally; only the
    //     slot's color is fixed.
    // Both feed encode_copper's `locked` (sliced never re-emits the
    // slot), and both flag the strips swap planner mask so mid-line
    // MOVEs can't overwrite the pinned color.
    std::array<bool, 8> reserved_mask_dpf{};
    std::vector<std::size_t> dpf_dither_excluded;
    dpf_dither_excluded.reserve(reserved_slots.size());
    for (auto& [idx, _] : reserved_slots) {
        if (idx < 8) {
            reserved_mask_dpf[idx] = true;
            dpf_dither_excluded.push_back(idx);
        }
    }
    for (auto& [idx, _] : locked_slots) {
        if (idx < 8) reserved_mask_dpf[idx] = true;
    }
    // Combined locked list for encode_copper. Reserves and locks both
    // pin colors; only the dither_excluded vector distinguishes them.
    std::vector<std::pair<std::size_t, Color3f>> copper_locked;
    copper_locked.reserve(reserved_slots.size() + locked_slots.size());
    copper_locked.insert(copper_locked.end(), reserved_slots.begin(), reserved_slots.end());
    copper_locked.insert(copper_locked.end(), locked_slots.begin(), locked_slots.end());
    auto copper_result = copper::encode_copper(
        src,
        /*depth=*/3,
        dither_settings,
        amiga::Chipset::ocs,
        sliced_share,
        dpf_user_pal.empty() ? nullptr : &dpf_user_pal,
        lock_color0,
        copper_locked,
        palette_diversity,
        /*skip_initial_swap_rows=*/0,
        /*is_lace=*/false,
        /*is_ehb=*/false,
        /*on_progress=*/{},
        sliced_spread_radius >= 0 ? static_cast<std::size_t>(sliced_spread_radius)
                                  : std::numeric_limits<std::size_t>::max(),
        sliced_spread_decay >= 0.0f ? sliced_spread_decay : -1.0f,
        sliced_vertical_dither,
        dpf_dither_excluded,
        /*quantizer_override=*/std::nullopt,
        sliced_beam);
    if (!copper_result) return std::unexpected{copper_result.error()};
    auto& sliced_palettes = copper_result->scanline_palettes;
    auto& base_palette_vec = copper_result->base_palette;
    std::array<Color3f, kBaseColors> base_palette{};
    for (std::size_t k = 0; k < kBaseColors && k < base_palette_vec.size(); ++k)
        base_palette[k] = base_palette_vec[k];

    // ---- 2. base_index storage. Rebuilt per-row inside the planner
    // against the per-line sliced-evolved palette (lines below) so the
    // per-strip cluster math sees the bindings the encoder will actually
    // produce. The previous code ran a global dither against the
    // FRAME-INIT (line-0) palette here, which produced stale bindings —
    // same root cause as the EHB v1.26.2 fix.
    std::vector<std::uint8_t> base_index(width * height, 0);

    // OKLab cache of the SOURCE pixels (used by per-strip swap planning).
    std::vector<color_space::OKLab> img_lab(width * height);
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            img_lab[y * width + x] = color_space::linear_to_oklab(src[x, y]);

    // ---- 3. Per-line greedy planner -------------------------------------
    std::vector<std::uint8_t> indices(width * height, 0);
    std::vector<std::vector<ScapMove>> line_moves(height);
    Image preview(width, height);

    // P / P_lab / strip_palettes are now per-row local state inside
    // the parallel worker (each row owns a private copy so the per-
    // row planner is thread-safe).

    // Force k_min=1 for DPF strips regardless of --lock-color0. PF2
    // index 0 maps to COLOR00 on OCS and COLOR08 on AGA (per BPLCON3
    // PF2OF=011) — keeping the two in sync mid-line would require
    // emitting two MOVEs per swap, which would shift subsequent slot
    // positions on the bus. Frame-init + per-line sliced MOVEs (both in
    // hblank) handle the dual-write without timing impact, so strips
    // simply never picks k=0 and the planner targets k=1..7.
    std::size_t k_min = 1u;

    // Stage-2 error diffusion setup. Honors the user's --dither choice
    // by pulling the diffusion kernel from dither.hpp. The buffer is
    // a whole-image OKLab error grid (matches the EHB+sliced path in
    // main.cpp): residuals diffuse in the perceptual space, get
    // strength-multiplied at scatter time, and per-channel-clamped on
    // read. Linear-RGB diffusion (the previous approach) blew up across
    // strip palette swaps because the residual magnitude isn't
    // perceptually proportional and DPF's tight 8-color palette has
    // gaps wider than the residuals could absorb.
    // ED scaffolding (kernel, error buf, structure bias, Riemersma) all
    // live inside dither::diffuse_raw_buffer (the post-pass-1 driver
    // call below). The sliced planner only needs to know whether dithering
    // is enabled at all (yliluoma family + ordered + ED kernel) so we
    // keep the policy flags here.

    constexpr int kVStart = 44;
    constexpr std::uint8_t kFillerReg = 31;  // COLOR31 — unread in OCS DPF 3+3
    constexpr std::uint16_t kFillerVal = 0x0000;
    double total_error = 0.0;
    std::size_t total_moves = 0;

    // strip_palettes[s] is the palette state during pixels in strip s.
    // strip 0 = entry palette (P at start of line); strip s+1 = palette
    // after slot s's MOVEs are applied. There are slots.size()+1 strips.
    std::size_t num_strips = table.slots.size() + 1;
    // strip_palettes / strip_pal_lab are per-row scratch — declared
    // inside the parallel worker so each thread has its own copy.

    // Captured per-row strip palettes for the post-loop driver call
    // (pass 2). Pass 1 fills strip_palettes[s] for the current row and
    // we snapshot into strip_palettes_per_row[y] before moving on.
    std::vector<std::vector<std::array<Color3f, kBaseColors>>> strip_palettes_per_row(
        height, std::vector<std::array<Color3f, kBaseColors>>(num_strips));
    std::vector<std::vector<std::array<color_space::OKLab, kBaseColors>>> strip_pal_lab_per_row(
        height, std::vector<std::array<color_space::OKLab, kBaseColors>>(num_strips));

    // slots[s].pixel_x is the LEFT edge of strip s+1 (i.e. strip s+1 covers
    // pixels [slots[s].pixel_x .. slots[s+1].pixel_x), and strip s+1 uses
    // the palette state AFTER slot s's MOVE has fired). Strip 0 is the
    // entry palette and covers [0 .. slots[0].pixel_x).
    auto strip_for_x = [&](std::size_t x) -> std::size_t {
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            if (x < static_cast<std::size_t>(table.slots[s].pixel_x)) return s;
        }
        return table.slots.size();
    };

    // Precompute pixel x → strip-index for the F-S boundary check below.
    std::vector<std::uint16_t> x_strip(width);
    for (std::size_t x = 0; x < width; ++x)
        x_strip[x] = static_cast<std::uint16_t>(strip_for_x(x));

    constexpr int kPasses = 6;
    // run_row below executes inside pipeline::parallel_for, so report_pass
    // can be invoked concurrently from N worker threads. Without a mutex,
    // the per-thread on_progress invocations interleave on stdout.
    std::mutex progress_mu;
    auto report_pass = [&](int pass_idx, float local) {
        if (on_progress) {
            float p = (static_cast<float>(pass_idx) + std::clamp(local, 0.0f, 1.0f)) /
                      static_cast<float>(kPasses);
            std::lock_guard<std::mutex> lock(progress_mu);
            on_progress(p, "encoding");
        }
    };
    if (on_progress) on_progress(0.0f, "encoding");
    std::atomic<std::size_t> total_moves_atomic{0};
    std::atomic<double> total_error_atomic{0.0};
    std::atomic<std::size_t> rows_done{0};
    for (int pass = 0; pass < kPasses; ++pass) {
        if (pass > 0) {
            // base_index is rebuilt per-row inside the planner against
            // the per-line sliced-evolved palette (no carry-over between
            // passes needed; matches EHB strips's v1.26.2+ behavior).
            for (auto& v : line_moves)
                v.clear();
            // err_buf is owned by dither::diffuse_raw_buffer (allocated
            // fresh each pass-2 call), so no manual reset is needed.
            // total_moves / total_error are re-derived from the atomics
            // and dither return value below; no scalar reset needed.
            total_moves_atomic.store(0);
            total_error_atomic.store(0.0);
            rows_done.store(0);
        }
        // hw_state tracks the actual hardware state of the 8 PF2 color
        // registers across lines. With --slice-changes 1..7 we can't fully
        // refresh the palette every line, so the PREVIOUS line's strips
        // swaps + partial sliced MOVEs decide what colors sit in those
        // registers when the next line's HBLANK starts. Strip 0 MUST be
        // encoded against this real state or pixels are encoded with a
        // palette the chip isn't actually displaying.
        //
        // Specialisation for --slice-changes 0 (default): every line's
        // HBLANK is a full 8-slot reset, so hw_state at the start of
        // strip 0 is always exactly target = sliced_palettes[y]. No state
        // carries between lines → rows are independent → parallel_for.
        // For --slice-changes > 0 we keep the carry-over and run serial.
        std::array<Color3f, kBaseColors> hw_state_init{};
        for (std::size_t k = 0; k < kBaseColors; ++k)
            hw_state_init[k] = base_palette[k];
        auto hw_state = hw_state_init;
        bool serial_path = (copper_changes_override > 0);
        auto run_row = [&](std::size_t y) {
            // Per-call local hw_state. Serial mode pulls from the outer
            // shared hw_state (carry-over); parallel mode uses init
            // (full-reset HBLANK below means the value is never read).
            std::array<Color3f, kBaseColors> hw_state_local;
            if (serial_path) {
                if (y == 0) hw_state = hw_state_init;
                hw_state_local = hw_state;
            } else {
                hw_state_local = hw_state_init;
            }
            // Per-row working state: must be local for thread safety.
            // P / P_lab / strip_palettes / strip_pal_lab were captured-by-
            // ref outside the loop; declared inside now so each parallel
            // worker has its own copy.
            auto P = base_palette;
            std::array<color_space::OKLab, kBaseColors> P_lab{};
            auto recompute_lab_local = [&]() {
                for (std::size_t k = 0; k < kBaseColors; ++k)
                    P_lab[k] = color_space::linear_to_oklab(P[k]);
            };
            recompute_lab_local();
            std::vector<std::array<Color3f, kBaseColors>> strip_palettes(num_strips);
            std::vector<std::array<color_space::OKLab, kBaseColors>> strip_pal_lab(num_strips);
            int abs_vpos = static_cast<int>(y) + kVStart;
            auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

            // Per-line target = sliced plan for this line, OR all-zero in
            // debug mode (hardware enters every line with 0x0000 there).
            std::array<Color3f, kBaseColors> target{};
            for (std::size_t k = 0; k < kBaseColors; ++k)
                target[k] = debug_overlay ? Color3f{0.0f, 0.0f, 0.0f} : sliced_palettes[y][k];

            // 1. Per-line sliced MOVEs in HBLANK.
            //
            //   Default (copper_changes_override == 0): unconditionally
            //   re-emit all 8 PF2 base colors (≤9 hblank MOVEs; k=0
            //   dual-writes COLOR00+COLOR08, k=1..7 single MOVE each),
            //   well below the 14-MOVE OCS hblank capacity. Whatever
            //   registers strips polluted on line y-1 get fully overwritten
            //   before line y's visible region starts.
            //
            //   --slice-changes N > 0: cap HBLANK to N MOVEs total. Diff
            //   target vs the previous line's target in OKLab; emit MOVEs
            //   for the top-N most-changed slots (k=0 costs 2 of the
            //   budget, others cost 1). Slots not emitted carry the
            //   previous line's value into strip 0 of this line — the
            //   strips visible-area swaps still get to evolve them.
            //   strips's k_min=1 means slot 0 isn't touched mid-line, so
            //   carrying its prev value is safe; for slots 1..7 the
            //   approximation (assume prev line landed near its sliced
            //   target, ignore strips residue) is good enough that rows
            //   stay independent for parallel_for.
            std::array<bool, kBaseColors> emitted{};
            if (copper_changes_override == 0) {
                // Full reset path (default; 9 hblank MOVEs).
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    auto regs = pf2_writes(k);
                    for (int reg : regs) {
                        if (reg < 0) continue;
                        line_moves[y].push_back(make_move(
                            static_cast<std::uint8_t>(reg), palette::linear_to_ocs(target[k]), -1));
                    }
                    emitted[k] = true;
                }
            } else {
                std::size_t hblank_budget = std::min<std::size_t>(copper_changes_override,
                                                                  kMaxCombined);
                // Score per-slot diff in OKLab² between actual hw_state
                // (the color the chip is sitting on at end of line y-1)
                // and target = sliced_palettes[y]. Top-K wins emit-budget.
                std::array<std::pair<int, float>, kBaseColors> diffs{};
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    auto a = color_space::linear_to_oklab(hw_state_local[k]);
                    auto b = color_space::linear_to_oklab(target[k]);
                    float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                    diffs[k] = {static_cast<int>(k), color_space::fma_dist_sq(dL, da, db)};
                }
                std::sort(diffs.begin(), diffs.end(), [](auto& a, auto& b) {
                    return a.second > b.second;
                });
                std::size_t move_cost = 0;
                for (auto& d : diffs) {
                    if (d.second <= 0.0f) break;  // no further differences
                    auto k = static_cast<std::size_t>(d.first);
                    std::size_t cost = (k == 0) ? 2 : 1;  // dual-write for k=0
                    if (move_cost + cost > hblank_budget) continue;
                    auto regs = pf2_writes(k);
                    for (int reg : regs) {
                        if (reg < 0) continue;
                        line_moves[y].push_back(make_move(
                            static_cast<std::uint8_t>(reg), palette::linear_to_ocs(target[k]), -1));
                    }
                    emitted[k] = true;
                    move_cost += cost;
                }
            }

            // Build strip-0 palette from REAL post-HBLANK hw state:
            //   emitted slots ← target (this line's sliced MOVE landed)
            //   skipped slots ← hw_state (carried from prev line)
            // This is what the chip actually displays in the leftmost
            // strip — encoding pixels against any other palette would
            // show the artefacts users hit with --slice-changes 1..7.
            for (std::size_t k = 0; k < kBaseColors; ++k)
                P[k] = emitted[k] ? target[k] : hw_state_local[k];
            recompute_lab_local();
            strip_palettes[0] = P;
            strip_pal_lab[0] = P_lab;

            // Re-bind base_index for this row against the per-line sliced-
            // evolved 8-palette. Previously the bindings came from a frame-
            // level dither against the FRAME-INIT palette; sliced evolves the
            // palette across lines so frame-init bindings make the cluster
            // planner score against stale clusters and pick swaps that hurt
            // the actual rendered output. Cost: width × 8 distance compares
            // per row, negligible at width=320.
            for (std::size_t x = 0; x < width; ++x) {
                auto& tgt = img_lab[y * width + x];
                std::size_t best_k = 0;
                float best_d = std::numeric_limits<float>::max();
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    float dL = tgt.L - P_lab[k].L;
                    float da = tgt.a - P_lab[k].a;
                    float db = tgt.b - P_lab[k].b;
                    float d = color_space::fma_dist_sq(dL, da, db);
                    if (d < best_d) {
                        best_d = d;
                        best_k = k;
                    }
                }
                base_index[y * width + x] = static_cast<std::uint8_t>(best_k);
            }

            // 2. Line-gate WAIT — opens the strips chain at HPOS=line_gate_hpos.
            line_moves[y].push_back(
                make_wait(static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));

            // 3. 20 strips MOVEs back-to-back. Joint beam-search planner:
            //    explores B parallel sequences of (slot → register, color)
            //    decisions instead of greedy max-reduction per slot. Greedy
            //    locked onto the most-populated register slot-after-slot
            //    because total summed error scales with cluster size — the
            //    planner has no incentive to pick under-utilised registers
            //    even when doing so would unlock far better total-line
            //    coverage. Beam search picks the chain with min total strip
            //    error across all slots, naturally favoring decisions that
            //    don't waste the line on micro-tweaking one register.
            //
            //    Score: per-line per-strip per-register cluster stats are
            //    pre-computed (count, OKLab centroid, spread). Strip error
            //    given palette P = Σ_k count[k]·||centroid[k]-P[k]||² +
            //    spread[k]. O(8) per state-evaluation.

            struct StripStats {
                std::vector<Color3f> cands;  // OCS-quantized
                std::vector<color_space::OKLab> cands_lab;
                std::array<std::uint32_t, kBaseColors> cnt{};
            };

            // Pre-compute per-strip stats. strips[0] = pixels [0..slot0).
            // strips[s+1] = pixels [slot[s] .. slot[s+1]) — the strip slot s
            // controls. Slot s's MOVE affects strips[s+1] (and beyond if no
            // later slot overrides P[k]).
            std::vector<StripStats> strips(num_strips);
            // Per-strip pixel OKLab arrays for the per-strip dither error
            // scorer. Replaces cluster-centroid math: we evaluate candidate
            // swaps against each pixel's nearest-of-8 picker outcome — which
            // is what the actual encoder's picker does. Same fix as
            // EHB v1.26.4. Cost: ~16 px per strip × 16 strips = 256 entries
            // per row, cheap.
            std::vector<std::vector<color_space::OKLab>> strip_pixels_lab(num_strips);
            std::vector<StripPixelsSoA> strip_pixels_soa(num_strips);
            auto strip_x_range = [&](std::size_t s) {
                std::size_t lo = (s == 0) ? std::size_t{0}
                                          : std::min(width,
                                                     static_cast<std::size_t>(
                                                         table.slots[s - 1].pixel_x));
                std::size_t hi = (s < table.slots.size())
                                     ? std::min(width,
                                                static_cast<std::size_t>(table.slots[s].pixel_x))
                                     : width;
                return std::pair<std::size_t, std::size_t>{lo, hi};
            };
            for (std::size_t s = 0; s < num_strips; ++s) {
                auto [x_lo, x_hi] = strip_x_range(s);
                if (x_lo >= x_hi) continue;
                strip_pixels_lab[s].reserve(x_hi - x_lo);
                for (std::size_t x = x_lo; x < x_hi; ++x)
                    strip_pixels_lab[s].push_back(img_lab[y * width + x]);
                build_strip_soa(strip_pixels_soa[s], strip_pixels_lab[s]);
                // Per-cluster centroid is still used to seed candidate set —
                // pixels near a cluster centroid are good swap targets even
                // though the SCORING is now per-pixel min-of-8.
                std::array<double, kBaseColors> sumL{}, suma{}, sumb{};
                for (std::size_t x = x_lo; x < x_hi; ++x) {
                    auto k = static_cast<std::size_t>(base_index[y * width + x]);
                    auto& lab = img_lab[y * width + x];
                    sumL[k] += static_cast<double>(lab.L);
                    suma[k] += static_cast<double>(lab.a);
                    sumb[k] += static_cast<double>(lab.b);
                    ++strips[s].cnt[k];
                }
                std::array<bool, 4096> seen{};
                auto ocs_key = [](const Color3f& c) {
                    int r = static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 15.0f));
                    int g = static_cast<int>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 15.0f));
                    int b = static_cast<int>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 15.0f));
                    return static_cast<std::size_t>((r << 8) | (g << 4) | b);
                };
                auto add_cand = [&](Color3f c) {
                    auto cs = palette::quantize_to_ocs(c);
                    auto key = ocs_key(cs);
                    if (!seen[key]) {
                        seen[key] = true;
                        strips[s].cands.push_back(cs);
                        strips[s].cands_lab.push_back(color_space::linear_to_oklab(cs));
                    }
                };
                for (std::size_t x = x_lo; x < x_hi; ++x)
                    add_cand(src[x, y]);
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    if (strips[s].cnt[k] == 0) continue;
                    auto cnt_d = static_cast<double>(strips[s].cnt[k]);
                    color_space::OKLab cd{static_cast<float>(sumL[k] / cnt_d),
                                          static_cast<float>(suma[k] / cnt_d),
                                          static_cast<float>(sumb[k] / cnt_d)};
                    add_cand(color_space::oklab_to_linear(cd).clamped());
                }
            }

            // Per-strip dither error scorer. For each pixel in the strip,
            // computes the OKLab² distance to its nearest-of-8 entry in the
            // 8-base palette — matches what the actual encoder picker does.
            // Replaces cluster-centroid math (which scored against frozen
            // mean colors that the picker doesn't actually use).
            // SoA-SIMD scorer; inverts loop nest (outer k, inner pixels)
            // so AVX2 packs 8 pixels per iter. See helpers at top of file.
            thread_local std::vector<float> tl_pixel_min_dpf;
            auto strip_err_dither =
                [&](std::size_t s, const std::array<color_space::OKLab, kBaseColors>& P_lab_v) {
                    auto& soa = strip_pixels_soa[s];
                    if (soa.valid_n == 0) return 0.0;
                    reset_pixel_min(tl_pixel_min_dpf, soa);
                    for (std::size_t k = k_min; k < kBaseColors; ++k) {
                        min_dist_update(
                            soa, P_lab_v[k].L, P_lab_v[k].a, P_lab_v[k].b, tl_pixel_min_dpf.data());
                    }
                    return sum_pixel_min(tl_pixel_min_dpf.data(), soa.padded_n);
                };

            // Beam search params. Tuned by sweep across the test image set
            // (lovers/photo/fromthe/space3/electrichues02). B=64 is the
            // sweet spot for DPF: peak preview-PSNR (33.95 dB) at ~1s per
            // 320×213 image. Wider beams (B=128, 192, 256) keep lowering
            // planner error but PSNR plateaus — the planner's OKLab²
            // metric drifts from blurred-sRGB PSNR past this point.
            // K=16 saturates given 7 modifiable regs × kPerRegCap=4 = 28.
            constexpr std::size_t kBeamWidth = 64;
            constexpr std::size_t kCandsPerSlot = 16;
            struct BeamNode {
                std::array<Color3f, kBaseColors> P;
                std::array<color_space::OKLab, kBaseColors> P_lab;
                std::array<int, 32> dec_reg{};
                std::array<Color3f, 32> dec_color{};
                double cum_err = 0;
            };
            std::vector<BeamNode> beam(1);
            beam[0].P = P;
            beam[0].P_lab = P_lab;
            for (auto& d : beam[0].dec_reg)
                d = -1;
            beam[0].cum_err = strip_err_dither(0, P_lab);

            std::vector<BeamNode> next;
            next.reserve(kBeamWidth * (kCandsPerSlot + 1));

            for (std::size_t s = 0; s < table.slots.size(); ++s) {
                next.clear();
                auto& st = strips[s + 1];
                bool strip_empty = (strip_pixels_lab[s + 1].empty());

                for (auto& state : beam) {
                    double filler_err = strip_empty ? 0.0 : strip_err_dither(s + 1, state.P_lab);
                    {
                        BeamNode child = state;
                        child.dec_reg[s] = -1;
                        child.cum_err += filler_err;
                        next.push_back(child);
                    }
                    if (strip_empty) continue;

                    // Per-strip dither error candidate scoring. EHB-style
                    // pixel_min_excl_k cache: for each candidate slot k,
                    // precompute every pixel's min distance over the 7 OTHER
                    // base slots. Then per candidate we just compare against
                    // c_lab once per pixel — O(width × |cands|) instead of
                    // O(width × 8 × |cands|). SoA via strip_pixels_soa[s+1].
                    struct Move {
                        int reg;
                        std::size_t cand_idx;
                        double err;
                    };
                    std::vector<Move> moves;
                    moves.reserve(kBaseColors * st.cands.size());

                    auto& soa_pixels = strip_pixels_soa[s + 1];
                    for (std::size_t k = k_min; k < kBaseColors; ++k) {
                        // --reserve-range: skip locked PF2 slots so the mid-line
                        // strips planner can't overwrite the user's fixed color.
                        if (reserved_mask_dpf[k]) continue;
                        reset_pixel_min(tl_pixel_min_dpf, soa_pixels);
                        for (std::size_t k2 = k_min; k2 < kBaseColors; ++k2) {
                            if (k2 == k) continue;
                            min_dist_update(soa_pixels,
                                            state.P_lab[k2].L,
                                            state.P_lab[k2].a,
                                            state.P_lab[k2].b,
                                            tl_pixel_min_dpf.data());
                        }
                        for (std::size_t ci = 0; ci < st.cands.size(); ++ci) {
                            auto& c_lab = st.cands_lab[ci];
                            double e = dist_min1_sum(
                                soa_pixels, tl_pixel_min_dpf.data(), c_lab.L, c_lab.a, c_lab.b);
                            if (e >= filler_err) continue;
                            moves.push_back({static_cast<int>(k), ci, e});
                        }
                    }
                    // Per-state per-register cap so beam expansion covers
                    // multiple registers — without it, the top-K moves can
                    // all target the same dominant register with slight
                    // color variations.
                    std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
                        return a.err < b.err;
                    });
                    constexpr std::size_t kPerRegCap = 4;
                    std::array<std::size_t, kBaseColors> reg_taken{};
                    std::vector<Move> picked;
                    picked.reserve(kCandsPerSlot);
                    for (auto& m : moves) {
                        auto rk = static_cast<std::size_t>(m.reg);
                        if (reg_taken[rk] >= kPerRegCap) continue;
                        picked.push_back(m);
                        ++reg_taken[rk];
                        if (picked.size() >= kCandsPerSlot) break;
                    }
                    moves = std::move(picked);

                    for (auto& m : moves) {
                        auto reg_idx = static_cast<std::size_t>(m.reg);
                        BeamNode child = state;
                        child.P[reg_idx] = st.cands[m.cand_idx];
                        child.P_lab[reg_idx] = st.cands_lab[m.cand_idx];
                        child.dec_reg[s] = m.reg;
                        child.dec_color[s] = st.cands[m.cand_idx];
                        child.cum_err += m.err;
                        next.push_back(child);
                    }
                }

                std::size_t keep_b = std::min(kBeamWidth, next.size());
                if (next.size() > keep_b) {
                    std::partial_sort(
                        next.begin(),
                        next.begin() + static_cast<std::ptrdiff_t>(keep_b),
                        next.end(),
                        [](const BeamNode& a, const BeamNode& b) { return a.cum_err < b.cum_err; });
                    next.resize(keep_b);
                }
                beam.swap(next);
            }

            auto& best = *std::min_element(
                beam.begin(), beam.end(), [](const BeamNode& a, const BeamNode& b) {
                    return a.cum_err < b.cum_err;
                });
            for (std::size_t s = 0; s < table.slots.size(); ++s) {
                int reg = best.dec_reg[s];
                if (reg < 0) {
                    line_moves[y].push_back(make_move(kFillerReg, kFillerVal, static_cast<int>(s)));
                } else {
                    auto reg_idx = static_cast<std::size_t>(reg);
                    Color3f col = best.dec_color[s];
                    P[reg_idx] = col;
                    P_lab[reg_idx] = color_space::linear_to_oklab(col);
                    line_moves[y].push_back(make_move(static_cast<std::uint8_t>(8 + reg_idx),
                                                      palette::linear_to_ocs(col),
                                                      static_cast<int>(s)));
                    total_moves_atomic.fetch_add(1);
                }
                strip_palettes[s + 1] = P;
                strip_pal_lab[s + 1] = P_lab;
            }

            // 4. End-of-line WAIT — release copper to the next line's section.
            line_moves[y].push_back(
                make_wait(static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));

            // Carry P forward as next-line hw_state — captures every
            // emitted sliced MOVE + every applied strips swap. Only writes
            // the shared outer hw_state in serial mode (parallel mode
            // never reads it).
            if (serial_path) hw_state = P;

            // Snapshot pass-1 strip state for this row; pass-2 dither runs
            // over the whole image once, after the per-row loop.
            strip_palettes_per_row[y] = strip_palettes;
            strip_pal_lab_per_row[y] = strip_pal_lab;

            if (on_progress) {
                auto done = rows_done.fetch_add(1) + 1;
                if (height > 0 && (done & 0xF) == 0xF) {
                    report_pass(pass, static_cast<float>(done) / static_cast<float>(height));
                }
            }
        };
        if (serial_path) {
            for (std::size_t y = 0; y < height; ++y)
                run_row(y);
        } else {
            pipeline::parallel_for(height, run_row);
        }
        total_moves = total_moves_atomic.load();

        // ---- Pass 2: whole-image dither against per-row, per-strip
        // palettes. Driver owns ED scaffolding (kernel, serpentine, bias
        // map, Riemersma queue scaling, ordered offsets);
        // picker resolves x_strip[x] → row's strip palette.
        {
            float te = dither::diffuse_raw_buffer(
                src,
                dither_settings,
                [&](const color_space::OKLab& target,
                    std::size_t x,
                    std::size_t y) -> dither::PickResult {
                    auto s = static_cast<std::size_t>(x_strip[x]);
                    auto& pal = strip_palettes_per_row[y][s];
                    auto& pl_lab = strip_pal_lab_per_row[y][s];
                    std::span<const color_space::OKLab> pl_span(pl_lab.data(), kBaseColors);

                    std::size_t k = 0;
                    color_space::OKLab chosen{};
                    float thr = dither::pick_palette_index_with_ostro(dither_settings.method,
                                                                      target,
                                                                      pl_span,
                                                                      x,
                                                                      y,
                                                                      dither_settings.strength,
                                                                      k_min,
                                                                      k,
                                                                      chosen);
                    indices[y * width + x] = static_cast<std::uint8_t>(k);
                    preview[x, y] = pal[k];
                    return {chosen, thr};
                });
            total_error = static_cast<double>(te);
        }

        // DBS post-pass refinement. Per-row, per-strip palettes resolve
        // through the same x_strip[x] lookup as the picker above; DBS
        // sweeps each pixel and tries all 8 candidates in that pixel's
        // effective strip palette, keeping any toggle that lowers the
        // HVS-blurred OKLab cost. After this pass we re-render `preview`
        // from the (possibly-changed) indices so caller-visible buffers
        // stay consistent.
        if (dither_settings.method == dither::Method::dbs) {
            dither::apply_dbs_post_pass(
                src,
                indices,
                [&](std::size_t x, std::size_t y) -> std::span<const color_space::OKLab> {
                    auto s = static_cast<std::size_t>(x_strip[x]);
                    auto& pl_lab = strip_pal_lab_per_row[y][s];
                    return {pl_lab.data(), kBaseColors};
                });
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < width; ++x) {
                    auto s = static_cast<std::size_t>(x_strip[x]);
                    preview[x, y] = strip_palettes_per_row[y][s][indices[y * width + x]];
                }
            }
        }
        report_pass(pass + 1, 0.0f);
    }  // kPasses
    if (on_progress) on_progress(1.0f, "done");

    // ---- 4. 3-plane PF2 encoding, then expand to 6-plane DPF ------------
    auto enc = bitplane::encode(indices,
                                width,
                                height,
                                /*depth=*/3,
                                bitplane::Layout::interleaved);
    if (!enc) return std::unexpected{enc.error()};
    auto expanded = bitplane::expand_to_dpf_pf2(*enc);
    if (!expanded) return std::unexpected{expanded.error()};

    // ---- 5. Output palette: PF2 base entries at OCS DPF addresses -----
    // Per the OCS DPF combiner rule (above), PF2 index 0 displays as
    // COLOR00 (NOT COLOR08), and PF2 indices 1..7 display as
    // COLOR09..15. COLOR08 is unused on OCS DPF. Address the frame-init
    // palette accordingly so the cpp viewer renders correctly on real
    // OCS hardware (and on AGA in OCS-DPF mode without BPLCON3 PF2OF).
    //
    // In debug_overlay mode all entries stay at 0x0000 — together with
    // the forced-zero per-line MOVEs this means the viewer's frame-init
    // writes black to every register and only strips MOVEs change colors.
    std::vector<Color3f> output_palette(16, Color3f{0.0f, 0.0f, 0.0f});
    if (!debug_overlay) {
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            auto regs = pf2_writes(k);
            for (int reg : regs) {
                if (reg < 0) continue;
                output_palette[static_cast<std::size_t>(reg)] = base_palette[k];
            }
        }
    }

    // ---- 5b. Optional PF1 ruler markers (slot-tuning aid) ---------------
    // Yellow vertical guides at 4 / 8 / 16 px, with hierarchy by height:
    //   * x % 16 == 0           → full height
    //   * x % 8  == 0  (not 16) → top half
    //   * x % 4  == 0  (not 8)  → top quarter
    // Markers paint into PF1 LSB (= dst plane 0 of the 6-plane DPF
    // output) at column x. PF1 in front of PF2, so non-zero PF1 pixels
    // override the image. palette[1] is recolored to yellow so PF1
    // index 1 displays the ruler color. Also recolor the same in the
    // per-pixel preview so PNG / stats reflect the markers.
    if (debug_overlay) {
        output_palette[1] = Color3f{1.0f, 0.0f, 0.0f};  // red

        auto& dst = *expanded;
        auto bpr = dst.bytes_per_row;
        auto h_full = height;
        auto h_half = height / 2;
        auto h_quarter = height / 4;

        auto set_pf1_lsb = [&](std::size_t x, std::size_t y) {
            auto row_off = dst.plane_row_offset(/*plane=*/0, y);
            dst.data[row_off + x / 8] |= static_cast<std::uint8_t>(1u << (7 - (x % 8)));
        };
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t marker_h = 0;
            if (x % 16 == 0)
                marker_h = h_full;
            else if (x % 8 == 0)
                marker_h = h_half;
            else if (x % 4 == 0)
                marker_h = h_quarter;
            else
                continue;
            for (std::size_t y = 0; y < marker_h; ++y) {
                set_pf1_lsb(x, y);
                preview[x, y] = Color3f{1.0f, 0.0f, 0.0f};
            }
        }
        (void)bpr;
    }

    ScapResult res;
    res.planes = *std::move(expanded);
    res.palette = std::move(output_palette);
    res.slot_table = table;
    res.total_error = static_cast<float>(total_error);
    res.avg_changes_per_line = height > 0
                                   ? static_cast<float>(total_moves) / static_cast<float>(height)
                                   : 0.0f;
    {
        std::size_t total_all = 0, row_max = 0;
        std::size_t total_hb = 0, hb_max = 0;
        std::size_t total_vis = 0, vis_max = 0;
        for (auto& row : line_moves) {
            std::size_t rm = 0, hb = 0, vis = 0;
            bool past_line_gate = false;
            for (auto& op : row) {
                if (op.kind == ScapOpKind::kWait) {
                    past_line_gate = true;
                    continue;
                }
                ++rm;
                if (past_line_gate)
                    ++vis;
                else
                    ++hb;
            }
            total_all += rm;
            if (rm > row_max) row_max = rm;
            total_hb += hb;
            if (hb > hb_max) hb_max = hb;
            total_vis += vis;
            if (vis > vis_max) vis_max = vis;
        }
        auto h = static_cast<float>(height ? height : 1);
        res.avg_total_moves_per_line = static_cast<float>(total_all) / h;
        res.max_moves_per_line = row_max;
        res.avg_hblank_moves_per_line = static_cast<float>(total_hb) / h;
        res.max_hblank_moves_per_line = hb_max;
        res.avg_visible_moves_per_line = static_cast<float>(total_vis) / h;
        res.max_visible_moves_per_line = vis_max;
    }
    res.line_moves = std::move(line_moves);
    // Snap preview to OCS RGB444 — strips is OCS-only, and the snap-defer
    // patches in copper/scap/ham left intermediate per-strip palettes at
    // full 8-bit precision. The actual chip displays RGB444; preview must
    // match. Without this, color counts > 4096 leak into the preview.
    for (auto& p : preview.pixels())
        p = palette::quantize_to_ocs(p);
    res.rendered = std::move(preview);
    return res;
}

// half_brite() lives in palette.hpp; pull it into this TU's lookup.
using palette::half_brite;

// EHB strips slot-tuning debug bundle. All bitplane pixels use a
// single shared register (index 2). Frame-init palette puts that
// register at black; strips slot s alternates the register between
// white (s even) and black (s odd) at the slot's MOVE position. Net
// visual: 16-px-wide black/white stripes with the transition AT the
// slot's actual hardware MOVE landing — the visible edge IS the
// timing measurement.
//
// PF1-style yellow rulers aren't available on EHB (no PF1 layer),
// so the ruler paints into the bitplane data with index 1 = yellow
// (locked in sliced). Ruler pixels override the stripe content but
// give stable x-coord references at 4/8/16-px hierarchy.
static Result<ScapResult> encode_scap_ehb_debug(std::size_t width, std::size_t height) {
    auto& table = kStrips6bplEhb;
    constexpr std::size_t kStripeReg = 2;  // shared register all pixels use
    constexpr std::uint16_t kBlack = 0x0000;
    constexpr int kVStart = 44;

    // ---- Bitplane data: every pixel = index kStripeReg = 0b00010 -------
    auto enc = bitplane::BitplaneData{};
    auto aligned_w = (width + 15u) & ~std::size_t{15};
    enc.width = width;
    enc.height = height;
    enc.depth = 6;
    enc.bytes_per_row = aligned_w / 8;
    enc.layout = bitplane::Layout::interleaved;
    enc.data.assign(enc.total_bytes(), 0);
    // Set every byte of plane 1 (= bit value 2) to 0xFF → all pixels = idx 2
    for (std::size_t y = 0; y < height; ++y) {
        auto off = enc.plane_row_offset(/*plane=*/1, y);
        std::fill_n(enc.data.data() + off, enc.bytes_per_row, static_cast<std::uint8_t>(0xFF));
    }
    // Yellow rulers: paint ruler pixels with index 1 = 0b00001.
    // Clear plane 1 (drop idx 2), set plane 0 (add idx 1).
    auto h_full = height;
    auto h_half = height / 2;
    auto h_quarter = height / 4;
    auto set_bit = [&](std::size_t plane, std::size_t y, std::size_t x, bool on) {
        auto off = enc.plane_row_offset(plane, y);
        auto byte = x / 8;
        auto mask = static_cast<std::uint8_t>(1u << (7 - (x % 8)));
        if (on)
            enc.data[off + byte] |= mask;
        else
            enc.data[off + byte] &= ~mask;
    };
    for (std::size_t x = 0; x < width; ++x) {
        std::size_t marker_h = 0;
        if (x % 16 == 0)
            marker_h = h_full;
        else if (x % 8 == 0)
            marker_h = h_half;
        else if (x % 4 == 0)
            marker_h = h_quarter;
        else
            continue;
        for (std::size_t yy = 0; yy < marker_h; ++yy) {
            set_bit(0, yy, x, true);   // plane 0 ON  → idx |= 1
            set_bit(1, yy, x, false);  // plane 1 OFF → idx &= ~2 (= idx 1)
        }
    }

    // ---- Output palette (32 base entries) ----------------------------
    std::vector<Color3f> palette(32, Color3f{0.0f, 0.0f, 0.0f});
    palette[1] = Color3f{1.0f, 0.0f, 0.0f};  // red ruler
    // palette[kStripeReg] = black (default 0x000); strips MOVEs change it.

    // ---- Per-line copper: 1 reset MOVE + line-gate WAIT + 19 swaps ----
    std::vector<std::vector<ScapMove>> line_moves(height);
    for (std::size_t y = 0; y < height; ++y) {
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);
        // Reset the shared register to black at top of each line (the
        // ONE per-line sliced MOVE we need; fits in hblank trivially).
        line_moves[y].push_back(make_move(static_cast<std::uint8_t>(kStripeReg), kBlack, -1));
        // Line-gate WAIT.
        line_moves[y].push_back(make_wait(static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));
        // 19 strips MOVEs: opposing primary/complement RGB pairs on the
        // shared register. Pair N cycles through (R,C), (G,M), (B,Y);
        // pair-mod-3 picks which axis. Each stripe is a single solid
        // saturated color. Vivid hues make slot positions easy to
        // pick out against the red ruler.
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            auto pair_n = s / 2;
            std::uint16_t color = 0, complement = 0;
            switch (pair_n % 3) {
            case 0:
                color = 0x0F00;
                complement = 0x00FF;
                break;  // R / C
            case 1:
                color = 0x00F0;
                complement = 0x0F0F;
                break;  // G / M
            case 2:
                color = 0x000F;
                complement = 0x0FF0;
                break;  // B / Y
            }
            std::uint16_t v = (s % 2 == 0) ? color : complement;
            line_moves[y].push_back(
                make_move(static_cast<std::uint8_t>(kStripeReg), v, static_cast<int>(s)));
        }
        // End-of-line WAIT.
        line_moves[y].push_back(
            make_wait(static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));
    }

    // Build a rendered preview matching what the planner expects: every
    // pixel = palette[kStripeReg] except ruler markers = palette[1].
    Image preview(width, height);
    for (std::size_t yy = 0; yy < height; ++yy) {
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t marker_h = 0;
            if (x % 16 == 0)
                marker_h = h_full;
            else if (x % 8 == 0)
                marker_h = h_half;
            else if (x % 4 == 0)
                marker_h = h_quarter;
            preview[x, yy] = (yy < marker_h)
                                 ? Color3f{1.0f, 0.0f, 0.0f}  // ruler red
                                 // Stripe approximation: white if "MOVE-after" position,
                                 // black if before. Just paint expected stripe pattern
                                 // assuming MOVEs land at slots[s].pixel_x.
                                 : ([&]() {
                                       Color3f c{0, 0, 0};
                                       for (std::size_t s = 0; s < table.slots.size(); ++s) {
                                           if (static_cast<int>(x) >= table.slots[s].pixel_x &&
                                               (s + 1 == table.slots.size() ||
                                                static_cast<int>(x) < table.slots[s + 1].pixel_x)) {
                                               // Mirror the strips MOVE values used above:
                                               // pair N gets (0xFFF - N·0x111, N·0x111).
                                               std::size_t pair_int = s / 2;
                                               // Mirror cpp: cycle (R,C), (G,M), (B,Y).
                                               Color3f base, comp;
                                               switch (pair_int % 3) {
                                               case 0:
                                                   base = {1, 0, 0};
                                                   comp = {0, 1, 1};
                                                   break;
                                               case 1:
                                                   base = {0, 1, 0};
                                                   comp = {1, 0, 1};
                                                   break;
                                               default:
                                                   base = {0, 0, 1};
                                                   comp = {1, 1, 0};
                                                   break;
                                               }
                                               c = (s % 2 == 0) ? base : comp;
                                               break;
                                           }
                                       }
                                       return c;
                                   })();
        }
    }

    ScapResult res;
    res.planes = std::move(enc);
    res.palette = std::move(palette);
    res.slot_table = table;
    res.total_error = 0.0f;
    res.avg_changes_per_line = 0.0f;
    res.avg_total_moves_per_line = static_cast<float>(line_moves.empty() ? 0
                                                                         : 1 + table.slots.size());
    res.max_moves_per_line = 1 + table.slots.size();
    res.avg_hblank_moves_per_line = 1.0f;
    res.max_hblank_moves_per_line = 1;
    res.avg_visible_moves_per_line = static_cast<float>(table.slots.size());
    res.max_visible_moves_per_line = table.slots.size();
    res.line_moves = std::move(line_moves);
    for (auto& p : preview.pixels())
        p = palette::quantize_to_ocs(p);
    res.rendered = std::move(preview);
    return res;
}

Result<ScapResult> encode_strips_ehb_ocs(
    const Image& image,
    int width_arg,
    int height_arg,
    bool lock_color0,
    const dither::Settings& dither_settings,
    std::size_t copper_changes_override,
    int palette_diversity,
    bool debug_overlay,
    std::function<void(float, std::string_view)> on_progress,
    bool enable_best,
    int sliced_spread_radius,
    float sliced_spread_decay,
    bool sliced_vertical_dither,
    std::span<const Color3f> external_palette,
    const std::vector<std::pair<std::size_t, Color3f>>& reserved_slots,
    bool sliced_beam) {
    // --best: 8 jitter seeds (32-base palette has shallower basins
    // than DPF's 8-base, so heavy jitter sampling buys less here).
    // Total 5×4×8 + 1 = 161 trials, ~30–40 s on 8 cores; 2× for the
    // beam sweep.
    if (enable_best) {
        // No beam axis here — strips' mid-line MOVE planner already
        // diversifies per-row enough that beam doesn't fire productively
        // (0 S2 gain across 5 test images). Honour caller's sliced_beam
        // verbatim. See the matching comment in encode_strips_dpf_ocs.
        auto best = pipeline::best_sweep<ScapResult>(
            image,
            dither_settings,
            palette_diversity,
            /*jitter_count=*/8,
            [&](const Image& jittered_in, const dither::Settings& d, int div) {
                return encode_strips_ehb_ocs(jittered_in,
                                             width_arg,
                                             height_arg,
                                             lock_color0,
                                             d,
                                             copper_changes_override,
                                             div,
                                             debug_overlay,
                                             /*on_progress=*/{},
                                             /*enable_best=*/false,
                                             sliced_spread_radius,
                                             sliced_spread_decay,
                                             sliced_vertical_dither,
                                             external_palette,
                                             reserved_slots,
                                             sliced_beam);
            },
            [](const ScapResult& r) -> const Image& { return r.rendered; },
            on_progress,
            /*jitter_amplitude=*/1.0f);
        if (best.has_value()) return std::move(*best);
    }
    auto& table = kStrips6bplEhb;
    if (table.slots.empty()) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "Strips EHB planner: kStrips6bplOcs slot table is empty",
        }};
    }

    auto width = (width_arg > 0) ? static_cast<std::size_t>(width_arg) : image.width();
    auto height = (height_arg > 0) ? static_cast<std::size_t>(height_arg) : image.height();
    if (debug_overlay) return encode_scap_ehb_debug(width, height);
    if (image.width() != width || image.height() != height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Strips EHB planner: image is {}x{} but caller asked "
                        "for {}x{} — resize before calling",
                        image.width(),
                        image.height(),
                        width,
                        height),
        }};
    }

    auto& src = image;
    constexpr std::size_t kBaseColors = 32;
    constexpr std::size_t kEffective = 64;
    constexpr int kRegBase = 0;

    // Reserved-slot mask: strips planner must skip these registers in its
    // mid-line swap candidate generation (encode_copper already keeps
    // them locked across sliced scanlines and excludes them from the dither
    // candidate set).
    std::array<bool, kBaseColors> reserved_mask_ehb{};
    for (auto& [idx, _] : reserved_slots)
        if (idx < kBaseColors) reserved_mask_ehb[idx] = true;

    // ---- 1. sliced first: per-line palette evolution.
    // strips is a layer ON TOP OF sliced, not a replacement. The sliced encoder
    // picks a per-line set of register diffs that evolves the 32-base
    // palette across scanlines; strips then adds 20 mid-line MOVEs per
    // line on top of that evolving state. Without this layering the
    // image looks single-palette per frame, which is very visible on
    // photographic content.
    // sliced and strips share the OCS hblank's MOVE budget. Adaptive split:
    //   * Each line's hblank fits up to kHblankCeiling MOVEs (=13 for
    //     OCS empirical safe ceiling). Hblank load on line y+1 is
    //     SLICED_changes[y+1] + STRIPS_swaps[y] (revert) — exceeding 13
    //     causes hardware overflow on busy images (verified per
    //     fantasy.png). So per-line: STRIPS_swaps[y] ≤ kHblankCeiling -
    //     SLICED_changes[y+1]. SLICED_changes per line comes straight from
    //     copper_result->scanline_changes (already planned).
    //   * --copper-changes N caps the COMBINED budget globally.
    //     sliced gets min(N, 2), strips gets the per-line adaptive value
    //     bounded by N - SLICED_share.
    //   * Auto: same adaptive logic, no global cap beyond hblank.
    constexpr std::size_t kHblankCeiling = 13;
    constexpr std::size_t kMaxCombinedEhb = 20;  // sliced=2 + strips=18 visible max
    std::size_t total_budget_ehb = (copper_changes_override > 0)
                                       ? std::min<std::size_t>(copper_changes_override,
                                                               kMaxCombinedEhb)
                                       : kMaxCombinedEhb;
    std::size_t sliced_share_ehb = std::min<std::size_t>(total_budget_ehb, 2u);
    std::size_t strips_share_ehb_max = total_budget_ehb - sliced_share_ehb;
    // External-palette plumbing for EHB: --palette becomes the 32-color
    // base palette; encode_copper produces a depth-5 frame, hardware
    // halfbrites mirror it.
    std::vector<Color3f> ehb_user_pal;
    if (!external_palette.empty()) {
        ehb_user_pal.assign(external_palette.begin(), external_palette.end());
        if (ehb_user_pal.size() > 32) ehb_user_pal.resize(32);
    } else {
        // No external palette: build the global EHB base ourselves
        // with PNN + pair-aware refinement (and 1-opt on --best),
        // then hand it to encode_copper. Same shape as the EHB+sliced
        // path in api.cpp — gives the strips planner a better seed
        // than encode_copper's internal histogram quantizer.
        auto qfn = [&](std::size_t k) -> Result<Palette> {
            auto q = quantize::quantize(src, k, quantize::Algorithm::pnn, palette_diversity);
            if (!q) return std::unexpected{q.error()};
            Palette p = std::move(*q);
            for (auto& c : p.colors)
                c = palette::quantize_to_ocs(c);
            return p;
        };
        // Subtract reserves from k1: without this, the quantizer fills
        // 31 slots and reserves overlay slots 1-15 (or wherever),
        // displacing the darks/mids the dither needs. Same bug as
        // copper.cpp and api.cpp std-lores; the fix mirrors them.
        std::size_t k1 = lock_color0 ? 31 : 32;
        if (k1 > reserved_slots.size())
            k1 -= reserved_slots.size();
        else
            k1 = 1;
        std::size_t kfb = std::min<std::size_t>(
            32, k1 + (lock_color0 ? std::size_t{1} : std::size_t{0}));
        auto qr = palette_locks::two_pass_quantize(qfn, k1, kfb, lock_color0);
        if (qr) {
            Palette seed_pal = std::move(*qr);
            // Build the 32-slot seed: lock_zero at 0, reserves at their
            // indices, quantizer colors at the remaining unlocked slots
            // (skipping reserved indices when filling so reserves don't
            // displace anything).
            std::vector<Color3f> placed(32, Color3f{0.0f, 0.0f, 0.0f});
            std::vector<bool> placed_locked(32, false);
            if (lock_color0) {
                placed[0] = Color3f{0.0f, 0.0f, 0.0f};
                placed_locked[0] = true;
            }
            for (auto& [idx, color] : reserved_slots) {
                if (idx < 32) {
                    placed[idx] = color;
                    placed_locked[idx] = true;
                }
            }
            std::size_t qi = 0;
            for (std::size_t i = 0; i < 32; ++i) {
                if (placed_locked[i]) continue;
                if (qi < seed_pal.colors.size()) placed[i] = seed_pal.colors[qi++];
            }
            seed_pal.colors = std::move(placed);
            // Build refine's locked mask: lock_color0 + reserved slots are
            // held fixed during refine so its iterative convergence is
            // not biased by the reserve color (and the unlocked slots'
            // colors stay deterministic across reserve-color choices).
            std::array<bool, 32> refine_locked{};
            if (lock_color0) refine_locked[0] = true;
            for (auto& [idx, _] : reserved_slots) {
                if (idx < refine_locked.size()) refine_locked[idx] = true;
            }
            palette::refine_ehb_base_palette(std::span<Color3f>(seed_pal.colors.data(), 32),
                                             src.pixels(),
                                             /*snap_to_ocs=*/true,
                                             /*max_iters=*/8,
                                             std::span<const bool>(refine_locked));
            if (lock_color0) seed_pal.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
            if (enable_best) {
                palette::extra_ehb_optimization(
                    std::span<Color3f>(seed_pal.colors.data(), 32),
                    src.pixels(),
                    /*snap_to_ocs=*/true,
                    /*max_passes=*/2,
                    [](std::size_t n, std::function<void(std::size_t)> f) {
                        pipeline::parallel_for(n, std::move(f));
                    },
                    on_progress);
                if (lock_color0) seed_pal.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
                // extra_ehb_optimization isn't lock-aware — re-stamp
                // reserves after it runs. (--best path only.)
                for (auto& [idx, color] : reserved_slots) {
                    if (idx < seed_pal.colors.size()) seed_pal.colors[idx] = color;
                }
            }
            ehb_user_pal = std::move(seed_pal.colors);
        }
    }
    // Forward reserved slots: encode_copper treats them as locked
    // (palette enforced each line, never swapped) AND adds them to
    // dither_excluded so the per-row picker can't choose them.
    std::vector<std::size_t> ehb_dither_excluded;
    ehb_dither_excluded.reserve(reserved_slots.size());
    for (auto& [idx, _] : reserved_slots)
        ehb_dither_excluded.push_back(idx);
    auto copper_result = copper::encode_copper(
        src,
        /*depth=*/5,
        dither_settings,
        amiga::Chipset::ocs,
        sliced_share_ehb,
        ehb_user_pal.empty() ? nullptr : &ehb_user_pal,
        lock_color0,
        reserved_slots,
        palette_diversity,
        /*skip_initial_swap_rows=*/0,
        /*is_lace=*/false,
        /*is_ehb=*/true,
        /*on_progress=*/{},
        sliced_spread_radius >= 0 ? static_cast<std::size_t>(sliced_spread_radius)
                                  : std::numeric_limits<std::size_t>::max(),
        sliced_spread_decay >= 0.0f ? sliced_spread_decay : -1.0f,
        sliced_vertical_dither,
        ehb_dither_excluded,
        /*quantizer_override=*/std::nullopt,
        sliced_beam);
    if (!copper_result) return std::unexpected{copper_result.error()};
    // Copies (not refs) so the joint-refinement pass below can reassign
    // them when it re-runs sliced with a refined base palette.
    auto sliced_palettes = copper_result->scanline_palettes;
    auto base_palette = copper_result->base_palette;

    // Build per-line 64-effective palette (32 base + 32 half-brite).
    auto build_effective_64 = [](const std::vector<Color3f>& base) {
        std::vector<Color3f> eff(kEffective);
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            eff[k] = base[k];
            eff[kBaseColors + k] = half_brite(base[k]);
        }
        return eff;
    };

    // ---- 2. base_index storage. The strips swap planner needs a
    // per-pixel-to-effective-slot binding for cluster centroid math.
    // We rebuild this per-row inside the planner against the actual
    // per-line sliced-evolved palette (lines ~1325+). The previous code
    // ran a global dither against the FRAME-INIT (line-0) palette
    // here, which produced stale bindings — fixed in v1.26.2 / v1.26.3
    // by removing that pass and rebuilding per-line.
    std::vector<std::uint8_t> base_index(width * height, 0);

    std::vector<color_space::OKLab> img_lab(width * height);
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            img_lab[y * width + x] = color_space::linear_to_oklab(src[x, y]);

    // ---- 3. Per-line greedy planner -----------------------------------
    std::vector<std::uint8_t> indices(width * height, 0);
    std::vector<std::vector<ScapMove>> line_moves(height);
    Image preview(width, height);

    std::vector<Color3f> P;
    std::vector<Color3f> P_eff;
    std::vector<color_space::OKLab> P_eff_lab(kEffective);
    auto recompute_lab = [&]() {
        for (std::size_t k = 0; k < kEffective; ++k)
            P_eff_lab[k] = color_space::linear_to_oklab(P_eff[k]);
    };

    // The dither picker should be free to pick any of the 64 effective
    // entries — `lock_color0` only constrains palette GENERATION
    // (forces base[0] = black for the Amiga border). Excluding it from
    // the picker forces dark pixels to a non-black slot, producing
    // visible colored noise in shadows. copper.cpp uses k_min=0 here
    // for the same reason (cap path, see copper.cpp:1003).
    std::size_t k_min = 0;

    // ED scaffolding (kernel, error buf, structure bias, Riemersma queue,
    // ordered offsets) lives inside
    // dither::diffuse_raw_buffer (the post-pass-1 driver call below).

    constexpr int kVStart = 44;
    double total_error = 0.0;
    std::size_t total_moves = 0;

    std::size_t num_strips = table.slots.size() + 1;
    std::vector<std::vector<Color3f>> strip_eff(num_strips, std::vector<Color3f>(kEffective));
    std::vector<std::vector<color_space::OKLab>> strip_eff_lab(
        num_strips, std::vector<color_space::OKLab>(kEffective));

    // Per-row snapshot for the post-loop driver call (pass 2). Each row
    // overwrites strip_eff[s]/strip_eff_lab[s] during pass-1 planning;
    // we copy the snapshot into the [y] slot before moving on.
    std::vector<std::vector<std::vector<Color3f>>> strip_eff_per_row(
        height, std::vector<std::vector<Color3f>>(num_strips, std::vector<Color3f>(kEffective)));
    std::vector<std::vector<std::vector<color_space::OKLab>>> strip_eff_lab_per_row(
        height,
        std::vector<std::vector<color_space::OKLab>>(num_strips,
                                                     std::vector<color_space::OKLab>(kEffective)));

    auto strip_for_x = [&](std::size_t x) -> std::size_t {
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            if (x < static_cast<std::size_t>(table.slots[s].pixel_x)) return s;
        }
        return table.slots.size();
    };
    std::vector<std::uint16_t> x_strip(width);
    for (std::size_t x = 0; x < width; ++x)
        x_strip[x] = static_cast<std::uint16_t>(strip_for_x(x));

    // Iterative index refinement (#2). Each pass after the first feeds
    // the previous pass's stage-2 indices back as base_index for the
    // swap planner, so the planner optimises against the binding the
    // encoder will actually produce. Empirically gains ~0.05 dB per
    // additional pass through pass 6, then plateaus.
    //
    // We tried full joint base-palette + sliced refinement (#3) — recompute
    // base from final indices, re-run sliced, re-dither stage 1 — but that
    // REGRESSED PSNR by ~0.9 dB on the 10-image sweep. Re-running sliced
    // from a different starting palette breaks the convergence the
    // index iteration was building toward. Pure index refinement wins.
    //
    // Actual hardware register state across lines. strips swaps leave
    // registers holding swap-colors at end-of-line; the per-line
    // sliced MOVEs need to diff against THIS, not against sliced_palettes
    // from the previous line.
    std::vector<Color3f> hw_state(kBaseColors);
    constexpr int kPasses = 6;
    auto report_pass = [&](int pass_idx, float local) {
        if (on_progress) {
            float p = (static_cast<float>(pass_idx) + std::clamp(local, 0.0f, 1.0f)) /
                      static_cast<float>(kPasses);
            on_progress(p, "encoding");
        }
    };
    if (on_progress) on_progress(0.0f, "encoding");
    for (int pass = 0; pass < kPasses; ++pass) {
        // Reset hw_state to the viewer's frame-init at each pass start.
        for (std::size_t k = 0; k < kBaseColors; ++k)
            hw_state[k] = base_palette[k];
        if (pass > 0) {
            // base_index is rebuilt per-row inside the planner against
            // the per-line sliced-evolved palette (no carry-over between
            // passes needed; stale bindings were the root cause of
            // dark-content regressions in v1.26.0/.1).
            for (auto& v : line_moves)
                v.clear();
            // err_buf is owned by dither::diffuse_raw_buffer (allocated
            // fresh each pass-2 call), so no manual reset is needed.
            // total_moves accumulates inside the per-line emit at line
            // ~2325; without an explicit reset it would carry pass-0..N-1
            // into pass-N and inflate avg_changes_per_line by ~6× (6
            // passes default). Reset alongside line_moves so the final
            // counter reflects only the last pass's MOVEs.
            total_moves = 0;
            // total_error is unconditionally reassigned later in this
            // pass (line ~2603 / 1330) before any read, so no reset
            // needed here.
        }
        for (std::size_t y = 0; y < height; ++y) {
            int abs_vpos = static_cast<int>(y) + kVStart;
            auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

            // Line entry palette = the sliced plan for this line, not a static
            // base. sliced_palettes[y] carries the evolved 32-base state from
            // previous lines (sliced's per-scanline diffs already applied).
            P = sliced_palettes[y];
            P_eff = build_effective_64(P);
            recompute_lab();
            strip_eff[0] = P_eff;
            strip_eff_lab[0] = P_eff_lab;

            // Re-bind base_index for this row against the per-line effective
            // 64-palette. Stage 2's pre-pass dithered against the FRAME-INIT
            // (line-0) palette, but the per-line sliced-evolved palette is
            // typically very different — frame-init bindings make the strips
            // cluster planner score against stale clusters and pick swaps
            // that hurt the actual rendered output (the gap was -10
            // SSIMULACRA2 on saturated content vs EHB+sliced). 320 × 64
            // dist evals per row × ~200 rows = ~4 M per encode; SoA SIMD
            // argmin over the 64-entry effective palette amortises the
            // per-row SoA build across all width pixels.
            oklab_simd::PaletteSoA row_soa{};
            {
                std::vector<bool> excl(kEffective, false);
                for (std::size_t k = 0; k < kEffective; ++k)
                    excl[k] = reserved_mask_ehb[k & (kBaseColors - 1)];
                oklab_simd::fill(P_eff_lab, excl, row_soa);
            }
            for (std::size_t x = 0; x < width; ++x) {
                auto& tgt = img_lab[y * width + x];
                base_index[y * width + x] = static_cast<std::uint8_t>(
                    oklab_simd::argmin(tgt, row_soa).index);
            }

            // 1. Per-line sliced MOVEs: diff vs the ACTUAL hardware register
            // state at end of the previous line. strips's mid-line swaps on
            // line y-1 may have left registers holding swap-colors rather
            // than sliced_palettes[y-1], so a diff vs sliced_palettes misses
            // them and the registers carry stale state into line y.
            {
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    if (hw_state[k].r != P[k].r || hw_state[k].g != P[k].g ||
                        hw_state[k].b != P[k].b) {
                        hw_state[k] = P[k];
                        line_moves[y].push_back(make_move(static_cast<std::uint8_t>(kRegBase + k),
                                                          palette::linear_to_ocs(P[k]),
                                                          -1));
                    }
                }
            }
            // 2. Line-gate WAIT.
            line_moves[y].push_back(
                make_wait(static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));

            // 3. strips MOVEs. Joint beam-search planner (matches DPF) with
            //    EHB-specific extras: each base[k] swap implicitly redefines
            //    half-brite[k] = halve(base[k]), so strip pixels can bind
            //    to either index k or index 32+k and both contribute to a
            //    swap's strip error. Hblank ceiling: per-line sliced on line
            //    y+1 emits a MOVE for every register where state.P[k] !=
            //    sliced_palettes[y+1][k] — beam expansion forbids candidates
            //    whose application would push that count past kHblankCeiling.
            //    useful_swap_cap = strips_share_ehb_max bounds total swaps
            //    per chain (sliced+strips combined budget).
            bool has_next_line = (y + 1 < height &&
                                  y + 1 < copper_result->scanline_palettes.size());

            // Per-strip cluster stats: for each register k, separate base
            // and half-brite clusters. Pixel binding[idx] in [0..63] →
            // k = idx & 31, is_half = idx >= 32.
            struct EClust {
                float L = 0, a = 0, b = 0;
                double spread = 0;
                std::uint16_t count = 0;
            };
            struct EStripStats {
                std::array<EClust, kBaseColors> cb{};         // base-bound cluster
                std::array<EClust, kBaseColors> ch{};         // half-bound cluster
                std::vector<Color3f> cands;                   // OCS-snapped
                std::vector<color_space::OKLab> cands_lab_b;  // OKLab(c)
                std::vector<color_space::OKLab> cands_lab_h;  // OKLab(halve(c))
            };

            std::vector<EStripStats> strips(num_strips);
            // Per-strip pixel OKLab arrays for the per-strip dither error
            // scorer. The cluster planner needs to evaluate candidate
            // swaps against each pixel's actual nearest-of-64 picker
            // outcome (the picker the encoder will use), not just against
            // frozen cluster centroids.
            std::vector<std::vector<color_space::OKLab>> strip_pixels_lab(num_strips);
            std::vector<StripPixelsSoA> strip_pixels_soa(num_strips);
            auto strip_x_range = [&](std::size_t s) {
                std::size_t lo = (s == 0) ? std::size_t{0}
                                          : std::min(width,
                                                     static_cast<std::size_t>(
                                                         table.slots[s - 1].pixel_x));
                std::size_t hi = (s < table.slots.size())
                                     ? std::min(width,
                                                static_cast<std::size_t>(table.slots[s].pixel_x))
                                     : width;
                return std::pair<std::size_t, std::size_t>{lo, hi};
            };
            for (std::size_t s = 0; s < num_strips; ++s) {
                auto [x_lo, x_hi] = strip_x_range(s);
                if (x_lo >= x_hi) continue;
                // Snapshot per-strip pixel OKLab (cheap; ~16 entries per
                // strip × 18 strips per row). Build SoA in parallel for the
                // SIMD distance helpers.
                strip_pixels_lab[s].reserve(x_hi - x_lo);
                for (std::size_t x = x_lo; x < x_hi; ++x)
                    strip_pixels_lab[s].push_back(img_lab[y * width + x]);
                build_strip_soa(strip_pixels_soa[s], strip_pixels_lab[s]);
                std::array<double, kBaseColors> sumLb{}, sumab{}, sumbb{};
                std::array<double, kBaseColors> sumLh{}, sumah{}, sumbh{};
                std::array<std::uint32_t, kBaseColors> cntb{}, cnth{};
                for (std::size_t x = x_lo; x < x_hi; ++x) {
                    auto idx = static_cast<std::size_t>(base_index[y * width + x]);
                    std::size_t k = idx & (kBaseColors - 1);
                    bool is_half = idx >= kBaseColors;
                    auto& lab = img_lab[y * width + x];
                    if (is_half) {
                        sumLh[k] += static_cast<double>(lab.L);
                        sumah[k] += static_cast<double>(lab.a);
                        sumbh[k] += static_cast<double>(lab.b);
                        ++cnth[k];
                    } else {
                        sumLb[k] += static_cast<double>(lab.L);
                        sumab[k] += static_cast<double>(lab.a);
                        sumbb[k] += static_cast<double>(lab.b);
                        ++cntb[k];
                    }
                }
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    if (cntb[k] > 0) {
                        strips[s].cb[k].count = static_cast<std::uint16_t>(cntb[k]);
                        strips[s].cb[k].L = static_cast<float>(sumLb[k] / cntb[k]);
                        strips[s].cb[k].a = static_cast<float>(sumab[k] / cntb[k]);
                        strips[s].cb[k].b = static_cast<float>(sumbb[k] / cntb[k]);
                    }
                    if (cnth[k] > 0) {
                        strips[s].ch[k].count = static_cast<std::uint16_t>(cnth[k]);
                        strips[s].ch[k].L = static_cast<float>(sumLh[k] / cnth[k]);
                        strips[s].ch[k].a = static_cast<float>(sumah[k] / cnth[k]);
                        strips[s].ch[k].b = static_cast<float>(sumbh[k] / cnth[k]);
                    }
                }
                for (std::size_t x = x_lo; x < x_hi; ++x) {
                    auto idx = static_cast<std::size_t>(base_index[y * width + x]);
                    std::size_t k = idx & (kBaseColors - 1);
                    bool is_half = idx >= kBaseColors;
                    auto& lab = img_lab[y * width + x];
                    auto& cl = is_half ? strips[s].ch[k] : strips[s].cb[k];
                    float dL = lab.L - cl.L;
                    float da = lab.a - cl.a;
                    float db = lab.b - cl.b;
                    cl.spread += static_cast<double>(color_space::fma_dist_sq(dL, da, db));
                }
                std::array<bool, 4096> seen{};
                auto ocs_key = [](const Color3f& c) -> std::size_t {
                    int r = static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 15.0f));
                    int g = static_cast<int>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 15.0f));
                    int b = static_cast<int>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 15.0f));
                    return static_cast<std::size_t>((r << 8) | (g << 4) | b);
                };
                auto add_cand = [&](Color3f c) {
                    auto cs = palette::quantize_to_ocs(c);
                    auto key = ocs_key(cs);
                    if (!seen[key]) {
                        seen[key] = true;
                        strips[s].cands.push_back(cs);
                        strips[s].cands_lab_b.push_back(color_space::linear_to_oklab(cs));
                        strips[s].cands_lab_h.push_back(
                            color_space::linear_to_oklab(half_brite(cs)));
                    }
                };
                for (std::size_t x = x_lo; x < x_hi; ++x)
                    add_cand(src[x, y]);
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    if (cntb[k] > 0) {
                        color_space::OKLab cd{
                            strips[s].cb[k].L, strips[s].cb[k].a, strips[s].cb[k].b};
                        add_cand(color_space::oklab_to_linear(cd).clamped());
                    }
                    if (cnth[k] > 0) {
                        // Half-brite-bound pixels want base ≈ 2×pixel.
                        color_space::OKLab dbl{std::min(2.0f * strips[s].ch[k].L, 1.0f),
                                               2.0f * strips[s].ch[k].a,
                                               2.0f * strips[s].ch[k].b};
                        add_cand(color_space::oklab_to_linear(dbl).clamped());
                    }
                }
            }

            // Per-strip dither-error scorer. For each pixel in the strip,
            // computes the OKLab² distance to its nearest-of-64 entry in
            // the effective palette (32 base + 32 halfbrites). SIMD'd via
            // SoA pixel buffers + AVX2 packed singles — see helpers at top
            // of file. Inverts the loop nest (outer k, inner pixels) so the
            // inner can vectorize across 8 pixels per AVX2 step.
            thread_local std::vector<float> tl_pixel_min;
            auto e_strip_dither = [&](std::size_t s,
                                      const std::array<color_space::OKLab, kBaseColors>& Plb,
                                      const std::array<color_space::OKLab, kBaseColors>& Plh) {
                auto& soa = strip_pixels_soa[s];
                if (soa.valid_n == 0) return 0.0;
                reset_pixel_min(tl_pixel_min, soa);
                for (std::size_t k = k_min; k < kBaseColors; ++k) {
                    if (reserved_mask_ehb[k]) continue;
                    min_dist_update(soa, Plb[k].L, Plb[k].a, Plb[k].b, tl_pixel_min.data());
                    min_dist_update(soa, Plh[k].L, Plh[k].a, Plh[k].b, tl_pixel_min.data());
                }
                return sum_pixel_min(tl_pixel_min.data(), soa.padded_n);
            };

            // Beam state. P holds 32 base linear-RGB; P_lab_b and P_lab_h
            // are the cached OKLab of base and halve(base) respectively.
            // B=2 is the sweet spot for EHB strips per the same sweep: PSNR
            // peaks at 40.49 dB. Wider beams keep lowering planner error
            // but worsen preview-PSNR because dither residuals scatter
            // into noise the planner doesn't see — the OKLab² metric
            // drifts hard from blurred-sRGB PSNR once the EHB plan is
            // already this tight (default error ~51 vs DPF ~140).
            //   B=1: err=52.13 psnr=40.31 dB  (= greedy)
            //   B=2: err=51.16 psnr=40.49 dB  ← peak
            //   B=3: err=50.74 psnr=40.37 dB
            //   B=4: err=50.57 psnr=40.45 dB
            //   B=16: err=50.03 psnr=40.19 dB (over-fits)
            constexpr std::size_t kBeamWidth = 2;
            constexpr std::size_t kCandsPerSlot = 16;
            constexpr std::size_t kEMaxSlots = 32;
            constexpr std::size_t kHblankCeilingLocal = kHblankCeiling;
            struct ENode {
                std::array<Color3f, kBaseColors> P;
                std::array<color_space::OKLab, kBaseColors> P_lab_b;
                std::array<color_space::OKLab, kBaseColors> P_lab_h;
                std::array<int, kEMaxSlots> dec_reg{};
                std::array<Color3f, kEMaxSlots> dec_color{};
                std::uint16_t projected_hblank = 0;
                std::uint16_t useful_swaps = 0;
                double cum_err = 0;
            };

            constexpr std::size_t kMaxVisibleMoves = 18;
            std::size_t slots_to_run = std::min(table.slots.size(), kMaxVisibleMoves);
            std::size_t useful_swap_cap = strips_share_ehb_max;

            ENode init{};
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                init.P[k] = P[k];
                init.P_lab_b[k] = color_space::linear_to_oklab(P[k]);
                init.P_lab_h[k] = color_space::linear_to_oklab(half_brite(P[k]));
            }
            for (auto& d : init.dec_reg)
                d = -1;
            if (has_next_line) {
                std::uint16_t h0 = 0;
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    auto& a = init.P[k];
                    auto& b = sliced_palettes[y + 1][k];
                    if (a.r != b.r || a.g != b.g || a.b != b.b) ++h0;
                }
                init.projected_hblank = h0;
            }
            init.cum_err = e_strip_dither(0, init.P_lab_b, init.P_lab_h);

            std::vector<ENode> beam{init};
            std::vector<ENode> next;
            next.reserve(kBeamWidth * (kCandsPerSlot + 1));

            for (std::size_t s = 0; s < slots_to_run; ++s) {
                next.clear();
                auto& st = strips[s + 1];
                bool strip_empty = true;
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    if (st.cb[k].count > 0 || st.ch[k].count > 0) {
                        strip_empty = false;
                        break;
                    }
                }

                for (auto& state : beam) {
                    double filler_err = strip_empty
                                            ? 0.0
                                            : e_strip_dither(s + 1, state.P_lab_b, state.P_lab_h);
                    {
                        ENode child = state;
                        child.dec_reg[s] = -1;
                        child.cum_err += filler_err;
                        next.push_back(child);
                    }
                    if (strip_empty) continue;
                    if (state.useful_swaps >= useful_swap_cap) continue;

                    // Per-pixel min-of-62 (excluding base[k] and halfbrite[k]
                    // for each k) — precomputed once per state-strip-k so
                    // the candidate eval becomes O(width × candidates),
                    // not O(width × 64 × candidates). SoA via
                    // strip_pixels_soa[s+1].
                    struct Move {
                        int reg;
                        std::size_t cand_idx;
                        int hblank_delta;
                        double err;
                    };
                    std::vector<Move> moves;
                    moves.reserve(kBaseColors * st.cands.size());

                    for (std::size_t k = k_min; k < kBaseColors; ++k) {
                        if (reserved_mask_ehb[k]) continue;
                        // Hblank-budget gate: precompute delta for register
                        // k swap-vs-current.
                        bool old_diff = false;
                        if (has_next_line) {
                            auto& a = state.P[k];
                            auto& b = sliced_palettes[y + 1][k];
                            old_diff = (a.r != b.r || a.g != b.g || a.b != b.b);
                        }
                        // Precompute pixel_min_excl_k[x]: min over all 64
                        // slots EXCEPT base[k] and halfbrite[k+32]. SIMD'd
                        // via SoA + AVX2 helpers (8 pixels/iter).
                        auto& soa_pixels = strip_pixels_soa[s + 1];
                        reset_pixel_min(tl_pixel_min, soa_pixels);
                        for (std::size_t k2 = k_min; k2 < kBaseColors; ++k2) {
                            if (k2 == k) continue;
                            if (reserved_mask_ehb[k2]) continue;
                            min_dist_update(soa_pixels,
                                            state.P_lab_b[k2].L,
                                            state.P_lab_b[k2].a,
                                            state.P_lab_b[k2].b,
                                            tl_pixel_min.data());
                            min_dist_update(soa_pixels,
                                            state.P_lab_h[k2].L,
                                            state.P_lab_h[k2].a,
                                            state.P_lab_h[k2].b,
                                            tl_pixel_min.data());
                        }
                        for (std::size_t ci = 0; ci < st.cands.size(); ++ci) {
                            auto& c_lab_b = st.cands_lab_b[ci];
                            auto& c_lab_h = st.cands_lab_h[ci];
                            double e = dist_min2_sum(soa_pixels,
                                                     tl_pixel_min.data(),
                                                     c_lab_b.L,
                                                     c_lab_b.a,
                                                     c_lab_b.b,
                                                     c_lab_h.L,
                                                     c_lab_h.a,
                                                     c_lab_h.b);
                            if (e >= filler_err) continue;
                            int delta = 0;
                            if (has_next_line) {
                                auto& cs = st.cands[ci];
                                auto& nxt = sliced_palettes[y + 1][k];
                                bool new_diff = (cs.r != nxt.r || cs.g != nxt.g || cs.b != nxt.b);
                                delta = (new_diff ? 1 : 0) - (old_diff ? 1 : 0);
                                if (state.projected_hblank +
                                        static_cast<std::size_t>(std::max(0, delta)) >
                                    kHblankCeilingLocal) {
                                    continue;
                                }
                            }
                            moves.push_back({static_cast<int>(k), ci, delta, e});
                        }
                    }
                    std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
                        return a.err < b.err;
                    });
                    constexpr std::size_t kPerRegCap = 1;
                    std::array<std::size_t, kBaseColors> reg_taken{};
                    std::vector<Move> picked;
                    picked.reserve(kCandsPerSlot);
                    for (auto& m : moves) {
                        auto rk = static_cast<std::size_t>(m.reg);
                        if (reg_taken[rk] >= kPerRegCap) continue;
                        picked.push_back(m);
                        ++reg_taken[rk];
                        if (picked.size() >= kCandsPerSlot) break;
                    }

                    for (auto& m : picked) {
                        auto reg_idx = static_cast<std::size_t>(m.reg);
                        ENode child = state;
                        child.P[reg_idx] = st.cands[m.cand_idx];
                        child.P_lab_b[reg_idx] = st.cands_lab_b[m.cand_idx];
                        child.P_lab_h[reg_idx] = st.cands_lab_h[m.cand_idx];
                        child.dec_reg[s] = m.reg;
                        child.dec_color[s] = st.cands[m.cand_idx];
                        child.cum_err += m.err;
                        child.projected_hblank = static_cast<std::uint16_t>(
                            static_cast<int>(child.projected_hblank) + m.hblank_delta);
                        ++child.useful_swaps;
                        next.push_back(child);
                    }
                }

                std::size_t keep_b = std::min(kBeamWidth, next.size());
                if (next.size() > keep_b) {
                    std::partial_sort(
                        next.begin(),
                        next.begin() + static_cast<std::ptrdiff_t>(keep_b),
                        next.end(),
                        [](const ENode& a, const ENode& b) { return a.cum_err < b.cum_err; });
                    next.resize(keep_b);
                }
                beam.swap(next);
            }

            auto& best = *std::min_element(
                beam.begin(), beam.end(), [](const ENode& a, const ENode& b) {
                    return a.cum_err < b.cum_err;
                });

            // Apply chain: emit per-slot MOVEs, update P/P_eff/P_eff_lab/
            // hw_state, snapshot strip palettes for the render pass.
            for (std::size_t s = 0; s < slots_to_run; ++s) {
                int reg = best.dec_reg[s];
                if (reg < 0) {
                    line_moves[y].push_back(make_move(
                        /*reg=*/0, palette::linear_to_ocs(hw_state[0]), static_cast<int>(s)));
                } else {
                    auto k = static_cast<std::size_t>(reg);
                    Color3f col = best.dec_color[s];
                    P[k] = col;
                    P_eff[k] = col;
                    P_eff[kBaseColors + k] = half_brite(col);
                    P_eff_lab[k] = color_space::linear_to_oklab(col);
                    P_eff_lab[kBaseColors + k] = color_space::linear_to_oklab(
                        P_eff[kBaseColors + k]);
                    hw_state[k] = col;
                    line_moves[y].push_back(make_move(static_cast<std::uint8_t>(kRegBase + reg),
                                                      palette::linear_to_ocs(col),
                                                      static_cast<int>(s)));
                    ++total_moves;
                }
                strip_eff[s + 1] = P_eff;
                strip_eff_lab[s + 1] = P_eff_lab;
            }
            // Skipped slots beyond slots_to_run keep the post-last-slot
            // palette state.
            for (std::size_t s = slots_to_run; s < num_strips - 1; ++s) {
                strip_eff[s + 1] = P_eff;
                strip_eff_lab[s + 1] = P_eff_lab;
            }

            // 4. End-of-line WAIT.
            line_moves[y].push_back(
                make_wait(static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));

            // Quality gate: estimate per-row error with vs. without the
            // strips swaps (every strip = entry palette). The planner's
            // cluster-centroid objective can recommend swaps that score
            // well on k-means but lose on the actual nearest-of-64 picker
            // — visible as 16-pixel-wide colored bars on dark/HDR content.
            // The underlying issue is that the planner doesn't model
            // pixel re-binding when a slot color changes; this gate is a
            // correctness backstop until the planner is reworked. Same
            // shape as the HAM6+strips gate (commit 6c516d9).
            bool any_scap_swap = false;
            for (auto& m : line_moves[y]) {
                if (m.kind == ScapOpKind::kMove && m.slot_index >= 0) {
                    any_scap_swap = true;
                    break;
                }
            }
            if (any_scap_swap) {
                auto pixel_min_err = [&](const std::vector<color_space::OKLab>& pal_lab,
                                         std::size_t px,
                                         std::size_t py) -> float {
                    auto& src_lab = img_lab[py * width + px];
                    float best_d = std::numeric_limits<float>::max();
                    for (std::size_t k = k_min; k < kEffective; ++k) {
                        std::size_t base_k = k & (kBaseColors - 1);
                        if (reserved_mask_ehb[base_k]) continue;
                        float dL = src_lab.L - pal_lab[k].L;
                        float da = src_lab.a - pal_lab[k].a;
                        float db = src_lab.b - pal_lab[k].b;
                        float d = color_space::fma_dist_sq(dL, da, db);
                        if (d < best_d) best_d = d;
                    }
                    return best_d;
                };
                double err_with = 0.0, err_without = 0.0;
                for (std::size_t x = 0; x < width; ++x) {
                    auto s = static_cast<std::size_t>(x_strip[x]);
                    err_with += static_cast<double>(pixel_min_err(strip_eff_lab[s], x, y));
                    err_without += static_cast<double>(pixel_min_err(strip_eff_lab[0], x, y));
                }
                if (err_without + 1e-9 < err_with) {
                    for (std::size_t s = 1; s < num_strips; ++s) {
                        strip_eff[s] = strip_eff[0];
                        strip_eff_lab[s] = strip_eff_lab[0];
                    }
                    auto& lm = line_moves[y];
                    std::size_t removed = 0;
                    lm.erase(std::remove_if(lm.begin(),
                                            lm.end(),
                                            [&](const ScapMove& m) {
                                                if (m.kind == ScapOpKind::kMove &&
                                                    m.slot_index >= 0) {
                                                    ++removed;
                                                    return true;
                                                }
                                                return false;
                                            }),
                             lm.end());
                    if (removed > total_moves)
                        total_moves = 0;
                    else
                        total_moves -= removed;
                }
            }

            // Snapshot pass-1 strip state for this row; pass-2 dither runs
            // over the whole image once, after the per-row loop.
            strip_eff_per_row[y] = strip_eff;
            strip_eff_lab_per_row[y] = strip_eff_lab;

            if (height > 0 && (y & 0xF) == 0xF) {
                report_pass(pass, static_cast<float>(y + 1) / static_cast<float>(height));
            }
        }

        // Reserved slots: build a candidate-mask over the 64 effective slots
        // that excludes reserved bases AND their half-brite copies, plus a
        // cand_to_full mapping for translating filtered indices back.
        std::array<bool, kEffective> eff_blocked{};
        for (auto& [idx, _] : reserved_slots) {
            if (idx < kBaseColors) {
                eff_blocked[idx] = true;
                eff_blocked[kBaseColors + idx] = true;
            }
        }
        bool has_excluded = std::any_of(
            eff_blocked.begin(), eff_blocked.end(), [](bool b) { return b; });

        // Stage-2 render across all 64 effective entries per pixel against
        // the per-row, per-strip palette. Driver owns ED scaffolding
        // (kernel, serpentine, structure bias, Riemersma,
        // ordered offsets); picker resolves x_strip[x] → row's strip
        // palette, then yliluoma family or nearest pair pick.
        {
            // When --reserve-range is active, filter each per-strip palette
            // to remove reserved/half-brite slots. The picker then operates
            // on a smaller candidate set; cand_to_full[y][s][k] maps back.
            std::vector<std::vector<std::vector<color_space::OKLab>>> eff_lab_filtered;
            std::vector<std::vector<std::vector<std::uint8_t>>> cand_to_full;
            if (has_excluded) {
                eff_lab_filtered.resize(height);
                cand_to_full.resize(height);
                for (std::size_t y = 0; y < height; ++y) {
                    eff_lab_filtered[y].resize(strip_eff_lab_per_row[y].size());
                    cand_to_full[y].resize(strip_eff_lab_per_row[y].size());
                    for (std::size_t s = 0; s < strip_eff_lab_per_row[y].size(); ++s) {
                        auto& src_lab = strip_eff_lab_per_row[y][s];
                        auto& dst_lab = eff_lab_filtered[y][s];
                        auto& dst_map = cand_to_full[y][s];
                        dst_lab.reserve(kEffective);
                        dst_map.reserve(kEffective);
                        for (std::size_t i = 0; i < kEffective; ++i) {
                            if (eff_blocked[i]) continue;
                            dst_lab.push_back(src_lab[i]);
                            dst_map.push_back(static_cast<std::uint8_t>(i));
                        }
                    }
                }
            }

            float te = dither::diffuse_raw_buffer(
                src,
                dither_settings,
                [&](const color_space::OKLab& target,
                    std::size_t x,
                    std::size_t y) -> dither::PickResult {
                    auto s = static_cast<std::size_t>(x_strip[x]);
                    auto& eff_pal = strip_eff_per_row[y][s];
                    std::size_t k = 0;
                    color_space::OKLab chosen{};
                    float thr;
                    std::uint8_t full_idx;
                    if (has_excluded) {
                        auto& eff_lab = eff_lab_filtered[y][s];
                        std::span<const color_space::OKLab> eff_span(eff_lab.data(),
                                                                     eff_lab.size());
                        thr = dither::pick_palette_index_with_ostro(dither_settings.method,
                                                                    target,
                                                                    eff_span,
                                                                    x,
                                                                    y,
                                                                    dither_settings.strength,
                                                                    k_min,
                                                                    k,
                                                                    chosen);
                        full_idx = cand_to_full[y][s][k];
                    } else {
                        auto& eff_lab = strip_eff_lab_per_row[y][s];
                        std::span<const color_space::OKLab> eff_span(eff_lab.data(), kEffective);
                        thr = dither::pick_palette_index_with_ostro(dither_settings.method,
                                                                    target,
                                                                    eff_span,
                                                                    x,
                                                                    y,
                                                                    dither_settings.strength,
                                                                    k_min,
                                                                    k,
                                                                    chosen);
                        full_idx = static_cast<std::uint8_t>(k);
                    }
                    indices[y * width + x] = full_idx;
                    preview[x, y] = eff_pal[full_idx];
                    return {chosen, thr};
                });
            total_error = static_cast<double>(te);
        }

        // DBS post-pass for strips+EHB. Same shape as the DPF strips path, but
        // the candidate set is the 64-entry effective palette (32 base +
        // 32 half-brites). DBS picks any of the 64 indices; the half-brite
        // bit is just bit 5 of the resulting index.
        if (dither_settings.method == dither::Method::dbs) {
            dither::apply_dbs_post_pass(
                src,
                indices,
                [&](std::size_t x, std::size_t y) -> std::span<const color_space::OKLab> {
                    auto s = static_cast<std::size_t>(x_strip[x]);
                    auto& eff_lab = strip_eff_lab_per_row[y][s];
                    return {eff_lab.data(), kEffective};
                });
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < width; ++x) {
                    auto s = static_cast<std::size_t>(x_strip[x]);
                    preview[x, y] = strip_eff_per_row[y][s][indices[y * width + x]];
                }
            }
        }
        report_pass(pass + 1, 0.0f);
    }  // kPasses
    if (on_progress) on_progress(1.0f, "done");

    // ---- 4. 6-plane bitplane encoding. The 6-bit index already encodes
    // half-brite as bit 5, which is exactly what the EHB hardware reads.
    auto enc = bitplane::encode(indices,
                                width,
                                height,
                                /*depth=*/6,
                                bitplane::Layout::interleaved);
    if (!enc) return std::unexpected{enc.error()};

    ScapResult res;
    res.planes = *std::move(enc);
    res.palette = std::move(base_palette);  // 32 base entries; HW derives 32 half-brites
    res.slot_table = table;
    res.total_error = static_cast<float>(total_error);
    res.avg_changes_per_line = height > 0
                                   ? static_cast<float>(total_moves) / static_cast<float>(height)
                                   : 0.0f;
    {
        std::size_t total_all = 0, row_max = 0;
        std::size_t total_hb = 0, hb_max = 0;
        std::size_t total_vis = 0, vis_max = 0;
        for (auto& row : line_moves) {
            std::size_t rm = 0, hb = 0, vis = 0;
            bool past_line_gate = false;
            for (auto& op : row) {
                if (op.kind == ScapOpKind::kWait) {
                    past_line_gate = true;
                    continue;
                }
                ++rm;
                if (past_line_gate)
                    ++vis;
                else
                    ++hb;
            }
            total_all += rm;
            if (rm > row_max) row_max = rm;
            total_hb += hb;
            if (hb > hb_max) hb_max = hb;
            total_vis += vis;
            if (vis > vis_max) vis_max = vis;
        }
        auto h = static_cast<float>(height ? height : 1);
        res.avg_total_moves_per_line = static_cast<float>(total_all) / h;
        res.max_moves_per_line = row_max;
        res.avg_hblank_moves_per_line = static_cast<float>(total_hb) / h;
        res.max_hblank_moves_per_line = hb_max;
        res.avg_visible_moves_per_line = static_cast<float>(total_vis) / h;
        res.max_visible_moves_per_line = vis_max;
    }
    res.line_moves = std::move(line_moves);
    // Snap rendered pixels to OCS 12-bit. The previous "no-snap"
    // version was based on a misread of the hardware: halve(0x11)
    // is NOT 0x09 (sRGB / 2), it's 0x00 (nibble 1 >> 1 = nibble 0,
    // 8-bit 0x00). The Amiga DAC takes 4-bit nibbles per channel
    // and halve is `nibble >> 1`, not `value * 0.5`. Producing
    // half-brite values like 0x09 (which 38% of EHB+strips+best
    // output pixels were sitting at) gives an inflated SSIMULACRA2
    // reading against pixels real hardware cannot display. The
    // collapse from "15 distinct darks" to ~8 wasn't the snap
    // throwing quality away — it was the snap reflecting what
    // hardware actually shows. Restoring the snap aligns the
    // preview/score with what the chip emits.
    for (auto& p : preview.pixels())
        p = palette::quantize_to_ocs(p);
    res.rendered = std::move(preview);
    return res;
}

// ---------------------------------------------------------------------------
// HAM6 + strips — v0 implementation
//
// HAM6 has the same 6-plane DMA pattern as EHB and DPF, so the
// kStrips6bplEhb slot table (19 mid-line MOVE positions) transfers
// directly. We mid-line-swap the 16 BASE palette registers; HAM SET ops
// resolve against whichever strip palette is currently active, while
// MODIFY ops continue to mutate the rolling output color irrespective
// of palette state.
//
// v0 simplifications (deliberate, see commit msg):
//   * Greedy single-pass strip swap planner: per-strip pixel histogram,
//     swap the K least-used base slots with the strip's most-frequent
//     RGB444-bucketed colors.
//   * No multi-pass joint refinement (EHB strips runs 6 passes).
//   * No best wiring.
//   * Inline HAM op selector — keeps scap.cpp self-contained without
//     needing to expose ham.cpp's anonymous-namespace helpers.
// ---------------------------------------------------------------------------
namespace {}  // namespace

Result<ScapResult> encode_strips_ham6_ocs(const Image& image,
                                          int width_arg,
                                          int height_arg,
                                          bool lock_color0,
                                          const dither::Settings& dither_settings,
                                          std::size_t copper_changes_override,
                                          int palette_diversity,
                                          std::function<void(float, std::string_view)> on_progress,
                                          int sliced_spread_radius,
                                          float sliced_spread_decay,
                                          bool sliced_vertical_dither,
                                          bool enable_best,
                                          std::span<const Color3f> external_palette,
                                          ham::HamMetric ham_metric) {
    // --best: 8 jitter seeds × 5 strengths × 4 diversities + 1
    // baseline = 161 trials. Same shape as EHB strips since HAM6's 16
    // base palette has comparable basin depth.
    if (enable_best) {
        auto best = pipeline::best_sweep<ScapResult>(
            image,
            dither_settings,
            palette_diversity,
            /*jitter_count=*/8,
            [&](const Image& jittered_in, const dither::Settings& d, int div) {
                return encode_strips_ham6_ocs(jittered_in,
                                              width_arg,
                                              height_arg,
                                              lock_color0,
                                              d,
                                              copper_changes_override,
                                              div,
                                              /*on_progress=*/{},
                                              sliced_spread_radius,
                                              sliced_spread_decay,
                                              sliced_vertical_dither,
                                              /*enable_best=*/false,
                                              external_palette,
                                              ham_metric);
            },
            [](const ScapResult& r) -> const Image& { return r.rendered; },
            on_progress,
            /*jitter_amplitude=*/1.0f);
        if (best.has_value()) return std::move(*best);
    }
    auto& table = kStrips6bplHam6;
    if (table.slots.empty()) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "Strips HAM6 planner: kStrips6bplHam6 slot table is empty",
        }};
    }
    auto width = (width_arg > 0) ? static_cast<std::size_t>(width_arg) : image.width();
    auto height = (height_arg > 0) ? static_cast<std::size_t>(height_arg) : image.height();
    if (image.width() != width || image.height() != height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Strips HAM6 planner: image is {}x{} but caller asked "
                        "for {}x{} — resize before calling",
                        image.width(),
                        image.height(),
                        width,
                        height),
        }};
    }

    constexpr std::size_t kBaseColors = 16;
    constexpr std::size_t kHblankCeiling = 13;  // MOVEs sliced can land
                                                // in hblank.
    constexpr std::size_t kVisibleBudget = 18;  // MOVEs strips can land
                                                // in the visible
                                                // raster (kStrips6bplEhb
                                                // has 19 slots; cap
                                                // at 18 mirrors EHB
                                                // strips's design).
    // Hblank and visible-line are SEPARATE DMA windows — they don't
    // compete. sliced MOVEs (slot_index = -1) all land in hblank; strips
    // MOVEs (slot_index >= 0) land mid-line during the visible
    // raster. The combined per-row MOVE count is hblank_used +
    // visible_used, but the BUDGET is per-window. copper_changes_
    // override applies ONLY to the visible (strips) budget — caller
    // can throttle strips without crippling sliced.
    std::size_t sliced_share = kHblankCeiling;
    std::size_t strips_share = (copper_changes_override > 0)
                                   ? std::min<std::size_t>(copper_changes_override, kVisibleBudget)
                                   : kVisibleBudget;

    // ---- 0. Pre-dither for HAM encoding -------------------------------
    // ham::encode_ham_copper internally pre-dithers when given an ED
    // dither_method, then runs DP on that dithered image. strips needs
    // to drive its OWN per-strip beam search on the SAME dithered
    // input — otherwise the sliced-planned palettes (derived from the
    // dithered image inside ham::encode_ham_copper) are mismatched
    // against the strips-encoded pixels (running on the raw image).
    // Solution: pre-dither once here, hand the dithered image to
    // ham::encode_ham_copper with dither=none so it doesn't dither
    // again, and feed the same dithered image to our per-strip DP.
    Image strips_input(width, height);
    if (dither::uses_error_diffusion(dither_settings.method)) {
        dither::Settings d{
            .method = dither_settings.method,
            .strength = dither_settings.strength,
            .error_clamp = dither_settings.error_clamp,
            .serpentine = true,
        };
        constexpr std::size_t kHam6DataBits = 4;
        dither::diffuse_raw_buffer(
            image,
            d,
            [&](const color_space::OKLab& target,
                std::size_t x,
                std::size_t y) -> dither::PickResult {
                auto adjusted = color_space::oklab_to_linear(target);
                adjusted.r = std::clamp(adjusted.r, 0.0f, 1.0f);
                adjusted.g = std::clamp(adjusted.g, 0.0f, 1.0f);
                adjusted.b = std::clamp(adjusted.b, 0.0f, 1.0f);
                auto srgb_adj = ham::linear_to_srgb8(adjusted);
                // HAM6 MODIFY precision = 4 bits → nibble replication.
                srgb_adj.r = static_cast<std::uint8_t>((srgb_adj.r >> (8 - kHam6DataBits)) * 17u);
                srgb_adj.g = static_cast<std::uint8_t>((srgb_adj.g >> (8 - kHam6DataBits)) * 17u);
                srgb_adj.b = static_cast<std::uint8_t>((srgb_adj.b >> (8 - kHam6DataBits)) * 17u);
                auto quantized = color_space::srgb_u8_to_linear(srgb_adj.r, srgb_adj.g, srgb_adj.b);
                strips_input[x, y] = quantized;
                return {color_space::linear_to_oklab(quantized), 0.5f};
            });
    } else {
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
                strips_input[x, y] = image[x, y];
    }

    // ---- 1. Per-line sliced base palette (16 colors, evolving across rows).
    // Use ham::encode_ham_copper for the per-line sliced plan. We pre-
    // dithered above (when applicable), so disable dither here to
    // avoid double-application — the sliced planner sees the same image
    // as our per-strip DP.
    ham::HamOptions ham_opts;
    ham_opts.dither_method = dither::Method::none;  // pre-dithered above
    ham_opts.palette_diversity = palette_diversity;
    ham_opts.metric = ham_metric;
    if (!external_palette.empty()) {
        ham_opts.external_palette.assign(external_palette.begin(), external_palette.end());
    }
    auto ham_cap_result = ham::encode_ham_copper(strips_input,
                                                 amiga::Mode::ham6,
                                                 amiga::Chipset::ocs,
                                                 ham_opts,
                                                 /*is_hires=*/false,
                                                 sliced_share);
    if (!ham_cap_result) return std::unexpected{ham_cap_result.error()};
    auto& sliced_palettes = ham_cap_result->scanline_palettes;
    auto base_palette = ham_cap_result->base_palette;
    (void)sliced_spread_radius;
    (void)sliced_spread_decay;
    (void)sliced_vertical_dither;

    // Strip layout helpers (same shape as EHB strips).
    std::size_t num_strips = table.slots.size() + 1;
    auto strip_for_x = [&](std::size_t x) -> std::size_t {
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            if (x < static_cast<std::size_t>(table.slots[s].pixel_x)) return s;
        }
        return table.slots.size();
    };
    auto strip_x_range = [&](std::size_t s) -> std::pair<std::size_t, std::size_t> {
        std::size_t lo = (s == 0) ? std::size_t{0}
                                  : std::min(width,
                                             static_cast<std::size_t>(table.slots[s - 1].pixel_x));
        std::size_t hi = (s < table.slots.size())
                             ? std::min(width, static_cast<std::size_t>(table.slots[s].pixel_x))
                             : width;
        return {lo, hi};
    };

    // Pre-compute pixel OKLab.
    std::vector<color_space::OKLab> img_lab(width * height);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x)
            img_lab[y * width + x] = color_space::linear_to_oklab(image[x, y]);
    }

    // ---- 2. Per-line HAM6+strips encoding -------------------------------
    constexpr int kVStart = 44;
    std::vector<std::uint8_t> ham_values(width * height, 0);
    std::vector<std::vector<ScapMove>> line_moves(height);
    std::vector<std::vector<Color3f>> scanline_palettes_full(height);
    Image preview(width, height);
    double total_error = 0.0;
    std::size_t total_moves = 0;

    if (on_progress) on_progress(0.0f, "encoding");
    std::atomic<double> total_error_atomic{0.0};
    std::atomic<std::size_t> total_moves_atomic{0};
    std::atomic<std::size_t> rows_done{0};

    // Actual hardware register state at end of the previous line. strips
    // swaps mid-line on row y-1 leave registers holding swap-colors
    // rather than sliced_palettes[y-1], so the per-line sliced MOVEs MUST
    // diff vs THIS to correctly restore the intended line-entry palette.
    // Carry-over forces serial when copper_changes_override > 0 (the
    // user asked for a budget that depends on prior-line state).
    // For the default override == 0, the parallel path treats every
    // line as starting from the base palette (same init the serial
    // path uses for row 0); the encoder is robust to that — it just
    // emits whatever HBLANK MOVEs are needed to bring the registers
    // to sliced_palettes[y]. Mirrors EHB+strips's serial_path
    // conditional. AMDuProf showed this loop running 1-thread for
    // ~273 s out of 282 s CPU; parallelising it gives near-linear
    // speedup on multi-core hosts.
    auto make_initial_hw_state = [&]() {
        std::vector<Color3f> s(kBaseColors);
        for (std::size_t k = 0; k < kBaseColors; ++k)
            s[k] = (k < base_palette.size()) ? base_palette[k] : Color3f{0.0f, 0.0f, 0.0f};
        return s;
    };
    std::vector<Color3f> outer_hw_state = make_initial_hw_state();
    const bool serial_path = (copper_changes_override > 0);

    auto run_row = [&](std::size_t y, std::vector<Color3f>& hw_state) {
        if (on_progress) {
            auto done = rows_done.fetch_add(1) + 1;
            if ((done & 0xF) == 0) {
                on_progress(static_cast<float>(done) / static_cast<float>(height), "encoding");
            }
        }
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // Strip working palette: starts as sliced_palettes[y] (16 colors),
        // mutates as strips MOVEs land. The HAM op picker uses whichever
        // strip palette is active at the current pixel.
        std::vector<Color3f> strip_pal = sliced_palettes[y];
        if (strip_pal.size() < kBaseColors) strip_pal.resize(kBaseColors);
        std::vector<color_space::OKLab> strip_pal_lab(kBaseColors);
        auto refresh_lab = [&]() {
            for (std::size_t k = 0; k < kBaseColors; ++k)
                strip_pal_lab[k] = color_space::linear_to_oklab(strip_pal[k]);
        };
        refresh_lab();

        // Per-line sliced MOVEs: diff vs the ACTUAL hardware register state
        // at end of the previous line, capped at the HBLANK budget. Slots
        // we couldn't restore in HBLANK keep their stale (strips-polluted)
        // value — we override strip_pal[k] with hw_state[k] for those so
        // the encoder sees what the chip is actually displaying at line
        // entry.
        constexpr std::size_t kHblankBudget = 13;
        struct CapDiff {
            std::size_t k;
            float dist_sq;
        };
        std::vector<CapDiff> diffs;
        diffs.reserve(kBaseColors);
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            if (strip_pal[k].r != hw_state[k].r || strip_pal[k].g != hw_state[k].g ||
                strip_pal[k].b != hw_state[k].b) {
                auto a = color_space::linear_to_oklab(strip_pal[k]);
                auto b = color_space::linear_to_oklab(hw_state[k]);
                float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                diffs.push_back({k, color_space::fma_dist_sq(dL, da, db)});
            }
        }
        std::sort(diffs.begin(), diffs.end(), [](const CapDiff& a, const CapDiff& b) {
            return a.dist_sq > b.dist_sq;
        });
        std::vector<bool> sliced_emitted(kBaseColors, false);
        std::size_t sliced_emit_count = std::min(diffs.size(), kHblankBudget);
        for (std::size_t i = 0; i < sliced_emit_count; ++i) {
            auto k = diffs[i].k;
            line_moves[y].push_back(
                make_move(static_cast<std::uint8_t>(k), palette::linear_to_ocs(strip_pal[k]), -1));
            hw_state[k] = strip_pal[k];
            sliced_emitted[k] = true;
        }
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            if (!sliced_emitted[k] &&
                (strip_pal[k].r != hw_state[k].r || strip_pal[k].g != hw_state[k].g ||
                 strip_pal[k].b != hw_state[k].b)) {
                strip_pal[k] = hw_state[k];
            }
        }
        refresh_lab();
        // Line gate WAIT.
        line_moves[y].push_back(make_wait(static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));

        // ---- 3. HAM-DP-aware swap planner -------------------------------
        // Per strip, generate candidates from cluster centroids + every
        // distinct pixel (OCS-bucketed). Pre-screen by centroid score to
        // a small top-K, then SCORE each survivor by running the actual
        // encoder (ham::encode_scanline_dp_per_strip) on the full row
        // with the candidate swap propagated forward. Accept the swap
        // only if it strictly reduces the row's HAM-DP error.
        //
        // This replaces the prior centroid-based scorer (which picked
        // swaps that minimized SET-only nearest-color error but
        // sometimes hurt the actual HAM encode because MODIFY ops can
        // already reach the color better than any palette swap would).
        //
        // The HAM rolling state crosses strip boundaries unchanged
        // (only the palette consulted for SET ops swaps), so encoding
        // the full row with the modified palette gives the correct
        // global error including chained-MODIFY effects.

        // Strip layout helpers used both here and at final-encode time.
        constexpr std::size_t kBeamWidth = 48;
        constexpr std::size_t kTripleBeam = 16;
        std::vector<std::uint16_t> strip_idx(width);
        for (std::size_t x = 0; x < width; ++x)
            strip_idx[x] = static_cast<std::uint16_t>(strip_for_x(x));
        std::vector<Color3f> row_pixels(width);
        for (std::size_t x = 0; x < width; ++x)
            row_pixels[x] = strips_input[x, y];
        auto row_span = std::span<const Color3f>(row_pixels.data(), width);
        auto idx_span = std::span<const std::uint16_t>(strip_idx.data(), width);

        // Build per-strip palette state by replaying the strips MOVEs
        // currently in line_moves[y]. strip 0 starts from the line-
        // entry palette (sliced_palettes[y]); each subsequent strip
        // applies the MOVEs whose slot_index matches s-1.
        auto build_strips = [&](std::vector<std::vector<Color3f>>& strip_pals,
                                std::vector<std::vector<ham::SRGBColor>>& strip_srgbs,
                                std::vector<ham::HamPrecomp>& strip_pres,
                                std::vector<std::span<const ham::SRGBColor>>& strip_srgb_spans) {
            strip_pals.assign(num_strips, {});
            strip_srgbs.assign(num_strips, {});
            strip_pres.clear();
            strip_pres.reserve(num_strips);
            strip_srgb_spans.clear();
            strip_srgb_spans.reserve(num_strips);
            std::vector<Color3f> running_pal = sliced_palettes[y];
            if (running_pal.size() < kBaseColors) running_pal.resize(kBaseColors);
            for (std::size_t s = 0; s < num_strips; ++s) {
                if (s > 0) {
                    for (auto& m : line_moves[y]) {
                        if (m.kind == ScapOpKind::kMove &&
                            m.slot_index == static_cast<int>(s - 1) && m.reg < kBaseColors) {
                            auto rgb12 = m.rgb_ocs & 0xFFFu;
                            float r = static_cast<float>((rgb12 >> 8) & 0xF) / 15.0f;
                            float g = static_cast<float>((rgb12 >> 4) & 0xF) / 15.0f;
                            float bv = static_cast<float>(rgb12 & 0xF) / 15.0f;
                            running_pal[m.reg] = color_space::srgb_to_linear(Color3f{r, g, bv});
                        }
                    }
                }
                strip_pals[s] = running_pal;
                strip_srgbs[s].resize(kBaseColors);
                for (std::size_t k = 0; k < kBaseColors; ++k)
                    strip_srgbs[s][k] = ham::linear_to_srgb8(running_pal[k]);
                strip_pres.emplace_back(std::span<const Color3f>(strip_pals[s].data(), kBaseColors),
                                        /*data_bits=*/4);
                strip_srgb_spans.emplace_back(strip_srgbs[s].data(), kBaseColors);
            }
        };

        // DP encode without triple refinement — used for candidate
        // scoring (refine adds ~50% cost without changing relative
        // ordering of nearby candidates much).
        auto encode_dp_only = [&](const std::vector<std::vector<ham::SRGBColor>>& strip_srgbs,
                                  std::span<const ham::HamPrecomp> pres_span,
                                  std::span<const std::span<const ham::SRGBColor>> srgbs_span)
            -> ham::ScanlineResult {
            ham::SRGBColor start = strip_srgbs[0].empty() ? ham::SRGBColor{0, 0, 0}
                                                          : strip_srgbs[0][0];
            return ham::encode_scanline_dp_per_strip(
                row_span, start, pres_span, srgbs_span, idx_span, kBeamWidth, ham_metric);
        };

        // Baseline: encode the row with no strips swaps yet (sliced only).
        std::vector<std::vector<Color3f>> cur_pals;
        std::vector<std::vector<ham::SRGBColor>> cur_srgbs;
        std::vector<ham::HamPrecomp> cur_pres;
        std::vector<std::span<const ham::SRGBColor>> cur_srgb_spans;
        build_strips(cur_pals, cur_srgbs, cur_pres, cur_srgb_spans);
        double cur_err = static_cast<double>(
            encode_dp_only(cur_srgbs,
                           std::span<const ham::HamPrecomp>(cur_pres),
                           std::span<const std::span<const ham::SRGBColor>>(cur_srgb_spans))
                .error);

        std::size_t strips_budget = strips_share;
        std::size_t strips_used = 0;
        for (std::size_t s = 1; s < num_strips; ++s) {
            if (strips_used >= strips_budget) break;
            auto [x_lo, x_hi] = strip_x_range(s);
            if (x_lo >= x_hi) continue;

            // Per-slot cluster: pixel assignments + cumulative LAB sum
            // for centroid computation, plus per-pixel residual squared
            // distance for the spread term.
            struct Clust {
                double L = 0, a = 0, b = 0, spread = 0;
                std::uint32_t count = 0;
            };
            std::array<Clust, kBaseColors> clust{};
            // First pass: nearest-slot assignment + per-cluster sum.
            std::vector<std::uint8_t> assign(x_hi - x_lo, 0);
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto& lab = img_lab[y * width + x];
                float best_d = std::numeric_limits<float>::max();
                std::size_t best_k = 0;
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    float dL = lab.L - strip_pal_lab[k].L;
                    float da = lab.a - strip_pal_lab[k].a;
                    float db = lab.b - strip_pal_lab[k].b;
                    float d = color_space::fma_dist_sq(dL, da, db);
                    if (d < best_d) {
                        best_d = d;
                        best_k = k;
                    }
                }
                assign[x - x_lo] = static_cast<std::uint8_t>(best_k);
                auto& c = clust[best_k];
                c.L += static_cast<double>(lab.L);
                c.a += static_cast<double>(lab.a);
                c.b += static_cast<double>(lab.b);
                c.spread += static_cast<double>(best_d);
                ++c.count;
            }

            // Compute centroid means for non-empty clusters.
            std::array<color_space::OKLab, kBaseColors> centroid{};
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (clust[k].count > 0) {
                    centroid[k] = color_space::OKLab{
                        static_cast<float>(clust[k].L / static_cast<double>(clust[k].count)),
                        static_cast<float>(clust[k].a / static_cast<double>(clust[k].count)),
                        static_cast<float>(clust[k].b / static_cast<double>(clust[k].count)),
                    };
                }
            }

            // Build candidate set: cluster centroids + every distinct
            // strip pixel (OCS-bucketed, dedup via 12-bit key).
            std::vector<Color3f> cands;
            std::vector<color_space::OKLab> cands_lab;
            std::array<bool, 4096> seen{};
            auto add_cand = [&](Color3f c) {
                auto cs = palette::quantize_to_ocs(c);
                auto key = static_cast<std::size_t>(palette::linear_to_ocs(cs) & 0xFFFu);
                if (!seen[key]) {
                    seen[key] = true;
                    cands.push_back(cs);
                    cands_lab.push_back(color_space::linear_to_oklab(cs));
                }
            };
            for (std::size_t x = x_lo; x < x_hi; ++x)
                add_cand(image[x, y]);
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (clust[k].count > 0) {
                    add_cand(color_space::oklab_to_linear(centroid[k]).clamped());
                }
            }

            // Pre-screen (slot, candidate) pairs by centroid score.
            // The DP scorer below is ~1 ms per evaluation, so we trim
            // the candidate set to the most promising kTopK before
            // running the actual encoder. Centroid score is a coarse
            // SET-only proxy — good enough for ranking but not for
            // final selection.
            std::size_t k_min = lock_color0 ? 1u : 0u;
            struct CandScore {
                std::size_t slot;
                std::size_t cand_idx;
                double centroid_red;
            };
            std::vector<CandScore> ranked;
            ranked.reserve(kBaseColors * cands.size());
            for (std::size_t k = k_min; k < kBaseColors; ++k) {
                if (clust[k].count == 0) continue;
                float dL_old = centroid[k].L - strip_pal_lab[k].L;
                float da_old = centroid[k].a - strip_pal_lab[k].a;
                float db_old = centroid[k].b - strip_pal_lab[k].b;
                double old_e = static_cast<double>(clust[k].count) *
                               static_cast<double>(
                                   color_space::fma_dist_sq(dL_old, da_old, db_old));
                for (std::size_t ci = 0; ci < cands.size(); ++ci) {
                    auto& cl = cands_lab[ci];
                    float dL = centroid[k].L - cl.L;
                    float da = centroid[k].a - cl.a;
                    float db = centroid[k].b - cl.b;
                    double new_e = static_cast<double>(clust[k].count) *
                                   static_cast<double>(color_space::fma_dist_sq(dL, da, db));
                    double red = old_e - new_e;
                    if (red > 0.0) ranked.push_back({k, ci, red});
                }
            }
            if (ranked.empty()) continue;
            constexpr std::size_t kTopK = 5;
            if (ranked.size() > kTopK) {
                std::partial_sort(ranked.begin(),
                                  ranked.begin() + kTopK,
                                  ranked.end(),
                                  [](const CandScore& a, const CandScore& b) {
                                      return a.centroid_red > b.centroid_red;
                                  });
                ranked.resize(kTopK);
            } else {
                std::sort(ranked.begin(), ranked.end(), [](const CandScore& a, const CandScore& b) {
                    return a.centroid_red > b.centroid_red;
                });
            }

            // DP-evaluate each survivor: tentatively push the swap as a
            // ScapMove, rebuild strip palettes (which propagates the
            // swap forward into all later strips), encode, score by
            // sl.error, restore. Pick the best swap iff it strictly
            // beats cur_err.
            std::size_t best_slot = 0;
            std::size_t best_cand = 0;
            double best_err = cur_err;
            bool found = false;
            for (auto& cs : ranked) {
                auto rgb_ocs = palette::linear_to_ocs(cands[cs.cand_idx]);
                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(cs.slot), rgb_ocs, static_cast<int>(s - 1)));
                build_strips(cur_pals, cur_srgbs, cur_pres, cur_srgb_spans);
                auto trial = encode_dp_only(
                    cur_srgbs,
                    std::span<const ham::HamPrecomp>(cur_pres),
                    std::span<const std::span<const ham::SRGBColor>>(cur_srgb_spans));
                line_moves[y].pop_back();
                double e = static_cast<double>(trial.error);
                if (e < best_err) {
                    best_err = e;
                    best_slot = cs.slot;
                    best_cand = cs.cand_idx;
                    found = true;
                }
            }
            if (!found) continue;

            // Apply best swap for real.
            strip_pal[best_slot] = cands[best_cand];
            strip_pal_lab[best_slot] = cands_lab[best_cand];
            auto rgb_ocs = palette::linear_to_ocs(cands[best_cand]);
            line_moves[y].push_back(
                make_move(static_cast<std::uint8_t>(best_slot), rgb_ocs, static_cast<int>(s - 1)));
            cur_err = best_err;
            ++strips_used;
            (void)kHblankCeiling;
        }
        // DEBUG: pad strips slots — every slot 0..num_slots-1 must have a
        // MOVE (filler if no swap was chosen) so per-line copper budget
        // is constant. Match DPF/EHB strips. Then close the chain with
        // the end-of-line WAIT.
        {
            constexpr std::uint8_t kFillerReg = 31;
            constexpr std::uint16_t kFillerVal = 0x0000;
            const std::size_t num_slots = (num_strips > 0) ? num_strips - 1 : 0;
            std::size_t strips_start = line_moves[y].size();
            for (std::size_t i = 0; i < line_moves[y].size(); ++i) {
                if (line_moves[y][i].kind == ScapOpKind::kMove &&
                    line_moves[y][i].slot_index >= 0) {
                    strips_start = i;
                    break;
                }
            }
            std::vector<ScapMove> strips_moves(line_moves[y].begin() +
                                                   static_cast<std::ptrdiff_t>(strips_start),
                                               line_moves[y].end());
            line_moves[y].resize(strips_start);
            std::size_t mi = 0;
            for (std::size_t slot = 0; slot < num_slots; ++slot) {
                if (mi < strips_moves.size() && strips_moves[mi].slot_index >= 0 &&
                    static_cast<std::size_t>(strips_moves[mi].slot_index) == slot) {
                    line_moves[y].push_back(strips_moves[mi]);
                    ++mi;
                } else {
                    line_moves[y].push_back(
                        make_move(kFillerReg, kFillerVal, static_cast<int>(slot)));
                }
            }
            line_moves[y].push_back(
                make_wait(static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));
        }
        // Rebuild final strip state for the post-loop encode_with.
        build_strips(cur_pals, cur_srgbs, cur_pres, cur_srgb_spans);
        scanline_palettes_full[y] = strip_pal;  // end-of-line state
        total_moves += line_moves[y].size();

        // ---- 4. Final encode with triple refinement -------------------
        // The HAM-DP-aware planner above only accepted swaps that
        // strictly reduced sl.error, so no quality gate is needed —
        // the swap set is monotonically optimal under the same metric
        // we evaluate here. Run one more DP encode + triple-pixel
        // refinement to produce the final ham_values for this row.
        auto pres_span = std::span<const ham::HamPrecomp>(cur_pres.data(), cur_pres.size());
        auto srgbs_span = std::span<const std::span<const ham::SRGBColor>>(cur_srgb_spans.data(),
                                                                           cur_srgb_spans.size());
        ham::SRGBColor start = cur_srgbs[0].empty() ? ham::SRGBColor{0, 0, 0} : cur_srgbs[0][0];
        auto sl = ham::encode_scanline_dp_per_strip(
            row_span, start, pres_span, srgbs_span, idx_span, kBeamWidth, ham_metric);
        ham::refine_scanline_triple_per_strip(
            sl.values, row_span, start, pres_span, srgbs_span, idx_span, kTripleBeam, ham_metric);
        auto& strip_srgbs = cur_srgbs;
        // Render the per-pixel preview by replaying the encoded values
        // through the strip's palette + HAM rolling state.
        ham::SRGBColor prev = start;
        for (std::size_t x = 0; x < width; ++x) {
            ham_values[y * width + x] = sl.values[x];
            std::size_t s = strip_idx[x];
            std::uint8_t v = sl.values[x];
            std::uint8_t ctrl = static_cast<std::uint8_t>(v >> 4);
            std::uint8_t data = static_cast<std::uint8_t>(v & 0xF);
            ham::SRGBColor out{};
            if (ctrl == 0u) {
                out = strip_srgbs[s][data];
            } else {
                std::uint8_t expanded = static_cast<std::uint8_t>(data * 17u);
                out = prev;
                if (ctrl == 0b01)
                    out.b = expanded;
                else if (ctrl == 0b10)
                    out.r = expanded;
                else if (ctrl == 0b11)
                    out.g = expanded;
            }
            preview[x, y] = color_space::srgb_u8_to_linear(out.r, out.g, out.b);
            prev = out;
        }
        // Walk this line's MOVEs to update hw_state for next row's sliced
        // diff. Any MOVE to a base-color register (0..15) lands in the
        // simulated hardware state. Filler MOVEs (reg=31) are ignored.
        for (auto& m : line_moves[y]) {
            if (m.kind != ScapOpKind::kMove) continue;
            if (m.reg >= kBaseColors) continue;
            auto rgb12 = m.rgb_ocs & 0xFFFu;
            float r = static_cast<float>((rgb12 >> 8) & 0xF) / 15.0f;
            float g = static_cast<float>((rgb12 >> 4) & 0xF) / 15.0f;
            float bv = static_cast<float>(rgb12 & 0xF) / 15.0f;
            hw_state[m.reg] = color_space::srgb_to_linear(Color3f{r, g, bv});
        }
        // total_moves: count of MOVEs in line_moves[y].
        total_moves_atomic.fetch_add(line_moves[y].size());
        // total_error: accumulate via CAS since std::atomic<double>::fetch_add
        // is C++20 but not always supported on libstdc++; CAS keeps it portable.
        double cur_te = total_error_atomic.load();
        double new_te;
        do {
            new_te = cur_te + static_cast<double>(sl.error);
        } while (!total_error_atomic.compare_exchange_weak(cur_te, new_te));
    };  // run_row

    if (serial_path) {
        for (std::size_t y = 0; y < height; ++y)
            run_row(y, outer_hw_state);
    } else {
        // Parallel: each worker gets its own hw_state seeded to base
        // palette (the line-entry assumption when there's no prior-row
        // carry-over). The outer hw_state stays as-is and is unused.
        pipeline::parallel_for(height, [&](std::size_t y) {
            std::vector<Color3f> local_hw_state = make_initial_hw_state();
            run_row(y, local_hw_state);
        });
    }
    total_error = total_error_atomic.load();
    total_moves = total_moves_atomic.load();

    // ---- 5. Pack 6-plane bitplane data --------------------------------
    auto planes = bitplane::encode(ham_values, width, height, 6);
    if (!planes) return std::unexpected{planes.error()};

    // ---- 6. Assemble result ------------------------------------------
    ScapResult res;
    res.planes = *std::move(planes);
    res.palette = base_palette;
    res.line_moves = std::move(line_moves);
    res.rendered = std::move(preview);
    res.total_error = static_cast<float>(total_error);
    res.avg_changes_per_line = static_cast<float>(ham_cap_result->changes_per_line);
    auto h_div = static_cast<float>(height ? height : 1);
    res.avg_total_moves_per_line = static_cast<float>(total_moves) / h_div;
    res.max_moves_per_line = 1 + table.slots.size();
    res.avg_hblank_moves_per_line = 1.0f;
    res.max_hblank_moves_per_line = 1;
    res.avg_visible_moves_per_line = static_cast<float>(table.slots.size());
    res.max_visible_moves_per_line = table.slots.size();
    if (on_progress) on_progress(1.0f, "done");
    for (auto& p : res.rendered.pixels())
        p = palette::quantize_to_ocs(p);
    return res;
}

// ---------------------------------------------------------------------------
// Probe A — sweep one COLOR09 write across HPOS 0x00..0xE3, one per line,
// on a 6-plane OCS DPF frame.
// ---------------------------------------------------------------------------
Result<ScapResult> make_scap_probe_a_dpf_ocs(int width, int height) {
    if (width <= 0) width = 320;
    if (height <= 0) height = 256;

    auto planes = make_dpf_pf2_index1_planes(static_cast<std::size_t>(width),
                                             static_cast<std::size_t>(height),
                                             /*total_planes=*/6,
                                             /*add_position_grid=*/true);
    if (!planes) return std::unexpected{planes.error()};

    ScapResult res;
    res.planes = *std::move(planes);
    res.slot_table = strips_table_for(6);
    res.probe_label = "probe_a_dpf_ocs";

    // Frame-start palette: 16 entries.
    //   reg 0  = black (bg, also PF1 idx 0 = transparent)
    //   reg 1  = yellow (PF1 minor tick every 16 px,  plane 0 only)
    //   reg 3  = red    (PF1 major tick every 64 px,  plane 0|2 -> idx 3)
    //   reg 9  = strips-controlled (PF2 color, swept by per-line copper)
    //   others = black
    res.palette.assign(16, Color3f{0.0f, 0.0f, 0.0f});
    res.palette[1] = Color3f{1.0f, 1.0f, 0.0f};  // yellow
    res.palette[3] = Color3f{1.0f, 0.0f, 0.0f};  // red

    res.line_moves.resize(static_cast<std::size_t>(height));

    // Image row 0 is rendered at PAL VSTART=44, so absolute VPOS = y + 44.
    constexpr int kVStart = 44;

    for (int y = 0; y < height; ++y) {
        auto& ops = res.line_moves[static_cast<std::size_t>(y)];
        int abs_vpos = y + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // 1. Reset COLOR09 to black at top-of-line.
        ops.push_back(make_wait(0x00, vp));
        ops.push_back(make_move(/*reg=*/9, /*rgb_ocs=*/0x0000));

        // 2. Anchor WAIT — deterministic phase reference.
        ops.push_back(make_wait(static_cast<std::uint8_t>(res.slot_table.line_gate_hpos), vp));

        // 3. Probe WAIT at swept HPOS, then MOVE white into COLOR09.
        ops.push_back(
            make_wait(static_cast<std::uint8_t>(hp_for_y(y, height)), vp, /*slot_index=*/0));
        ops.push_back(make_move(/*reg=*/9, /*rgb_ocs=*/0x0FFF, /*slot_index=*/0));
    }

    return res;
}

// Probes B/C/D — placeholders until Probe A data is in.
Result<ScapResult> make_scap_probe_b_dpf_ocs(int /*w*/, int /*h*/) {
    return std::unexpected{Error{
        ErrorCode::unsupported_mode,
        "Strips Probe B not implemented yet (needs Probe A slot data first)",
    }};
}

Result<ScapResult> make_scap_probe_c_dpf_aga(int /*w*/, int /*h*/) {
    return std::unexpected{Error{
        ErrorCode::unsupported_mode,
        "Strips Probe C not implemented yet (AGA bandwidth, deferred)",
    }};
}

Result<ScapResult> make_scap_probe_d_dpf_ocs(int /*w*/, int /*h*/) {
    return std::unexpected{Error{
        ErrorCode::unsupported_mode,
        "Strips Probe D not implemented yet (at-x vs after-x pixel mapping)",
    }};
}

}  // namespace png2amiga::strips
