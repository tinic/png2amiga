#pragma once

#include "types.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Backend select for fast_cbrt4 — mirrors quantize.cpp: WASM SIMD on
// Emscripten, NEON on AArch64, SSE2 on x86 (covers both MSVC and x86
// GCC/Clang). Scalar fallback covers everything else.
#if defined(__wasm_simd128__)
    #include <wasm_simd128.h>
    #define PNG2AMIGA_CBRT_BACKEND_WASM 1
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define PNG2AMIGA_CBRT_BACKEND_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #include <immintrin.h>
    #define PNG2AMIGA_CBRT_BACKEND_SSE2 1
#endif

namespace png2amiga::color_space {

// ---------------------------------------------------------------------------
// sRGB <-> linear conversion
// ---------------------------------------------------------------------------

constexpr float srgb_to_linear(float s) noexcept {
    if (s <= 0.04045f) {
        return s / 12.92f;
    }
    return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

constexpr float linear_to_srgb(float l) noexcept {
    if (l <= 0.0031308f) {
        return l * 12.92f;
    }
    return 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

constexpr Color3f srgb_to_linear(Color3f srgb) noexcept {
    return {
        srgb_to_linear(srgb.r),
        srgb_to_linear(srgb.g),
        srgb_to_linear(srgb.b),
    };
}

constexpr Color3f linear_to_srgb(Color3f linear) noexcept {
    return {
        linear_to_srgb(linear.r),
        linear_to_srgb(linear.g),
        linear_to_srgb(linear.b),
    };
}

// LUT: byte value -> linear float
// constexpr on GCC (constexpr pow since C++14 as a GCC extension); runtime-
// init on clang (libc++ doesn't have constexpr std::pow yet) and MSVC (STL
// hasn't shipped C++26 P1383 constexpr <cmath> as of MSVC 14.50).
#if defined(__GNUC__) && !defined(__clang__)
#define PNG2AMIGA_LUT_CONSTEXPR constexpr
#else
// `inline` (not blank) — these definitions live in headers; without `inline`
// each TU emits its own copy and the linker rejects the program with LNK2005
// on MSVC. `constexpr` implies inline, which is why GCC didn't need this.
#define PNG2AMIGA_LUT_CONSTEXPR inline
#endif

#if defined(__GNUC__) && !defined(__clang__)
// GCC: fully constexpr LUT computed at compile time
constexpr auto make_srgb_lut() noexcept {
    std::array<float, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        lut[static_cast<std::size_t>(i)] =
            srgb_to_linear(static_cast<float>(i) / 255.0f);
    }
    return lut;
}

inline constexpr auto srgb_lut = make_srgb_lut();

constexpr Color3f srgb_u8_to_linear(std::uint8_t r, std::uint8_t g,
                                     std::uint8_t b) noexcept {
    return {srgb_lut[r], srgb_lut[g], srgb_lut[b]};
}
#else
// Clang/Emscripten: runtime-initialized LUT via function-local static
inline const std::array<float, 256>& get_srgb_lut() noexcept {
    static const auto lut = [] {
        std::array<float, 256> l{};
        for (int i = 0; i < 256; ++i)
            l[static_cast<std::size_t>(i)] =
                srgb_to_linear(static_cast<float>(i) / 255.0f);
        return l;
    }();
    return lut;
}

inline Color3f srgb_u8_to_linear(std::uint8_t r, std::uint8_t g,
                                  std::uint8_t b) noexcept {
    auto& lut = get_srgb_lut();
    return {lut[r], lut[g], lut[b]};
}
#endif

// Convert sRGB hex (0xRRGGBB) to linear Color3f
PNG2AMIGA_LUT_CONSTEXPR Color3f srgb_hex_to_linear(std::uint32_t hex) noexcept {
    auto r = static_cast<std::uint8_t>((hex >> 16) & 0xFF);
    auto g = static_cast<std::uint8_t>((hex >> 8) & 0xFF);
    auto b = static_cast<std::uint8_t>(hex & 0xFF);
    return srgb_u8_to_linear(r, g, b);
}

// ---------------------------------------------------------------------------
// OKLab color space (perceptual)
// ---------------------------------------------------------------------------

struct OKLab {
    float L{};
    float a{};
    float b{};
};

// Squared 3-component distance using std::fma for scalar paths. Saves one
// rounding step per add (vfmadd231ss has the same throughput as a separate
// vmulss + vaddss but tighter precision and better port routing on modern
// x86). For SIMD code paths use _mm*_fmadd_ps directly — this helper is
// the scalar entry point so all callers route through one definition.
[[gnu::always_inline]]
inline float fma_dist_sq(float dx, float dy, float dz) noexcept {
    return std::fma(dx, dx, std::fma(dy, dy, dz * dz));
}

[[gnu::always_inline]]
inline float fma_dist_sq(OKLab a, OKLab b) noexcept {
    return fma_dist_sq(a.L - b.L, a.a - b.a, a.b - b.b);
}

[[gnu::always_inline]]
inline double fma_dist_sq(double dx, double dy, double dz) noexcept {
    return std::fma(dx, dx, std::fma(dy, dy, dz * dz));
}


// 4-lane cube root for OKLab LMS conversion (lane 3 is padding).
//
// Walczyk-style approximation: bit-twiddle seed + 2 Newton iterations.
// The integer /3 of the classical bit-trick is replaced by a chain of
// shift-and-adds (5/16 → 85/256 → 21845/65536) that auto-vectorises to
// AVX2 vpsrld/vpaddd cleanly — the vectorized Granlund-Montgomery /3
// in the previous Halley variant was the pipeline bottleneck.
//
// Audit results vs std::cbrt over all 16.7M sRGB triples (tools/cbrt_audit):
//   - OCS code mismatches: 521 (0.0031%), worst per-channel delta 3 nibbles
//   - Mean ULP error on raw cbrt: 2.2; worst 12 ULP
//   - Speed under GCC: 3.9 ns/oklab vs 7.8 ns std::cbrt (2.0x), vs 12.9 ns
//     for the prior 1-Halley variant (3.3x). Under MSVC ucrt the win is
//     larger — ucrt's scalar cbrtf was ~50% of total CPU time before.
//
// Inputs are RGB-to-LMS outputs in [0, ~1.5], always non-negative; we don't
// handle denormals/Inf/NaN. The (absbits != 0) mask preserves cbrt(0) = 0
// exactly (otherwise the magic constant + Newton would produce a tiny
// non-zero result for a 0 input).
#if defined(__GNUC__) || defined(__clang__)
using f32x4 [[gnu::vector_size(16)]] = float;
using u32x4 [[gnu::vector_size(16)]] = std::uint32_t;
#else
// MSVC fallback. The GCC vector-extension type supports aggregate init
// `{a,b,c,d}`, subscript `v[i]`, and elementwise/scalar arithmetic. A plain
// aggregate of `float[4]` covers all three with brace-elision so call sites
// don't need to change. u32x4 isn't used in the MSVC build path
// (only by tools/cbrt_audit.cpp, which is GCC-only).
struct f32x4 {
    float v[4];
    constexpr float& operator[](std::size_t i) noexcept { return v[i]; }
    constexpr float operator[](std::size_t i) const noexcept { return v[i]; }
};
constexpr f32x4 operator+(f32x4 a, f32x4 b) noexcept {
    return {{a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]}};
}
constexpr f32x4 operator*(f32x4 a, float s) noexcept {
    return {{a[0] * s, a[1] * s, a[2] * s, a[3] * s}};
}
constexpr f32x4 operator*(float s, f32x4 a) noexcept { return a * s; }
#endif

// Helper: load 4 floats from f32x4 (vector_size or struct) into a typed
// pointer for explicit-intrinsic loads.
[[gnu::always_inline]]
inline const float* fast_cbrt4_data(const f32x4& x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return reinterpret_cast<const float*>(&x);
#else
    return x.v;
#endif
}

[[gnu::always_inline]]
inline f32x4 fast_cbrt4_make(const float* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return f32x4{p[0], p[1], p[2], p[3]};
#else
    return f32x4{{p[0], p[1], p[2], p[3]}};
#endif
}

[[gnu::always_inline]]
inline f32x4 fast_cbrt4(f32x4 x) noexcept {
#if defined(PNG2AMIGA_CBRT_BACKEND_WASM)
    // WASM SIMD path. wasm_v128 covers both i32 and f32 lanes; reinterpret
    // is free (just a type-system pun, no instruction).
    v128_t v = wasm_v128_load(fast_cbrt4_data(x));
    v128_t sign_mask = wasm_i32x4_splat(static_cast<int32_t>(0x80000000u));
    v128_t sign    = wasm_v128_and(v, sign_mask);
    v128_t absbits = wasm_v128_andnot(v, sign_mask);

    v128_t t = wasm_i32x4_add(wasm_u32x4_shr(absbits, 2),
                              wasm_u32x4_shr(absbits, 4));
    t = wasm_i32x4_add(t, wasm_u32x4_shr(t, 4));
    t = wasm_i32x4_add(t, wasm_u32x4_shr(t, 8));
    v128_t seed = wasm_i32x4_add(wasm_i32x4_splat(0x2a5137a0), t);

    v128_t absx = absbits;       // bit-equivalent reinterpret
    v128_t y    = seed;
    v128_t third = wasm_f32x4_splat(0.33333333f);
    v128_t two   = wasm_f32x4_splat(2.0f);

    // Newton iter 1: y <- (2y + absx / y^2) / 3
    v128_t yy = wasm_f32x4_mul(y, y);
    y = wasm_f32x4_mul(third,
            wasm_f32x4_add(wasm_f32x4_mul(two, y),
                           wasm_f32x4_div(absx, yy)));
    // Newton iter 2
    yy = wasm_f32x4_mul(y, y);
    y = wasm_f32x4_mul(third,
            wasm_f32x4_add(wasm_f32x4_mul(two, y),
                           wasm_f32x4_div(absx, yy)));

    // Zero-input mask: cmp_gt over signed i32 since absbits >= 0.
    v128_t nonzero = wasm_i32x4_gt(absbits, wasm_i32x4_splat(0));
    v128_t out = wasm_v128_or(wasm_v128_and(y, nonzero), sign);

    alignas(16) float buf[4];
    wasm_v128_store(buf, out);
    return fast_cbrt4_make(buf);

#elif defined(PNG2AMIGA_CBRT_BACKEND_NEON)
    // ARM NEON path. vbicq does andnot in argument order (a & ~b), so we
    // pass absbits = vbicq(v, sign_mask).
    float32x4_t vf = vld1q_f32(fast_cbrt4_data(x));
    uint32x4_t v = vreinterpretq_u32_f32(vf);
    uint32x4_t sign_mask = vdupq_n_u32(0x80000000u);
    uint32x4_t sign = vandq_u32(v, sign_mask);
    uint32x4_t absbits = vbicq_u32(v, sign_mask);

    uint32x4_t t = vaddq_u32(vshrq_n_u32(absbits, 2),
                             vshrq_n_u32(absbits, 4));
    t = vaddq_u32(t, vshrq_n_u32(t, 4));
    t = vaddq_u32(t, vshrq_n_u32(t, 8));
    uint32x4_t seed = vaddq_u32(vdupq_n_u32(0x2a5137a0u), t);

    float32x4_t absx = vreinterpretq_f32_u32(absbits);
    float32x4_t y    = vreinterpretq_f32_u32(seed);
    float32x4_t third = vdupq_n_f32(0.33333333f);
    float32x4_t two   = vdupq_n_f32(2.0f);

    // Newton iter 1: y <- (2y + absx / y^2) / 3
    float32x4_t yy = vmulq_f32(y, y);
    y = vmulq_f32(third, vaddq_f32(vmulq_f32(two, y),
                                   vdivq_f32(absx, yy)));
    // Newton iter 2
    yy = vmulq_f32(y, y);
    y = vmulq_f32(third, vaddq_f32(vmulq_f32(two, y),
                                   vdivq_f32(absx, yy)));

    // Zero-input mask: cmp_gt over the unsigned absbits.
    uint32x4_t nonzero = vcgtq_u32(absbits, vdupq_n_u32(0));
    uint32x4_t out_bits = vorrq_u32(
        vandq_u32(vreinterpretq_u32_f32(y), nonzero), sign);

    alignas(16) float buf[4];
    vst1q_f32(buf, vreinterpretq_f32_u32(out_bits));
    return fast_cbrt4_make(buf);

#elif defined(PNG2AMIGA_CBRT_BACKEND_SSE2)
    // x86 SSE2/AVX2 path. Covers MSVC and x86 GCC/Clang — under MSVC the
    // earlier scalar-per-lane fallback compiled to 4× vdivss per Newton
    // step (~400 Mcyc each in the per-instruction profile); explicit
    // __m128 gives one vdivps per step covering all 4 lanes.
    __m128 vf = _mm_loadu_ps(fast_cbrt4_data(x));
    __m128i v = _mm_castps_si128(vf);
    __m128i sign_mask = _mm_set1_epi32(static_cast<int>(0x80000000));
    __m128i sign    = _mm_and_si128(v, sign_mask);
    __m128i absbits = _mm_andnot_si128(sign_mask, v);

    __m128i t = _mm_add_epi32(_mm_srli_epi32(absbits, 2),
                              _mm_srli_epi32(absbits, 4));
    t = _mm_add_epi32(t, _mm_srli_epi32(t, 4));
    t = _mm_add_epi32(t, _mm_srli_epi32(t, 8));
    __m128i seed = _mm_add_epi32(_mm_set1_epi32(0x2a5137a0), t);

    __m128 absx = _mm_castsi128_ps(absbits);
    __m128 y    = _mm_castsi128_ps(seed);
    __m128 third = _mm_set1_ps(0.33333333f);
    __m128 two   = _mm_set1_ps(2.0f);

    // Newton iter 1: y <- (2y + absx / y^2) / 3
    __m128 yy = _mm_mul_ps(y, y);
    y = _mm_mul_ps(third, _mm_add_ps(_mm_mul_ps(two, y),
                                     _mm_div_ps(absx, yy)));
    // Newton iter 2
    yy = _mm_mul_ps(y, y);
    y = _mm_mul_ps(third, _mm_add_ps(_mm_mul_ps(two, y),
                                     _mm_div_ps(absx, yy)));

    __m128i nonzero = _mm_cmpgt_epi32(absbits, _mm_setzero_si128());
    __m128i out_bits = _mm_or_si128(
        _mm_and_si128(_mm_castps_si128(y), nonzero), sign);

    alignas(16) float buf[4];
    _mm_storeu_ps(buf, _mm_castsi128_ps(out_bits));
    return fast_cbrt4_make(buf);

#else
    // Scalar fallback for exotic targets without WASM/NEON/SSE2.
    auto cbrt1 = [](float fx) -> float {
        std::uint32_t bits = std::bit_cast<std::uint32_t>(fx);
        std::uint32_t sign = bits & 0x80000000u;
        std::uint32_t absbits = bits & 0x7fffffffu;
        std::uint32_t t = (absbits >> 2u) + (absbits >> 4u);
        t = t + (t >> 4u);
        t = t + (t >> 8u);
        std::uint32_t i = 0x2a5137a0u + t;
        float absx = std::bit_cast<float>(absbits);
        float y = std::bit_cast<float>(i);
        y = 0.33333333f * (2.0f * y + absx / (y * y));
        y = 0.33333333f * (2.0f * y + absx / (y * y));
        std::uint32_t mask = (absbits != 0u) ? 0xffffffffu : 0u;
        std::uint32_t result = (std::bit_cast<std::uint32_t>(y) & mask) | sign;
        return std::bit_cast<float>(result);
    };
    const float* p = fast_cbrt4_data(x);
    float buf[4] = {cbrt1(p[0]), cbrt1(p[1]), cbrt1(p[2]), cbrt1(p[3])};
    return fast_cbrt4_make(buf);
#endif
}

[[gnu::always_inline]]
inline OKLab linear_to_oklab(Color3f c) noexcept {
    // LMS matrix applied as column-vec linear combinations of (r, g, b).
    // Pack LMS into a single f32x4 (lane 3 ignored) so the cbrt can SIMD.
    f32x4 lms =
        f32x4{0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f} * c.r +
        f32x4{0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f} * c.g +
        f32x4{0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f} * c.b;
    f32x4 lms_ = fast_cbrt4(lms);
    return {
        0.2104542553f * lms_[0] + 0.7936177850f * lms_[1] - 0.0040720468f * lms_[2],
        1.9779984951f * lms_[0] - 2.4285922050f * lms_[1] + 0.4505937099f * lms_[2],
        0.0259040371f * lms_[0] + 0.7827717662f * lms_[1] - 0.8086757660f * lms_[2],
    };
}

// ---------------------------------------------------------------------------
// Fast 8-bit sRGB → OKLab path.
//
// HAM's DP beam search calls linear_to_oklab(srgb8_to_linear(rgb)) tens of
// millions of times per conversion, always with 8-bit sRGB inputs. We
// fold the sRGB→linear LUT and the linear→LMS matrix multiply into a single
// per-channel LUT of LMS contributions:
//
//   LMS(r, g, b) = srgb_lms_r[r] + srgb_lms_g[g] + srgb_lms_b[b]
//
// Each entry is an f32x4 (L, M, S, pad). One load+add per channel replaces
// an sRGB-linearize lookup and a 3-column matrix-vector multiply. The
// remaining cbrt + final matrix stays the same shape.
// ---------------------------------------------------------------------------

namespace detail {
inline const std::array<std::array<f32x4, 256>, 3>& srgb_lms_lut() noexcept {
    static const auto lut = [] {
        std::array<std::array<f32x4, 256>, 3> t{};
        for (int i = 0; i < 256; ++i) {
            float linear = srgb_to_linear(static_cast<float>(i) / 255.0f);
            auto idx = static_cast<std::size_t>(i);
            t[0][idx] = f32x4{0.4122214708f, 0.2119034982f,
                              0.0883024619f, 0.0f} * linear;
            t[1][idx] = f32x4{0.5363325363f, 0.6806995451f,
                              0.2817188376f, 0.0f} * linear;
            t[2][idx] = f32x4{0.0514459929f, 0.1073969566f,
                              0.6299787005f, 0.0f} * linear;
        }
        return t;
    }();
    return lut;
}
}  // namespace detail

// LMS (as f32x4, lane 3 unused) for an 8-bit sRGB color. Splits per channel
// so callers with one varying channel can cache the other two.
[[gnu::always_inline]]
inline f32x4 srgb8_to_lms(std::uint8_t r, std::uint8_t g,
                          std::uint8_t b) noexcept {
    auto& t = detail::srgb_lms_lut();
    return t[0][r] + t[1][g] + t[2][b];
}

// Convert cbrt(LMS) to OKLab (final matrix). Shared between variants.
[[gnu::always_inline]]
inline OKLab lms_cbrt_to_oklab(f32x4 lms_) noexcept {
    return {
        0.2104542553f * lms_[0] + 0.7936177850f * lms_[1] - 0.0040720468f * lms_[2],
        1.9779984951f * lms_[0] - 2.4285922050f * lms_[1] + 0.4505937099f * lms_[2],
        0.0259040371f * lms_[0] + 0.7827717662f * lms_[1] - 0.8086757660f * lms_[2],
    };
}

[[gnu::always_inline]]
inline OKLab srgb8_to_oklab(std::uint8_t r, std::uint8_t g,
                            std::uint8_t b) noexcept {
    return lms_cbrt_to_oklab(fast_cbrt4(srgb8_to_lms(r, g, b)));
}

constexpr Color3f oklab_to_linear(OKLab lab) noexcept {
    float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    return {
        +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    };
}

// Squared perceptual distance in OKLab space
// OKLab channel weights for perceptual distance.
// Boosting 'a' (red-green axis) preserves reds better — OKLab
// slightly underweights red perception vs human sensitivity.
// Tuned OKLab weights: slightly de-weight luminance to preserve more
// color variation in the palette. Optimized on fantasy.png lores 5bpl.
// Use --weight-l/--weight-a/--weight-b CLI flags to experiment.
inline float WEIGHT_L = 0.85f;
inline float WEIGHT_A = 1.05f;   // red-green axis
inline float WEIGHT_B = 1.0f;    // blue-yellow axis

inline float perceptual_distance_sq(Color3f a, Color3f b) noexcept {
    auto la = linear_to_oklab(a);
    auto lb = linear_to_oklab(b);
    float dL = (la.L - lb.L) * WEIGHT_L;
    float da = (la.a - lb.a) * WEIGHT_A;
    float db = (la.b - lb.b) * WEIGHT_B;
    return fma_dist_sq(dL, da, db);
}

// Compute PSNR in 8-bit sRGB space between two images after a small
// Gaussian blur (σ≈1.5).  For dithered output, the blur simulates
// viewing-distance / eye-optics averaging: the high-frequency dither
// pattern integrates out, leaving the perceived color close to the
// original.  Plain PSNR would punish the deliberate dither noise.
// Returns dB (higher is better); +inf for identical images.
inline float compute_psnr_blurred(std::span<const Color3f> original,
                                   std::span<const Color3f> rendered,
                                   std::size_t width,
                                   std::size_t height) noexcept {
    auto n = width * height;
    if (n == 0 || original.size() < n || rendered.size() < n)
        return 0.0f;

    // 7-tap Gaussian, σ=1.5
    constexpr std::array<float, 7> kernel = {
        0.0369f, 0.1110f, 0.2167f, 0.2708f, 0.2167f, 0.1110f, 0.0369f};
    constexpr int krad = 3;

    // Separable blur: horizontal pass, then vertical
    auto blur = [&](std::span<const Color3f> src, std::vector<Color3f>& dst) {
        std::vector<Color3f> tmp(n);
        // Horizontal
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                Color3f acc{0, 0, 0};
                float wsum = 0;
                for (int k = -krad; k <= krad; ++k) {
                    auto xi = static_cast<int>(x) + k;
                    if (xi < 0 || static_cast<std::size_t>(xi) >= width) continue;
                    float w = kernel[static_cast<std::size_t>(k + krad)];
                    auto& p = src[y * width + static_cast<std::size_t>(xi)];
                    acc.r += p.r * w;
                    acc.g += p.g * w;
                    acc.b += p.b * w;
                    wsum += w;
                }
                acc.r /= wsum; acc.g /= wsum; acc.b /= wsum;
                tmp[y * width + x] = acc;
            }
        }
        // Vertical
        dst.resize(n);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                Color3f acc{0, 0, 0};
                float wsum = 0;
                for (int k = -krad; k <= krad; ++k) {
                    auto yi = static_cast<int>(y) + k;
                    if (yi < 0 || static_cast<std::size_t>(yi) >= height) continue;
                    float w = kernel[static_cast<std::size_t>(k + krad)];
                    auto& p = tmp[static_cast<std::size_t>(yi) * width + x];
                    acc.r += p.r * w;
                    acc.g += p.g * w;
                    acc.b += p.b * w;
                    wsum += w;
                }
                acc.r /= wsum; acc.g /= wsum; acc.b /= wsum;
                dst[y * width + x] = acc;
            }
        }
    };

    std::vector<Color3f> a_blur, b_blur;
    blur(original, a_blur);
    blur(rendered, b_blur);

    double sum_sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        auto a = linear_to_srgb(a_blur[i]).clamped();
        auto b = linear_to_srgb(b_blur[i]).clamped();
        double dr = static_cast<double>(a.r - b.r) * 255.0;
        double dg = static_cast<double>(a.g - b.g) * 255.0;
        double db_ = static_cast<double>(a.b - b.b) * 255.0;
        sum_sq += fma_dist_sq(dr, dg, db_);
    }
    if (sum_sq < 1e-12) return std::numeric_limits<float>::infinity();
    double mse = sum_sq / (static_cast<double>(n) * 3.0);
    return static_cast<float>(10.0 * std::log10(255.0 * 255.0 / mse));
}

} // namespace png2amiga::color_space
