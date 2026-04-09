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
};

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
                                   std::size_t override_changes = 0);  // 0 = auto

// ---------------------------------------------------------------------------
// Render a copper-palette image back to an Image for preview.
//
// Uses scanline_palettes[y] to look up colors for each pixel index on
// scanline y.
// ---------------------------------------------------------------------------

Result<Image> render_copper(const bitplane::BitplaneData& planes,
                            const std::vector<std::vector<Color3f>>& scanline_palettes);

} // namespace png2amiga::copper
