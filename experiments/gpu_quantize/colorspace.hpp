// Self-contained sRGB <-> linear <-> OKLab conversion for the GPU
// quantization experiment. Mirrors src/color_space.hpp but stripped
// of the SIMD-cbrt and GCC-only constexpr LUT paths so the file
// compiles cleanly under Apple clang in this experimental build.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace expq {

struct RGB { float r, g, b; };
struct OKLab { float L, a, b; };

inline float srgb_to_linear(float s) noexcept {
    return s <= 0.04045f ? s / 12.92f
                          : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

inline float linear_to_srgb(float l) noexcept {
    return l <= 0.0031308f ? l * 12.92f
                            : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

inline const std::array<float, 256>& srgb_lut() noexcept {
    static const auto lut = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i)
            a[std::size_t(i)] = srgb_to_linear(float(i) / 255.0f);
        return a;
    }();
    return lut;
}

inline RGB srgb_u8_to_linear(std::uint8_t r, std::uint8_t g,
                              std::uint8_t b) noexcept {
    auto& lut = srgb_lut();
    return {lut[r], lut[g], lut[b]};
}

inline std::uint8_t linear_to_srgb_u8(float l) noexcept {
    float s = linear_to_srgb(std::max(0.0f, std::min(1.0f, l)));
    int v = int(std::lround(s * 255.0f));
    return std::uint8_t(std::max(0, std::min(255, v)));
}

inline OKLab linear_to_oklab(RGB c) noexcept {
    float l = 0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b;
    float m = 0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b;
    float s = 0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b;

    l = std::cbrt(l);
    m = std::cbrt(m);
    s = std::cbrt(s);

    return {
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
    };
}

inline RGB oklab_to_linear(OKLab lab) noexcept {
    float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    return {
         4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    };
}

inline float dist_sq(OKLab a, OKLab b) noexcept {
    float dL = a.L - b.L;
    float da = a.a - b.a;
    float db = a.b - b.b;
    return dL * dL + da * da + db * db;
}

} // namespace expq
