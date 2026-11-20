#pragma once

#include "dither.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// nibnib (github.com/raphlinus/nibnib): RGB565 at 4 bits per pixel, built
// for decoding at a few cycles per pixel on an RP2350.
//
// File: u16 width, u16 height (LE), 256 × u32 LE palette, then per row the
// first two pixels as RGB565 (u16 LE each) and one palette index per
// following pixel pair, padded to a multiple of 4 bytes. Decoding a pair
// doubles the previous pixel into both halves of a u32 and adds the
// palette word (wrapping); the low half is the left pixel. The palette
// thus holds pair deltas, carries between fields included.
//
// The decoder is fixed; everything here is encoder search: a palette
// fitted to the image and a beam search over each row's codes, scored
// in OKLab against the source or the dithered RGB565 target.
// ---------------------------------------------------------------------------

namespace png2amiga::nibnib {

struct Options {
    dither::Settings dither;
    // Normal: diverse source-scored palette fit and 64-state row search.
    // Best: sweep four RGB565 fits, an original-image-scored fit, an earlier
    // palette checkpoint and decoder-aware palette refinement, then try
    // a distance-weighted diverse seed fit. Keep the encode with the
    // highest SSIMULACRA2.
    bool best = false;
    std::function<void(float, std::string_view)> on_progress;
};

struct Encoded {
    std::vector<std::uint8_t> file;  // the complete .nibnib file
    std::array<std::uint32_t, 256> palette{};
    Image rendered;         // decoded, linear RGB
    float quant_error = 0;  // summed OKLab² error vs the source
};

// `image` is linear RGB; its width must be even and at least 2.
Result<Encoded> encode(const Image& image, const Options& opts = {});

// Decodes a .nibnib file to linear RGB.
Result<Image> decode(std::span<const std::uint8_t> file);

std::size_t line_stride(std::size_t width) noexcept;

}  // namespace png2amiga::nibnib
