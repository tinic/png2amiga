#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "types.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::degas {

// Write Degas Elite .PI1 (low-res) or .PI2 (med-res) file.
// Bitplane data must be in word_interleaved layout.
Result<std::vector<std::uint8_t>> encode(
    const bitplane::BitplaneData& planes,
    std::span<const Color3f> palette,
    amiga::Mode mode);

Result<void> save(std::string_view path,
                  const bitplane::BitplaneData& planes,
                  std::span<const Color3f> palette,
                  amiga::Mode mode);

} // namespace png2amiga::degas
