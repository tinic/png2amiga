#pragma once

#include "types.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Native (x86_64 + ARM64) compilers emit a hardware FMA for std::fma at
// -ffp-contract=fast. WASM doesn't — Emscripten lowers std::fma to a
// libm `fmaf` libcall (no f32 FMA in the base WASM ISA; the relaxed-
// SIMD f32x4.relaxed_madd is only the SIMD form). The scalar libcall
// ate 25.9% of active CPU on an AGA d=8 sliced encode in the Linux
// Node profile (May 2026, AMD EPYC 7413). On WASM, use plain
// `a*b + c` — -ffp-contract=fast still permits the compiler to FMA-
// fuse where the target supports it; on WASM where it doesn't, two
// cheap mul/add ops beat a libcall.
#if defined(__EMSCRIPTEN__) || defined(__wasm__)
    #define PNG2AMIGA_FMA(a, b, c) ((a) * (b) + (c))
#else
    #define PNG2AMIGA_FMA(a, b, c) std::fma((a), (b), (c))
#endif

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

// Portable force-inline. MSVC ignores [[gnu::always_inline]] silently
// and falls back to its own heuristics; for hot SIMD intrinsics that
// need to be folded into the caller (no spill / reload through a
// memory parameter), __forceinline is required. Clang errors on
// `[[msvc::forceinline]]` when it isn't itself MSVC, so we can't use
// the attribute syntax portably.
#if defined(_MSC_VER) && !defined(__clang__)
#define PNG2AMIGA_INLINE_HOT __forceinline
#else
#define PNG2AMIGA_INLINE_HOT inline __attribute__((always_inline))
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
        lut[static_cast<std::size_t>(i)] = srgb_to_linear(static_cast<float>(i) / 255.0f);
    }
    return lut;
}

inline constexpr auto srgb_lut = make_srgb_lut();

constexpr Color3f srgb_u8_to_linear(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return {srgb_lut[r], srgb_lut[g], srgb_lut[b]};
}
#else
// Clang/Emscripten: runtime-initialized LUT via function-local static
inline const std::array<float, 256>& get_srgb_lut() noexcept {
    static const auto lut = [] {
        std::array<float, 256> l{};
        for (int i = 0; i < 256; ++i)
            l[static_cast<std::size_t>(i)] = srgb_to_linear(static_cast<float>(i) / 255.0f);
        return l;
    }();
    return lut;
}

inline Color3f srgb_u8_to_linear(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
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

// Defined here (before the SIMD section) so the fused srgb_to_oklab_simd
// can return one. Full OKLab utility (linear_to_oklab, distances, batch
// helpers) lives further down.
struct OKLab {
    float L{};
    float a{};
    float b{};
};

// ---------------------------------------------------------------------------
// Fixed-exponent pow(x, 2.4) approximation for the sRGB → linear step.
//
// Direct degree-9 minimax fit of pow_branch(s) = ((s+0.055)/1.055)^2.4
// on s ∈ [0, 1] — i.e. the polynomial input is `s` itself, the
// (s+0.055)/1.055 scaling is baked into the coefficients. Single Horner
// evaluation, no division, no log/exp split.
//
// Bench: tools/bench_pow24.cpp
//   M3:    14.79 × over std::pow (NEON x4)
//   Zen 1: 20.64 × over std::pow (SSE4 x4) / 37.21 × (AVX2 x8)
// Accuracy:
//   max abs err 1.19e-06, max rel err 2.06e-04
//   well below 4-bit OCS quantization step (6.25 % per channel).
//
// Coefficients fitted via tools/fit_srgb_pow24.py.
// ---------------------------------------------------------------------------
namespace pow24_fixed {
constexpr float c0 = +8.3605809601e-04f;
constexpr float c1 = +3.6089365513e-02f;
constexpr float c2 = +4.7321428955e-01f;
constexpr float c3 = +9.5458970791e-01f;
constexpr float c4 = -1.2557098121e+00f;
constexpr float c5 = +1.9300281423e+00f;
constexpr float c6 = -2.2409846367e+00f;
constexpr float c7 = +1.7148746033e+00f;
constexpr float c8 = -7.5960070594e-01f;
constexpr float c9 = +1.4666385867e-01f;
}  // namespace pow24_fixed

// ---------------------------------------------------------------------------
// SIMD sRGB → linear for the dither hot loop.
//
// apply_error_diffusion calls srgb_to_linear(target_s) once per pixel.
// Vectorising the per-pixel 3-channel pow at SSE/NEON/WASM 4-wide gives
// ~3.5–4.5× over scalar std::pow at ~1 ULP precision (bench: tools/
// bench_pow24.cpp). Polynomial coefficients lifted from SLEEF's xlogf /
// xexpf (sleefsimdsp.c, BSD/Boost license).
//
// Measured speedups for srgb_to_linear_simd vs scalar std::pow on a
// large random workload:
//   * AMD Zen 1 / MSVC      : 4.4× (SSE4)
//   * Apple Silicon M3      : 3.5× (NEON)
//   * WASM SIMD (browser)   : measured at integration time
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

namespace detail {

PNG2AMIGA_INLINE_HOT __m128 sleef_lnf_sse4(__m128 d) noexcept {
    __m128 d_scaled = _mm_mul_ps(d, _mm_set1_ps(4.0f / 3.0f));
    __m128i e_int = _mm_sub_epi32(
        _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(d_scaled), 23), _mm_set1_epi32(0xFF)),
        _mm_set1_epi32(127));
    __m128 e = _mm_cvtepi32_ps(e_int);
    __m128i m_bits = _mm_sub_epi32(_mm_castps_si128(d), _mm_slli_epi32(e_int, 23));
    __m128 m = _mm_castsi128_ps(m_bits);
    __m128 x = _mm_div_ps(_mm_sub_ps(m, _mm_set1_ps(1.0f)), _mm_add_ps(m, _mm_set1_ps(1.0f)));
    __m128 x2 = _mm_mul_ps(x, x);
    __m128 t = _mm_set1_ps(0.2392828464508056640625f);
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(0.28518211841583251953125f));
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(0.400005877017974853515625f));
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(0.666666686534881591796875f));
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(2.0f));
    return _mm_fmadd_ps(e, _mm_set1_ps(0.693147180559945286226764f), _mm_mul_ps(x, t));
}

