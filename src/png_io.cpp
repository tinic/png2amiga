#include "png_io.hpp"
#include "color_space.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <cmath>
#include <memory>
#include <string>

namespace png2amiga::png_io {

namespace {

struct StbiFree {
    void operator()(stbi_uc* p) const noexcept { stbi_image_free(p); }
};

using StbiPtr = std::unique_ptr<stbi_uc[], StbiFree>;

} // namespace

Result<Image> load(std::string_view path) {
    auto path_str = std::string(path);

    int w{};
    int h{};
    int channels{};
    // Always request 4 channels (RGBA) so we catch all forms of
    // transparency including palette tRNS chunks
    StbiPtr data{stbi_load(path_str.c_str(), &w, &h, &channels, 4)};

    if (!data) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            std::string("Failed to load: ") + stbi_failure_reason(),
        }};
    }

    auto width = static_cast<std::size_t>(w);
    auto height = static_cast<std::size_t>(h);
    auto pixel_count = width * height;

    std::vector<Color3f> pixels(pixel_count);
    const auto* raw = data.get();

    // Check if any pixel has non-opaque alpha
    bool any_transparent = false;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        if (raw[i * 4 + 3] < 255) {
            any_transparent = true;
            break;
        }
    }

    if (any_transparent) {
        std::vector<float> alpha(pixel_count);
        for (std::size_t i = 0; i < pixel_count; ++i) {
            auto base = i * 4;
            pixels[i] = color_space::srgb_u8_to_linear(
                raw[base + 0], raw[base + 1], raw[base + 2]);
            alpha[i] = static_cast<float>(raw[base + 3]) / 255.0f;
        }
        return Image{width, height, std::move(pixels), std::move(alpha)};
    }

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto base = i * 4;
        pixels[i] = color_space::srgb_u8_to_linear(
            raw[base + 0], raw[base + 1], raw[base + 2]);
    }

    return Image{width, height, std::move(pixels)};
}

Result<void> save(std::string_view path, const Image& image) {
    auto path_str = std::string(path);
    auto w = image.width();
    auto h = image.height();
    auto pixel_count = w * h;

    std::vector<std::uint8_t> out(pixel_count * 3);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto y = i / w;
        auto x = i % w;
        auto srgb = color_space::linear_to_srgb(image[x, y]).clamped();

        auto base = i * 3;
        out[base + 0] =
            static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f));
        out[base + 1] =
            static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f));
        out[base + 2] =
            static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f));
    }

    int result = stbi_write_png(path_str.c_str(), static_cast<int>(w),
                                static_cast<int>(h), 3, out.data(),
                                static_cast<int>(w * 3));

    if (result == 0) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to write PNG: " + path_str,
        }};
    }

    return {};
}

namespace {

void png_write_callback(void* context, void* data, int size) {
    auto* vec = static_cast<std::vector<std::uint8_t>*>(context);
    auto* bytes = static_cast<const std::uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

} // namespace

Result<std::vector<std::uint8_t>> encode(const Image& image) {
    auto w = image.width();
    auto h = image.height();
    auto pixel_count = w * h;

    std::vector<std::uint8_t> rgb(pixel_count * 3);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto y = i / w;
        auto x = i % w;
        auto srgb = color_space::linear_to_srgb(image[x, y]).clamped();

        auto base = i * 3;
        rgb[base + 0] =
            static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f));
        rgb[base + 1] =
            static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f));
        rgb[base + 2] =
            static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f));
    }

    std::vector<std::uint8_t> png_data;
    int result = stbi_write_png_to_func(
        png_write_callback, &png_data, static_cast<int>(w),
        static_cast<int>(h), 3, rgb.data(), static_cast<int>(w * 3));

    if (result == 0) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to encode PNG in memory",
        }};
    }

    return png_data;
}

Result<void> save(std::string_view path, const Image& image,
                  const std::vector<bool>& transparency_mask) {
    auto path_str = std::string(path);
    auto w = image.width();
    auto h = image.height();
    auto pixel_count = w * h;

    std::vector<std::uint8_t> out(pixel_count * 4);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto y = i / w;
        auto x = i % w;
        auto srgb = color_space::linear_to_srgb(image[x, y]).clamped();
        bool transparent = (i < transparency_mask.size()) && transparency_mask[i];

        auto base = i * 4;
        out[base + 0] =
            static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f));
        out[base + 1] =
            static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f));
        out[base + 2] =
            static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f));
        out[base + 3] = transparent ? std::uint8_t{0} : std::uint8_t{255};
    }

    int result = stbi_write_png(path_str.c_str(), static_cast<int>(w),
                                static_cast<int>(h), 4, out.data(),
                                static_cast<int>(w * 4));

    if (result == 0) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to write PNG: " + path_str,
        }};
    }

    return {};
}

Result<std::vector<std::uint8_t>> encode(const Image& image,
                                         const std::vector<bool>& transparency_mask) {
    auto w = image.width();
    auto h = image.height();
    auto pixel_count = w * h;

    std::vector<std::uint8_t> rgba(pixel_count * 4);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto y = i / w;
        auto x = i % w;
        auto srgb = color_space::linear_to_srgb(image[x, y]).clamped();
        bool transparent = (i < transparency_mask.size()) && transparency_mask[i];

        auto base = i * 4;
        rgba[base + 0] =
            static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f));
        rgba[base + 1] =
            static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f));
        rgba[base + 2] =
            static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f));
        rgba[base + 3] = transparent ? std::uint8_t{0} : std::uint8_t{255};
    }

    std::vector<std::uint8_t> png_data;
    int result = stbi_write_png_to_func(
        png_write_callback, &png_data, static_cast<int>(w),
        static_cast<int>(h), 4, rgba.data(), static_cast<int>(w * 4));

    if (result == 0) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to encode PNG in memory",
        }};
    }

    return png_data;
}

} // namespace png2amiga::png_io
