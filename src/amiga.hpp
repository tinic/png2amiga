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

    // IBM PC / DOS
    vga_13h,            // VGA Mode 13h: 320x200, 8bpp chunky, 256 colors, 18-bit DAC

    // VGA high-res planar: same 4-plane layout as EGA modes 10h/12h, but
    // driven by VGA's programmable 18-bit DAC — any 16 of 262144 colors.
    vga_10h,            // VGA Mode 10h: 640x350, 4 bitplanes, 16 colors
    vga_12h,            // VGA Mode 12h: 640x480, 4 bitplanes, 16 colors, square pixels

    // EGA (Enhanced Graphics Adapter) — 16 colors from a 64-entry IrgbIRGB
    // palette (2-bit per channel, 4 levels: 0, 85, 170, 255). 4-plane planar.
    ega_320,            // EGA 320x200, 4 bitplanes, 16 colors
    ega_640,            // EGA 640x200, 4 bitplanes, 16 colors
    ega_hi,             // EGA 640x350, 4 bitplanes, 16 colors

    // CGA (Color Graphics Adapter) — fixed palettes, two-bank memory.
    // 320x200 2bpp (4 colors), 640x200 1bpp (2 colors). --cga-palette
    // selects which of the fixed palette variants (0/1 × low/high intensity).
    cga_320,            // CGA 320x200, 4 colors from palette 0/1 × low/high
    cga_640,            // CGA 640x200, monochrome (bg + 1 fg)
    cga_composite,      // CGA 160x200, 16 colors via NTSC composite artifacting
                        // (stored as 320x200 2bpp; pairs of 2bpp pixels form
                        //  a 4-bit pattern that the NTSC decoder interprets
                        //  as one of 16 colors — used by King's Quest,
                        //  Space Quest, etc. and by the AREA 5150 demo).
    cga_text80x100,     // CGA 80x25 text mode with CRTC hack for 2-scanline
                        // rows: glyph-matched graphics at 80x100 "super-chunky"
                        // resolution. 16000 bytes (char+attr per cell). Fits
                        // in real CGA's 16 KB VRAM. Used in 8088 MPH's 1K-
                        // color mode, AREA 5150, many demos.

    // SNES Mode 7 — affine-transformable 8bpp background.
    snes_mode7_256,     // 256×224, 8bpp pixels = palette index, 256-entry
                        // CGRAM palette of BGR555 (5/5/5 = 32K colours).
    snes_mode7_direct,  // 256×224, 8bpp pixels = R3G3B2 low bits + hardware
                        // "Direct Color" sub-palette adds 1 high bit per
                        // channel → effective RGB443 (4/4/3 = 2048 colours)
                        // per-pixel, no palette table needed.
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
    // Pixel aspect ratio = buffer_pixel_width / buffer_pixel_height on the
    // target hardware display. 1.0 = square; 2.0 = wide (lores interlace);
    // 0.5 = tall (hires); 0.833 = VGA 320x200 tall (pixel 1.2x taller than wide).
    // Amiga modes use integer scale_x / scale_y; VGA modes set this
    // explicitly for non-integer PAR (1.2 CRT pixel).
    float par = 1.0f;
};

// ---------------------------------------------------------------------------
// Resolution presets
// ---------------------------------------------------------------------------