PNG2AMIGA_INLINE_HOT __m128 sleef_expf_sse4(__m128 d) noexcept {
    __m128 q_f = _mm_round_ps(_mm_mul_ps(d, _mm_set1_ps(1.4426950408889634f)),
                              _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m128i q = _mm_cvtps_epi32(q_f);
    __m128 s = _mm_fmadd_ps(q_f, _mm_set1_ps(-0.693145751953125f), d);
    s = _mm_fmadd_ps(q_f, _mm_set1_ps(-1.4286067653e-06f), s);
    __m128 u = _mm_set1_ps(0.000198527617612853646278381f);
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.00139304355252534151077271f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.00833336077630519866943359f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.0416664853692054748535156f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.166666671633720397949219f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.5f));
    u = _mm_add_ps(_mm_set1_ps(1.0f), _mm_fmadd_ps(_mm_mul_ps(s, s), u, s));
    __m128i shifted = _mm_slli_epi32(_mm_add_epi32(q, _mm_set1_epi32(127)), 23);
    return _mm_mul_ps(u, _mm_castsi128_ps(shifted));
}

}  // namespace detail

PNG2AMIGA_INLINE_HOT Color3f srgb_to_linear_simd(Color3f c) noexcept {
    __m128 s = _mm_setr_ps(c.r, c.g, c.b, 0.0f);
    __m128 lin = _mm_mul_ps(s, _mm_set1_ps(1.0f / 12.92f));
    __m128 base = _mm_mul_ps(_mm_add_ps(s, _mm_set1_ps(0.055f)), _mm_set1_ps(1.0f / 1.055f));
    __m128 ln_b = detail::sleef_lnf_sse4(base);
    __m128 pw = detail::sleef_expf_sse4(_mm_mul_ps(_mm_set1_ps(2.4f), ln_b));
    __m128 mask = _mm_cmple_ps(s, _mm_set1_ps(0.04045f));
    __m128 r = _mm_blendv_ps(pw, lin, mask);
    alignas(16) float out[4];
    _mm_store_ps(out, r);
    return Color3f{out[0], out[1], out[2]};
}

namespace detail {

// In-register fast_cbrt — same algorithm as fast_cbrt4, takes __m128
// directly so the fused srgb_to_oklab path doesn't roundtrip through
// f32x4 storage.
PNG2AMIGA_INLINE_HOT __m128 fast_cbrt_m128_sse4(__m128 vf) noexcept {
    __m128i v = _mm_castps_si128(vf);
    __m128i sign_mask = _mm_set1_epi32(static_cast<int>(0x80000000));
    __m128i sign = _mm_and_si128(v, sign_mask);
    __m128i absbits = _mm_andnot_si128(sign_mask, v);
    __m128i t = _mm_add_epi32(_mm_srli_epi32(absbits, 2), _mm_srli_epi32(absbits, 4));
    t = _mm_add_epi32(t, _mm_srli_epi32(t, 4));
    t = _mm_add_epi32(t, _mm_srli_epi32(t, 8));
    __m128i seed = _mm_add_epi32(_mm_set1_epi32(0x2a5137a0), t);
    __m128 absx = _mm_castsi128_ps(absbits);
    __m128 y = _mm_castsi128_ps(seed);
    __m128 third = _mm_set1_ps(0.33333333f);
    __m128 two = _mm_set1_ps(2.0f);
    __m128 yy = _mm_mul_ps(y, y);
    y = _mm_mul_ps(third, _mm_add_ps(_mm_mul_ps(two, y), _mm_div_ps(absx, yy)));
    yy = _mm_mul_ps(y, y);
    y = _mm_mul_ps(third, _mm_add_ps(_mm_mul_ps(two, y), _mm_div_ps(absx, yy)));
    __m128i nonzero = _mm_cmpgt_epi32(absbits, _mm_setzero_si128());
    __m128i out_bits = _mm_or_si128(_mm_and_si128(_mm_castps_si128(y), nonzero), sign);
    return _mm_castsi128_ps(out_bits);
}

}  // namespace detail

