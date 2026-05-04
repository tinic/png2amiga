#pragma once

#include "types.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

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

// 4-lane cube root wrapper around std::cbrt (lane 3 is padding).
// We swept all 16.7M 8-bit sRGB triples and confirmed the previous
// IEEE-754 + 1-Halley approximation produced a different OCS code from
// std::cbrt for ~0.028% of inputs, with worst-case drift of 4 nibbles in
// a single channel; the second-Halley variant got us to 0.002% but still
// not bit-exact. Full conversion overhead from std::cbrt over the
// approximation is ~2% (the OCS search dominates), so we use the exact
// path everywhere.
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

[[gnu::always_inline]]
inline f32x4 fast_cbrt4(f32x4 x) noexcept {
    return f32x4{std::cbrt(x[0]), std::cbrt(x[1]), std::cbrt(x[2]), 0.0f};
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
    return dL * dL + da * da + db * db;
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
        sum_sq += dr * dr + dg * dg + db_ * db_;
    }
    if (sum_sq < 1e-12) return std::numeric_limits<float>::infinity();
    double mse = sum_sq / (static_cast<double>(n) * 3.0);
    return static_cast<float>(10.0 * std::log10(255.0 * 255.0 / mse));
}

} // namespace png2amiga::color_space
