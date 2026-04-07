#pragma once

#include "amiga.hpp"
#include "color_space.hpp"
#include "types.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace png2amiga::palette {

// ---------------------------------------------------------------------------
// OCS/ECS 12-bit color: 4 bits per channel (0-15 mapped to 0x00-0xFF)
// Amiga hardware stores R/G/B nibbles: 0xRGB
// ---------------------------------------------------------------------------

constexpr Color3f ocs_to_linear(std::uint16_t rgb12) noexcept {
    auto r4 = static_cast<std::uint8_t>((rgb12 >> 8) & 0xF);
    auto g4 = static_cast<std::uint8_t>((rgb12 >> 4) & 0xF);
    auto b4 = static_cast<std::uint8_t>(rgb12 & 0xF);
    // 4-bit to 8-bit: replicate nibble (0xN -> 0xNN)
    auto r8 = static_cast<std::uint8_t>((r4 << 4) | r4);
    auto g8 = static_cast<std::uint8_t>((g4 << 4) | g4);
    auto b8 = static_cast<std::uint8_t>((b4 << 4) | b4);
    return color_space::srgb_u8_to_linear(r8, g8, b8);
}

constexpr std::uint16_t linear_to_ocs(Color3f color) noexcept {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r4 = static_cast<std::uint16_t>(
        static_cast<int>(srgb.r * 15.0f + 0.5f) & 0xF);
    auto g4 = static_cast<std::uint16_t>(
        static_cast<int>(srgb.g * 15.0f + 0.5f) & 0xF);
    auto b4 = static_cast<std::uint16_t>(
        static_cast<int>(srgb.b * 15.0f + 0.5f) & 0xF);
    return static_cast<std::uint16_t>((r4 << 8) | (g4 << 4) | b4);
}

// ---------------------------------------------------------------------------
// AGA 24-bit color: 8 bits per channel
// Stored as 0x00RRGGBB
// ---------------------------------------------------------------------------

constexpr Color3f aga_to_linear(std::uint32_t rgb24) noexcept {
    auto r = static_cast<std::uint8_t>((rgb24 >> 16) & 0xFF);
    auto g = static_cast<std::uint8_t>((rgb24 >> 8) & 0xFF);
    auto b = static_cast<std::uint8_t>(rgb24 & 0xFF);
    return color_space::srgb_u8_to_linear(r, g, b);
}

constexpr std::uint32_t linear_to_aga(Color3f color) noexcept {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r = static_cast<std::uint32_t>(srgb.r * 255.0f + 0.5f) & 0xFF;
    auto g = static_cast<std::uint32_t>(srgb.g * 255.0f + 0.5f) & 0xFF;
    auto b = static_cast<std::uint32_t>(srgb.b * 255.0f + 0.5f) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

// ---------------------------------------------------------------------------
// Quantize a Color3f to OCS 12-bit precision (snap to nearest OCS color)
// ---------------------------------------------------------------------------

constexpr Color3f quantize_to_ocs(Color3f color) noexcept {
    return ocs_to_linear(linear_to_ocs(color));
}

// ---------------------------------------------------------------------------
// Generate full OCS palette (all 4096 possible 12-bit colors)
// ---------------------------------------------------------------------------

inline Palette all_ocs_colors() {
    Palette pal;
    pal.name = "ocs-4096";
    pal.colors.reserve(4096);
    for (std::uint16_t i = 0; i < 4096; ++i) {
        pal.colors.push_back(ocs_to_linear(i));
    }
    return pal;
}

// ---------------------------------------------------------------------------
// Commonly used Amiga palettes
// ---------------------------------------------------------------------------

// Default Workbench 2.0 palette (4 colors)
#if defined(__GNUC__) && !defined(__clang__)
constexpr
#else
inline
#endif
auto workbench_20_colors() {
    return std::array<Color3f, 4>{
        color_space::srgb_hex_to_linear(0x959595),  // Grey (background)
        color_space::srgb_hex_to_linear(0x000000),  // Black
        color_space::srgb_hex_to_linear(0xFFFFFF),  // White
        color_space::srgb_hex_to_linear(0x3B67A2),  // Blue
    };
}

// Classic Amiga boing ball red/white
#if defined(__GNUC__) && !defined(__clang__)
constexpr
#else
inline
#endif
auto boing_ball_colors() {
    return std::array<Color3f, 2>{
        color_space::srgb_hex_to_linear(0xFFFFFF),  // White
        color_space::srgb_hex_to_linear(0xFF0000),  // Red
    };
}

// ---------------------------------------------------------------------------
// Palette lookup by name
// ---------------------------------------------------------------------------

inline Palette by_name(std::string_view name) {
    if (name == "ocs-4096" || name == "ocs") {
        return all_ocs_colors();
    }
    if (name == "workbench") {
        auto colors = workbench_20_colors();
        return Palette{"workbench", {colors.begin(), colors.end()}};
    }
    // Default: full OCS palette for maximum flexibility
    return all_ocs_colors();
}

// ---------------------------------------------------------------------------
// Find nearest palette color (brute force, perceptual in OKLab)
// ---------------------------------------------------------------------------

inline std::size_t find_nearest(Color3f color,
                                std::span<const Color3f> palette) noexcept {
    std::size_t best = 0;
    float best_dist = color_space::perceptual_distance_sq(color, palette[0]);

    for (std::size_t i = 1; i < palette.size(); ++i) {
        float dist = color_space::perceptual_distance_sq(color, palette[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// EHB (Extra Half-Brite) palette generation
//
// Given a set of 32 base colors, produce 64-color palette where entries
// 32-63 are the half-brightness versions of entries 0-31.
// The Amiga hardware generates half-brite by halving each RGB component.
// We compute this in sRGB space (as the hardware operates on DAC values).
// ---------------------------------------------------------------------------

inline Palette make_ehb_palette(std::span<const Color3f> base_colors) {
    Palette pal;
    pal.name = "ehb";
    pal.colors.reserve(64);

    // First 32 entries: base colors
    for (auto& c : base_colors) {
        pal.colors.push_back(c);
    }
    // Pad to 32 if fewer were provided
    while (pal.colors.size() < 32) {
        pal.colors.push_back(Color3f{0.0f, 0.0f, 0.0f});
    }

    // Entries 32-63: half-brightness copies
    // The Amiga EHB hardware halves each sRGB DAC value, so we convert to
    // sRGB, halve, and convert back to linear.
    for (std::size_t i = 0; i < 32; ++i) {
        auto srgb = color_space::linear_to_srgb(pal.colors[i]).clamped();
        Color3f half_srgb{srgb.r * 0.5f, srgb.g * 0.5f, srgb.b * 0.5f};
        pal.colors.push_back(color_space::srgb_to_linear(half_srgb));
    }

    return pal;
}

} // namespace png2amiga::palette
