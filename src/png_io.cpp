#include "png_io.hpp"
#include "color_space.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

// stb_image_write exposes stbi_zlib_compress in its implementation block,
// but the public header doesn't forward-declare it. Add a manual extern.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len,
                                             int* out_len, int quality);

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

// ---------------------------------------------------------------------------
// Palettized PNG (PNG-8 with PLTE chunk)
//
// Hand-rolled minimal writer because stb_image_write only does RGB/RGBA.
// Uses stb's exposed stbi_zlib_compress for the IDAT chunk.
// PNG format reference: RFC 2083 / W3C PNG spec.
// ---------------------------------------------------------------------------

namespace {

// Standard PNG CRC-32 (poly 0xEDB88320). Lazy-initialized table.
std::uint32_t png_crc32(const std::uint8_t* data, std::size_t len) {
    static std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void write_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// Append a PNG chunk: length, type, data, crc(type+data).
void write_chunk(std::vector<std::uint8_t>& out,
                 const char (&type)[5],
                 const std::uint8_t* data, std::size_t len) {
    write_be32(out, static_cast<std::uint32_t>(len));
    auto crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    auto crc = png_crc32(out.data() + crc_start, out.size() - crc_start);
    write_be32(out, crc);
}

} // namespace

Result<std::vector<std::uint8_t>> encode_palettized(
    const std::vector<std::uint8_t>& indices,
    const std::vector<Color3f>& palette,
    std::size_t width, std::size_t height,
    int transparent_index) {

    if (width == 0 || height == 0)
        return std::unexpected{Error{ErrorCode::invalid_dimensions, "zero dim"}};
    if (indices.size() < width * height)
        return std::unexpected{Error{ErrorCode::invalid_dimensions, "indices too short"}};
    if (palette.empty() || palette.size() > 256)
        return std::unexpected{Error{ErrorCode::invalid_depth, "palette must have 1-256 colors"}};

    std::vector<std::uint8_t> out;
    out.reserve(width * height + 1024);

    // 8-byte PNG signature
    static constexpr std::uint8_t sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    // IHDR (13 bytes): width, height, bit_depth=8, color_type=3 (palette),
    //                  compression=0, filter=0, interlace=0
    {
        std::uint8_t hdr[13];
        auto put_be32 = [](std::uint8_t* p, std::uint32_t v) {
            p[0] = static_cast<std::uint8_t>(v >> 24);
            p[1] = static_cast<std::uint8_t>(v >> 16);
            p[2] = static_cast<std::uint8_t>(v >> 8);
            p[3] = static_cast<std::uint8_t>(v);
        };
        put_be32(hdr + 0, static_cast<std::uint32_t>(width));
        put_be32(hdr + 4, static_cast<std::uint32_t>(height));
        hdr[8] = 8;   // bit depth
        hdr[9] = 3;   // color type: indexed
        hdr[10] = 0;  // compression
        hdr[11] = 0;  // filter
        hdr[12] = 0;  // interlace
        write_chunk(out, "IHDR", hdr, sizeof(hdr));
    }

    // PLTE: 3 bytes per color (sRGB-encoded)
    {
        std::vector<std::uint8_t> plte(palette.size() * 3);
        for (std::size_t i = 0; i < palette.size(); ++i) {
            auto srgb = color_space::linear_to_srgb(palette[i]).clamped();
            plte[i * 3 + 0] = static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f));
            plte[i * 3 + 1] = static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f));
            plte[i * 3 + 2] = static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f));
        }
        write_chunk(out, "PLTE", plte.data(), plte.size());
    }

    // tRNS: optional, alpha per palette entry (entries beyond array length are opaque)
    if (transparent_index >= 0 &&
        static_cast<std::size_t>(transparent_index) < palette.size()) {
        auto ti = static_cast<std::size_t>(transparent_index);
        std::vector<std::uint8_t> trns(ti + 1, 255);
        trns[ti] = 0;
        write_chunk(out, "tRNS", trns.data(), trns.size());
    }

    // IDAT: filter byte (0 = no filter) per scanline + raw indices, deflated
    {
        std::vector<std::uint8_t> raw;
        raw.reserve(height * (width + 1));
        for (std::size_t y = 0; y < height; ++y) {
            raw.push_back(0);  // filter type none
            raw.insert(raw.end(),
                       indices.data() + y * width,
                       indices.data() + (y + 1) * width);
        }
        int zlen = 0;
        unsigned char* zbuf = stbi_zlib_compress(
            raw.data(), static_cast<int>(raw.size()), &zlen,
            8 /* level */);
        if (!zbuf) {
            return std::unexpected{Error{ErrorCode::write_failed, "deflate failed"}};
        }
        write_chunk(out, "IDAT", zbuf, static_cast<std::size_t>(zlen));
        std::free(zbuf);  // stbi_zlib_compress allocates with malloc by default
    }

    // IEND: empty chunk
    write_chunk(out, "IEND", nullptr, 0);

    return out;
}

Result<void> save_palettized(std::string_view path,
                             const std::vector<std::uint8_t>& indices,
                             const std::vector<Color3f>& palette,
                             std::size_t width, std::size_t height,
                             int transparent_index) {
    auto encoded = encode_palettized(indices, palette, width, height, transparent_index);
    if (!encoded) return std::unexpected{encoded.error()};
    auto path_str = std::string(path);
    std::ofstream f(path_str, std::ios::binary);
    if (!f) return std::unexpected{Error{ErrorCode::write_failed, "open " + path_str}};
    f.write(reinterpret_cast<const char*>(encoded->data()),
            static_cast<std::streamsize>(encoded->size()));
    if (!f) return std::unexpected{Error{ErrorCode::write_failed, "write " + path_str}};
    return {};
}

} // namespace png2amiga::png_io
