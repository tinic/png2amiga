#include "scale.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace png2amiga::scale {

namespace {

constexpr int kLanczosA = 3;

// Lanczos-3: sinc(x) * sinc(x/3) for |x| < 3. The standard
// "sharp downscale, sharp upscale" filter; chosen as the only
// kernel because it consistently looks best across photographic
// content + line art at the resolutions this tool targets
// (320x... lores, 640x... hires).
inline float lanczos3(float x) noexcept {
    if (x == 0.0f) return 1.0f;
    if (x <= -static_cast<float>(kLanczosA) ||
        x >=  static_cast<float>(kLanczosA)) return 0.0f;
    constexpr float pi = std::numbers::pi_v<float>;
    float pix = pi * x;
    float pix_a = pix / static_cast<float>(kLanczosA);
    return (std::sin(pix) / pix) * (std::sin(pix_a) / pix_a);
}

// Precomputed kernel weights for one destination column or row.
struct KernelRow {
    int first;                 // first source index (already clamped)
    int count;                 // number of taps
    std::vector<float> w;      // normalized weights, summed to 1
};

std::vector<KernelRow> build_kernel_rows(std::size_t src_size,
                                         std::size_t dst_size) {
    float ratio = static_cast<float>(src_size) / static_cast<float>(dst_size);
    // Proper anti-aliasing: stretch the filter by `ratio` when
    // downscaling so it averages over exactly one destination
    // pixel's worth of source. Upscaling keeps filter_scale = 1
    // (the kernel's natural support is a tap-per-source-pixel).
    // The earlier `ratio * 0.75` hack under-filtered to "look
    // sharper" but actually aliased — sharpness now comes from
    // Lanczos itself, not from leaking high frequencies.
    float filter_scale = std::max(1.0f, ratio);
    int support = static_cast<int>(std::ceil(
        static_cast<float>(kLanczosA) * filter_scale));
    int src_last = static_cast<int>(src_size) - 1;

    std::vector<KernelRow> rows(dst_size);
    for (std::size_t i = 0; i < dst_size; ++i) {
        float src_center = (static_cast<float>(i) + 0.5f) * ratio - 0.5f;
        int center = static_cast<int>(std::floor(src_center));
        int lo = std::max(0, center - support + 1);
        int hi = std::min(src_last, center + support);

        auto& r = rows[i];
        r.first = lo;
        r.count = hi - lo + 1;
        r.w.resize(static_cast<std::size_t>(r.count));
        float wsum = 0.0f;
        for (int k = 0; k < r.count; ++k) {
            int sx = lo + k;
            float dist = (src_center - static_cast<float>(sx)) / filter_scale;
            float w = lanczos3(dist);
            r.w[static_cast<std::size_t>(k)] = w;
            wsum += w;
        }
        if (wsum > 0.0f) {
            float inv = 1.0f / wsum;
            for (auto& w : r.w) w *= inv;
        }
    }
    return rows;
}

Image scale_horizontal(const Image& src, std::size_t dst_width) {
    auto src_w = src.width();
    auto src_h = src.height();
    Image dst(dst_width, src_h);

    auto rows = build_kernel_rows(src_w, dst_width);

    for (std::size_t y = 0; y < src_h; ++y) {
        for (std::size_t x = 0; x < dst_width; ++x) {
            const auto& r = rows[x];
            Color3f sum{};
            for (int k = 0; k < r.count; ++k) {
                auto sx = static_cast<std::size_t>(r.first + k);
                sum += src[sx, y] * r.w[static_cast<std::size_t>(k)];
            }
            // Lanczos can produce small negative lobes at sharp
            // edges; clamp to the valid linear-RGB range so the
            // downstream quantiser sees physical colours.
            dst[x, y] = sum.clamped();
        }
    }

    return dst;
}

Image scale_vertical(const Image& src, std::size_t dst_height) {
    auto src_w = src.width();
    auto src_h = src.height();
    Image dst(src_w, dst_height);

    auto rows = build_kernel_rows(src_h, dst_height);

    for (std::size_t y = 0; y < dst_height; ++y) {
        const auto& r = rows[y];
        for (std::size_t x = 0; x < src_w; ++x) {
            Color3f sum{};
            for (int k = 0; k < r.count; ++k) {
                auto sy = static_cast<std::size_t>(r.first + k);
                sum += src[x, sy] * r.w[static_cast<std::size_t>(k)];
            }
            dst[x, y] = sum.clamped();
        }
    }

    return dst;
}

} // namespace

Result<Image> resample(const Image& src, std::size_t dst_width,
                       std::size_t dst_height) {
    if (dst_width == 0 || dst_height == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Target dimensions must be non-zero",
        }};
    }

    auto intermediate = scale_horizontal(src, dst_width);
    return scale_vertical(intermediate, dst_height);
}

} // namespace png2amiga::scale
