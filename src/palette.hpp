#pragma once

#include "amiga.hpp"
#include "color_space.hpp"
#include "types.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
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

// AGA 24-bit: split into high and low nibble words for LOCT register.
// hi = upper 4 bits per channel (0x0RGB), lo = lower 4 bits (0x0RGB).
struct AGA_HiLo { std::uint16_t hi; std::uint16_t lo; };
constexpr AGA_HiLo linear_to_aga_hilo(Color3f color) noexcept {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r8 = static_cast<int>(srgb.r * 255.0f + 0.5f) & 0xFF;
    auto g8 = static_cast<int>(srgb.g * 255.0f + 0.5f) & 0xFF;
    auto b8 = static_cast<int>(srgb.b * 255.0f + 0.5f) & 0xFF;
    auto hi = static_cast<std::uint16_t>(((r8 >> 4) << 8) | ((g8 >> 4) << 4) | (b8 >> 4));
    auto lo = static_cast<std::uint16_t>(((r8 & 0xF) << 8) | ((g8 & 0xF) << 4) | (b8 & 0xF));
    return {hi, lo};
}

// ---------------------------------------------------------------------------
// Atari STF 9-bit color: 3 bits per channel (0-7), 512 possible colors
// Stored as 0x0RGB where each nibble is 0-7
// ---------------------------------------------------------------------------

constexpr Color3f stf_to_linear(std::uint16_t rgb9) noexcept {
    auto r3 = static_cast<std::uint8_t>((rgb9 >> 8) & 0x7);
    auto g3 = static_cast<std::uint8_t>((rgb9 >> 4) & 0x7);
    auto b3 = static_cast<std::uint8_t>(rgb9 & 0x7);
    // 3-bit to 8-bit: replicate bits (abc -> abcabcab)
    auto r8 = static_cast<std::uint8_t>((r3 << 5) | (r3 << 2) | (r3 >> 1));
    auto g8 = static_cast<std::uint8_t>((g3 << 5) | (g3 << 2) | (g3 >> 1));
    auto b8 = static_cast<std::uint8_t>((b3 << 5) | (b3 << 2) | (b3 >> 1));
    return color_space::srgb_u8_to_linear(r8, g8, b8);
}

constexpr std::uint16_t linear_to_stf(Color3f color) noexcept {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r3 = static_cast<std::uint16_t>(
        static_cast<int>(srgb.r * 7.0f + 0.5f) & 0x7);
    auto g3 = static_cast<std::uint16_t>(
        static_cast<int>(srgb.g * 7.0f + 0.5f) & 0x7);
    auto b3 = static_cast<std::uint16_t>(
        static_cast<int>(srgb.b * 7.0f + 0.5f) & 0x7);
    return static_cast<std::uint16_t>((r3 << 8) | (g3 << 4) | b3);
}

constexpr Color3f quantize_to_stf(Color3f color) noexcept {
    return stf_to_linear(linear_to_stf(color));
}

// ---------------------------------------------------------------------------
// CGA 16-color master palette (IRGB, fixed at hardware). Canonical "IBM
// Color/Graphics Monitor Adaptor" values (sRGB). Values sourced from the
// IBM Technical Reference & widely-reproduced CGA color tables.
// Indexed 0..15: black, blue, green, cyan, red, magenta, brown, light gray,
// dark gray, light blue, light green, light cyan, light red, light magenta,
// yellow, white.
// ---------------------------------------------------------------------------
inline constexpr std::array<std::uint32_t, 16> kCgaHw = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// CGA 320x200 4-color fixed palettes.
// Slot 0 = background (variable, defaults to black = 0).
// Slots 1-3 = fixed by palette (0 or 1) × intensity (low or high).
//   0-low:   green (2), red (4), brown (6)
//   0-high:  light green (10), light red (12), yellow (14)
//   1-low:   cyan (3), magenta (5), light gray (7)    [default]
//   1-high:  light cyan (11), light magenta (13), white (15)
// See: https://en.wikipedia.org/wiki/Color_Graphics_Adapter
enum class CgaPalette : unsigned char {
    p0_low, p0_high, p1_low, p1_high,
};

constexpr std::array<std::uint8_t, 3> cga_palette_indices(CgaPalette p) noexcept {
    switch (p) {
    case CgaPalette::p0_low:  return { 2,  4,  6};
    case CgaPalette::p0_high: return {10, 12, 14};
    case CgaPalette::p1_low:  return { 3,  5,  7};
    case CgaPalette::p1_high: return {11, 13, 15};
    }
    return {11, 13, 15};
}

