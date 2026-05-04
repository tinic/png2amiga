#pragma once

// Single-frame GIF89a writer. Used by `--mode gif` for benchmark
// comparisons against gifsicle / ImageMagick / etc. Not animation —
// just one image, one global colour table, LZW-compressed pixel
// indices.
//
// API mirrors png_io::save_palettized: caller hands in the dithered
// 8-bit indices and the linear-RGB palette; the writer emits a valid
// GIF89a file.

#include "types.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::gif {

// indices: row-major image data, one byte per pixel, value < palette.size().
// palette: linear-RGB colours (≤ 256). Quantises to 8-bit sRGB on write.
Result<void>
save_palettized(std::string_view path,
                std::span<const std::uint8_t> indices,
                std::span<const Color3f> palette,
                std::size_t width, std::size_t height);

// Same payload, returned in memory instead of written to disk. Useful
// for shootout / benchmark scripts that pipe results.
Result<std::vector<std::uint8_t>>
encode_palettized(std::span<const std::uint8_t> indices,
                  std::span<const Color3f> palette,
                  std::size_t width, std::size_t height);

}  // namespace png2amiga::gif
