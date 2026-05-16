#pragma once

// Thin glyph-access wrapper that exposes only the CGA 8x8 font data
// + a per-scanline accessor — WITHOUT the consteval canonical-bitmap
// initializer in cga_font.hpp.
//
// Why two headers: cga_font.hpp's compile-time bitmap dedup (used by
// the glyph-matching cga-text encoder) costs ~30M constexpr ops to
// evaluate, which pushes large translation units past GCC's
// -fconstexpr-ops-limit when combined with our other constexpr work.
// Consumers that only need read-only glyph bytes (e.g. the cga-
// composite-text artifact-decode encoder in api.cpp) can include
// this header instead and pay none of that cost.

#include <array>
#include <cstdint>

namespace png2amiga::palette {

#include "cga_font_data.inc"  // inline constexpr kCgaFont8x8 (2 KB)

// Get a specific scanline (0..7) of CGA glyph `ch` as an 8-bit pattern.
// MSB-first: bit 7 = leftmost pixel.
constexpr std::uint8_t cga_glyph_scanline(std::uint8_t ch, std::size_t line) noexcept {
    return kCgaFont8x8[static_cast<std::size_t>(ch) * 8 + (line & 7)];
}

}  // namespace png2amiga::palette
