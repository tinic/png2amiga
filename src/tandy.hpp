#pragma once

#include "amiga.hpp"
#include "color_space.hpp"
#include "types.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace png2amiga::tandy {

// Conventional RGBI swatches; register numbers are B,G,R,I bits.
inline constexpr std::array<std::uint32_t, 16> rgbi{
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

inline const std::array<Color3f, 16> linear_rgbi = [] {
    std::array<Color3f, 16> colors{};
    for (std::size_t i = 0; i < 16; ++i) colors[i] = color_space::srgb_hex_to_linear(rgbi[i]);
    return colors;
}();

inline std::uint8_t hardware_index(Color3f color) {
    float best = std::numeric_limits<float>::max();
    std::uint8_t index = 0;
    for (std::uint8_t k = 0; k < 16; ++k) {
        auto c = linear_rgbi[k];
        float d = color_space::fma_dist_sq(color.r - c.r, color.g - c.g, color.b - c.b);
        if (d < best) { best = d; index = k; }
    }
    return index;
}

// Mode 0Ah has four global palette slots. Exhaustively choose 4 of the
// 16 RGBI colours using an image histogram. Exact <=4-colour art gets a
// zero-cost palette containing every source colour, irrespective of indices.
inline std::vector<Color3f> four_colors(const Image& image) {
    // 4x4x4 RGB buckets retain the actual mean of off-gamut colours.
    // RGBI's 16 colours occupy distinct buckets, so pixel art stays exact.
    std::array<std::size_t, 64> weights{};
    std::array<Color3f, 64> sums{};
    auto bucket = [](float v) -> std::size_t {
        if (v < 0.0236524f) return 0;
        if (v < 0.2140411f) return 1;
        if (v < 0.6618756f) return 2;
        return 3;
    };
    for (auto c : image.pixels()) {
        auto i = bucket(c.r) * 16 + bucket(c.g) * 4 + bucket(c.b);
        ++weights[i];
        sums[i].r += c.r; sums[i].g += c.g; sums[i].b += c.b;
    }
    std::array<color_space::OKLab, 16> lab;
    for (std::size_t i = 0; i < 16; ++i)
        lab[i] = color_space::linear_to_oklab(linear_rgbi[i]);
    std::array<std::array<float, 16>, 64> distance{};
    for (std::size_t i = 0; i < 64; ++i) {
        if (!weights[i]) continue;
        auto scale = 1.0f / static_cast<float>(weights[i]);
        auto sample = color_space::linear_to_oklab(Color3f{
            sums[i].r * scale, sums[i].g * scale, sums[i].b * scale});
        for (std::size_t k = 0; k < 16; ++k)
            distance[i][k] = color_space::fma_dist_sq(
                sample.L - lab[k].L, sample.a - lab[k].a, sample.b - lab[k].b);
    }
    std::array<std::size_t, 4> picked{0, 1, 2, 3};
    double best = std::numeric_limits<double>::max();
    double best_tie = std::numeric_limits<double>::max();
    for (std::size_t a = 0; a < 13; ++a)
        for (std::size_t b = a + 1; b < 14; ++b)
            for (std::size_t c = b + 1; c < 15; ++c)
                for (std::size_t d = c + 1; d < 16; ++d) {
                    double cost = 0, tie = 0;
                    for (std::size_t i = 0; i < 64; ++i) {
                        auto weight = static_cast<double>(weights[i]);
                        cost += weight * static_cast<double>(
                            std::min({distance[i][a], distance[i][b], distance[i][c], distance[i][d]}));
                        tie += weight * static_cast<double>(
                            distance[i][a] + distance[i][b] + distance[i][c] + distance[i][d]);
                    }
                    // Give otherwise-unused slots nearby colours, rather
                    // than arbitrary dark primaries, so dithering can mix.
                    if (cost < best || (cost == best && tie < best_tie)) {
                        best = cost; best_tie = tie; picked = {a, b, c, d};
                    }
                }
    std::vector<Color3f> palette;
    for (auto k : picked) palette.push_back(linear_rgbi[k]);
    return palette;
}

inline std::size_t frame_size(amiga::Mode mode) {
    return mode == amiga::Mode::tandy_160 ? 16384 : 32768;
}

inline std::array<std::uint8_t, 16> palette_registers(std::span<const Color3f> colors) {
    std::array<std::uint8_t, 16> out{};
    for (std::size_t i = 0; i < 16; ++i)
        out[i] = i < colors.size() ? hardware_index(colors[i]) : static_cast<std::uint8_t>(i);
    return out;
}

// Tandy technical reference: 8 KB banks, y mod 2 for 160x200 and
// y mod 4 for 320/640x200. Mode 0Ah stores adjacent byte pairs as the
// low/high bitplanes of 8 pixels (not ordinary CGA packed 2bpp).
// Reference decoder: DOSBox Staging VGA_Draw_2BPPHiRes_Line.
inline std::vector<std::uint8_t> pack(std::span<const std::uint8_t> indices, amiga::Mode mode) {
    const auto width = amiga::get_mode_params(mode).screen_width;
    const auto banks = mode == amiga::Mode::tandy_160 ? 2u : 4u;
    const auto row_bytes = mode == amiga::Mode::tandy_160 ? 80u : 160u;
    std::vector<std::uint8_t> raw(frame_size(mode), 0);
    for (std::size_t y = 0; y < 200; ++y) {
        auto row = (y % banks) * 8192 + (y / banks) * row_bytes;
        for (std::size_t x = 0; x < width; ++x) {
            auto index = indices[y * width + x];
            if (mode == amiga::Mode::tandy_640) {
                auto offset = row + (x / 8) * 2;
                auto bit = static_cast<std::uint8_t>(0x80u >> (x % 8));
                if (index & 1) raw[offset] |= bit;
                if (index & 2) raw[offset + 1] |= bit;
            } else {
                auto shift = (x % 2 == 0) ? 4 : 0;
                raw[row + x / 2] |= static_cast<std::uint8_t>((index & 15) << shift);
            }
        }
    }
    return raw;
}

inline std::vector<std::uint8_t> raw_export(std::span<const std::uint8_t> frame,
                                           std::span<const Color3f> palette) {
    std::vector<std::uint8_t> out(frame.begin(), frame.end());
    auto regs = palette_registers(palette);
    out.insert(out.end(), regs.begin(), regs.end());
    return out;
}
} // namespace png2amiga::tandy