// Fused srgb → oklab. Keeps data in __m128 across pow / LMS-mul /
// cbrt; only extracts to scalars at the final OKLab matrix step.
// Per-pixel call site that previously chained
//   Color3f lin = srgb_to_linear(c);
//   OKLab oklab = linear_to_oklab(lin);
// collapses into one call with no Color3f roundtrip — which matters
// per the v1.71.x perf round (`project_perf_dead_ends_2026_05.md`):
// the un-fused version was neutral on Zen 1 even with proven 4× pow
// speedup, because the Color3f pack/unpack ate the win.
PNG2AMIGA_INLINE_HOT OKLab srgb_to_oklab_simd(Color3f c) noexcept {
    __m128 srgb = _mm_setr_ps(c.r, c.g, c.b, 0.0f);
    // 1) sRGB → linear (fixed-2.4 degree-9 polynomial, branchless).
    __m128 lin_branch = _mm_mul_ps(srgb, _mm_set1_ps(1.0f / 12.92f));
    __m128 p = _mm_set1_ps(pow24_fixed::c9);
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c8));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c7));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c6));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c5));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c4));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c3));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c2));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c1));
    p = _mm_fmadd_ps(p, srgb, _mm_set1_ps(pow24_fixed::c0));
    __m128 mask = _mm_cmple_ps(srgb, _mm_set1_ps(0.04045f));
    __m128 lin = _mm_blendv_ps(p, lin_branch, mask);
    // 2) Linear → LMS via 3 broadcast + FMA.
    __m128 r_b = _mm_shuffle_ps(lin, lin, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 g_b = _mm_shuffle_ps(lin, lin, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 b_b = _mm_shuffle_ps(lin, lin, _MM_SHUFFLE(2, 2, 2, 2));
    __m128 lms = _mm_mul_ps(_mm_setr_ps(0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f), r_b);
    lms = _mm_fmadd_ps(_mm_setr_ps(0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f), g_b, lms);
    lms = _mm_fmadd_ps(_mm_setr_ps(0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f), b_b, lms);
    // 3) cbrt (4-wide, lane 3 padding).
    __m128 lmsc = detail::fast_cbrt_m128_sse4(lms);
    // 4) Final OKLab matrix — 3 dot products. Scalar tail since the
    // result is a 3-float OKLab struct anyway.
    alignas(16) float out[4];
    _mm_store_ps(out, lmsc);
    float l = out[0], m = out[1], s = out[2];
    return {
        PNG2AMIGA_FMA(0.2104542553f, l, PNG2AMIGA_FMA(0.7936177850f, m, -0.0040720468f * s)),
        PNG2AMIGA_FMA(1.9779984951f, l, PNG2AMIGA_FMA(-2.4285922050f, m, 0.4505937099f * s)),
        PNG2AMIGA_FMA(0.0259040371f, l, PNG2AMIGA_FMA(0.7827717662f, m, -0.8086757660f * s)),
    };
}

#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>

namespace detail {

PNG2AMIGA_INLINE_HOT float32x4_t sleef_lnf_neon(float32x4_t d) noexcept {
    float32x4_t d_scaled = vmulq_n_f32(d, 4.0f / 3.0f);
    int32x4_t e_int = vsubq_s32(
        vandq_s32(vshrq_n_s32(vreinterpretq_s32_f32(d_scaled), 23), vdupq_n_s32(0xFF)),
        vdupq_n_s32(127));
    float32x4_t e = vcvtq_f32_s32(e_int);
    int32x4_t m_bits = vsubq_s32(vreinterpretq_s32_f32(d), vshlq_n_s32(e_int, 23));
    float32x4_t m = vreinterpretq_f32_s32(m_bits);
    float32x4_t x = vdivq_f32(vsubq_f32(m, vdupq_n_f32(1.0f)), vaddq_f32(m, vdupq_n_f32(1.0f)));
    float32x4_t x2 = vmulq_f32(x, x);
    float32x4_t t = vdupq_n_f32(0.2392828464508056640625f);
    t = vfmaq_f32(vdupq_n_f32(0.28518211841583251953125f), t, x2);
    t = vfmaq_f32(vdupq_n_f32(0.400005877017974853515625f), t, x2);
    t = vfmaq_f32(vdupq_n_f32(0.666666686534881591796875f), t, x2);
    t = vfmaq_f32(vdupq_n_f32(2.0f), t, x2);
    return vfmaq_f32(vmulq_f32(x, t), e, vdupq_n_f32(0.693147180559945286226764f));
}

PNG2AMIGA_INLINE_HOT float32x4_t sleef_expf_neon(float32x4_t d) noexcept {
    float32x4_t q_f = vrndnq_f32(vmulq_n_f32(d, 1.4426950408889634f));
    int32x4_t q = vcvtq_s32_f32(q_f);
    float32x4_t s = vfmaq_f32(d, q_f, vdupq_n_f32(-0.693145751953125f));
    s = vfmaq_f32(s, q_f, vdupq_n_f32(-1.4286067653e-06f));
    float32x4_t u = vdupq_n_f32(0.000198527617612853646278381f);
    u = vfmaq_f32(vdupq_n_f32(0.00139304355252534151077271f), u, s);
    u = vfmaq_f32(vdupq_n_f32(0.00833336077630519866943359f), u, s);
    u = vfmaq_f32(vdupq_n_f32(0.0416664853692054748535156f), u, s);
    u = vfmaq_f32(vdupq_n_f32(0.166666671633720397949219f), u, s);
    u = vfmaq_f32(vdupq_n_f32(0.5f), u, s);
    u = vaddq_f32(vdupq_n_f32(1.0f), vfmaq_f32(s, vmulq_f32(s, s), u));
    int32x4_t shifted = vshlq_n_s32(vaddq_s32(q, vdupq_n_s32(127)), 23);
    return vmulq_f32(u, vreinterpretq_f32_s32(shifted));
}

}  // namespace detail

PNG2AMIGA_INLINE_HOT Color3f srgb_to_linear_simd(Color3f c) noexcept {
    alignas(16) float in[4] = {c.r, c.g, c.b, 0.0f};
    float32x4_t s = vld1q_f32(in);
    float32x4_t lin = vmulq_n_f32(s, 1.0f / 12.92f);
    float32x4_t base = vmulq_n_f32(vaddq_f32(s, vdupq_n_f32(0.055f)), 1.0f / 1.055f);
    float32x4_t ln_b = detail::sleef_lnf_neon(base);
    float32x4_t pw = detail::sleef_expf_neon(vmulq_n_f32(ln_b, 2.4f));
    uint32x4_t mask = vcleq_f32(s, vdupq_n_f32(0.04045f));
    float32x4_t r = vbslq_f32(mask, lin, pw);
    alignas(16) float out[4];
    vst1q_f32(out, r);
    return Color3f{out[0], out[1], out[2]};
}

namespace detail {

PNG2AMIGA_INLINE_HOT float32x4_t fast_cbrt_neon(float32x4_t vf) noexcept {
    uint32x4_t v = vreinterpretq_u32_f32(vf);
    uint32x4_t sign_mask = vdupq_n_u32(0x80000000u);
    uint32x4_t sign = vandq_u32(v, sign_mask);
    uint32x4_t absbits = vbicq_u32(v, sign_mask);
    uint32x4_t t = vaddq_u32(vshrq_n_u32(absbits, 2), vshrq_n_u32(absbits, 4));
    t = vaddq_u32(t, vshrq_n_u32(t, 4));
    t = vaddq_u32(t, vshrq_n_u32(t, 8));
    uint32x4_t seed = vaddq_u32(vdupq_n_u32(0x2a5137a0u), t);
    float32x4_t absx = vreinterpretq_f32_u32(absbits);
    float32x4_t y = vreinterpretq_f32_u32(seed);
    float32x4_t third = vdupq_n_f32(0.33333333f);
    float32x4_t two = vdupq_n_f32(2.0f);
    float32x4_t yy = vmulq_f32(y, y);
    y = vmulq_f32(third, vaddq_f32(vmulq_f32(two, y), vdivq_f32(absx, yy)));
    yy = vmulq_f32(y, y);
    y = vmulq_f32(third, vaddq_f32(vmulq_f32(two, y), vdivq_f32(absx, yy)));
    uint32x4_t nonzero = vcgtq_u32(absbits, vdupq_n_u32(0));
    uint32x4_t out_bits = vorrq_u32(vandq_u32(vreinterpretq_u32_f32(y), nonzero), sign);
    return vreinterpretq_f32_u32(out_bits);
}

}  // namespace detail

PNG2AMIGA_INLINE_HOT OKLab srgb_to_oklab_simd(Color3f c) noexcept {
    alignas(16) float in[4] = {c.r, c.g, c.b, 0.0f};
    float32x4_t srgb = vld1q_f32(in);
    // 1) sRGB → linear (fixed-2.4 degree-9 polynomial, branchless).
    float32x4_t lin_branch = vmulq_n_f32(srgb, 1.0f / 12.92f);
    float32x4_t p = vdupq_n_f32(pow24_fixed::c9);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c8), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c7), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c6), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c5), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c4), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c3), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c2), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c1), p, srgb);
    p = vfmaq_f32(vdupq_n_f32(pow24_fixed::c0), p, srgb);
    uint32x4_t mask = vcleq_f32(srgb, vdupq_n_f32(0.04045f));
    float32x4_t lin = vbslq_f32(mask, lin_branch, p);
    // 2) Linear → LMS via 3 broadcast + FMA.
    float32x4_t r_b = vdupq_laneq_f32(lin, 0);
    float32x4_t g_b = vdupq_laneq_f32(lin, 1);
    float32x4_t b_b = vdupq_laneq_f32(lin, 2);
    alignas(16) const float col_r[4] = {0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f};
    alignas(16) const float col_g[4] = {0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f};
    alignas(16) const float col_b[4] = {0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f};
    float32x4_t lms = vmulq_f32(vld1q_f32(col_r), r_b);
    lms = vfmaq_f32(lms, vld1q_f32(col_g), g_b);
    lms = vfmaq_f32(lms, vld1q_f32(col_b), b_b);
    // 3) cbrt.
    float32x4_t lmsc = detail::fast_cbrt_neon(lms);
    // 4) Final OKLab matrix — scalar tail.
    alignas(16) float out[4];
    vst1q_f32(out, lmsc);
    float l = out[0], m = out[1], s = out[2];
    return {
        PNG2AMIGA_FMA(0.2104542553f, l, PNG2AMIGA_FMA(0.7936177850f, m, -0.0040720468f * s)),
        PNG2AMIGA_FMA(1.9779984951f, l, PNG2AMIGA_FMA(-2.4285922050f, m, 0.4505937099f * s)),
        PNG2AMIGA_FMA(0.0259040371f, l, PNG2AMIGA_FMA(0.7827717662f, m, -0.8086757660f * s)),
    };
}

