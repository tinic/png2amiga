#pragma once

// SNES Mode 7 output writers.
//
// Two binary outputs:
//
//   write_snes_mode7_256(path, indices, palette)
//       Raw 8bpp pixel array (W×H bytes; pixel = palette index 0..255)
//       to `path`. Companion palette written to `path` with `.pal` swapped
//       for the binary extension: 256 × 16-bit BGR555 little-endian.
//
//   write_snes_mode7_direct(path, pixels)
//       Raw 8bpp pixel array, each byte already in BGGGRRR Direct Color
//       format (see snes_color::pack_rgb443_byte). No palette companion.
//
// `write_snes_mode7_h` emits a C header equivalent — `const uint8_t pixels[]`
// + `const uint16_t palette[]` (256 mode) — for inclusion in SNES homebrew
// projects.

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::snes_io {

// Save palette + pixel arrays to disk for snes_mode7_256 mode.
// `out_path` ends in .bin (or any extension) — the palette goes to the
// same stem with `.pal` appended.
Result<void> write_snes_mode7_256(std::string_view out_path,
                                  std::span<const std::uint8_t> indices,
                                  std::span<const Color3f> palette,
                                  std::size_t width, std::size_t height);

// Save raw Direct Color pixel array (no palette).
Result<void> write_snes_mode7_direct(std::string_view out_path,
                                     std::span<const std::uint8_t> pixels,
                                     std::size_t width, std::size_t height);

// Encode the same data as a C header. `symbol` becomes the array prefix
// (`const uint8_t {symbol}_pixels[]`, `const uint16_t {symbol}_palette[]`).
Result<std::vector<std::uint8_t>> encode_snes_mode7_256_header(
    std::span<const std::uint8_t> indices,
    std::span<const Color3f> palette,
    std::size_t width, std::size_t height,
    std::string_view symbol);

Result<std::vector<std::uint8_t>> encode_snes_mode7_direct_header(
    std::span<const std::uint8_t> pixels,
    std::size_t width, std::size_t height,
    std::string_view symbol);

} // namespace png2amiga::snes_io
