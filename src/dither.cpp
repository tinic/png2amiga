#include "dither.hpp"
#include "color_space.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace png2amiga::dither {

namespace {

// ===========================================================================
// Ordered dither matrices (normalized to [-0.5, 0.5])
// ===========================================================================

constexpr auto make_bayer2x2() noexcept {
    constexpr std::array<std::array<int, 2>, 2> raw = {{
        {{0, 2}},
        {{3, 1}},
    }};
    std::array<std::array<float, 2>, 2> m{};
    for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 4.0f - 0.5f;
    return m;
}

constexpr auto make_bayer4x4() noexcept {
    constexpr std::array<std::array<int, 4>, 4> raw = {{
        {{ 0,  8,  2, 10}},
        {{12,  4, 14,  6}},
        {{ 3, 11,  1,  9}},
        {{15,  7, 13,  5}},
    }};
    std::array<std::array<float, 4>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 16.0f - 0.5f;
    return m;
}

constexpr auto make_bayer8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{ 0, 32,  8, 40,  2, 34, 10, 42}},
        {{48, 16, 56, 24, 50, 18, 58, 26}},
        {{12, 44,  4, 36, 14, 46,  6, 38}},
        {{60, 28, 52, 20, 62, 30, 54, 22}},
        {{ 3, 35, 11, 43,  1, 33,  9, 41}},
        {{51, 19, 59, 27, 49, 17, 57, 25}},
        {{15, 47,  7, 39, 13, 45,  5, 37}},
        {{63, 31, 55, 23, 61, 29, 53, 21}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 64.0f - 0.5f;
    return m;
}

constexpr auto make_checker() noexcept {
    return std::array<std::array<float, 2>, 2>{{
        {{-0.25f,  0.25f}},
        {{ 0.25f, -0.25f}},
    }};
}

