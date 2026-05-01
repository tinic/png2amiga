#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <limits>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace png2amiga::copper {

// ---------------------------------------------------------------------------
// Copper palette mode
//
// The Amiga Copper coprocessor can change palette registers each scanline,
// but only a limited number per line due to horizontal timing constraints:
//   OCS: ~4 color register changes per scanline (12-bit, 1 MOVE each)
//   AGA: ~8 color register changes per scanline (24-bit, 2 MOVEs each)
//
// Encoding strategy:
//   1. Generate a global base palette via quantization (good for whole image).
//   2. For each scanline, compute ideal per-row colors, then greedily pick
//      the K best palette register swaps (K = changes_per_line) to minimize
//      error for that row. Palette state accumulates across scanlines.
//   3. Dither each scanline against its effective palette and encode.
//
// The copper list stores per-scanline register changes (index + color).
// ---------------------------------------------------------------------------

// A single copper register change: set palette[reg] = color
struct CopperChange {
    std::uint8_t reg;       // palette register index (0..N-1)
    Color3f color;          // new color value

    // Nibble-skip optimization for AGA: when the new color shares its high
    // (or low) 4-bit nibble (per channel) with the slot's previous value,
    // that LOCT pass's write is unnecessary. The viewer skips writes where
    // the table entry's reg is 0xFFFF.
    // Set by the encoder; ignored on OCS where LOCT does not exist.
    bool skip_hi = false;
    bool skip_lo = false;

    // Average X position of pixels using this register on the scanline.
    // Used for spatial sorting: leftmost swaps first so reducing K drops
    // the rightmost (least critical) swaps. Not serialized to viewer data.
    float avg_x = 0.0f;
};

// ---------------------------------------------------------------------------
// CopperResult -- per-scanline palette changes + bitplane data
// ---------------------------------------------------------------------------

struct CopperResult {
    bitplane::BitplaneData planes;

    // The base palette (global, written once at frame start)
    std::vector<Color3f> base_palette;

    // Per-scanline register changes: changes[y] has at most K entries
    std::vector<std::vector<CopperChange>> scanline_changes;

    // Full effective palette per scanline (for preview rendering / COPL)
    std::vector<std::vector<Color3f>> scanline_palettes;

    std::size_t num_colors{};           // palette size (2^depth)
    std::size_t changes_per_line{};     // K budget (max per line)
    float avg_changes_per_line{};       // actual average changes made
    float total_error{};

    // Worst-case copper MOVE instructions emitted per scanline (post-clustering).
    // Used by callers to budget-check against max_moves_budget().
    std::size_t max_moves_per_line{};
};

// ---------------------------------------------------------------------------
// Copper instruction budget per scanline — the number of copper instructions
// (MOVEs + WAITs) that fit in the post-DDFSTOP gap before the next line's
// bitplane DMA resumes.
//
// The gap is determined by DDFSTOP + horizontal blank, both of which are a
// function of the display window width (320 for lores, 640 for hires — same
// horizontal area, same DDFSTOP). Depth, chipset, and FMODE do not change
// the location of DDFSTOP, so the budget is a single constant across modes.
//
// Empirically measured on real AGA hardware with e9k-debugger overlay: 15.
// This total includes the per-line WAIT that gates the copper on the
// appropriate scanline, so the MOVE budget for palette changes is 15 - 1 = 14.
//
// Interlace shifts the per-line WAIT HP from 0xDD (HPOS 220) to 0xE3 (HPOS
// 226) so MOVEs land past the active-display right edge. That steals 6 CCK
// (1.5 MOVEs) of post-display gap, so the safe per-line MOVE budget drops
// from 14 to 13.
// ---------------------------------------------------------------------------
inline constexpr std::size_t COPPER_SLOTS_PER_LINE = 15;
inline constexpr std::size_t MOVE_BUDGET_PER_LINE = COPPER_SLOTS_PER_LINE - 1;
inline constexpr std::size_t MOVE_BUDGET_PER_LINE_LACE = COPPER_SLOTS_PER_LINE - 2;

