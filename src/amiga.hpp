#pragma once

#include <cstddef>
#include <utility>

namespace png2amiga::amiga {

// ---------------------------------------------------------------------------
// Amiga graphics modes
// ---------------------------------------------------------------------------

enum class Mode : unsigned char {
    // Standard bitplane modes (OCS/ECS/AGA)
    lores,              // 320-wide, square pixels
    lores_interlace,    // 320-wide, interlaced (wide pixels: 2:1)
    hires,              // 640-wide (tall pixels: 1:2)
    hires_interlace,    // 640-wide, interlaced (square pixels)

    // Hold-And-Modify modes
    ham6,               // OCS/ECS: 6 bitplanes, 16 base + modify R/G/B
    ham8,               // AGA only: 8 bitplanes, 64 base + modify R/G/B

    // Extra Half-Brite
    ehb,                // OCS/ECS: 6 bitplanes, 32 colors + 32 half-bright

    // Atari ST/STE
    stf_low,            // STF 320x200, 4 bitplanes, 16 colors, 9-bit palette
    stf_med,            // STF 640x200, 2 bitplanes, 4 colors, 9-bit palette
    stf_hi,             // STF 640x400, 1 bitplane, monochrome
    ste_low,            // STE 320x200, 4 bitplanes, 16 colors, 12-bit palette
    ste_med,            // STE 640x200, 2 bitplanes, 4 colors, 12-bit palette
    ste_hi,             // STE 640x400, 1 bitplane, monochrome
};

// ---------------------------------------------------------------------------
// Chipset capability
// ---------------------------------------------------------------------------

enum class Chipset : unsigned char {
    ocs,    // Original Chip Set (A1000, A500, A2000) — 12-bit color, 6 bitplanes
    ecs,    // Enhanced Chip Set (A500+, A600, A3000) — same palette as OCS
    aga,    // Advanced Graphics Architecture (A1200, A4000) — 24-bit color, 8 bitplanes
};

// ---------------------------------------------------------------------------
// ModeParams — runtime parameters for a given mode
//
// screen_height is 0 — callers compute it from source aspect ratio.
// ---------------------------------------------------------------------------

struct ModeParams {
    std::size_t screen_width;
    std::size_t screen_height;      // 0 = compute from source aspect ratio
    std::size_t bitplane_depth;     // number of bitplanes (1-8)
    std::size_t max_colors;         // 2^depth (or special for HAM/EHB)
    bool is_ham;
    bool is_ehb;
    bool is_hires;
    bool is_interlaced;
    // Preview pixel scaling: 1=normal, 2=double that axis
    std::size_t preview_scale_x;    // 2 for lores_interlace (wide pixels)
    std::size_t preview_scale_y;    // 2 for hires (tall pixels)
};

// ---------------------------------------------------------------------------
// Resolution presets
// ---------------------------------------------------------------------------

constexpr ModeParams get_mode_params(Mode mode) noexcept {
    //                     w    h  dp  col  ham   ehb   hi    lace  sx sy
    switch (mode) {
    case Mode::lores:
        return {320, 0, 5, 32,  false, false, false, false, 1, 1};
    case Mode::lores_interlace:
        return {320, 0, 5, 32,  false, false, false, true,  2, 1}; // wide pixels
    case Mode::hires:
        return {640, 0, 4, 16,  false, false, true,  false, 1, 2}; // tall pixels
    case Mode::hires_interlace:
        return {640, 0, 4, 16,  false, false, true,  true,  1, 1}; // square
    case Mode::ham6:
        return {320, 0, 6, 16,  true,  false, false, false, 1, 1};
    case Mode::ham8:
        return {320, 0, 8, 64,  true,  false, false, false, 1, 1};
    case Mode::ehb:
        return {320, 0, 6, 64,  false, true,  false, false, 1, 1};
    // Atari ST/STE — fixed 200 lines, square pixels (low) or tall pixels (med)
    case Mode::stf_low:
        return {320, 200, 4, 16, false, false, false, false, 2, 2};
    case Mode::stf_med:
        return {640, 200, 2,  4, false, false, true,  false, 1, 2};
    case Mode::ste_low:
        return {320, 200, 4, 16, false, false, false, false, 2, 2};
    case Mode::ste_med:
        return {640, 200, 2,  4, false, false, true,  false, 1, 2};
    case Mode::stf_hi:
    case Mode::ste_hi:
        return {640, 400, 1,  2, false, false, true,  false, 1, 1};
    }
    std::unreachable();
}

// Default width for a mode (320 for lores/ham/ehb, 640 for hires)
constexpr std::size_t default_width(Mode mode) noexcept {
    return get_mode_params(mode).screen_width;
}

// Check if a mode is any HAM variant
constexpr bool is_ham(Mode mode) noexcept {
    return get_mode_params(mode).is_ham;
}

// Get HAM data bits (bitplanes - 2). Only meaningful for HAM modes.
constexpr std::size_t ham_data_bits(Mode mode) noexcept {
    return get_mode_params(mode).bitplane_depth - 2;
}

// Get HAM base palette size (2^data_bits). Only meaningful for HAM modes.
constexpr std::size_t ham_base_colors(Mode mode) noexcept {
    return std::size_t{1} << ham_data_bits(mode);
}

// Check if a mode is an Atari ST/STE mode
constexpr bool is_atari(Mode mode) noexcept {
    return mode == Mode::stf_low || mode == Mode::stf_med || mode == Mode::stf_hi ||
           mode == Mode::ste_low || mode == Mode::ste_med || mode == Mode::ste_hi;
}

// Check if a mode is Atari STF (9-bit palette)
constexpr bool is_stf(Mode mode) noexcept {
    return mode == Mode::stf_low || mode == Mode::stf_med || mode == Mode::stf_hi;
}

// Check if a mode is Atari monochrome high-res
constexpr bool is_atari_hi(Mode mode) noexcept {
    return mode == Mode::stf_hi || mode == Mode::ste_hi;
}

// Maximum bitplane depth for a chipset (raw hardware limit)
constexpr std::size_t max_depth(Chipset chipset) noexcept {
    switch (chipset) {
    case Chipset::ocs:
    case Chipset::ecs:
        return 6;
    case Chipset::aga:
        return 8;
    }
    std::unreachable();
}

// Maximum user-configurable depth for a mode + chipset.
// HAM/EHB have fixed depths (not user-configurable).
// Standard modes: OCS lores=5, hires=4; AGA=8.
constexpr std::size_t max_user_depth(Mode mode, Chipset chipset) noexcept {
    // HAM/EHB/Atari depths are fixed by the mode, not user-configurable
    if (is_ham(mode)) return get_mode_params(mode).bitplane_depth;
    if (mode == Mode::ehb) return 6;
    if (is_atari(mode)) return get_mode_params(mode).bitplane_depth;

    // AGA standard modes
    if (chipset == Chipset::aga) return 8;

    // OCS/ECS: hires max 4, lores max 5
    // (6th bitplane is reserved for HAM/EHB)
    if (mode == Mode::hires || mode == Mode::hires_interlace) return 4;
    return 5;  // lores / lores_interlace
}

// Bits per color channel for a chipset
constexpr std::size_t color_bits(Chipset chipset) noexcept {
    switch (chipset) {
    case Chipset::ocs:
    case Chipset::ecs:
        return 4;   // 12-bit color (4 bits per channel)
    case Chipset::aga:
        return 8;   // 24-bit color (8 bits per channel)
    }
    std::unreachable();
}

} // namespace png2amiga::amiga
