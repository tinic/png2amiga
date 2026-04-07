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

Image scale_horizontal(const Image& src, std::size_t dst_width,
                        KernelFn kernel_fn) {
    auto src_w = src.width();
    auto src_h = src.height();
    Image dst(dst_width, src_h);

    float ratio = static_cast<float>(src_w) / static_cast<float>(dst_width);

    for (std::size_t y = 0; y < src_h; ++y) {
        for (std::size_t x = 0; x < dst_width; ++x) {
            float src_x = (static_cast<float>(x) + 0.5f) * ratio - 0.5f;
            auto center = static_cast<int>(std::floor(src_x));

            Color3f sum{};
            float weight_sum = 0.0f;

            for (int k = -1; k <= 2; ++k) {
                int sx = center + k;
                sx = std::clamp(sx, 0, static_cast<int>(src_w) - 1);

                float w = kernel_fn(src_x - static_cast<float>(sx));
                sum += src[static_cast<std::size_t>(sx), y] * w;
                weight_sum += w;
            }

            if (weight_sum > 0.0f) {
                sum *= 1.0f / weight_sum;
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

    float ratio = static_cast<float>(src_h) / static_cast<float>(dst_height);

    for (std::size_t y = 0; y < dst_height; ++y) {
        float src_y = (static_cast<float>(y) + 0.5f) * ratio - 0.5f;
        auto center = static_cast<int>(std::floor(src_y));

        for (std::size_t x = 0; x < src_w; ++x) {
            Color3f sum{};
            float weight_sum = 0.0f;

            for (int k = -1; k <= 2; ++k) {
                int sy = center + k;
                sy = std::clamp(sy, 0, static_cast<int>(src_h) - 1);

                float w = kernel_fn(src_y - static_cast<float>(sy));
                sum += src[x, static_cast<std::size_t>(sy)] * w;
                weight_sum += w;
            }

            if (weight_sum > 0.0f) {
                sum *= 1.0f / weight_sum;
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
