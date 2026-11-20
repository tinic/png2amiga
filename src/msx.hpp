#pragma once

#include "amiga.hpp"
#include "dither.hpp"
#include "types.hpp"
#include <span>
#include <vector>

// Hardware reference: https://map.grauw.nl/resources/video/yamaha_v9938.pdf
// SCREEN 8 blue ramp cross-checked against openMSX SDLRasterizer::precalcColorIndex.
namespace png2amiga::msx {
struct EncodeResult {
    Image rendered;
    std::vector<Color3f> palette;
    // CPU-visible VRAM starting at address 0, including the BASIC palette
    // mirror for SCREEN 5/6/7. SCREEN 7/8 bank interleaving is handled by VDP.
    std::vector<std::uint8_t> vram;
    float total_error = 0;
};
int screen_number(amiga::Mode mode);
Result<EncodeResult> encode(const Image& image, amiga::Mode mode,
                           const dither::Settings& settings, bool refine_cells = false);
// MSX BASIC BSAVE/BLOAD seven-byte header: FE, start, inclusive end, exec (LE).
Result<std::vector<std::uint8_t>> screen_file(amiga::Mode mode,
                                            std::span<const std::uint8_t> vram);
} // namespace png2amiga::msx
