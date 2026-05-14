#pragma once

#include "types.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::palette_io {

// ---------------------------------------------------------------------------
// Palette file I/O
//
// Supported formats (auto-detected from file content):
//
// 1. GIMP Palette (.gpl) — text file starting with "GIMP Palette" header,
//    then R G B lines (0-255 per channel), optional tab-separated names.
//
// 2. IFF CMAP — binary IFF file containing a CMAP chunk (3 bytes per color:
//    R, G, B in sRGB 0-255). Typically from Deluxe Paint .iff/.ilbm files.
//
// 3. Text hex — one RRGGBB hex color per line (like ProMotion NG exports).
//    Lines starting with '#' or ';' are comments. Empty lines are skipped.
//
// All formats return colors in linear RGB (Color3f), ready for the pipeline.
// ---------------------------------------------------------------------------

// Load a palette from a file, auto-detecting format.
// Returns linear RGB colors.
Result<Palette> load_palette(std::string_view path);

// Load from in-memory bytes, auto-detecting format.
Result<Palette> load_palette_from_memory(std::span<const std::uint8_t> data);

// ---------------------------------------------------------------------------
// Write OCS 12-bit palette to file
//
// Writes palette as big-endian 16-bit values in 0x0RGB format (OCS 12-bit).
// 2 bytes per color, suitable for direct hardware register loading.
// ---------------------------------------------------------------------------

Result<void> save_ocs_palette(std::string_view path, std::span<const Color3f> palette);

Result<std::vector<std::uint8_t>> encode_ocs_palette(std::span<const Color3f> palette);

}  // namespace png2amiga::palette_io
