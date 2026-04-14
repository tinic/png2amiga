#pragma once

// CGA 8x8 font data. Used by cga_text80x100 / cga_text80x200 graphics modes
// for per-cell glyph matching.
//
// Source: viler-int10h/vga-text-mode-fonts (CGA.F08), a public-domain
// reconstruction of the IBM PC 5150/5160 Color Graphics Adapter character
// ROM. 256 glyphs × 8 scanlines × 1 byte each = 2048 bytes total. Each
// byte is an 8-pixel scanline, MSB-first: bit 7 = leftmost pixel.

#include <array>
#include <cstdint>

namespace png2amiga::palette {

#include "cga_font_data.inc"
#include "ega_font_data.inc"

// Get a specific scanline (0..7) of CGA glyph `ch` as an 8-bit pattern.
constexpr std::uint8_t cga_glyph_scanline(std::uint8_t ch, std::size_t line) noexcept {
    return kCgaFont8x8[static_cast<std::size_t>(ch) * 8 + (line & 7)];
}

// Get a specific scanline (0..13) of EGA 8x14 glyph `ch`.
constexpr std::uint8_t ega_glyph_scanline(std::uint8_t ch, std::size_t line) noexcept {
    return kEga8x14Font[static_cast<std::size_t>(ch) * 14 +
                        (line < 14 ? line : line % 14)];
}

// Font descriptor passed to glyph-matching encoders. Covers both CGA 8x8
// (used in standard text mode) and EGA 8x14 (used at 640x350).
struct FontRef {
    const std::uint8_t* data;       // pointer to contiguous glyph bytes
    std::size_t glyph_height;       // scanlines per glyph (8 or 14)
};

inline constexpr FontRef kFontCga8x8{kCgaFont8x8.data(), 8};
inline constexpr FontRef kFontEga8x14{kEga8x14Font.data(), 14};

constexpr std::uint8_t font_scanline(const FontRef& f, std::uint8_t ch,
                                     std::size_t line) noexcept {
    return f.data[static_cast<std::size_t>(ch) * f.glyph_height +
                  (line < f.glyph_height ? line : line % f.glyph_height)];
}

} // namespace png2amiga::palette
