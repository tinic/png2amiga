#include "msx.hpp"
#include "thomson.hpp"
#include "quantize.hpp"
#include "palette.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace png2amiga::msx {
namespace {
// Conventional TMS9918 RGB approximation. Index 0 is transparent, so the
// SCREEN 2 packer substitutes opaque black (1); analog colors vary by VDP.
constexpr std::array<std::uint32_t, 16> tms_rgb = {
    0x000000, 0x000000, 0x21c842, 0x5edc78,
    0x5455ed, 0x7d76fc, 0xd4524d, 0x42ebf5,
    0xfc5554, 0xff7978, 0xd4c154, 0xe6ce80,
    0x21b03b, 0xc95bba, 0xcccccc, 0xffffff,
};
int channel(float c) {
    return static_cast<int>(std::lround(std::clamp(color_space::linear_to_srgb(c), 0.0f, 1.0f) * 7));
}
Color3f rgb3(int r, int g, int b) {
    return {color_space::srgb_to_linear(static_cast<float>(r) / 7),
            color_space::srgb_to_linear(static_cast<float>(g) / 7),
            color_space::srgb_to_linear(static_cast<float>(b) / 7)};
}
std::size_t palette_address(int screen) { return screen == 7 ? 0xfa80 : 0x7680; }
std::size_t frame_size(int screen) {
    if (screen == 2) return 0x3800;
    if (screen == 8) return 256 * 212;
    return palette_address(screen) + 32;
}
std::vector<Color3f> quantize_rgb3(const Image& image, std::size_t n) {
    std::vector<Color3f> colors;
    std::vector<int> codes;
    // Preserve the principal clusters, refill slots lost to RGB333 snapping.
    for (std::size_t k = n; k <= 256 && colors.size() < n; k *= 2) {
        auto p = quantize::quantize(image, k);
        if (!p) break;
        for (auto c : p->colors) {
            int r = channel(c.r), g = channel(c.g), b = channel(c.b);
            int code = (r << 6) | (g << 3) | b;
            if (std::find(codes.begin(), codes.end(), code) != codes.end()) continue;
            codes.push_back(code);
            colors.push_back(rgb3(r,g,b));
            if (colors.size() == n) break;
        }
    }
    colors.resize(n, Color3f{0,0,0});
    return colors;
}
} // namespace

int screen_number(amiga::Mode mode) {
    switch (mode) {
    case amiga::Mode::msx1_screen2: return 2;
    case amiga::Mode::msx2_screen5: return 5;
    case amiga::Mode::msx2_screen6: return 6;
    case amiga::Mode::msx2_screen7: return 7;
    case amiga::Mode::msx2_screen8: return 8;
    default: return 0;
    }
}
Result<EncodeResult> encode(const Image& image, amiga::Mode mode,
                           const dither::Settings& settings, bool refine_cells) {
    int screen = screen_number(mode);
    if (!screen) return std::unexpected{Error{ErrorCode::unsupported_mode, "Not an MSX mode"}};
    auto params = amiga::get_mode_params(mode);
    if (image.width() != params.screen_width || image.height() != params.screen_height)
        return std::unexpected{Error{ErrorCode::invalid_dimensions, "MSX requires its native screen dimensions"}};
    EncodeResult out;
    out.vram.assign(frame_size(screen), 0);
    if (screen == 2) {
        for (auto rgb : tms_rgb) out.palette.push_back(color_space::srgb_hex_to_linear(rgb));
        auto cells = thomson::encode_msx_cells(image, out.palette, settings, refine_cells);
        if (!cells) return std::unexpected{cells.error()};
        // Three independent 256-pattern banks. Name table repeats 0..255.
        for (std::size_t i = 0; i < 768; ++i) out.vram[0x1800 + i] = static_cast<std::uint8_t>(i);
        out.vram[0x1b00] = 208; // sprite-list terminator, TMS9918
        for (std::size_t y = 0; y < 192; ++y) {
            for (std::size_t x = 0; x < 32; ++x) {
                auto address = (y / 8) * 256 + x * 8 + y % 8;
                out.vram[address] = cells->page_b[y * 32 + x];
                out.vram[0x2000 + address] = cells->page_a[y * 32 + x];
            }
        }
        out.rendered = std::move(cells->rendered);
    } else {
        if (screen == 8) {
            // V9938 G7 is GGGRRRBB. Blue levels are 0,2,4,7 (not 0,1/3,2/3,1).
            for (int i = 0; i < 256; ++i) {
                int b = i & 3;
                out.palette.push_back(rgb3((i >> 2) & 7, i >> 5, b == 3 ? 7 : b * 2));
            }
        } else {
            out.palette = quantize_rgb3(image, screen == 6 ? 4 : 16);
            auto base = palette_address(screen);
            for (std::size_t i = 0; i < out.palette.size(); ++i) {
                auto c = out.palette[i];
                out.vram[base + 2*i] = static_cast<std::uint8_t>((channel(c.r) << 4) | channel(c.b));
                out.vram[base + 2*i+1] = static_cast<std::uint8_t>(channel(c.g));
            }
            out.vram[screen == 7 ? 0xfa00 : 0x7600] = 216; // mode-2 sprite terminator
        }
        auto d = dither::apply(image, out.palette, settings);
        out.rendered = Image(image.width(), image.height());
        std::size_t bpp = screen == 8 ? 8 : screen == 6 ? 2 : 4;
        std::size_t ppb = 8 / bpp;
        for (std::size_t i = 0; i < d.indices.size(); ++i) {
            auto q = d.indices[i];
            out.vram[i / ppb] |= static_cast<std::uint8_t>(q << (8 - bpp * (i % ppb + 1)));
            out.rendered.pixels()[i] = out.palette[q];
        }
    }
    for (std::size_t i = 0; i < image.pixels().size(); ++i)
        out.total_error += color_space::perceptual_distance_sq(image.pixels()[i], out.rendered.pixels()[i]);
    return out;
}
Result<std::vector<std::uint8_t>> screen_file(amiga::Mode mode, std::span<const std::uint8_t> vram) {
    int screen = screen_number(mode);
    if (!screen || vram.size() != frame_size(screen))
        return std::unexpected{Error{ErrorCode::unsupported_mode, "Invalid MSX screen data"}};
    auto end = vram.size() - 1;
    std::vector<std::uint8_t> out{0xfe, 0, 0, static_cast<std::uint8_t>(end),
                                static_cast<std::uint8_t>(end >> 8), 0, 0};
    out.insert(out.end(), vram.begin(), vram.end());
    return out;
}
} // namespace png2amiga::msx