constexpr auto make_h2x4() noexcept {
    // 2x4 Bayer — at 2:1 pixel ratio, tiles as perceptually square 4x4 block
    constexpr std::array<std::array<int, 2>, 4> raw = {{
        {{0, 4}},
        {{6, 2}},
        {{1, 5}},
        {{7, 3}},
    }};
    std::array<std::array<float, 2>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto make_clustered_dot() noexcept {
    // 4x4 clustered dot — pixels form round dots at 2:1 pixel ratio,
    // cluster grows from center outward
    constexpr std::array<std::array<int, 4>, 4> raw = {{
        {{12,  5,  6, 13}},
        {{ 4,  0,  1,  7}},
        {{11,  3,  2,  8}},
        {{15, 10,  9, 14}},
    }};
    std::array<std::array<float, 4>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 16.0f - 0.5f;
    return m;
}

constexpr auto make_line2() noexcept {
    // 1x2 alternating rows
    return std::array<std::array<float, 1>, 2>{{
        {{-0.25f}},
        {{ 0.25f}},
    }};
}

constexpr auto make_vline2() noexcept {
    // 2x1 alternating columns (vertical lines — transpose of line2)
    return std::array<std::array<float, 2>, 1>{{
        {{-0.25f, 0.25f}},
    }};
}

constexpr auto make_vline_checker() noexcept {
    // 2x2 column-biased: columns have large threshold difference,
    // rows get subtle offset — produces vertical lines with pixel variation
    // (transpose of line_checker)
    return std::array<std::array<float, 2>, 2>{{
        {{-0.35f,  0.15f}},
        {{-0.15f,  0.35f}},
    }};
}

constexpr auto make_vline4() noexcept {
    // 4x1 vertical gradient (transpose of line4)
    return std::array<std::array<float, 4>, 1>{{
        {{-0.375f, -0.125f, 0.125f, 0.375f}},
    }};
}

constexpr auto make_vline8() noexcept {
    // 8x1 finest vertical gradient (transpose of line8)
    constexpr std::array<int, 8> raw = {0, 4, 2, 6, 1, 5, 3, 7};
    std::array<std::array<float, 8>, 1> m{};
    for (std::size_t x = 0; x < 8; ++x)
        m[0][x] = (static_cast<float>(raw[x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto make_line_checker() noexcept {
    // 2x2 line-biased: rows have large threshold difference,
    // columns get subtle offset — produces horizontal lines with pixel variation
    return std::array<std::array<float, 2>, 2>{{
        {{-0.35f, -0.15f}},
        {{ 0.15f,  0.35f}},
    }};
}

constexpr auto make_line4() noexcept {
    // 1x4 smooth gradient
    return std::array<std::array<float, 1>, 4>{{
        {{-0.375f}},
        {{-0.125f}},
        {{ 0.125f}},
        {{ 0.375f}},
    }};
}

constexpr auto make_line8() noexcept {
    // 1x8 finest horizontal gradient
    constexpr std::array<int, 8> raw = {0, 4, 2, 6, 1, 5, 3, 7};
    std::array<std::array<float, 1>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        m[y][0] = (static_cast<float>(raw[y]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto bayer2 = make_bayer2x2();
constexpr auto bayer4 = make_bayer4x4();
constexpr auto bayer8 = make_bayer8x8();
constexpr auto checker_mat = make_checker();
constexpr auto h2x4_mat = make_h2x4();

// V 4x2: vertical bias (4 wide, 2 tall) — good for hires 1:2 tall pixels
constexpr auto make_v4x2() noexcept {
    return std::array<std::array<float, 4>, 2>{{
        {{-0.35f, -0.15f,  0.15f,  0.35f}},
        {{ 0.25f,  0.05f, -0.05f, -0.25f}},
    }};
}

// Bayer 4x2: standard Bayer ordering in 4 wide x 2 tall
constexpr auto make_bayer4x2() noexcept {
    constexpr std::array<std::array<int, 4>, 2> raw = {{
        {{0, 4, 1, 5}},
        {{6, 2, 7, 3}},
    }};
    std::array<std::array<float, 4>, 2> m{};
    for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

// Bayer 2x4: standard Bayer ordering in 2 wide x 4 tall
constexpr auto make_bayer2x4() noexcept {
    constexpr std::array<std::array<int, 2>, 4> raw = {{
        {{0, 6}},
        {{4, 2}},
        {{1, 7}},
        {{5, 3}},
    }};
    std::array<std::array<float, 2>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto v4x2_mat = make_v4x2();
constexpr auto bayer4x2_mat = make_bayer4x2();
constexpr auto bayer2x4_mat = make_bayer2x4();
constexpr auto clustered_mat = make_clustered_dot();
constexpr auto line2_mat = make_line2();
constexpr auto vline2_mat = make_vline2();
constexpr auto vline_checker_mat = make_vline_checker();
constexpr auto vline4_mat = make_vline4();
constexpr auto vline8_mat = make_vline8();
constexpr auto line_checker_mat = make_line_checker();
constexpr auto line4_mat = make_line4();
constexpr auto line8_mat = make_line8();

// 45-degree halftone 8x8 (newspaper/print look, 32 gray levels)
constexpr auto make_halftone8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{13,  7,  8, 14, 17, 21, 22, 18}},
        {{ 6,  1,  3,  9, 28, 31, 29, 23}},
        {{ 5,  2,  4, 10, 27, 32, 30, 24}},
        {{16, 12, 11, 15, 20, 26, 25, 19}},
        {{17, 21, 22, 18, 13,  7,  8, 14}},
        {{28, 31, 29, 23,  6,  1,  3,  9}},
        {{27, 32, 30, 24,  5,  2,  4, 10}},
        {{20, 26, 25, 19, 16, 12, 11, 15}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) - 0.5f) / 32.0f - 0.5f;
    return m;
}

// Spiral 5x5 (organic dot growth outward from center)
constexpr auto make_spiral5x5() noexcept {
    constexpr std::array<std::array<int, 5>, 5> raw = {{
        {{20, 21, 22, 23, 24}},
        {{19,  6,  7,  8,  9}},
        {{18,  5,  0,  1, 10}},
        {{17,  4,  3,  2, 11}},
        {{16, 15, 14, 13, 12}},
    }};
    std::array<std::array<float, 5>, 5> m{};
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 5; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 25.0f - 0.5f;
    return m;
}

// Non-rectangular hexagonal tiling 8x8 (breaks Bayer cross-hatch)
constexpr auto make_hex8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{3, 4, 2, 7, 1, 6, 0, 5}},
        {{6, 0, 5, 3, 4, 2, 7, 1}},
        {{2, 7, 1, 6, 0, 5, 3, 4}},
        {{5, 3, 4, 2, 7, 1, 6, 0}},
        {{1, 6, 0, 5, 3, 4, 2, 7}},
        {{4, 2, 7, 1, 6, 0, 5, 3}},
        {{0, 5, 3, 4, 2, 7, 1, 6}},
        {{7, 1, 6, 0, 5, 3, 4, 2}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

// Blue noise 64x64 — generated from Interleaved Gradient Noise formula
// fract(52.9829189 * fract(0.06711056*x + 0.00583715*y))
// Rank-ordered to produce 4096 threshold levels. No visible pattern.
constexpr auto make_blue_noise64() noexcept {
    std::array<std::array<float, 64>, 64> m{};
    // Generate IGN values and use directly as thresholds
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            auto fx = static_cast<float>(x);
            auto fy = static_cast<float>(y);
            float v = 52.9829189f * (0.06711056f * fx + 0.00583715f * fy);
            v = v - static_cast<float>(static_cast<int>(v)); // fract
            if (v < 0.0f) v += 1.0f;
            v = 52.9829189f * v;
            v = v - static_cast<float>(static_cast<int>(v)); // fract
            if (v < 0.0f) v += 1.0f;
            m[y][x] = v - 0.5f;  // center around 0
        }
    }
    return m;
}

// Diagonal clustered dot 8x8 (newspaper halftone, 64 levels)
constexpr auto make_diagonal8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{24, 10, 12, 26, 35, 47, 49, 37}},
        {{ 8,  0,  2, 14, 45, 59, 61, 51}},
        {{22,  6,  4, 16, 43, 57, 63, 53}},
        {{30, 20, 18, 28, 33, 41, 55, 39}},
        {{34, 46, 48, 36, 25, 11, 13, 27}},
        {{44, 58, 60, 50,  9,  1,  3, 15}},
        {{42, 56, 62, 52, 23,  7,  5, 17}},
        {{32, 40, 54, 38, 31, 21, 19, 29}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 64.0f - 0.5f;
    return m;
}

// Non-rectangular slanted square 5x5
constexpr auto make_hex5x5() noexcept {
    constexpr std::array<std::array<int, 5>, 5> raw = {{
        {{4, 3, 0, 1, 2}},
        {{0, 1, 2, 4, 3}},
        {{2, 4, 3, 0, 1}},
        {{3, 0, 1, 2, 4}},
        {{1, 2, 4, 3, 0}},
    }};
    std::array<std::array<float, 5>, 5> m{};
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 5; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 5.0f - 0.5f;
    return m;
}

constexpr auto halftone8x8_mat = make_halftone8x8();
constexpr auto diagonal8x8_mat = make_diagonal8x8();
constexpr auto spiral5x5_mat = make_spiral5x5();
constexpr auto hex8x8_mat = make_hex8x8();
constexpr auto hex5x5_mat = make_hex5x5();
constexpr auto blue_noise_mat = make_blue_noise64();

// ===========================================================================
// OKLab arithmetic helpers
// ===========================================================================

using OKLab = color_space::OKLab;

constexpr OKLab oklab_add(OKLab a, OKLab b) noexcept {
    return {a.L + b.L, a.a + b.a, a.b + b.b};
}

constexpr OKLab oklab_sub(OKLab a, OKLab b) noexcept {
    return {a.L - b.L, a.a - b.a, a.b - b.b};
}

constexpr OKLab oklab_scale(OKLab v, float s) noexcept {
    return {v.L * s, v.a * s, v.b * s};
}

constexpr OKLab oklab_clamp(OKLab e, float max_mag) noexcept {
    return {
        std::clamp(e.L, -max_mag, max_mag),
        std::clamp(e.a, -max_mag, max_mag),
        std::clamp(e.b, -max_mag, max_mag),
    };
}

// ===========================================================================
// Precompute palette in OKLab space
// ===========================================================================

std::vector<OKLab> precompute_palette_lab(std::span<const Color3f> palette) {
    std::vector<OKLab> lab(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i) {
        lab[i] = color_space::linear_to_oklab(palette[i]);
    }
    return lab;
}

// Find nearest palette color in OKLab space.
// Returns palette index and the OKLab value of the chosen color.
struct NearestResult {
    std::size_t index;
    OKLab color_lab;
    float dist_sq;
};

NearestResult find_nearest_oklab(OKLab pixel_lab,
                                 std::span<const OKLab> palette_lab) {
    float best_dist = std::numeric_limits<float>::max();
    std::size_t best_idx = 0;
    OKLab best_lab{};

    for (std::size_t i = 0; i < palette_lab.size(); ++i) {
        auto& cl = palette_lab[i];
        float dL = pixel_lab.L - cl.L;
        float da = pixel_lab.a - cl.a;
        float db = pixel_lab.b - cl.b;
        float dist = dL * dL + da * da + db * db;

        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
            best_lab = cl;
        }
    }

    return {best_idx, best_lab, best_dist};
}

// ===========================================================================
// Ordered dithering
// ===========================================================================

template <std::size_t W, std::size_t H>
DitherResult apply_ordered(const Image& image,
                           const std::array<std::array<float, W>, H>& matrix,
                           std::span<const OKLab> palette_lab,
                           float strength) {
    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto pixel_lab = color_space::linear_to_oklab(image[x, y]);

            float threshold = matrix[y % H][x % W];

            // Apply threshold bias in OKLab space.
            // L channel gets larger bias (luminance is most perceptually
            // significant). Chroma channels get subtle bias.
            pixel_lab.L += threshold * strength * 0.15f;
            pixel_lab.a += threshold * strength * 0.03f;
            pixel_lab.b += threshold * strength * 0.03f;

            auto [idx, chosen, dist_sq] =
                find_nearest_oklab(pixel_lab, palette_lab);
            result.indices[y * w + x] = static_cast<std::uint8_t>(idx);
            result.total_error += dist_sq;
        }
    }

    return result;
}

// ===========================================================================
// Error diffusion
// ===========================================================================

// DiffusionEntry is declared publicly in dither.hpp.

// Floyd-Steinberg
//        * 7/16
//  3/16 5/16 1/16
constexpr std::array floyd_steinberg_kernel = {
    DiffusionEntry{ 1, 0, 7.0f / 16.0f},
    DiffusionEntry{-1, 1, 3.0f / 16.0f},
    DiffusionEntry{ 0, 1, 5.0f / 16.0f},
    DiffusionEntry{ 1, 1, 1.0f / 16.0f},
};

// Atkinson: distributes only 75% of error (6/8), cleaner for limited palettes
//      * 1/8 1/8
//  1/8 1/8 1/8
//      1/8
constexpr std::array atkinson_kernel = {
    DiffusionEntry{ 1, 0, 1.0f / 8.0f},
    DiffusionEntry{ 2, 0, 1.0f / 8.0f},
    DiffusionEntry{-1, 1, 1.0f / 8.0f},
    DiffusionEntry{ 0, 1, 1.0f / 8.0f},
    DiffusionEntry{ 1, 1, 1.0f / 8.0f},
    DiffusionEntry{ 0, 2, 1.0f / 8.0f},
};

// Sierra Lite
//    * 2/4
//  1/4 1/4
constexpr std::array sierra_lite_kernel = {
    DiffusionEntry{ 1, 0, 2.0f / 4.0f},
    DiffusionEntry{-1, 1, 1.0f / 4.0f},
    DiffusionEntry{ 0, 1, 1.0f / 4.0f},
};

// Stucki: wider kernel, smooth gradients
//              *   8/42  4/42
//  2/42  4/42  8/42  4/42  2/42
//  1/42  2/42  4/42  2/42  1/42
constexpr std::array stucki_kernel = {
    DiffusionEntry{ 1, 0, 8.0f / 42.0f},
    DiffusionEntry{ 2, 0, 4.0f / 42.0f},
    DiffusionEntry{-2, 1, 2.0f / 42.0f},
    DiffusionEntry{-1, 1, 4.0f / 42.0f},
    DiffusionEntry{ 0, 1, 8.0f / 42.0f},
    DiffusionEntry{ 1, 1, 4.0f / 42.0f},
    DiffusionEntry{ 2, 1, 2.0f / 42.0f},
    DiffusionEntry{-2, 2, 1.0f / 42.0f},
    DiffusionEntry{-1, 2, 2.0f / 42.0f},
    DiffusionEntry{ 0, 2, 4.0f / 42.0f},
    DiffusionEntry{ 1, 2, 2.0f / 42.0f},
    DiffusionEntry{ 2, 2, 1.0f / 42.0f},
};

// Jarvis-Judice-Ninke: wide 5x3 kernel
//              *   7/48  5/48
//  3/48  5/48  7/48  5/48  3/48
//  1/48  3/48  5/48  3/48  1/48
constexpr std::array jarvis_kernel = {
    DiffusionEntry{ 1, 0, 7.0f / 48.0f},
    DiffusionEntry{ 2, 0, 5.0f / 48.0f},
    DiffusionEntry{-2, 1, 3.0f / 48.0f},
    DiffusionEntry{-1, 1, 5.0f / 48.0f},
    DiffusionEntry{ 0, 1, 7.0f / 48.0f},
    DiffusionEntry{ 1, 1, 5.0f / 48.0f},
    DiffusionEntry{ 2, 1, 3.0f / 48.0f},
    DiffusionEntry{-2, 2, 1.0f / 48.0f},
    DiffusionEntry{-1, 2, 3.0f / 48.0f},
    DiffusionEntry{ 0, 2, 5.0f / 48.0f},
    DiffusionEntry{ 1, 2, 3.0f / 48.0f},
    DiffusionEntry{ 2, 2, 1.0f / 48.0f},
};

DitherResult apply_error_diffusion(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val,
    bool serpentine,
    std::span<const DiffusionEntry> kernel) {

    auto w = image.width();
    auto h = image.height();

    // Adapt error clamp to palette granularity: fewer colors have larger
    // quantization errors — tighter clamping prevents overshooting past
    // sparse palette entries.  32 colors keeps the caller's default.
    auto num_colors = palette_lab.size();
    float ec = error_clamp_val;
    if (num_colors > 0 && num_colors <= 64) {
        ec = error_clamp_val *
             std::sqrt(static_cast<float>(num_colors) / 32.0f);
    }

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    // Precompute image in OKLab
    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);
        }
    }

    // Error buffer (OKLab)
    std::vector<OKLab> error_buf(w * h);

    for (std::size_t y = 0; y < h; ++y) {
        bool reverse = serpentine && (y % 2 == 1);

        for (std::size_t step = 0; step < w; ++step) {
            std::size_t x = reverse ? (w - 1 - step) : step;
            auto buf_idx = y * w + x;

            // Add accumulated error to the original pixel
            auto clamped_error = oklab_clamp(error_buf[buf_idx], ec);
            auto adjusted = oklab_add(image_lab[buf_idx], clamped_error);

            // Find nearest palette color
            auto [idx, chosen_lab, dist_sq] =
                find_nearest_oklab(adjusted, palette_lab);
            result.indices[buf_idx] = static_cast<std::uint8_t>(idx);
            result.total_error += dist_sq;

            // Compute quantization error and distribute
            auto quant_error =
                oklab_scale(oklab_sub(adjusted, chosen_lab), strength);

            for (auto& [kdx, kdy, weight] : kernel) {
                int actual_dx = reverse ? -kdx : kdx;
                auto nx = static_cast<int>(x) + actual_dx;
                auto ny = static_cast<int>(y) + kdy;

                if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                    ny >= 0 && static_cast<std::size_t>(ny) < h) {
                    auto nidx = static_cast<std::size_t>(ny) * w +
                                static_cast<std::size_t>(nx);
                    error_buf[nidx] = oklab_clamp(
                        oklab_add(error_buf[nidx],
                                  oklab_scale(quant_error, weight)),
                        ec);
                }
            }
        }
    }

    return result;
}

