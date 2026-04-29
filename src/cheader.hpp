#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "copper.hpp"
#include "scap.hpp"
#include "types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace png2amiga::cheader {

// ---------------------------------------------------------------------------
// C header output
//
// Generates a .h file containing bitplane data as UWORD arrays, plus
// palette data and metadata defines. Designed for direct inclusion in
// Amiga C projects compiled with VBCC or m68k-amigaos-gcc.
//
// Output format:
//   #ifndef IMAGE_NAME_H
//   #define IMAGE_NAME_H
//
//   #define IMAGE_NAME_WIDTH   320
//   #define IMAGE_NAME_HEIGHT  256
//   #define IMAGE_NAME_DEPTH   5
//   #define IMAGE_NAME_BPR     40    /* bytes per row per plane */
//   #define IMAGE_NAME_CAMG    0x0000
//
//   const UWORD image_name_plane0[] = { 0x1234, 0x5678, ... };
//   const UWORD image_name_plane1[] = { ... };
//   ...
//   const UWORD image_name_palette[] = { 0x0RGB, 0x0RGB, ... };
//
//   #endif
// ---------------------------------------------------------------------------

struct CHeaderOptions {
    std::string symbol_name = "image";   // base name for symbols
    bool interleaved = false;            // if true, emit single interleaved array
    bool hires = false;                  // override HIRES for compound modes (ham8-hires etc.)
    bool interlace = false;              // set LACE bit in CAMG
    bool aga = false;                    // AGA chipset (24-bit palette)
    bool fade_in = false;                // 16-step fade-in from black
    bool dpf = false;                    // dual playfield (CAMG 0x0400, BPLCON0 DBLPF)

    // Copper: per-scanline register changes (nullptr = no copper)
    const std::vector<std::vector<copper::CopperChange>>* copper_changes = nullptr;
    std::size_t copper_changes_per_line = 0;
    // Optional: per-scanline effective palette. When `interlace` is set we
    // rebuild the diffs so each field sees transitions between the rows it
    // actually draws (row y ← row y-2) rather than the default row y-1.
    const std::vector<std::vector<Color3f>>* copper_scanline_palettes = nullptr;

    // SCAP (Super Copper-Augmented Palette): mid-line COLORxx writes at
    // fixed horizontal slot positions, anchored by a per-line WAIT.
    // line_moves[y] is a sequence of raw WAIT/MOVE ops emitted verbatim
    // into the copper list for image row y. When populated, the viewer
    // skips the CAP path entirely and installs the SCAP list as its
    // primary copper list. Used by the calibration probe pipeline first;
    // the production planner emits the same shape.
    const std::vector<std::vector<scap::ScapMove>>* scap_line_moves = nullptr;
    // Informational — included as comments in the emitted copper list.
    std::string scap_label;
    int scap_anchor_hpos = 0;
    int scap_total_planes = 0;

    // Multi-frame viewer (--batch --batch-format cpp). When non-empty, the
    // viewer emits one bitplane data array per frame (including the primary
    // `planes` arg as frame 0) and rebinds the copper list's BPLxPT entries
    // on every left-click. Wraps from last frame back to frame 0; right-click
    // (or the same exit path as before) terminates. All frames must share
    // dimensions / depth / layout (atlas-encoded by the batch driver, so this
    // is automatic). Interlace is unsupported and rejected up the stack.
    std::span<const bitplane::BitplaneData> extra_frame_planes;
    std::vector<std::string> frame_labels;  // one per frame, for stem-named symbols

    // Caller-provided count of unique RGB colours actually present in the
    // rendered preview. Differs from palette.size() for HAM (MODIFY ops
    // create intermediate colours), EHB (halfbrites get used), CAP / SCAP
    // (per-scanline palette evolution). Reported in the viewer's exit
    // message as "Colors: N". 0 = caller didn't compute it; viewer
    // omits the line.
    std::size_t total_unique_colors = 0;
};

// ---------------------------------------------------------------------------
// Generate C header content as a string
// ---------------------------------------------------------------------------

Result<std::string> generate(const bitplane::BitplaneData& planes,
                             std::span<const Color3f> palette,
                             amiga::Mode mode,
                             const CHeaderOptions& options = {});

// ---------------------------------------------------------------------------
// Write C header to file
// ---------------------------------------------------------------------------

Result<void> save(std::string_view path,
                  const bitplane::BitplaneData& planes,
                  std::span<const Color3f> palette,
                  amiga::Mode mode,
                  const CHeaderOptions& options = {});

// ---------------------------------------------------------------------------
// Generate a standalone AmigaOS viewer .c file
//
// Contains image data + a main() that opens a screen, copies bitplanes
// to chip RAM, sets palette (+ copper if present), waits for mouse
// click, and exits cleanly. Ready to compile with VBCC:
//   vc +kick13 -lamiga -o viewer output.c
// ---------------------------------------------------------------------------

Result<std::string> generate_viewer(const bitplane::BitplaneData& planes,
                                    std::span<const Color3f> palette,
                                    amiga::Mode mode,
                                    const CHeaderOptions& options = {});

Result<void> save_viewer(std::string_view path,
                         const bitplane::BitplaneData& planes,
                         std::span<const Color3f> palette,
                         amiga::Mode mode,
                         const CHeaderOptions& options = {});

} // namespace png2amiga::cheader
