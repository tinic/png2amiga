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

} // namespace png2amiga::png_io