// ===========================================================================
// Ostromoukhov variable-coefficient error diffusion.
// Uses 3 coefficients that vary with the "threshold" level — the fractional
// position between the two nearest palette colors.  The coefficients are
// from Ostromoukhov's 2001 paper, simplified to a smooth interpolation
// between three regimes (near-black, midtone, near-white).
// ===========================================================================

DitherResult apply_ostromoukhov(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val,
    bool serpentine) {

    auto w = image.width();
    auto h = image.height();

    auto num_colors = palette_lab.size();
    float ec = error_clamp_val;
    if (num_colors > 0 && num_colors <= 64) {
        ec = error_clamp_val *
             std::sqrt(static_cast<float>(num_colors) / 32.0f);
    }

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    std::vector<OKLab> error_buf(w * h);

    for (std::size_t y = 0; y < h; ++y) {
        bool reverse = serpentine && (y % 2 == 1);
        for (std::size_t step = 0; step < w; ++step) {
            std::size_t x = reverse ? (w - 1 - step) : step;
            auto buf_idx = y * w + x;

            auto clamped_error = oklab_clamp(error_buf[buf_idx], ec);
            auto adjusted = oklab_add(image_lab[buf_idx], clamped_error);

            // Find nearest AND second-nearest to compute threshold level
            float best_d = std::numeric_limits<float>::max();
            float second_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            OKLab best_lab{};
            for (std::size_t k = 0; k < palette_lab.size(); ++k) {
                float dL = adjusted.L - palette_lab[k].L;
                float da = adjusted.a - palette_lab[k].a;
                float db = adjusted.b - palette_lab[k].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) {
                    second_d = best_d;
                    best_d = d;
                    best_k = k;
                    best_lab = palette_lab[k];
                } else if (d < second_d) {
                    second_d = d;
                }
            }
            result.indices[buf_idx] = static_cast<std::uint8_t>(best_k);
            result.total_error += best_d;

            // Threshold: 0 = pixel is exactly on nearest, 1 = equidistant
            float threshold = 0.0f;
            if (second_d > 1e-12f) {
                float sqrt_best = std::sqrt(best_d);
                float sqrt_second = std::sqrt(second_d);
                threshold = sqrt_best / (sqrt_best + sqrt_second);
            }

            // Variable coefficients: at threshold=0 (near palette color),
            // distribute less error (pixel is well-served). At threshold=0.5
            // (equidistant), distribute more aggressively.
            // F-S base: right=7/16, bottom-left=3/16, bottom=5/16, bottom-right=1/16
            // Scale by threshold: more aggressive diffusion for uncertain pixels
            float scale = 0.6f + 0.8f * threshold;  // 0.6 to 1.4
            float w0 = (7.0f / 16.0f) * scale;
            float w1 = (3.0f / 16.0f) * scale;
            float w2 = (5.0f / 16.0f) * scale;
            float w3 = (1.0f / 16.0f) * scale;

            auto quant_error =
                oklab_scale(oklab_sub(adjusted, best_lab), strength);

            struct { int dx, dy; float wt; } entries[] = {
                {1, 0, w0}, {-1, 1, w1}, {0, 1, w2}, {1, 1, w3}};
            for (auto& [kdx, kdy, wt] : entries) {
                int actual_dx = reverse ? -kdx : kdx;
                auto nx = static_cast<int>(x) + actual_dx;
                auto ny = static_cast<int>(y) + kdy;
                if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                    ny >= 0 && static_cast<std::size_t>(ny) < h) {
                    auto nidx = static_cast<std::size_t>(ny) * w +
                                static_cast<std::size_t>(nx);
                    error_buf[nidx] = oklab_clamp(
                        oklab_add(error_buf[nidx],
                                  oklab_scale(quant_error, wt)),
                        ec);
                }
            }
        }
    }
    return result;
}

