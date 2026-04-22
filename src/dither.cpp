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

// Find nearest + second-nearest palette entries.
struct NearestPair {
    std::size_t idxA;    // nearest
    std::size_t idxB;    // second-nearest
    float distA;          // OKLab ΔE² to nearest
    float distB;          // OKLab ΔE² to second-nearest
};

NearestPair find_nearest_pair(OKLab pixel,
                               std::span<const OKLab> palette_lab) noexcept {
    float best = std::numeric_limits<float>::max();
    float second = std::numeric_limits<float>::max();
    std::size_t bi = 0, si = 0;
    for (std::size_t k = 0; k < palette_lab.size(); ++k) {
        float dL = pixel.L - palette_lab[k].L;
        float da = pixel.a - palette_lab[k].a;
        float db = pixel.b - palette_lab[k].b;
        float d = dL * dL + da * da + db * db;
        if (d < best) {
            second = best; si = bi;
            best = d; bi = k;
        } else if (d < second) {
            second = d; si = k;
        }
    }
    return {bi, si, best, second};
}

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

    // Perturb each pixel's OKLab by the Bayer threshold before finding the
    // nearest palette color. Previous binary A-vs-B threshold only averaged
    // two nearest palette entries and produced visibly muted dithering on
    // small palettes; perturbation lets a pixel "reach" distant colors when
    // the bias shifts it significantly, giving the classic punchy ordered-
    // dither look that matches tools like GIMP / ImageMagick.
    //
    // Perturbation magnitude is tuned so strength=1 produces clearly visible
    // dithering without drowning the original colors.
    constexpr float kPerturbL = 0.12f;
    constexpr float kPerturbAB = 0.06f;

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto pixel_lab = color_space::linear_to_oklab(image[x, y]);

            // Bayer value in [-0.5, +0.5).
            float thr = matrix[y % H][x % W];
            pixel_lab.L += thr * strength * kPerturbL;
            pixel_lab.a += thr * strength * kPerturbAB;
            pixel_lab.b += thr * strength * kPerturbAB;

            std::size_t best = 0;
            float best_d = std::numeric_limits<float>::infinity();
            for (std::size_t i = 0; i < palette_lab.size(); ++i) {
                float dL = pixel_lab.L - palette_lab[i].L;
                float da = pixel_lab.a - palette_lab[i].a;
                float db = pixel_lab.b - palette_lab[i].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best = i; }
            }
            auto chosen = palette_lab[best];
            // Report error against the (unperturbed) target for metric consistency.
            auto orig = color_space::linear_to_oklab(image[x, y]);
            float dL = orig.L - chosen.L;
            float da = orig.a - chosen.a;
            float db = orig.b - chosen.b;
            result.indices[y * w + x] = static_cast<std::uint8_t>(best);
            result.total_error += dL * dL + da * da + db * db;
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

    float ec = error_clamp_val;

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

// ===========================================================================
// Gilbert-curve error diffusion
//
// Instead of scanning row-by-row, walk a generalized Hilbert (Červený's
// "Gilbert") space-filling curve that covers any W×H rectangle. At each
// step, quantize the current pixel and diffuse the error to the NEXT
// few pixels on the curve. Because the curve has O(1) spatial locality
// everywhere, the error propagates along a locally-coherent path and
// the directional hatching artifacts you see with raster-order FS on
// flat regions disappear.
//
// Curve generation follows Červený's recursive subdivision
// (https://github.com/jakubcerveny/gilbert).  We store the visit order
// as a flat list of (x, y) pairs, then walk it linearly for diffusion.
// ===========================================================================

