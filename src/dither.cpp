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
constexpr auto clustered_mat = make_clustered_dot();
constexpr auto line2_mat = make_line2();
constexpr auto line_checker_mat = make_line_checker();
constexpr auto line4_mat = make_line4();
constexpr auto line8_mat = make_line8();

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

struct DiffusionEntry {
    int dx;
    int dy;
    float weight;
};

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
            auto clamped_error = oklab_clamp(error_buf[buf_idx],
                                             error_clamp_val);
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
                        error_clamp_val);
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
    case Method::clustered_dot:
        return apply_ordered(image, clustered_mat, pal_span,
                             settings.strength);
    case Method::line2:
        return apply_ordered(image, line2_mat, pal_span,
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
    }

    return apply_none(image, pal_span);
}

float ordered_threshold(Method method, std::size_t x, std::size_t y) {
    switch (method) {
    case Method::bayer2x2:      return bayer2[y % 2][x % 2];
    case Method::bayer4x4:      return bayer4[y % 4][x % 4];
    case Method::bayer8x8:      return bayer8[y % 8][x % 8];
    case Method::checker:       return checker_mat[y % 2][x % 2];
    case Method::h2x4:          return h2x4_mat[y % 4][x % 2];
    case Method::clustered_dot: return clustered_mat[y % 4][x % 4];
    case Method::line2:         return line2_mat[y % 2][0];
    case Method::line_checker:  return line_checker_mat[y % 2][x % 2];
    case Method::line4:         return line4_mat[y % 4][0];
    case Method::line8:         return line8_mat[y % 8][0];
    default:                    return 0.0f;
    }
}

} // namespace png2amiga::dither