// ===========================================================================
// No-dither fallback (plain nearest-color mapping)
// ===========================================================================

DitherResult apply_none(const Image& image,
                        std::span<const OKLab> palette_lab) {
    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto pixel_lab = color_space::linear_to_oklab(image[x, y]);
            auto [idx, chosen, dist_sq] =
                find_nearest_oklab(pixel_lab, palette_lab);
            result.indices[y * w + x] = static_cast<std::uint8_t>(idx);
            result.total_error += dist_sq;
        }
    }

    return result;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

DitherResult apply(const Image& image,
                   std::span<const Color3f> palette,
                   const Settings& settings) {

    auto palette_lab = precompute_palette_lab(palette);
    std::span<const OKLab> pal_span{palette_lab};

    switch (settings.method) {
    case Method::none:
        return apply_none(image, pal_span);

    // Ordered dithering
    case Method::bayer2x2:
        return apply_ordered(image, bayer2, pal_span,
                             settings.strength);
    case Method::bayer4x4:
        return apply_ordered(image, bayer4, pal_span,
                             settings.strength);
    case Method::bayer8x8:
        return apply_ordered(image, bayer8, pal_span,
                             settings.strength);
    case Method::checker:
        return apply_ordered(image, checker_mat, pal_span,
                             settings.strength);
    case Method::h2x4:
        return apply_ordered(image, h2x4_mat, pal_span,
                             settings.strength);
    case Method::v4x2:
        return apply_ordered(image, v4x2_mat, pal_span,
                             settings.strength);
    case Method::bayer4x2:
        return apply_ordered(image, bayer4x2_mat, pal_span,
                             settings.strength);
    case Method::bayer2x4:
        return apply_ordered(image, bayer2x4_mat, pal_span,
                             settings.strength);
    case Method::clustered_dot:
        return apply_ordered(image, clustered_mat, pal_span,
                             settings.strength);
    case Method::line2:
        return apply_ordered(image, line2_mat, pal_span,
                             settings.strength);
    case Method::vline2:
        return apply_ordered(image, vline2_mat, pal_span,
                             settings.strength);
    case Method::vline_checker:
        return apply_ordered(image, vline_checker_mat, pal_span,
                             settings.strength);
    case Method::vline4:
        return apply_ordered(image, vline4_mat, pal_span,
                             settings.strength);
    case Method::vline8:
        return apply_ordered(image, vline8_mat, pal_span,
                             settings.strength);
    case Method::line_checker:
        return apply_ordered(image, line_checker_mat, pal_span,
                             settings.strength);
    case Method::line4:
        return apply_ordered(image, line4_mat, pal_span,
                             settings.strength);
    case Method::line8:
        return apply_ordered(image, line8_mat, pal_span,
                             settings.strength);
    case Method::halftone8x8:
        return apply_ordered(image, halftone8x8_mat, pal_span,
                             settings.strength);
    case Method::diagonal8x8:
        return apply_ordered(image, diagonal8x8_mat, pal_span,
                             settings.strength);
    case Method::spiral5x5:
        return apply_ordered(image, spiral5x5_mat, pal_span,
                             settings.strength);
    case Method::hex8x8:
        return apply_ordered(image, hex8x8_mat, pal_span,
                             settings.strength);
    case Method::hex5x5:
        return apply_ordered(image, hex5x5_mat, pal_span,
                             settings.strength);
    case Method::blue_noise:
        return apply_ordered(image, blue_noise_mat, pal_span,
                             settings.strength);
    case Method::ign:
    case Method::white_noise:
    case Method::r2_sequence:
    case Method::crosshatch:
    case Method::radial:
    case Method::value_noise: {
        // Analytical threshold methods — compute per-pixel, no matrix
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        auto method = settings.method;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto pixel_lab = color_space::linear_to_oklab(image[x, y]);
                float thr = ordered_threshold(method, x, y);
                pixel_lab.L += thr * settings.strength * 0.15f;
                pixel_lab.a += thr * settings.strength * 0.03f;
                pixel_lab.b += thr * settings.strength * 0.03f;
                auto [idx, chosen, dist_sq] =
                    find_nearest_oklab(pixel_lab, pal_span);
                r.indices[y * w + x] = static_cast<std::uint8_t>(idx);
                r.total_error += dist_sq;
            }
        }
        return r;
    }

    // Error diffusion
    case Method::floyd_steinberg:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, floyd_steinberg_kernel);
    case Method::atkinson:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, atkinson_kernel);
    case Method::sierra_lite:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, sierra_lite_kernel);
    case Method::stucki:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, stucki_kernel);
    case Method::jarvis:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, jarvis_kernel);

    case Method::ostromoukhov:
        return apply_ostromoukhov(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine);
    }

    return apply_none(image, pal_span);
}

