#pragma once

#include "types.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

namespace png2amiga::png_io {

Result<Image> load(std::string_view path);
Result<void> save(std::string_view path, const Image& image);

// Save with transparency mask: pixels where mask[i]==true get alpha=0
Result<void> save(std::string_view path, const Image& image,
                  const std::vector<bool>& transparency_mask);

// Encode image to in-memory PNG bytes
Result<std::vector<std::uint8_t>> encode(const Image& image);

// Encode with transparency mask
Result<std::vector<std::uint8_t>> encode(const Image& image,
                                         const std::vector<bool>& transparency_mask);

// ---------------------------------------------------------------------------
// Palettized PNG (PNG-8 with PLTE chunk).
//
// indices:           one byte per pixel (palette index)
// palette:           up to 256 linear RGB colors (sRGB-encoded into PLTE)
// width, height:     image dimensions
// transparent_index: optional palette index that should render as fully
//                    transparent (emits a tRNS chunk). -1 = no transparency.
//
// Produces a much smaller file than 24-bit RGB for low-color images.
// Caller should only use this for modes that have a single global palette
// (lores/hires/EHB without copper). HAM and copper have dynamic palettes.
// ---------------------------------------------------------------------------
Result<std::vector<std::uint8_t>> encode_palettized(
    const std::vector<std::uint8_t>& indices,
    const std::vector<Color3f>& palette,
    std::size_t width, std::size_t height,
    int transparent_index = -1);

Result<void> save_palettized(std::string_view path,
                             const std::vector<std::uint8_t>& indices,
                             const std::vector<Color3f>& palette,
                             std::size_t width, std::size_t height,
                             int transparent_index = -1);

} // namespace png2amiga::png_io