#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>

namespace detail {

[[gnu::always_inline]]
inline v128_t sleef_lnf_wasm(v128_t d) noexcept {
    v128_t d_scaled = wasm_f32x4_mul(d, wasm_f32x4_splat(4.0f / 3.0f));
    v128_t e_int = wasm_i32x4_sub(
        wasm_v128_and(wasm_u32x4_shr(d_scaled, 23), wasm_i32x4_splat(0xFF)), wasm_i32x4_splat(127));
    v128_t e = wasm_f32x4_convert_i32x4(e_int);
    v128_t m = wasm_i32x4_sub(d, wasm_i32x4_shl(e_int, 23));
    v128_t x = wasm_f32x4_div(wasm_f32x4_sub(m, wasm_f32x4_splat(1.0f)),
                              wasm_f32x4_add(m, wasm_f32x4_splat(1.0f)));
    v128_t x2 = wasm_f32x4_mul(x, x);
    v128_t t = wasm_f32x4_splat(0.2392828464508056640625f);
    t = wasm_f32x4_add(wasm_f32x4_mul(t, x2), wasm_f32x4_splat(0.28518211841583251953125f));
    t = wasm_f32x4_add(wasm_f32x4_mul(t, x2), wasm_f32x4_splat(0.400005877017974853515625f));
    t = wasm_f32x4_add(wasm_f32x4_mul(t, x2), wasm_f32x4_splat(0.666666686534881591796875f));
    t = wasm_f32x4_add(wasm_f32x4_mul(t, x2), wasm_f32x4_splat(2.0f));
    return wasm_f32x4_add(wasm_f32x4_mul(e, wasm_f32x4_splat(0.693147180559945286226764f)),
                          wasm_f32x4_mul(x, t));
}

[[gnu::always_inline]]
inline v128_t sleef_expf_wasm(v128_t d) noexcept {
    v128_t q_f = wasm_f32x4_nearest(wasm_f32x4_mul(d, wasm_f32x4_splat(1.4426950408889634f)));
    v128_t q = wasm_i32x4_trunc_sat_f32x4(q_f);
    v128_t s = wasm_f32x4_add(wasm_f32x4_mul(q_f, wasm_f32x4_splat(-0.693145751953125f)), d);
    s = wasm_f32x4_add(wasm_f32x4_mul(q_f, wasm_f32x4_splat(-1.4286067653e-06f)), s);
    v128_t u = wasm_f32x4_splat(0.000198527617612853646278381f);
    u = wasm_f32x4_add(wasm_f32x4_mul(u, s), wasm_f32x4_splat(0.00139304355252534151077271f));
    u = wasm_f32x4_add(wasm_f32x4_mul(u, s), wasm_f32x4_splat(0.00833336077630519866943359f));
    u = wasm_f32x4_add(wasm_f32x4_mul(u, s), wasm_f32x4_splat(0.0416664853692054748535156f));
    u = wasm_f32x4_add(wasm_f32x4_mul(u, s), wasm_f32x4_splat(0.166666671633720397949219f));
    u = wasm_f32x4_add(wasm_f32x4_mul(u, s), wasm_f32x4_splat(0.5f));
    u = wasm_f32x4_add(wasm_f32x4_splat(1.0f),
                       wasm_f32x4_add(wasm_f32x4_mul(wasm_f32x4_mul(s, s), u), s));
    v128_t shifted = wasm_i32x4_shl(wasm_i32x4_add(q, wasm_i32x4_splat(127)), 23);
    return wasm_f32x4_mul(u, shifted);
}

}  // namespace detail

