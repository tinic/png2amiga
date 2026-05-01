#pragma once

#include "amiga.hpp"
#include "api.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace png2amiga::palette_locks {

using LockSpec = api::LockSpec;
using PinSpec  = api::PinSpec;

// ---------------------------------------------------------------------------
// Convert a LockSpec (sRGB 0-255) into a linear Color3f, snapped to the
// chipset/mode precision (OCS 12-bit nibble replication, STF 9-bit, AGA raw).
// ---------------------------------------------------------------------------
Color3f to_color(const LockSpec& lock,
                 amiga::Chipset chipset,
                 amiga::Mode mode);

// ---------------------------------------------------------------------------
// Validate locks against the palette size. Errors:
//  - index out of range
//  - duplicate indices
//  - color components out of range
// ---------------------------------------------------------------------------
Result<void> validate_locks(const std::vector<LockSpec>& locks,
                            std::size_t max_colors);

// ---------------------------------------------------------------------------
// Validate pins against the palette size and image bounds. Errors:
//  - target index out of range
//  - target equals an existing locked slot
//  - source pixel out of bounds
//  - duplicate target indices
// ---------------------------------------------------------------------------
Result<void> validate_pins(const std::vector<PinSpec>& pins,
                           const std::vector<LockSpec>& locks,
                           std::size_t max_colors,
                           std::size_t image_w,
                           std::size_t image_h,
                           bool lock_zero_black);

// ---------------------------------------------------------------------------
// True if a lock at index 0 exists (which overrides the implicit black-0).
// ---------------------------------------------------------------------------
bool has_lock_at_zero(const std::vector<LockSpec>& locks);

// ---------------------------------------------------------------------------
// Number of palette slots the quantizer should produce given lock count
// and an implicit "color 0 = black" reservation. Result clamped to >= 1.
// ---------------------------------------------------------------------------
std::size_t quant_count(std::size_t max_colors,
                        const std::vector<LockSpec>& locks,
                        bool lock_zero_black);

// ---------------------------------------------------------------------------
// Build the final palette by combining quantized colors with locked colors
// and (when applicable) the implicit black at index 0.
//
// `quantized` length must equal quant_count(max_colors, locks, lock_zero).
// Locked slots are placed at their requested indices; remaining slots take
// the quantized colors in order.
//
// Returns the assembled palette of `max_colors` entries plus a `locked` mask
// (true for any slot that pins must NOT target).
// ---------------------------------------------------------------------------
struct AssembledPalette {
    Palette palette;
    std::vector<bool> locked;
};
AssembledPalette assemble_locked_palette(
    const Palette& quantized,
    const std::vector<LockSpec>& locks,
    std::size_t max_colors,
    bool lock_zero_black,
    amiga::Chipset chipset,
    amiga::Mode mode);

// ---------------------------------------------------------------------------
// Apply pin-index swaps after dithering. Mutates the palette colors,
// the index map, and the `locked` mask (pin targets become locked too,
// so subsequent pins can't stomp them).
// ---------------------------------------------------------------------------
Result<void> apply_pins(Palette& palette,
                        std::vector<std::uint8_t>& indices,
                        std::vector<bool>& locked,
                        const std::vector<PinSpec>& pins,
                        std::size_t image_w,
                        std::size_t image_h);

} // namespace png2amiga::palette_locks
