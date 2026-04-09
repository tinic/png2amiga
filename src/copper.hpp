#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
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
    // 4-bit nibble (per channel) with the slot's previous value, the LOCT=0
    // (high) write is unnecessary — only the LOCT=1 (low) write is needed.
    // The viewer skips writes where the hi-table entry's reg is 0xFFFF.
    // Set by the encoder; ignored on OCS where LOCT does not exist.
    bool skip_hi = false;
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
    // Used by callers to budget-check against MAX_MOVES_PER_LINE.
    std::size_t max_moves_per_line{};
};

// ---------------------------------------------------------------------------
// MOVE budget per scanline (empirically measured at hires-8 + FMODE=3 + copper).
// Tightest worst case: 18 MOVE instructions fit in the post-DDFSTOP gap before
// the next line's bitplane DMA starts. We use 18 directly: 17 production worst
// case + 1 marker MOVE was confirmed to fit; 17 + 2 markers overran by 1 MOVE.
// ---------------------------------------------------------------------------
inline constexpr std::size_t MAX_MOVES_PER_LINE = 18;

// ---------------------------------------------------------------------------
// Compute the MOVE count emitted for one scanline given its sorted change list,
// matching the viewer's emission strategy ("force first BPLCON3 per pass, no
// per-scanline reset"). Accounts for the nibble-skip optimization: changes
// with skip_hi=true are dropped from the hi pass entirely (their bank also
// disappears from the hi pass count if it had no other live entries).
//
// Formulas:
//   OCS:                  K MOVEs (no LOCT)
//   AGA <=32 colors:      (K - K_skip_hi) + 2 + K  (hi loop + LOCT on/off + lo loop)
//   AGA bank-switching:   (B_hi + K_hi) + (B_lo + K)
//                         where B_hi = distinct banks among non-skipped hi entries
//                               K_hi = number of non-skipped hi entries
//                               B_lo = distinct banks across all entries
// ---------------------------------------------------------------------------
constexpr std::size_t moves_for_line(
    const std::vector<CopperChange>& changes,
    bool aga, bool aga_banks) noexcept {
    if (changes.empty()) return 0;
    if (!aga) return changes.size();  // OCS: K MOVEs

    std::size_t k = changes.size();
    std::size_t k_skip_hi = 0;
    for (auto& ch : changes) if (ch.skip_hi) ++k_skip_hi;
    std::size_t k_hi = k - k_skip_hi;

    if (!aga_banks) {
        // AGA <=32: hi-color writes (skipped honored) + LOCT on/off + lo-color writes
        return k_hi + 2 + k;
    }

    // AGA bank-switching: count distinct banks in sorted changes for both passes
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
        int b = static_cast<int>(ch.reg / 32);
        if (b != prev_bank_lo) { ++banks_lo; prev_bank_lo = b; }
    }
    return banks_hi + k_hi + banks_lo + k;
}

// ---------------------------------------------------------------------------
// Maximum copper changes per line (empirically tested DMA limits).
// ---------------------------------------------------------------------------

constexpr std::size_t max_changes_per_line(std::size_t depth, bool is_ham,
                                           [[maybe_unused]] bool is_hires,
                                           amiga::Chipset chipset) noexcept {
    // Hardware-tested conservative limits (worst-case BPLCON3 + LOCT).
    // Copper changes written at end-of-display (past DDFSTOP).
    if (chipset == amiga::Chipset::aga) {
        if (is_ham) {
            if (depth == 6) return 7;   // HAM6 AGA
            if (depth == 8) return 4;   // HAM8 AGA
        }
        if (depth >= 6) return 4;
        if (depth >= 3) return 7;
        if (depth == 2) return 4;
        return 2;  // 1bpl
    }
    // OCS
    if (is_ham) return 16;  // HAM6 OCS
    if (depth >= 5) return 16;
    if (depth == 4) return 16;
    if (depth == 3) return 8;
    if (depth == 2) return 4;
    return 2;  // 1bpl
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
                                   bool is_ham = false,
                                   bool is_hires = false,
                                   std::size_t override_changes = 0,  // 0 = auto
                                   const std::vector<Color3f>* user_palette = nullptr);

// ---------------------------------------------------------------------------
// Render a copper-palette image back to an Image for preview.
//
// Uses scanline_palettes[y] to look up colors for each pixel index on
// scanline y.
// ---------------------------------------------------------------------------

Result<Image> render_copper(const bitplane::BitplaneData& planes,
                            const std::vector<std::vector<Color3f>>& scanline_palettes);

} // namespace png2amiga::copper