namespace {

inline int sgn(int v) noexcept { return (v > 0) - (v < 0); }

// Recursive subdivision. (x, y) is the starting cell; a = (ax, ay) is the
// major axis vector; b = (bx, by) is the minor axis vector. Each emitted
// coordinate is pushed onto `out`.
void gilbert_recurse(int x, int y, int ax, int ay, int bx, int by,
                     std::vector<std::pair<int, int>>& out);

void gilbert_emit_row(int x, int y, int ax, int ay,
                      std::vector<std::pair<int, int>>& out) {
    int dax = sgn(ax), day = sgn(ay);
    int len = std::abs(ax) + std::abs(ay);
    for (int i = 0; i < len; ++i) {
        out.emplace_back(x, y);
        x += dax; y += day;
    }
}

void gilbert_recurse(int x, int y, int ax, int ay, int bx, int by,
                     std::vector<std::pair<int, int>>& out) {
    int w = std::abs(ax) + std::abs(ay);
    int h = std::abs(bx) + std::abs(by);

    if (h == 0) return;
    if (h == 1) { gilbert_emit_row(x, y, ax, ay, out); return; }
    if (w == 1) { gilbert_emit_row(x, y, bx, by, out); return; }

    int dax = sgn(ax), day = sgn(ay);
    int dbx = sgn(bx), dby = sgn(by);

    int ax2 = ax / 2, ay2 = ay / 2;
    int bx2 = bx / 2, by2 = by / 2;
    int w2 = std::abs(ax2) + std::abs(ay2);
    int h2 = std::abs(bx2) + std::abs(by2);

    if (2 * w > 3 * h) {
        // Prefer horizontal split
        if ((w2 % 2) && w > 2) { ax2 += dax; ay2 += day; }
        gilbert_recurse(x, y, ax2, ay2, bx, by, out);
        gilbert_recurse(x + ax2, y + ay2, ax - ax2, ay - ay2, bx, by, out);
    } else {
        // Prefer vertical split
        if ((h2 % 2) && h > 2) { bx2 += dbx; by2 += dby; }
        gilbert_recurse(x, y, bx2, by2, ax2, ay2, out);
        gilbert_recurse(x + bx2, y + by2, ax, ay,
                        bx - bx2, by - by2, out);
        gilbert_recurse(x + (ax - dax) + (bx2 - dbx),
                        y + (ay - day) + (by2 - dby),
                        -bx2, -by2, -(ax - ax2), -(ay - ay2), out);
    }
}

std::vector<std::pair<int, int>> gilbert_curve(int w, int h) {
    std::vector<std::pair<int, int>> out;
    out.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    if (w >= h) gilbert_recurse(0, 0, w, 0, 0, h, out);
    else        gilbert_recurse(0, 0, 0, h, w, 0, out);
    return out;
}

} // namespace