PNG2AMIGA_INLINE_HOT Color3f srgb_to_linear_simd(Color3f c) noexcept {
    alignas(16) float in[4] = {c.r, c.g, c.b, 0.0f};
    v128_t s = wasm_v128_load(in);
    v128_t lin = wasm_f32x4_mul(s, wasm_f32x4_splat(1.0f / 12.92f));
    v128_t base = wasm_f32x4_mul(wasm_f32x4_add(s, wasm_f32x4_splat(0.055f)),
                                 wasm_f32x4_splat(1.0f / 1.055f));
    v128_t ln_b = detail::sleef_lnf_wasm(base);
    v128_t pw = detail::sleef_expf_wasm(wasm_f32x4_mul(ln_b, wasm_f32x4_splat(2.4f)));
    v128_t mask = wasm_f32x4_le(s, wasm_f32x4_splat(0.04045f));
    v128_t r = wasm_v128_bitselect(lin, pw, mask);
    alignas(16) float out[4];
    wasm_v128_store(out, r);
    return Color3f{out[0], out[1], out[2]};
}

namespace detail {

PNG2AMIGA_INLINE_HOT v128_t fast_cbrt_wasm(v128_t vf) noexcept {
    v128_t sign_mask = wasm_i32x4_splat(static_cast<int32_t>(0x80000000u));
    v128_t sign = wasm_v128_and(vf, sign_mask);
    v128_t absbits = wasm_v128_andnot(vf, sign_mask);
    v128_t t = wasm_i32x4_add(wasm_u32x4_shr(absbits, 2), wasm_u32x4_shr(absbits, 4));
    t = wasm_i32x4_add(t, wasm_u32x4_shr(t, 4));
    t = wasm_i32x4_add(t, wasm_u32x4_shr(t, 8));
    v128_t seed = wasm_i32x4_add(wasm_i32x4_splat(0x2a5137a0), t);
    v128_t absx = absbits;
    v128_t y = seed;
    v128_t third = wasm_f32x4_splat(0.33333333f);
    v128_t two = wasm_f32x4_splat(2.0f);
    v128_t yy = wasm_f32x4_mul(y, y);
    y = wasm_f32x4_mul(third, wasm_f32x4_add(wasm_f32x4_mul(two, y), wasm_f32x4_div(absx, yy)));
    yy = wasm_f32x4_mul(y, y);
    y = wasm_f32x4_mul(third, wasm_f32x4_add(wasm_f32x4_mul(two, y), wasm_f32x4_div(absx, yy)));
    v128_t nonzero = wasm_i32x4_gt(absbits, wasm_i32x4_splat(0));
    v128_t out_bits = wasm_v128_or(wasm_v128_and(y, nonzero), sign);
    return out_bits;
}

}  // namespace detail

