#include "scale.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace png2amiga::scale {

namespace {

// Mitchell-Netravali: B=1/3, C=1/3
constexpr float mitchell(float x) noexcept {
    x = std::abs(x);
    if (x < 1.0f) {
        return (7.0f * x * x * x - 12.0f * x * x + 16.0f / 3.0f) / 6.0f;
    }
    if (x < 2.0f) {
        return (-7.0f / 3.0f * x * x * x + 12.0f * x * x - 20.0f * x +
                32.0f / 3.0f) /
               6.0f;
    }
    return 0.0f;
}

// Catmull-Rom: B=0, C=1/2
constexpr float catmull_rom(float x) noexcept {
    x = std::abs(x);
    if (x < 1.0f) {
        return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    }
    if (x < 2.0f) {
        return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    }
    return 0.0f;
}

using KernelFn = float (*)(float);

// Precomputed kernel weights for one destination column (horizontal) or row
// (vertical). Same weights apply to every row/column of the source — the
// naive loop was recomputing them per-pixel.
struct KernelRow {
    int first;                 // first source index (already clamped)
    int count;                 // number of taps
    std::vector<float> w;      // normalized weights, summed to 1
};

std::vector<KernelRow> build_kernel_rows(std::size_t src_size,
                                         std::size_t dst_size,
                                         KernelFn kernel_fn) {
    float ratio = static_cast<float>(src_size) / static_cast<float>(dst_size);
    float filter_scale = std::max(1.0f, ratio * 0.75f);
    int support = static_cast<int>(std::ceil(2.0f * filter_scale));
    int src_last = static_cast<int>(src_size) - 1;

    std::vector<KernelRow> rows(dst_size);
    for (std::size_t i = 0; i < dst_size; ++i) {
        float src_center = (static_cast<float>(i) + 0.5f) * ratio - 0.5f;
        int center = static_cast<int>(std::floor(src_center));
        int lo = std::max(0, center - support);
        int hi = std::min(src_last, center + support);

        auto& r = rows[i];
        r.first = lo;
        r.count = hi - lo + 1;
        r.w.resize(static_cast<std::size_t>(r.count));
        float wsum = 0.0f;
        for (int k = 0; k < r.count; ++k) {
            int sx = lo + k;
            float dist = (src_center - static_cast<float>(sx)) / filter_scale;
            float w = kernel_fn(dist);
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

Image scale_horizontal(const Image& src, std::size_t dst_width,
                        KernelFn kernel_fn) {
    auto src_w = src.width();
    auto src_h = src.height();
    Image dst(dst_width, src_h);

    auto rows = build_kernel_rows(src_w, dst_width, kernel_fn);

    for (std::size_t y = 0; y < src_h; ++y) {
        for (std::size_t x = 0; x < dst_width; ++x) {
            const auto& r = rows[x];
            Color3f sum{};
            for (int k = 0; k < r.count; ++k) {
                auto sx = static_cast<std::size_t>(r.first + k);
                sum += src[sx, y] * r.w[static_cast<std::size_t>(k)];
            }
            dst[x, y] = sum;
        }
    }

    return dst;
}

Image scale_vertical(const Image& src, std::size_t dst_height,
                      KernelFn kernel_fn) {
    auto src_w = src.width();
    auto src_h = src.height();
    Image dst(src_w, dst_height);

    auto rows = build_kernel_rows(src_h, dst_height, kernel_fn);

    for (std::size_t y = 0; y < dst_height; ++y) {
        const auto& r = rows[y];
        for (std::size_t x = 0; x < src_w; ++x) {
            Color3f sum{};
            for (int k = 0; k < r.count; ++k) {
                auto sy = static_cast<std::size_t>(r.first + k);
                sum += src[x, sy] * r.w[static_cast<std::size_t>(k)];
            }
            dst[x, y] = sum;
        }
    }

    return dst;
}

} // namespace

Result<Image> bicubic(const Image& src, std::size_t dst_width,
                      std::size_t dst_height, Kernel kernel) {
    if (dst_width == 0 || dst_height == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Target dimensions must be non-zero",
        }};
    }

    KernelFn fn = (kernel == Kernel::catmull_rom) ? catmull_rom : mitchell;

    auto intermediate = scale_horizontal(src, dst_width, fn);
    return scale_vertical(intermediate, dst_height, fn);
}

} // namespace png2amiga::scale
