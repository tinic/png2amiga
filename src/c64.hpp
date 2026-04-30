#pragma once

// Commodore 64 / VIC-II encoder. Initial scope: c64-multicolor only,
// 160×200 logical resolution at 4 colours per 4×8 cell (1 shared
// background + 3 per-cell foregrounds). Brute-force per-cell triple
// search using the fixed 16-colour Pepto palette. The encoder
// produces a rendered preview Image at the logical 160×200 raster;
// the pipeline's preview_scale handles 2:1 hardware doubling.
//
// This is the first proof-of-fit of the png2c64 → png2amiga merge:
// c64 chipset + one mode end-to-end. Future work adds c64-hires,
// FLI/AFLI, sprite, charset, and .prg / .crt writers.

#include "amiga.hpp"
#include "color_space.hpp"
#include "types.hpp"

#include <span>

namespace png2amiga::c64 {

struct EncodeResult {
    Image rendered;                              // 160×200 logical preview
    std::vector<std::uint8_t> bitmap;            // 8000 bytes (40 cols × 25 rows × 8)
    std::vector<std::uint8_t> screen_ram;        // 1000 bytes (1 nibble pair / cell)
    std::vector<std::uint8_t> color_ram;         // 1000 bytes (1 nibble / cell)
    std::uint8_t bg_color = 0;                   // shared background colour 0..15
};

// Encode a 160×200 logical image to c64-multicolor. The input image is
// resampled to 160×200 by the pipeline before this is called. Uses the
// Pepto palette by default (kC64Pepto in palette.hpp).
Result<EncodeResult> encode_multicolor(const Image& image);

// VIC-II Pepto palette as a span of linear-RGB Color3f. Defined in
// c64.cpp (lazy-init from kC64Pepto).
std::span<const Color3f, 16> pepto_palette();

}  // namespace png2amiga::c64
