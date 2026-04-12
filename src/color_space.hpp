#pragma once

#include "types.hpp"
#include <algorithm>
#include <array>
#include <bit>
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
// constexpr on GCC (constexpr pow), const on clang/emscripten (runtime init)
#if defined(__GNUC__) && !defined(__clang__)
#define PNG2AMIGA_LUT_CONSTEXPR constexpr
#else
#define PNG2AMIGA_LUT_CONSTEXPR
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
constexpr Color3f srgb_hex_to_linear(std::uint32_t hex) noexcept {
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

// Fast cube root via IEEE 754 bit hack + one Halley iteration.
// ~6-7 significant digits — full float precision recovery.
inline float fast_cbrt(float x) noexcept {
    if (x == 0.0f) return 0.0f;
    float sign = 1.0f;
    if (x < 0.0f) { sign = -1.0f; x = -x; }

    auto i = std::bit_cast<std::uint32_t>(x);
    i = i / 3 + 0x2a508bfe;
    float y = std::bit_cast<float>(i);

    // Halley iteration (cubically convergent)
    float y3 = y * y * y;
    y *= (y3 + 2.0f * x) / (2.0f * y3 + x);

    return sign * y;
}

inline OKLab linear_to_oklab(Color3f c) noexcept {
    float l = 0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b;
    float m = 0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b;
    float s = 0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b;

    float l_ = fast_cbrt(l);
    float m_ = fast_cbrt(m);
    float s_ = fast_cbrt(s);

    return {
        0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
    };
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