// ---------------------------------------------------------------------------
// Compute the MOVE count emitted for one scanline given its sorted change list,
// matching the viewer's emission strategy ("force first BPLCON3 per pass, no
// per-scanline reset"). Accounts for the nibble-skip optimization: changes
// with skip_hi/skip_lo=true are dropped from their respective passes (their
// bank also disappears from the pass count if no other live entries remain).
//
// Formulas:
//   OCS:                  K MOVEs (no LOCT)
//   AGA <=32 colors:      K_hi + 2 + K_lo  (hi loop + LOCT on/off + lo loop)
//   AGA bank-switching:   (B_hi + K_hi) + (B_lo + K_lo)
// ---------------------------------------------------------------------------
constexpr std::size_t moves_for_line(
    const std::vector<CopperChange>& changes,
    bool aga, bool aga_banks) noexcept {
    if (changes.empty()) return 0;
    if (!aga) return changes.size();  // OCS: K MOVEs

    std::size_t k_skip_hi = 0, k_skip_lo = 0;
    for (auto& ch : changes) {
        if (ch.skip_hi) ++k_skip_hi;
        if (ch.skip_lo) ++k_skip_lo;
    }
    std::size_t k = changes.size();
    std::size_t k_hi = k - k_skip_hi;
    std::size_t k_lo = k - k_skip_lo;

    if (!aga_banks) {
        return k_hi + 2 + k_lo;
    }

    // AGA bank-switching: count distinct banks in non-skipped entries per pass
    int prev_bank_hi = -1;
    std::size_t banks_hi = 0;
    for (auto& ch : changes) {
        if (ch.skip_hi) continue;
        int b = static_cast<int>(ch.reg / 32);
        if (b != prev_bank_hi) { ++banks_hi; prev_bank_hi = b; }
    }
    int prev_bank_lo = -1;
    std::size_t banks_lo = 0;
    for (auto& ch : changes) {
        if (ch.skip_lo) continue;
        int b = static_cast<int>(ch.reg / 32);
        if (b != prev_bank_lo) { ++banks_lo; prev_bank_lo = b; }
    }
    return banks_hi + k_hi + banks_lo + k_lo;
}

// ---------------------------------------------------------------------------
// Maximum copper changes per line — static per chipset, derived from the
// 14-MOVE budget and the worst-case per-change cost:
//
//   OCS: 1 MOVE per change (no LOCT, no bank switching)   → K = 14
//   AGA: 4 MOVEs per change (worst case = unsaturated bank-switching at d8,
//        each change in its own bank)                      → K = 14 / 4 = 3
//
// The real hardware budget is mode-independent (DDFSTOP is fixed by display
// width, not depth/FMODE/chipset). Previous per-depth tables were extrapolated
// from incomplete data and produced values that overshot on real AGA hardware
// for deep modes. Users can override via --copper-changes.
// ---------------------------------------------------------------------------

constexpr std::size_t max_changes_per_line(
    [[maybe_unused]] std::size_t depth,
    [[maybe_unused]] bool is_ham,
    [[maybe_unused]] bool is_hires,
    amiga::Chipset chipset,
    bool is_lace = false) noexcept {
    if (chipset == amiga::Chipset::aga) return 3;
    // OCS: 14 MOVEs/line normally; interlace loses ~1.5 CCK of post-display
    // gap because the per-line WAIT is pushed to HPOS 226 (vs 220), so one
    // fewer MOVE fits safely.
    return is_lace ? 13 : 14;
}

// ---------------------------------------------------------------------------
// Encode an image using per-scanline copper palette changes.
//
// image:           preprocessed, scaled image (linear RGB)
// depth:           bitplane depth (1-6 for OCS, 1-8 for AGA)
// dither_settings: dithering method and parameters
// chipset:         determines changes_per_line
// ---------------------------------------------------------------------------