PNG2AMIGA_INLINE_HOT OKLab srgb_to_oklab_simd(Color3f c) noexcept {
    alignas(16) float in[4] = {c.r, c.g, c.b, 0.0f};
    v128_t srgb = wasm_v128_load(in);
    // 1) sRGB → linear (fixed-2.4 degree-9 polynomial, branchless).
    v128_t lin_branch = wasm_f32x4_mul(srgb, wasm_f32x4_splat(1.0f / 12.92f));
    auto madd = [&](v128_t prev, float k) {
        return wasm_f32x4_add(wasm_f32x4_mul(prev, srgb), wasm_f32x4_splat(k));
    };
    v128_t p = wasm_f32x4_splat(pow24_fixed::c9);
    p = madd(p, pow24_fixed::c8);
    p = madd(p, pow24_fixed::c7);
    p = madd(p, pow24_fixed::c6);
    p = madd(p, pow24_fixed::c5);
    p = madd(p, pow24_fixed::c4);
    p = madd(p, pow24_fixed::c3);
    p = madd(p, pow24_fixed::c2);
    p = madd(p, pow24_fixed::c1);
    p = madd(p, pow24_fixed::c0);
    v128_t mask = wasm_f32x4_le(srgb, wasm_f32x4_splat(0.04045f));
    v128_t lin = wasm_v128_bitselect(lin_branch, p, mask);
    // 2) Linear → LMS via 3 broadcast + FMA.
    v128_t r_b = wasm_f32x4_splat(wasm_f32x4_extract_lane(lin, 0));
    v128_t g_b = wasm_f32x4_splat(wasm_f32x4_extract_lane(lin, 1));
    v128_t b_b = wasm_f32x4_splat(wasm_f32x4_extract_lane(lin, 2));
    alignas(16) const float col_r[4] = {0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f};
    alignas(16) const float col_g[4] = {0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f};
    alignas(16) const float col_b[4] = {0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f};
    v128_t lms = wasm_f32x4_mul(wasm_v128_load(col_r), r_b);
    lms = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(col_g), g_b), lms);
    lms = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(col_b), b_b), lms);
    // 3) cbrt.
    v128_t lmsc = detail::fast_cbrt_wasm(lms);
    // 4) Final OKLab matrix — scalar tail.
    alignas(16) float out[4];
    wasm_v128_store(out, lmsc);
    float l = out[0], m = out[1], s = out[2];
    return {
        PNG2AMIGA_FMA(0.2104542553f, l, PNG2AMIGA_FMA(0.7936177850f, m, -0.0040720468f * s)),
        PNG2AMIGA_FMA(1.9779984951f, l, PNG2AMIGA_FMA(-2.4285922050f, m, 0.4505937099f * s)),
        PNG2AMIGA_FMA(0.0259040371f, l, PNG2AMIGA_FMA(0.7827717662f, m, -0.8086757660f * s)),
    };
}

#else  // No SIMD ISA detected — analytic scalar fallback.
PNG2AMIGA_INLINE_HOT Color3f srgb_to_linear_simd(Color3f c) noexcept {
    return srgb_to_linear(c);
}
PNG2AMIGA_INLINE_HOT OKLab srgb_to_oklab_simd(Color3f c) noexcept {
    return linear_to_oklab(srgb_to_linear(c));
}
#endif

// ---------------------------------------------------------------------------
// OKLab color space (perceptual)
// ---------------------------------------------------------------------------
//
// (struct OKLab is forward-defined above so srgb_to_oklab_simd can return
//  one. This section adds the rest of the OKLab utility surface.)

// Squared 3-component distance using std::fma for scalar paths. Saves one
// rounding step per add (vfmadd231ss has the same throughput as a separate
// vmulss + vaddss but tighter precision and better port routing on modern
// x86). For SIMD code paths use _mm*_fmadd_ps directly — this helper is
// the scalar entry point so all callers route through one definition.
[[gnu::always_inline]]
inline float fma_dist_sq(float dx, float dy, float dz) noexcept {
    return PNG2AMIGA_FMA(dx, dx, PNG2AMIGA_FMA(dy, dy, dz * dz));
}

