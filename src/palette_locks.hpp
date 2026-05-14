#pragma once

#include "amiga.hpp"
#include "api.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace png2amiga::palette_locks {

using LockSpec = api::LockSpec;
using PinSpec = api::PinSpec;
using ReserveSpec = api::ReserveSpec;

// ---------------------------------------------------------------------------
// Convert a LockSpec (sRGB 0-255) into a linear Color3f, snapped to the
// chipset/mode precision (OCS 12-bit nibble replication, STF 9-bit, AGA raw).
// ---------------------------------------------------------------------------
Color3f to_color(const LockSpec& lock, amiga::Chipset chipset, amiga::Mode mode);

// ---------------------------------------------------------------------------
// Validate locks against the palette size. Errors:
//  - index out of range
//  - duplicate indices
//  - color components out of range
// ---------------------------------------------------------------------------
Result<void> validate_locks(const std::vector<LockSpec>& locks, std::size_t max_colors);

// ---------------------------------------------------------------------------
// Validate reserves: in-range, no duplicates, no overlap with locks
// or with the implicit black-zero. Returns max_in_palette = the
// number of reserve entries that fall within max_colors (entries
// from open-end ranges may have been parsed beyond max_colors and
// are silently clipped here).
// ---------------------------------------------------------------------------
Result<std::size_t> validate_reserves(const std::vector<ReserveSpec>& reserves,
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
                           const std::vector<ReserveSpec>& reserves,
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
    amiga::Mode mode,
    // Indices to SKIP when filling unlocked slots with quantized
    // colors. Use this to make the fill avoid landing in slots that
    // will be overwritten by a post-assemble reserve overlay — without
    // it, fill puts quantized colors at the front-of-unlocked, the
    // reserve overlay displaces them, and the unreserved tail goes
    // unfilled (3bpp + reserve 1-4 → slots 5-7 black). Reserves are
    // NOT placed by assemble; the caller overlays them and they're
    // not in the dedupe set so quantizer entries that bit-match
    // reserves don't get dropped.
    const std::vector<bool>& reserved_skip = {});

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
void finalize_palette(std::vector<Color3f>& colors, std::size_t num_colors, bool lock_color0);

// True if the palette contains a bit-exact black entry. Used inside
// two_pass_quantize() below; exposed because some sites need to peek
// at intermediate quantizer output between snap and diversify.
bool contains_locked_black(const Palette& palette);

// Two-pass quantize-with-lock-color-0. Calls `quantize_fn(kfirst)`;
// if `lock_color0` is set and the result contains pure black,
// re-calls `quantize_fn(kfallback)` (typically kfirst+1) so the
// downstream finalize_palette dedupe has a spare to drop.
//
// Why two passes: when slot 0 is reserved for black, the simplest
// approach is to ask the quantizer for K-1 colors and prepend the
// locked black. Most images don't drive the quantizer to pick black
// in K-1 clusters, so the K-1 partition cleanly fills slots 1..K-1.
// On dark sources the quantizer DOES pick black at K-1; in that
// case we re-quantize at K so finalize_palette can dedupe without
// leaving an empty slot. Asking for K up-front always (the naive
// fix) shifts every cluster boundary and perturbs encoder quality.
//
// Caller is expected to pass a quantize_fn that:
//   • takes the slot count K
//   • returns a Result<Palette> of K colors, snapped to the target
//     chipset/mode gamut and diversified (caller-side concern)
// The returned Palette is then ready for `finalize_palette()` or
// `assemble_locked_palette()` as appropriate.
Result<Palette> two_pass_quantize(const std::function<Result<Palette>(std::size_t)>& quantize_fn,
                                  std::size_t kfirst,
                                  std::size_t kfallback,
                                  bool lock_color0);

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

// ---------------------------------------------------------------------------
// Slot-budget counts for the quantizer when locks + reserves + lock_zero
// claim some of the palette. Used by api.cpp run_pipeline (web/library)
// and main.cpp std-lores (CLI) — keep them in sync via this helper so
// future consolidation lands without divergence.
//
//   qcount    = max - lock_zero - locks.size() - reserves.size()
//               (floored at 1; the count we ask the quantizer to
//                produce — exactly the unlocked-and-unreserved slot
//                count, NOT the larger no-subtract count which would
//                size the quantizer for the wrong gamut.)
//   kfallback = min(max_colors, qcount + lock_zero)
//               (handed to two_pass_quantize as the retry K when the
//                first pass picks pure black; lock_zero needs one more
//                so the dedupe pass has a spare to drop.)
// ---------------------------------------------------------------------------
struct QuantCounts {
    std::size_t qcount;
    std::size_t kfallback;
};
QuantCounts quant_counts_for_assemble(std::size_t max_colors,
                                      const std::vector<LockSpec>& locks,
                                      std::size_t reserve_count,
                                      bool lock_zero_black);

// ---------------------------------------------------------------------------
// One-call assemble + reserve overlay. Both api.cpp and main.cpp's
// std-lores branches went through three rewrites in May 2026 because
// the assemble + reserve flow had four interlocking concerns:
//
//   • qcount must subtract reserves (else the quantizer optimises for
//     the wrong slot count and the dither's gamut collapses).
//   • assemble's fill must SKIP reserved indices (else reserves
//     displace quantized colors, leaving the tail empty).
//   • assemble's lock list must NOT include reserves (else dedupe
//     drops every quantizer entry that bit-matches a reserve, leaving
//     the tail empty for a different reason).
//   • reserves get overlaid AFTER assemble at their requested indices.
//
// This helper bakes all four into one call so future maintainers
// don't have to re-derive the right combination. Caller still owns
// the qcount + two_pass_quantize step (the quantizer signature
// differs slightly between paths — auto_quantize vs quantize::quantize
// — and is left to the caller).
//
// Reserves are applied via the same chipset/mode snap that locks use
// (palette_locks::to_color), so the placed colors match what the
// hardware can actually display.
// ---------------------------------------------------------------------------
AssembledPalette assemble_with_reserves(const Palette& quantized,
                                        const std::vector<LockSpec>& locks,
                                        const std::vector<ReserveSpec>& reserves,
                                        std::size_t max_colors,
                                        bool lock_zero_black,
                                        amiga::Chipset chipset,
                                        amiga::Mode mode);

// ---------------------------------------------------------------------------
// Sort an indexed palette's unlocked entries by perceptual brightness
// (OKLab L) and remap the dithered indices so the rendered image is
// unchanged. Locked slots stay at their original positions; unlocked
// slots fill the remaining indices in ascending-L order.
//
// `sort_n` bounds the sort range to the first N palette entries — for
// EHB this is 32 (the base section); the half-brite section follows
// the base order automatically and is handled by `hb_mirror=true`,
// which also remaps any 32..63 indices via base_perm.
//
// Not safe for HAM modes (the index encodes hardware operations, not
// palette positions).
// ---------------------------------------------------------------------------
void sort_by_brightness(std::vector<Color3f>& palette,
                        const std::vector<bool>& locked,
                        std::vector<std::uint8_t>& indices,
                        std::size_t sort_n,
                        bool hb_mirror = false);

}  // namespace png2amiga::palette_locks