std::span<const DiffusionEntry> error_diffusion_kernel(Method method) {
    switch (method) {
    case Method::floyd_steinberg: return floyd_steinberg_kernel;
    case Method::atkinson:        return atkinson_kernel;
    case Method::sierra_lite:     return sierra_lite_kernel;
    case Method::stucki:          return stucki_kernel;
    case Method::jarvis:          return jarvis_kernel;
    default:                      return {};
    }
}

float ordered_threshold(Method method, std::size_t x, std::size_t y) {
    switch (method) {
    case Method::bayer2x2:      return bayer2[y % 2][x % 2];
    case Method::bayer4x4:      return bayer4[y % 4][x % 4];
    case Method::bayer8x8:      return bayer8[y % 8][x % 8];
    case Method::checker:       return checker_mat[y % 2][x % 2];
    case Method::h2x4:          return h2x4_mat[y % 4][x % 2];
    case Method::v4x2:          return v4x2_mat[y % 2][x % 4];
    case Method::bayer4x2:      return bayer4x2_mat[y % 2][x % 4];
    case Method::bayer2x4:      return bayer2x4_mat[y % 4][x % 2];
    case Method::clustered_dot: return clustered_mat[y % 4][x % 4];
    case Method::line2:         return line2_mat[y % 2][0];
    case Method::vline2:        return vline2_mat[0][x % 2];
    case Method::vline_checker: return vline_checker_mat[y % 2][x % 2];
    case Method::vline4:        return vline4_mat[0][x % 4];
    case Method::vline8:        return vline8_mat[0][x % 8];
    case Method::line_checker:  return line_checker_mat[y % 2][x % 2];
    case Method::line4:         return line4_mat[y % 4][0];
    case Method::line8:         return line8_mat[y % 8][0];
    case Method::halftone8x8:  return halftone8x8_mat[y % 8][x % 8];
    case Method::diagonal8x8:  return diagonal8x8_mat[y % 8][x % 8];
    case Method::spiral5x5:    return spiral5x5_mat[y % 5][x % 5];
    case Method::hex8x8:       return hex8x8_mat[y % 8][x % 8];
    case Method::hex5x5:       return hex5x5_mat[y % 5][x % 5];
    case Method::blue_noise:   return blue_noise_mat[y % 64][x % 64];
    case Method::ign: {
        // Jimenez 2014 Interleaved Gradient Noise
        auto fx = static_cast<float>(x);
        auto fy = static_cast<float>(y);
        float v = 52.9829189f * std::fmod(0.06711056f * fx + 0.00583715f * fy, 1.0f);
        return std::fmod(v, 1.0f) - 0.5f;
    }
    case Method::white_noise: {
        // Integer hash (Wang) for pure random noise
        auto seed = static_cast<std::uint32_t>(y * 65537 + x);
        seed = (seed ^ 61u) ^ (seed >> 16u);
        seed *= 9u;
        seed ^= seed >> 4u;
        seed *= 0x27d4eb2du;
        seed ^= seed >> 15u;
        return static_cast<float>(seed & 0xFFFFu) / 65536.0f - 0.5f;
    }
    case Method::r2_sequence: {
        // Martin Roberts R2: generalized golden ratio for 2D
        constexpr float phi1 = 0.7548776662f;  // 1/plastic number
        constexpr float phi2 = 0.5698402910f;  // 1/plastic^2
        float v = std::fmod(static_cast<float>(x) * phi1 +
                            static_cast<float>(y) * phi2 + 0.5f, 1.0f);
        return v - 0.5f;
    }
    case Method::crosshatch: {
        // Overlaid line patterns at 0°, 45°, 90°, 135° — threshold is
        // the minimum distance to any line, giving pen-and-ink texture
        auto fx = static_cast<float>(x);
        auto fy = static_cast<float>(y);
        float d0 = std::fmod(fy, 8.0f) / 8.0f;                          // horizontal
        float d1 = std::fmod(fx, 8.0f) / 8.0f;                          // vertical
        float d2 = std::fmod((fx + fy) * 0.7071f, 8.0f) / 8.0f;         // 45°
        float d3 = std::fmod((fx - fy + 512.0f) * 0.7071f, 8.0f) / 8.0f; // 135°
        // Triangle wave each to [0,1]
        d0 = 1.0f - std::abs(d0 * 2.0f - 1.0f);
        d1 = 1.0f - std::abs(d1 * 2.0f - 1.0f);
        d2 = 1.0f - std::abs(d2 * 2.0f - 1.0f);
        d3 = 1.0f - std::abs(d3 * 2.0f - 1.0f);
        // Progressive reveal: horizontal first, then +vertical, +diagonals
        float t = std::min({d0, d0 * 0.5f + d1 * 0.5f,
                            d0 * 0.3f + d1 * 0.3f + d2 * 0.4f,
                            d0 * 0.25f + d1 * 0.25f + d2 * 0.25f + d3 * 0.25f});
        return t - 0.5f;
    }
    case Method::radial: {
        // Concentric circles — threshold based on distance from center
        auto fx = static_cast<float>(x) - 160.0f;  // assume 320 wide
        auto fy = static_cast<float>(y) - 128.0f;  // assume 256 tall
        float r = std::sqrt(fx * fx + fy * fy);
        float v = std::fmod(r * 0.15f, 1.0f);
        return (1.0f - std::abs(v * 2.0f - 1.0f)) - 0.5f;
    }
    case Method::value_noise: {
        // 2D value noise: hash at integer grid + bilinear interpolation
        auto hash = [](int ix, int iy) -> float {
            auto s = static_cast<std::uint32_t>(ix * 374761393 + iy * 668265263 + 1013904223);
            s = (s ^ (s >> 13u)) * 1274126177u;
            return static_cast<float>(s & 0xFFFFu) / 65536.0f;
        };
        constexpr float scale = 0.125f;  // noise cell size = 8 pixels
        float fx = static_cast<float>(x) * scale;
        float fy = static_cast<float>(y) * scale;
        int ix = static_cast<int>(std::floor(fx));
        int iy = static_cast<int>(std::floor(fy));
        float tx = fx - static_cast<float>(ix);
        float ty = fy - static_cast<float>(iy);
        // Smoothstep for less blocky interpolation
        tx = tx * tx * (3.0f - 2.0f * tx);
        ty = ty * ty * (3.0f - 2.0f * ty);
        float v00 = hash(ix, iy), v10 = hash(ix + 1, iy);
        float v01 = hash(ix, iy + 1), v11 = hash(ix + 1, iy + 1);
        float v = v00 * (1 - tx) * (1 - ty) + v10 * tx * (1 - ty) +
                  v01 * (1 - tx) * ty + v11 * tx * ty;
        return v - 0.5f;
    }
    default:                    return 0.0f;
    }
}

} // namespace png2amiga::dither
