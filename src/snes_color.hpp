#pragma once

// SNES Mode 7 colour quanta.
//
// Two formats covered:
//
//   * BGR555  — native SNES CGRAM entry (5/5/5 bits, 32 768 colours).
//                Used by snes_mode7_256: 256-entry palette of BGR555 values.
//
//   * RGB443  — effective output of SNES Mode 7 "Direct Color" (4/4/3 bits,
//                2048 colours). The 8-bit pixel byte carries the LOW bits
//                (3R + 3G + 2B) and the hardware sub-palette adds 1 high bit
//                per channel; the resulting display colour lives on the
//                4-4-3 grid. We quantise source pixels to that grid for the
//                snes_mode7_direct mode and pack the 8-bit pixel byte from
//                the lower R3G3B2 bits.
//
// All operations work in linear RGB (sRGB → linear → quantise → linear →
// sRGB) so the dither pipeline scores against the perceptually-correct
// quantised palette in OKLab without surprises at the gamma boundary.

#include "types.hpp"

#include <cstdint>

namespace png2amiga::snes_color {

// Snap a single linear-RGB sample to the BGR555 grid (5 bits per channel
// in sRGB space) and return the snapped linear colour.
Color3f bgr555_quantize(Color3f linear) noexcept;

// Snap a single linear-RGB sample to the RGB443 grid (R: 4 bits, G: 4 bits,
// B: 3 bits, all in sRGB space) and return the snapped linear colour.
Color3f rgb443_quantize(Color3f linear) noexcept;

// Pack a linear-RGB colour into a 16-bit BGR555 word (CGRAM ready), little-
// endian on output. Format: 0bbbbbgggggrrrrr in big-endian terms; in C
// the bits land as ((b & 0x1F) << 10) | ((g & 0x1F) << 5) | (r & 0x1F).
std::uint16_t to_bgr555_word(Color3f linear) noexcept;

// Pack a linear-RGB colour into the 8-bit Direct Color pixel byte:
// bits = bbgggrrr (lowest 2 bits of B, lowest 3 bits of G, lowest 3 bits
// of R). On real hardware the upper R/G/B bits come from the active
// sub-palette; here we just emit the low-bit pixel byte and the encoder
// trusts the receiver to use the right palette entry to recover the full
// 4-4-3 colour.
std::uint8_t pack_rgb443_byte(Color3f linear) noexcept;

} // namespace png2amiga::snes_color