DitherResult apply_gilbert(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val) {

    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    auto curve = gilbert_curve(static_cast<int>(w), static_cast<int>(h));

    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    // Flatness map: 1.0 in smooth regions, 0.0 at edges. Error diffusion
    // on a space-filling curve tends to produce curve-following patterns
    // in flat regions (because the error doesn't have anywhere to "escape"
    // laterally). Adding blue-noise perturbation in flat regions breaks
    // those patterns without disturbing edges where accuracy matters most.
    //
    // Gradient is the sum of |L - neighbor.L| over 4-neighbors, in the
    // OKLab L channel only (banding is primarily a luminance phenomenon).
    // Threshold 0.03 (≈ 8 8-bit levels) — above this, the region is
    // treated as edge and gets no noise injection.
    std::vector<float> flatness(w * h, 1.0f);
    {
        constexpr float grad_thresh = 0.015f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float L = image_lab[y * w + x].L;
                float g = 0.0f;
                if (x > 0)     g += std::abs(L - image_lab[y * w + (x - 1)].L);
                if (x + 1 < w) g += std::abs(L - image_lab[y * w + (x + 1)].L);
                if (y > 0)     g += std::abs(L - image_lab[(y - 1) * w + x].L);
                if (y + 1 < h) g += std::abs(L - image_lab[(y + 1) * w + x].L);
                float f = 1.0f - std::min(g / grad_thresh, 1.0f);
                flatness[y * w + x] = f;
            }
        }
    }

    // Blue-noise amplitude scales with palette spacing. For a 32-color
    // palette the average OKLab nearest-neighbor distance is ~0.05; we
    // inject up to ±(amp * 0.5) of perturbation in flat regions, enough
    // to shift the nearest-color decision occasionally but not dominate.
    float noise_amp = 0.0f;
    if (palette_lab.size() >= 2) {
        // Rough average nearest-neighbor distance
        float total_nn = 0.0f;
        for (std::size_t i = 0; i < palette_lab.size(); ++i) {
            float best = std::numeric_limits<float>::max();
            for (std::size_t j = 0; j < palette_lab.size(); ++j) {
                if (i == j) continue;
                float dL = palette_lab[i].L - palette_lab[j].L;
                float da = palette_lab[i].a - palette_lab[j].a;
                float db = palette_lab[i].b - palette_lab[j].b;
                float d = dL * dL + da * da + db * db;
                if (d < best) best = d;
            }
            total_nn += std::sqrt(best);
        }
        noise_amp = 0.15f * (total_nn / static_cast<float>(palette_lab.size()));
    }

    // Error accumulator per pixel index
    std::vector<OKLab> error_buf(w * h);

    // Diffusion weights along the curve: most of the error goes to the
    // immediate next cell (spatially adjacent on the curve), with a small
    // tail to a few more. FS's pyramidal weights assume a 2D kernel shape,
    // which is a poor fit for 1D curve diffusion. Biasing toward the
    // next cell keeps the error local (where it belongs) and avoids
    // spreading error across distant, unrelated pixels when the curve
    // loops around.
    //
    // 12/16 + 3/16 + 1/16 = 16/16 = 1.0 (same total as FS).
    constexpr std::array<float, 3> weights = {
        12.0f / 16.0f, 3.0f / 16.0f, 1.0f / 16.0f,
    };

    float ec = error_clamp_val;

    for (std::size_t i = 0; i < curve.size(); ++i) {
        auto [x, y] = curve[i];
        auto idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x);

        auto clamped = oklab_clamp(error_buf[idx], ec);
        auto target = oklab_add(image_lab[idx], clamped);

        // Blue-noise perturbation in flat regions only. The noise is
        // applied to the NEAREST-COLOR LOOKUP target but NOT to the
        // propagated error — otherwise noise accumulates along the
        // curve instead of cancelling out. This is the key insight from
        // nQuant's GilbertCurve + BlueNoise combination.
        auto lookup = target;
        if (noise_amp > 0.0f && flatness[idx] > 0.0f) {
            auto ux = static_cast<std::size_t>(x) & 63;
            auto uy = static_cast<std::size_t>(y) & 63;
            float n = blue_noise_mat[uy][ux] * 2.0f * flatness[idx] * noise_amp;
            lookup.L += n;
            lookup.a += n;
            lookup.b += n;
        }

        // Find nearest palette entry (using perturbed lookup)
        float best_d = std::numeric_limits<float>::max();
        std::size_t best_k = 0;
        OKLab best_lab{};
        for (std::size_t k = 0; k < palette_lab.size(); ++k) {
            float dL = lookup.L - palette_lab[k].L;
            float da = lookup.a - palette_lab[k].a;
            float db = lookup.b - palette_lab[k].b;
            float d = dL * dL + da * da + db * db;
            if (d < best_d) { best_d = d; best_k = k; best_lab = palette_lab[k]; }
        }
        result.indices[idx] = static_cast<std::uint8_t>(best_k);
        result.total_error += best_d;

        // Error propagation uses the UNPERTURBED target, so blue noise
        // doesn't compound through the diffusion chain.
        auto err = oklab_sub(target, best_lab);
        auto scaled = oklab_scale(err, strength);

        // Distribute to next 4 cells along the curve
        for (std::size_t k = 0; k < weights.size() && i + 1 + k < curve.size(); ++k) {
            auto [nx, ny] = curve[i + 1 + k];
            auto nidx = static_cast<std::size_t>(ny) * w +
                        static_cast<std::size_t>(nx);
            auto w_e = oklab_scale(scaled, weights[k]);
            error_buf[nidx] = oklab_clamp(oklab_add(error_buf[nidx], w_e), ec);
        }
    }

    return result;
}

DitherResult apply_ostromoukhov(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val,
    bool serpentine) {

    auto w = image.width();
    auto h = image.height();

    float ec = error_clamp_val;

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
        // Analytical threshold methods — same "nearest vs second-nearest"
        // palette selection as matrix-based ordered dithering, but with
        // per-pixel analytical threshold (IGN, R2, white noise, etc.).
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        auto method = settings.method;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto pixel_lab = color_space::linear_to_oklab(image[x, y]);
                auto np = find_nearest_pair(pixel_lab, pal_span);
                float total = np.distA + np.distB;
                float t = (total > 1e-12f)
                    ? (std::sqrt(np.distA) /
                       (std::sqrt(np.distA) + std::sqrt(np.distB)))
                    : 0.0f;
                // ordered_threshold returns value in [-0.5, 0.5)
                float thr = ordered_threshold(method, x, y) + 0.5f;
                bool use_b = (thr < t * settings.strength);
                auto idx = use_b ? np.idxB : np.idxA;
                auto chosen = pal_span[idx];
                float dL = pixel_lab.L - chosen.L;
                float da = pixel_lab.a - chosen.a;
                float db = pixel_lab.b - chosen.b;
                r.indices[y * w + x] = static_cast<std::uint8_t>(idx);
                r.total_error += dL * dL + da * da + db * db;
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

    case Method::gilbert:
        return apply_gilbert(
            image, pal_span,
            settings.strength, settings.error_clamp);
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
    case Method::ostromoukhov:    return floyd_steinberg_kernel;  // F-S base kernel
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
