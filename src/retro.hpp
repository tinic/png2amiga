#pragma once
#include "amiga.hpp"
#include "dither.hpp"
#include <span>
#include <string>
namespace png2amiga::retro {
struct EncodeResult {
    Image rendered;
    std::vector<Color3f> palette;
    std::vector<std::uint8_t> bytes;
    float total_error = 0;
};
// SPU register lifetime, not rectangular palette bands.
constexpr std::size_t spectrum_slot(std::size_t x, std::size_t c) {
    int boundary = 10 * static_cast<int>(c) + ((c & 1) ? -5 : 1);
    return c + (static_cast<int>(x) < boundary         ? 0
                : static_cast<int>(x) < boundary + 160 ? 16
                                                       : 32);
}
Result<EncodeResult> encode(const Image&, amiga::Mode, const dither::Settings&);
Result<std::vector<std::uint8_t>> executable(amiga::Mode, std::span<const std::uint8_t>);
Result<std::string> viewer_source(amiga::Mode, std::span<const std::uint8_t>);
}  // namespace png2amiga::retro
