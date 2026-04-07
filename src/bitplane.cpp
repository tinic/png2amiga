#include "bitplane.hpp"
#include "color_space.hpp"
#include "palette.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace png2amiga::bitplane {

// ---------------------------------------------------------------------------
// Amiga bitplane encoding
//
// The Amiga stores pixel data as separate bitplanes. Each bitplane contains
// one bit of the color index for every pixel. For N bitplanes, pixel (x,y)
// has color index:
//   bit 0 from plane 0, bit 1 from plane 1, ..., bit N-1 from plane N-1
//
// Within each bitplane row, bits are packed MSB-first into bytes:
//   byte 0 bit 7 = pixel 0, byte 0 bit 6 = pixel 1, ...
//
// Rows are word-aligned (padded to multiple of 16 pixels / 2 bytes).
// ---------------------------------------------------------------------------

Result<BitplaneData> encode(const std::vector<std::uint8_t>& pixel_indices,
                            std::size_t width, std::size_t height,
                            std::size_t depth, Layout layout) {
    if (depth == 0 || depth > 8) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("Bitplane depth must be 1-8, got {}", depth),
        }};
    }

    if (width == 0 || height == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Image dimensions must be non-zero",
        }};
    }

    if (pixel_indices.size() < width * height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Expected {} pixel indices, got {}",
                        width * height, pixel_indices.size()),
        }};
    }

    auto max_index = static_cast<std::uint8_t>((1u << depth) - 1u);

    // Word-aligned bytes per row: round width up to multiple of 16
    auto aligned_width = (width + 15u) & ~std::size_t{15};
    auto bytes_per_row = aligned_width / 8;

    BitplaneData result;
    result.width = width;
    result.height = height;
    result.depth = depth;
    result.bytes_per_row = bytes_per_row;
    result.layout = layout;
    result.data.resize(result.total_bytes(), 0);

    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            auto index = pixel_indices[y * width + x];
            // Clamp to valid range
            if (index > max_index) index = max_index;

            // For each bitplane, extract the corresponding bit and set it
            for (std::size_t plane = 0; plane < depth; ++plane) {
                if ((index >> plane) & 1u) {
                    auto offset = result.plane_row_offset(plane, y);
                    auto byte_idx = x / 8;
                    auto bit_idx = 7u - (x % 8);  // MSB first
                    result.data[offset + byte_idx] |=
                        static_cast<std::uint8_t>(1u << bit_idx);
                }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Decode bitplane data back to indexed pixels
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> decode(const BitplaneData& planes) {
    if (planes.depth == 0 || planes.depth > 8) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("Invalid bitplane depth: {}", planes.depth),
        }};
    }

    std::vector<std::uint8_t> indices(planes.width * planes.height, 0);

    for (std::size_t y = 0; y < planes.height; ++y) {
        for (std::size_t x = 0; x < planes.width; ++x) {
            std::uint8_t color = 0;

            for (std::size_t plane = 0; plane < planes.depth; ++plane) {
                auto offset = planes.plane_row_offset(plane, y);
                auto byte_idx = x / 8;
                auto bit_idx = 7u - (x % 8);

                if ((planes.data[offset + byte_idx] >> bit_idx) & 1u) {
                    color |= static_cast<std::uint8_t>(1u << plane);
                }
            }

            indices[y * planes.width + x] = color;
        }
    }

    return indices;
}

// ---------------------------------------------------------------------------
// Encode an Image directly to bitplane data
// ---------------------------------------------------------------------------

Result<EncodeResult> encode_image(const Image& image, const Palette& pal,
                                  std::size_t depth, Layout layout) {
    if (depth == 0 || depth > 8) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("Bitplane depth must be 1-8, got {}", depth),
        }};
    }

    auto max_colors = std::size_t{1} << depth;
    auto w = image.width();
    auto h = image.height();

    // Build a working palette of at most max_colors entries.
    // If the provided palette has more, we use the first max_colors.
    // If it has fewer, we use what's available.
    auto pal_size = std::min(pal.size(), max_colors);
    auto pal_span = std::span<const Color3f>(pal.colors.data(), pal_size);

    std::vector<std::uint8_t> indices(w * h);
    float total_error = 0.0f;

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto pixel = image[x, y];
            auto best = palette::find_nearest(pixel, pal_span);
            indices[y * w + x] = static_cast<std::uint8_t>(best);
            total_error += color_space::perceptual_distance_sq(
                pixel, pal_span[best]);
        }
    }

    auto planes = encode(indices, w, h, depth, layout);
    if (!planes) return std::unexpected{planes.error()};

    std::vector<Color3f> used_palette(pal_span.begin(), pal_span.end());

    return EncodeResult{
        *std::move(planes),
        std::move(indices),
        std::move(used_palette),
        total_error,
    };
}

// ---------------------------------------------------------------------------
// Render bitplane data to preview Image
// ---------------------------------------------------------------------------

Result<Image> render(const BitplaneData& planes,
                     std::span<const Color3f> pal) {
    auto decoded = decode(planes);
    if (!decoded) return std::unexpected{decoded.error()};

    Image image(planes.width, planes.height);

    for (std::size_t y = 0; y < planes.height; ++y) {
        for (std::size_t x = 0; x < planes.width; ++x) {
            auto index = (*decoded)[y * planes.width + x];
            if (index < pal.size()) {
                image[x, y] = pal[index];
            } else {
                image[x, y] = Color3f{0.0f, 0.0f, 0.0f};
            }
        }
    }

    return image;
}

} // namespace png2amiga::bitplane