[[gnu::always_inline]]
inline float fma_dist_sq(OKLab a, OKLab b) noexcept {
    return fma_dist_sq(a.L - b.L, a.a - b.a, a.b - b.b);
}

[[gnu::always_inline]]
inline double fma_dist_sq(double dx, double dy, double dz) noexcept {
    return PNG2AMIGA_FMA(dx, dx, PNG2AMIGA_FMA(dy, dy, dz * dz));
}

// 3-coefficient FMA dot product: c0*x0 + c1*x1 + c2*x2. Used by the OKLab
// matrix-row computations (3 rows of 3 mul-adds each, twice over for
// linear<->OKLab). Two std::fma calls + one mul = 3 ops vs the naive
// 3 mul + 2 add = 5 ops; same throughput on FMA hardware but tighter
// rounding and better port pressure. (WASM: routed through PNG2AMIGA_FMA
// to dodge the fmaf libcall.)
[[gnu::always_inline]]
inline float fma_dot3(float c0, float x0, float c1, float x1, float c2, float x2) noexcept {
    return PNG2AMIGA_FMA(c0, x0, PNG2AMIGA_FMA(c1, x1, c2 * x2));
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
constexpr f32x4 operator*(float s, f32x4 a) noexcept {
    return a * s;
}
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
    v128_t sign = wasm_v128_and(v, sign_mask);
    v128_t absbits = wasm_v128_andnot(v, sign_mask);

    v128_t t = wasm_i32x4_add(wasm_u32x4_shr(absbits, 2), wasm_u32x4_shr(absbits, 4));
    t = wasm_i32x4_add(t, wasm_u32x4_shr(t, 4));
    t = wasm_i32x4_add(t, wasm_u32x4_shr(t, 8));
    v128_t seed = wasm_i32x4_add(wasm_i32x4_splat(0x2a5137a0), t);

    v128_t absx = absbits;  // bit-equivalent reinterpret
    v128_t y = seed;
    v128_t third = wasm_f32x4_splat(0.33333333f);
    v128_t two = wasm_f32x4_splat(2.0f);

    // Newton iter 1: y <- (2y + absx / y^2) / 3
    v128_t yy = wasm_f32x4_mul(y, y);
    y = wasm_f32x4_mul(third, wasm_f32x4_add(wasm_f32x4_mul(two, y), wasm_f32x4_div(absx, yy)));
    // Newton iter 2
    yy = wasm_f32x4_mul(y, y);
    y = wasm_f32x4_mul(third, wasm_f32x4_add(wasm_f32x4_mul(two, y), wasm_f32x4_div(absx, yy)));

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

    uint32x4_t t = vaddq_u32(vshrq_n_u32(absbits, 2), vshrq_n_u32(absbits, 4));
    t = vaddq_u32(t, vshrq_n_u32(t, 4));
    t = vaddq_u32(t, vshrq_n_u32(t, 8));
    uint32x4_t seed = vaddq_u32(vdupq_n_u32(0x2a5137a0u), t);

    float32x4_t absx = vreinterpretq_f32_u32(absbits);
    float32x4_t y = vreinterpretq_f32_u32(seed);
    float32x4_t third = vdupq_n_f32(0.33333333f);
    float32x4_t two = vdupq_n_f32(2.0f);

    // Newton iter 1: y <- (2y + absx / y^2) / 3
    float32x4_t yy = vmulq_f32(y, y);
    y = vmulq_f32(third, vaddq_f32(vmulq_f32(two, y), vdivq_f32(absx, yy)));
    // Newton iter 2
    yy = vmulq_f32(y, y);
    y = vmulq_f32(third, vaddq_f32(vmulq_f32(two, y), vdivq_f32(absx, yy)));

    // Zero-input mask: cmp_gt over the unsigned absbits.
    uint32x4_t nonzero = vcgtq_u32(absbits, vdupq_n_u32(0));
    uint32x4_t out_bits = vorrq_u32(vandq_u32(vreinterpretq_u32_f32(y), nonzero), sign);

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
    __m128i sign = _mm_and_si128(v, sign_mask);
    __m128i absbits = _mm_andnot_si128(sign_mask, v);

    __m128i t = _mm_add_epi32(_mm_srli_epi32(absbits, 2), _mm_srli_epi32(absbits, 4));
    t = _mm_add_epi32(t, _mm_srli_epi32(t, 4));
    t = _mm_add_epi32(t, _mm_srli_epi32(t, 8));
    __m128i seed = _mm_add_epi32(_mm_set1_epi32(0x2a5137a0), t);

    __m128 absx = _mm_castsi128_ps(absbits);
    __m128 y = _mm_castsi128_ps(seed);
    __m128 third = _mm_set1_ps(0.33333333f);
    __m128 two = _mm_set1_ps(2.0f);

    // Newton iter 1: y <- (2y + absx / y^2) / 3
    __m128 yy = _mm_mul_ps(y, y);
    y = _mm_mul_ps(third, _mm_add_ps(_mm_mul_ps(two, y), _mm_div_ps(absx, yy)));
    // Newton iter 2
    yy = _mm_mul_ps(y, y);
    y = _mm_mul_ps(third, _mm_add_ps(_mm_mul_ps(two, y), _mm_div_ps(absx, yy)));

    __m128i nonzero = _mm_cmpgt_epi32(absbits, _mm_setzero_si128());
    __m128i out_bits = _mm_or_si128(_mm_and_si128(_mm_castps_si128(y), nonzero), sign);

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
// Forward decl — definition below, after the f32x4 LMS LUT helpers.
inline OKLab lms_cbrt_to_oklab(f32x4 lms_) noexcept;

[[gnu::always_inline]]
inline OKLab linear_to_oklab(Color3f c) noexcept {
    // LMS matrix applied as column-vec linear combinations of (r, g, b).
    // Pack LMS into a single f32x4 (lane 3 ignored) so the cbrt can SIMD.
    f32x4 lms = f32x4{0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f} * c.r +
                f32x4{0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f} * c.g +
                f32x4{0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f} * c.b;
    return lms_cbrt_to_oklab(fast_cbrt4(lms));
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
            t[0][idx] = f32x4{0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f} * linear;
            t[1][idx] = f32x4{0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f} * linear;
            t[2][idx] = f32x4{0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f} * linear;
        }
        return t;
    }();
    return lut;
}
}  // namespace detail