constexpr ModeParams get_mode_params(Mode mode) noexcept {
    //                     w    h  dp  col  ham   ehb   hi    lace  sx sy
    switch (mode) {
    case Mode::lores:
        return {320, 0, 5, 32,  false, false, false, false, 1, 1, 1.0f};
    case Mode::lores_interlace:
        return {320, 0, 5, 32,  false, false, false, true,  2, 1, 2.0f};
    case Mode::hires:
        return {640, 0, 4, 16,  false, false, true,  false, 1, 2, 0.5f};
    case Mode::hires_interlace:
        return {640, 0, 4, 16,  false, false, true,  true,  1, 1, 1.0f};
    case Mode::ham6:
        return {320, 0, 6, 16,  true,  false, false, false, 1, 1, 1.0f};
    case Mode::ham8:
        return {320, 0, 8, 64,  true,  false, false, false, 1, 1, 1.0f};
    case Mode::ehb:
        return {320, 0, 6, 64,  false, true,  false, false, 1, 1, 1.0f};
    // Atari ST/STE — fixed 200 lines, square pixels (low) or tall pixels (med)
    case Mode::stf_low:
        return {320, 200, 4, 16, false, false, false, false, 2, 2, 1.0f};
    case Mode::stf_med:
        return {640, 200, 2,  4, false, false, true,  false, 1, 2, 0.5f};
    case Mode::ste_low:
        return {320, 200, 4, 16, false, false, false, false, 2, 2, 1.0f};
    case Mode::ste_med:
        return {640, 200, 2,  4, false, false, true,  false, 1, 2, 0.5f};
    case Mode::stf_hi:
    case Mode::ste_hi:
        return {640, 400, 1,  2, false, false, true,  false, 1, 1, 1.0f};
    // VGA modes: 18-bit DAC (262144 colors), 256-color modes are chunky-8bpp
    // or Mode X / Mode Y column-planar. 320x200 displays with 1.2 tall PAR;
    // Mode X at 320x240 is native 4:3.
    case Mode::vga_13h:
        return {320, 200, 8, 256, false, false, false, false, 1, 1, 0.833f};
    // VGA Mode 10h: 640x350 planar, tall-ish pixel (~0.73 PAR, same as EGA hi).
    // VGA Mode 12h: 640x480 planar, truly square (4:3 CRT, 1:1 pixel).
    case Mode::vga_10h:
        return {640, 350, 4, 16, false, false, true, false, 1, 1, 0.730f};
    case Mode::vga_12h:
        return {640, 480, 4, 16, false, false, true, false, 1, 1, 1.0f};
    // EGA modes: 16-color planar, 64-color IrgbIRGB palette.
    // 320x200 displays 1.2 tall PAR; 640x200 displays 2.4 tall (half-width
    // of 13h); 640x350 displays ~1.37 tall (the "proper" EGA mode).
    case Mode::ega_320:
        return {320, 200, 4, 16, false, false, false, false, 1, 1, 0.833f};
    case Mode::ega_640:
        return {640, 200, 4, 16, false, false, true,  false, 1, 2, 0.417f};
    case Mode::ega_hi:
        return {640, 350, 4, 16, false, false, true,  false, 1, 1, 0.730f};
    // CGA: same 1.2 PAR as VGA 13h. cga_320 is 2bpp packed, cga_640 is 1bpp.
    case Mode::cga_320:
        return {320, 200, 2,  4, false, false, false, false, 1, 1, 0.833f};
    case Mode::cga_640:
        return {640, 200, 1,  2, false, false, true,  false, 1, 2, 0.417f};
    case Mode::cga_composite:
        // Hardware buffer is 320x200 (2bpp packed) but each composite pixel
        // spans 2 buffer pixels, yielding 160x200 effective resolution with
        // 16 colors. screen_width is the effective (composite) width; the
        // chunky-pixel stage encodes to 160x200 then we pair-pack to 320x200.
        return {160, 200, 4, 16, false, false, false, false, 2, 1, 1.667f};
    // CGA text-mode hacks: glyphs selected per-cell, 16 fg x 16 bg attr.
    // Logical display is still 640x200; "depth" is conceptually the 4-bit
    // bg + 4-bit fg attr byte width (not a bitplane count). bitplane_depth
    // reports 16 here so the palette-handling logic uses 16 colors.
    case Mode::cga_text80x100:
        return {640, 200, 4, 16, false, false, true, false, 1, 2, 0.417f};
    // SNES Mode 7 — 256×224 native, 8bpp chunky pixels. Display PAR is
    // (4/3)÷(256/224) = 896/768 ≈ 1.167 — pixels render slightly wider
    // than tall on a 4:3 CRT. Same auto-native-par opt-in plumbing as
    // the DOS modes (the web UI toggles options.nativePar on mode
    // entry).
    case Mode::snes_mode7_256:
        return {256, 224, 8, 256,  false, false, false, false, 1, 1, 1.167f};
    case Mode::snes_mode7_direct:
        // depth=8 (chunky), max_colors=2048 (effective RGB443 grid). The
        // "palette" is implicit hardware sub-palette; we quantise pixels
        // directly to the 4-4-3 grid with no separate palette table.
        return {256, 224, 8, 2048, false, false, false, false, 1, 1, 1.167f};
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

// Check if a mode is an IBM PC / DOS VGA mode
constexpr bool is_vga(Mode mode) noexcept {
    return mode == Mode::vga_13h || mode == Mode::vga_10h ||
           mode == Mode::vga_12h;
}

// Check if a mode is a VGA hires planar mode (4-plane, 16-color,
// programmable 18-bit DAC — Mode 10h 640x350 or Mode 12h 640x480).
constexpr bool is_vga_planar(Mode mode) noexcept {
    return mode == Mode::vga_10h || mode == Mode::vga_12h;
}

// Check if a mode is an IBM PC / DOS EGA mode
constexpr bool is_ega(Mode mode) noexcept {
    return mode == Mode::ega_320 || mode == Mode::ega_640 ||
           mode == Mode::ega_hi;
}

// Check if a mode is an IBM PC / DOS CGA mode
constexpr bool is_cga(Mode mode) noexcept {
    return mode == Mode::cga_320 || mode == Mode::cga_640 ||
           mode == Mode::cga_composite ||
           mode == Mode::cga_text80x100;
}

// Check if the mode uses NTSC composite artifacting to produce its colors.
// Affects encoding (pixel pairs → color) and should disable ordered/error
// dithering in ways that break the artifact pattern.
constexpr bool is_composite(Mode mode) noexcept {
    return mode == Mode::cga_composite;
}

// Check if the mode is a CGA text-mode graphics hack (glyph-per-cell).
constexpr bool is_cga_text(Mode mode) noexcept {
    return mode == Mode::cga_text80x100;
}

// Check if a mode uses chunky (1 byte per pixel) output instead of bitplane
// encoding. VGA 13h, plus the SNES Mode 7 modes — all 8bpp linear pixel
// arrays.
constexpr bool is_chunky(Mode mode) noexcept {
    return mode == Mode::vga_13h ||
           mode == Mode::snes_mode7_256 ||
           mode == Mode::snes_mode7_direct;
}

// Check if a mode is one of the SNES Mode 7 variants (256-palette or
// Direct Color RGB443).
constexpr bool is_snes(Mode mode) noexcept {
    return mode == Mode::snes_mode7_256 ||
           mode == Mode::snes_mode7_direct;
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