Result<CopperResult> encode_copper(const Image& image,
                                   std::size_t depth,
                                   const dither::Settings& dither_settings,
                                   amiga::Chipset chipset = amiga::Chipset::ocs,
                                   std::size_t override_changes = 0,  // 0 = auto
                                   const std::vector<Color3f>* user_palette = nullptr,
                                   bool lock_color0 = true,
                                   // Locked palette slots: fixed colors that copper
                                   // must never swap (e.g., for blitter objects).
                                   const std::vector<std::pair<std::size_t, Color3f>>& locked = {},
                                   int palette_diversity = 0,
                                   // Number of initial rows that display with
                                   // base palette only (no swaps). Needed for
                                   // interlace: row 0 and row 1 are each field's
                                   // first visible line with no prior scanline
                                   // to schedule a pre-display WAIT on.
                                   std::size_t skip_initial_swap_rows = 0,
                                   // Interlace: reduces per-line MOVE budget by
                                   // 1 (HP=226 vs 220 → less post-display gap).
                                   bool is_lace = false,
                                   // EHB-aware mode: depth must be 5 (32 base
                                   // colors). The planner scores swaps against
                                   // the 64-effective palette (32 base + 32
                                   // hardware half-brite). A swap on base slot
                                   // k automatically also updates effective
                                   // slot k+32 = halve(base[k]). Without this
                                   // flag, CAP+EHB optimises a 32-color
                                   // objective while rendering hits 64 — for
                                   // high-dynamic-range images the 32-color
                                   // planner picks middle-tone bases and
                                   // 64-effective ends up covering neither
                                   // very-dark nor very-bright gamut ends.
                                   bool is_ehb = false,
                                   // Progress callback. Called periodically
                                   // with (progress 0..1, stage label).
                                   // Must be thread-safe — encoder may
                                   // invoke from worker contexts.
                                   std::function<void(float, std::string_view)>
                                       on_progress = {},
                                   // Per-line palette planner neighbour-row
                                   // smoothing. SIZE_MAX / -1.0f means
                                   // "use the depth/is_ehb-aware default";
                                   // explicit values from the CLI flags
                                   // --cap-spread-radius / --cap-spread-decay
                                   // override that derivation. The
                                   // depth-conditional defaults come from
                                   // a 25-config A/B sweep on FS encodes:
                                   //   is_ehb (EHB+CAP):     r=4, d=0.3
                                   //   depth=3 (DPF):        r=3, d=0.85
                                   //   depth=5 (lores+CAP):  r=2, d=0.85
                                   //   else (HAM6+CAP/SCAP): r=4, d=0.85
                                   // Net wins of +0.5–1.2 dB on plain
                                   // CAP modes, marginal elsewhere.
                                   std::size_t neighbor_radius =
                                       std::numeric_limits<std::size_t>::max(),
                                   float neighbor_decay = -1.0f,
                                   // Slots the dither pass must never pick.
                                   // Used by --reserve-range: those slots
                                   // hold a fixed user colour (also locked
                                   // via `locked`), and image content must
                                   // route around them across all scanlines.
                                   const std::vector<std::size_t>& dither_excluded = {});

// ---------------------------------------------------------------------------
// Render a copper-palette image back to an Image for preview.
//
// Uses scanline_palettes[y] to look up colors for each pixel index on
// scanline y.
// ---------------------------------------------------------------------------

Result<Image> render_copper(const bitplane::BitplaneData& planes,
                            const std::vector<std::vector<Color3f>>& scanline_palettes);

// Lace-aware overload: simulates the K-swap budget the cheader emitter
// applies in lace mode (lace_rebuild picks the top-K diffs against the
// previous SAME-FIELD row, drops anything past `cap_changes_per_line`).
// Use this for the preview path so the rendered image matches what the
// generated viewer will actually display on hardware — without it, the
// preview shows the planner's idealised per-row palette, but the chip
// only gets the budget-clipped version.
//
// `chipset` is also applied to the rendered colors: OCS quantises every
// palette entry through `quantize_to_ocs` (12-bit RGB nibble-replicated
// to 24-bit) so the preview shows true 12-bit OCS colors instead of the
// planner's full-precision Color3f.
Result<Image> render_copper_capped(const bitplane::BitplaneData& planes,
                                   const std::vector<std::vector<Color3f>>& scanline_palettes,
                                   std::span<const Color3f> base_palette,
                                   std::size_t cap_changes_per_line,
                                   bool is_lace,
                                   amiga::Chipset chipset);

} // namespace png2amiga::copper