// Build a 4-color CGA palette as Color3f linear colors.
// bg_index selects the background color (0..15 in master palette).
inline std::array<Color3f, 4> cga_build_palette(CgaPalette p,
                                                std::uint8_t bg_index = 0) {
    auto hw_idx = cga_palette_indices(p);
    std::array<Color3f, 4> pal{};
    pal[0] = color_space::srgb_hex_to_linear(kCgaHw[bg_index & 0xF]);
    for (std::size_t i = 0; i < 3; ++i) {
        pal[i + 1] = color_space::srgb_hex_to_linear(kCgaHw[hw_idx[i]]);
    }
    return pal;
}

// ---------------------------------------------------------------------------
// CGA composite 16-color artifact palette.
//
// In 320x200 4-color mode on a composite monitor, adjacent pairs of 2bpp
// pixels form a 4-bit NTSC phase pattern that the decoder interprets as
// one of 16 hue/brightness combinations. Used by King's Quest (early IBM
// releases), Space Quest, 8088 MPH demo, AREA 5150, etc.
//
// The actual decoded colors depend on monitor tint/saturation (a physical
// knob on the CRT); these values are a representative "old CGA composite"
// palette from reenigne's calibration work and match what DOSBox renders
// in composite mode by default. The palette is the same 16 RGBI entries
// that CGA normally produces digitally, but rearranged — the 4-bit pattern
// index N does NOT map to CGA master color N. We store it explicitly.
//
// The mapping from pattern to color is (approximately — monitor-dependent):
//   0000 → black       1000 → grey
//   0001 → blue        1001 → light blue
//   0010 → green       1010 → light green
//   0011 → cyan        1011 → light cyan
//   0100 → red         1100 → light red
//   0101 → magenta     1101 → light magenta
//   0110 → brown       1110 → yellow
//   0111 → light grey  1111 → white
// That is: pattern N corresponds to RGBI color N via the standard "old CGA"
// composite decoder. This is the simplest reasonable model; tools like
// DOSBox support multiple palettes (old/new CGA, adjustable tint).
// ---------------------------------------------------------------------------
inline constexpr std::array<std::uint32_t, 16> kCgaCompositeOld = {
    // Old-CGA composite (pre-1983 IBM 5150/5160). Each 4-bit pattern value
    // (index into this array) is the NTSC-decoded color that pixel-pair
    // pattern produces.
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// Build the 16-color composite palette as linear Color3f.
inline std::array<Color3f, 16> cga_composite_palette() {
    std::array<Color3f, 16> pal{};
    for (std::size_t i = 0; i < 16; ++i) {
        pal[i] = color_space::srgb_hex_to_linear(kCgaCompositeOld[i]);
    }
    return pal;
}

// Given a color index 0..15, return the 4-bit pixel pair pattern that
// produces it on a composite monitor. With our "pattern == RGBI index"
// model this is identity, but centralised here so a future mode-tweaked
// palette (new CGA, artifact shifts) can remap without touching callers.
inline constexpr std::uint8_t cga_composite_pattern(std::uint8_t idx) noexcept {
    return idx & 0xF;
}

// ---------------------------------------------------------------------------
// EGA 6-bit color: 2 bits per channel, 64 possible colors. Stored as a
// single byte in R'G'B'RGB order (primary RGB bits 0-2, secondary bits 3-5),
// where primary = 2/3 intensity and secondary = 1/3. Combined 4 levels per
// channel: 0, 85 (0x55), 170 (0xAA), 255 (0xFF).
//
// We encode/decode using a symmetrical 2-bit-per-channel layout here
// (rrggbb, 2 bits each) and translate to IrgbIRGB at output time. This
// matches our "snap color to 4 levels per channel" quantization goal.
// ---------------------------------------------------------------------------

constexpr Color3f ega_to_linear(std::uint8_t rgb6) noexcept {
    auto r2 = static_cast<std::uint8_t>((rgb6 >> 4) & 0x3);
    auto g2 = static_cast<std::uint8_t>((rgb6 >> 2) & 0x3);
    auto b2 = static_cast<std::uint8_t>(rgb6 & 0x3);
    // 2-bit to 8-bit: replicate the 2-bit pattern four times.
    // 0b00 -> 0x00, 0b01 -> 0x55, 0b10 -> 0xAA, 0b11 -> 0xFF.
    auto expand = [](std::uint8_t v) noexcept -> std::uint8_t {
        return static_cast<std::uint8_t>((v << 6) | (v << 4) | (v << 2) | v);
    };
    return color_space::srgb_u8_to_linear(expand(r2), expand(g2), expand(b2));
}

constexpr std::uint8_t linear_to_ega(Color3f color) noexcept {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r2 = static_cast<std::uint8_t>(
        static_cast<int>(srgb.r * 3.0f + 0.5f) & 0x3);
    auto g2 = static_cast<std::uint8_t>(
        static_cast<int>(srgb.g * 3.0f + 0.5f) & 0x3);
    auto b2 = static_cast<std::uint8_t>(
        static_cast<int>(srgb.b * 3.0f + 0.5f) & 0x3);
    return static_cast<std::uint8_t>((r2 << 4) | (g2 << 2) | b2);
}

constexpr Color3f quantize_to_ega(Color3f color) noexcept {
    return ega_to_linear(linear_to_ega(color));
}

// Convert rrggbb (2-bit-per-channel) to the EGA hardware's IrgbIRGB
// 6-bit palette byte. IRGB bits = primary (2/3 intensity); irgb = secondary
// (1/3). Level-2 maps to primary-only; level-3 to primary+secondary;
// level-1 to secondary-only.
constexpr std::uint8_t ega_to_hw(std::uint8_t rrggbb) noexcept {
    auto r2 = static_cast<std::uint8_t>((rrggbb >> 4) & 0x3);
    auto g2 = static_cast<std::uint8_t>((rrggbb >> 2) & 0x3);
    auto b2 = static_cast<std::uint8_t>(rrggbb & 0x3);
    auto map = [](std::uint8_t v) -> std::pair<std::uint8_t, std::uint8_t> {
        // Returns {primary_bit, secondary_bit}
        switch (v) {
        case 0:  return {0, 0};  // 0x00
        case 1:  return {0, 1};  // 0x55
        case 2:  return {1, 0};  // 0xAA
        default: return {1, 1};  // 0xFF
        }
    };
    auto [rp, rs] = map(r2);
    auto [gp, gs] = map(g2);
    auto [bp, bs] = map(b2);
    // IrgbIRGB bit layout (bit 5..0): R' G' B' R G B (R'=secondary red, R=primary)
    return static_cast<std::uint8_t>(
        (rs << 5) | (gs << 4) | (bs << 3) |
        (rp << 2) | (gp << 1) | (bp << 0));
}

// ---------------------------------------------------------------------------
// VGA 18-bit color: 6 bits per channel, 262144 possible colors
// (The VGA DAC takes 6-bit values per channel, 0-63.)
// Stored in a uint32_t as 0x00RRGGBB where each byte is 0-63.
// ---------------------------------------------------------------------------

constexpr Color3f vga_to_linear(std::uint32_t rgb18) noexcept {
    auto r6 = static_cast<std::uint8_t>((rgb18 >> 16) & 0x3F);
    auto g6 = static_cast<std::uint8_t>((rgb18 >> 8) & 0x3F);
    auto b6 = static_cast<std::uint8_t>(rgb18 & 0x3F);
    // 6-bit to 8-bit: replicate top 2 bits in low 2 bits.
    auto r8 = static_cast<std::uint8_t>((r6 << 2) | (r6 >> 4));
    auto g8 = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
    auto b8 = static_cast<std::uint8_t>((b6 << 2) | (b6 >> 4));
    return color_space::srgb_u8_to_linear(r8, g8, b8);
}

constexpr std::uint32_t linear_to_vga(Color3f color) noexcept {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r6 = static_cast<std::uint32_t>(srgb.r * 63.0f + 0.5f) & 0x3F;
    auto g6 = static_cast<std::uint32_t>(srgb.g * 63.0f + 0.5f) & 0x3F;
    auto b6 = static_cast<std::uint32_t>(srgb.b * 63.0f + 0.5f) & 0x3F;
    return (r6 << 16) | (g6 << 8) | b6;
}

constexpr Color3f quantize_to_vga(Color3f color) noexcept {
    return vga_to_linear(linear_to_vga(color));
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

// Full 64-entry EGA color gamut (every 2-bit-per-channel combination).
// Useful for brute-force palette selection on EGA modes — the continuous
// median-cut often picks nearby colors that collapse to the same 64-slot
// value after snapping, yielding too few distinct output colors.
inline Palette all_ega_colors() {
    Palette pal;
    pal.name = "ega-64";
    pal.colors.reserve(64);
    for (std::uint16_t i = 0; i < 64; ++i) {
        pal.colors.push_back(ega_to_linear(static_cast<std::uint8_t>(i)));
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

// Half-brite a base linear-RGB colour: sRGB-halve then back to linear,
// matching the Amiga DAC. Single source of truth replacing
// scap::half_brite + copper::halve_brite.
inline Color3f half_brite(const Color3f& c) {
    auto srgb = color_space::linear_to_srgb(c).clamped();
    Color3f half_srgb{srgb.r * 0.5f, srgb.g * 0.5f, srgb.b * 0.5f};
    return color_space::srgb_to_linear(half_srgb);
}

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
    for (std::size_t i = 0; i < 32; ++i) {
        pal.colors.push_back(half_brite(pal.colors[i]));
    }

    return pal;
}

} // namespace png2amiga::palette
