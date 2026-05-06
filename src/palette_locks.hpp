#pragma once

#include "amiga.hpp"
#include "api.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace png2amiga::palette_locks {

using LockSpec    = api::LockSpec;
using PinSpec     = api::PinSpec;
using ReserveSpec = api::ReserveSpec;

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
// Validate reserves: in-range, no duplicates, no overlap with locks
// or with the implicit black-zero. Returns max_in_palette = the
// number of reserve entries that fall within max_colors (entries
// from open-end ranges may have been parsed beyond max_colors and
// are silently clipped here).
// ---------------------------------------------------------------------------
Result<std::size_t> validate_reserves(
    const std::vector<ReserveSpec>& reserves,
    const std::vector<LockSpec>& locks,
    std::size_t max_colors,
    bool lock_zero_black);

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
// Finalize a quantizer-built palette for a slot count when slot 0 is
// optionally reserved for black. Used by every code path that calls
// quantize directly + insert/prepend (0,0,0) at slot 0:
//
//   • copper::encode_copper auto-quantize path
//   • ham::choose_ham_palette
//   • api.cpp's EHB / sliced+EHB / strips+EHB seed_pal builders
//   • strips.cpp's seed_pal builder
//
// Without this dedupe these paths can emit two slots both showing
// #000000 (the locked-black at slot 0 and the quantizer's natural
// black at slot 1) — a wasted palette entry. The helper:
//
//   1. If lock_color0 and the input contains a bit-exact-black entry,
//      drop the FIRST such occurrence (so the prepend doesn't dup).
//      Continuous near-black floats are NOT dropped — they stay in
//      the palette even if their byte-display is #000000, because
//      they carry distinct centroid information.
//   2. If lock_color0, prepend (0,0,0) at slot 0.
//   3. Pad with (0,0,0) if size < num_colors.
//   4. Trim if size > num_colors.
//
// Caller is expected to ask the quantizer for at least num_colors
// candidates so the dedupe in (1) has a spare to substitute.
// ---------------------------------------------------------------------------
void finalize_palette(std::vector<Color3f>& colors,
                      std::size_t num_colors,
                      bool lock_color0);

// True if the palette contains a bit-exact black entry. Used by the
// two-pass quantize-with-lock pattern: ask the quantizer for K-1 (so
// the prepended locked-black fills the last slot without dropping a
// pick), then check via this helper whether the K-1 partition
// happened to include pure black — in which case the caller should
// re-quantize at K and let finalize_palette dedupe.
bool contains_locked_black(const Palette& palette);

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
