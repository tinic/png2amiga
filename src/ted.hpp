#pragma once

// Commodore TED (Plus/4, C16) encoders. Two modes:
//
//   ted_320x200 — hires, 2 colors per 8×8 cell (C64-hires-like).
//   ted_160x200 — multicolor, 4 colors per 4×8 cell: 2 global (whole-image
//                 background pair $FF15/$FF16) + 2 per-cell (C64-multicolor-
//                 like). 160 logical px → 2:1 hardware doubling.
//
// The TED palette is FIXED: 121 unique colors selected by a color byte
// (luma<<4)|chroma, luma = bits 6-4 (0..7), chroma = bits 3-0 (0..15);
// chroma 0 = black at every luma. The encoder picks the nearest of the 121
// (OKLab) and emits the corresponding luma/chroma nibbles.

#include "amiga.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstdint>
#include <vector>

namespace png2amiga::ted {

struct EncodeResult {
    Image rendered;                    // preview at the logical buffer size
    std::vector<std::uint8_t> bitmap;  // 8000 bytes (1bpp hires / 2bpp mc)
    std::vector<std::uint8_t> luma;    // 1000 bytes (per 8×8 / 4×8 cell)
    std::vector<std::uint8_t> chroma;  // 1000 bytes
    std::uint8_t bg0 = 0;              // multicolor only: $FF15 color byte
    std::uint8_t bg1 = 0;              // multicolor only: $FF16 color byte
    float total_error = 0.0f;
};

// Encode an image already at the mode's native dimensions (320×200 hires /
// 160×200 multicolor).
Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings = {});

}  // namespace png2amiga::ted