// LMS (as f32x4, lane 3 unused) for an 8-bit sRGB color. Splits per channel
// so callers with one varying channel can cache the other two.
[[gnu::always_inline]]
inline f32x4 srgb8_to_lms(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    auto& t = detail::srgb_lms_lut();
    return t[0][r] + t[1][g] + t[2][b];
}

// Convert cbrt(LMS) to OKLab (final matrix). Shared between variants.
[[gnu::always_inline]]
inline OKLab lms_cbrt_to_oklab(f32x4 lms_) noexcept {
    return {
        fma_dot3(0.2104542553f, lms_[0], 0.7936177850f, lms_[1], -0.0040720468f, lms_[2]),
        fma_dot3(1.9779984951f, lms_[0], -2.4285922050f, lms_[1], 0.4505937099f, lms_[2]),
        fma_dot3(0.0259040371f, lms_[0], 0.7827717662f, lms_[1], -0.8086757660f, lms_[2]),
    };
}

[[gnu::always_inline]]
inline OKLab srgb8_to_oklab(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return lms_cbrt_to_oklab(fast_cbrt4(srgb8_to_lms(r, g, b)));
}

// 4-candidate batched cbrt + OKLab. Inputs are packed by channel:
//   L = (L_0, L_1, L_2, L_3), same for M and S.
// Three `fast_cbrt4` calls cover all 12 channel values with every lane
// productive — vs four single-candidate calls that each waste lane 3.
// Saves 25 % of cbrt work in the HAM hot loop's modify-{R,G,B} batches.
struct OKLabBatch4 {
    OKLab labs[4];
};
[[gnu::always_inline]]
inline OKLabBatch4 lms4_to_oklab4(f32x4 L, f32x4 M, f32x4 S) noexcept {
    f32x4 cL = fast_cbrt4(L);
    f32x4 cM = fast_cbrt4(M);
    f32x4 cS = fast_cbrt4(S);
    OKLabBatch4 out;
    for (int i = 0; i < 4; ++i) {
        out.labs[i] = {
            fma_dot3(0.2104542553f, cL[i], 0.7936177850f, cM[i], -0.0040720468f, cS[i]),
            fma_dot3(1.9779984951f, cL[i], -2.4285922050f, cM[i], 0.4505937099f, cS[i]),
            fma_dot3(0.0259040371f, cL[i], 0.7827717662f, cM[i], -0.8086757660f, cS[i]),
        };
    }
    return out;
}

inline Color3f oklab_to_linear(OKLab lab) noexcept {
    // Inverse OKLab matrix: L + c1*a + c2*b shape, fold via nested fma.
    float l_ = PNG2AMIGA_FMA(0.3963377774f, lab.a, PNG2AMIGA_FMA(0.2158037573f, lab.b, lab.L));
    float m_ = PNG2AMIGA_FMA(-0.1055613458f, lab.a, PNG2AMIGA_FMA(-0.0638541728f, lab.b, lab.L));
    float s_ = PNG2AMIGA_FMA(-0.0894841775f, lab.a, PNG2AMIGA_FMA(-1.2914855480f, lab.b, lab.L));

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    return {
        fma_dot3(4.0767416621f, l, -3.3077115913f, m, 0.2309699292f, s),
        fma_dot3(-1.2684380046f, l, 2.6097574011f, m, -0.3413193965f, s),
        fma_dot3(-0.0041960863f, l, -0.7034186147f, m, 1.7076147010f, s),
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
inline float WEIGHT_A = 1.05f;  // red-green axis
inline float WEIGHT_B = 1.0f;   // blue-yellow axis

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
    if (n == 0 || original.size() < n || rendered.size() < n) return 0.0f;

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
                acc.r /= wsum;
                acc.g /= wsum;
                acc.b /= wsum;
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
                acc.r /= wsum;
                acc.g /= wsum;
                acc.b /= wsum;
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

}  // namespace png2amiga::color_space
