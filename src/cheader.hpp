#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "copper.hpp"
#include "strips.hpp"
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
    std::string symbol_name = "image";  // base name for symbols
    bool interleaved = false;           // if true, emit single interleaved array
    bool hires = false;                 // override HIRES for compound modes (ham8-hires etc.)
    bool interlace = false;             // set LACE bit in CAMG
    bool aga = false;                   // AGA chipset (24-bit palette)
    bool fade_in = false;               // 16-step fade-in from black
    bool dpf = false;                   // dual playfield (CAMG 0x0400, BPLCON0 DBLPF)

    // Copper: per-scanline register changes (nullptr = no copper)
    const std::vector<std::vector<copper::CopperChange>>* copper_changes = nullptr;
    std::size_t copper_changes_per_line = 0;
    // Experimental: emit per-line WAITs with the vertical comparator
    // masked off (IR2 = 0x80FE instead of 0xFFFE). Only the very first
    // WAIT carries an explicit V to anchor display start; every
    // subsequent WAIT just waits for the H counter to reach HP again,
    // which (since the previous MOVE block finishes past HP) fires
    // exactly once per line. Side effects:
    //   - The 0xFFDF "past line 255" wrap marker becomes unnecessary
    //     — V is no longer being compared, so V wraparound at line 256
    //     doesn't matter. The whole `if (line == 255)` special case
    //     drops out and frees its slot.
    //   - Opens a path to dynamic fade-in by modifying just the H byte
    //     of subsequent WAITs in real time (no V juggling needed).
    // Off by default — production users keep the proven anchor-every-
    // line behavior until this has soaked.
    bool copper_wait_h_only = false;
    // Optional: per-scanline effective palette. When `interlace` is set we
    // rebuild the diffs so each field sees transitions between the rows it
    // actually draws (row y ← row y-2) rather than the default row y-1.
    const std::vector<std::vector<Color3f>>* copper_scanline_palettes = nullptr;

    // strips (Super Sliced palette): mid-line COLORxx writes at
    // fixed horizontal slot positions, anchored by a per-line WAIT.
    // line_moves[y] is a sequence of raw WAIT/MOVE ops emitted verbatim
    // into the copper list for image row y. When populated, the viewer
    // skips the sliced path entirely and installs the strips list as its
    // primary copper list. Used by the calibration probe pipeline first;
    // the production planner emits the same shape.
    const std::vector<std::vector<strips::ScapMove>>* strips_line_moves = nullptr;
    // Informational — included as comments in the emitted copper list.
    std::string strips_label;
    int strips_anchor_hpos = 0;
    int strips_total_planes = 0;

    // Multi-frame viewer (--batch --batch-format cpp). When non-empty, the
    // viewer emits one bitplane data array per frame (including the primary
    // `planes` arg as frame 0) and rebinds the copper list's BPLxPT entries
    // on every left-click. Wraps from last frame back to frame 0; right-click
    // (or the same exit path as before) terminates. All frames must share
    // dimensions / depth / layout (atlas-encoded by the batch driver, so this
    // is automatic). Interlace is unsupported and rejected up the stack.
    std::span<const bitplane::BitplaneData> extra_frame_planes;
    std::vector<std::string> frame_labels;  // one per frame, for stem-named symbols

    // Caller-provided count of unique RGB colors actually present in the
    // rendered preview. Differs from palette.size() for HAM (MODIFY ops
    // create intermediate colors), EHB (halfbrites get used), sliced / strips
    // (per-scanline palette evolution). Reported in the viewer's exit
    // message as "Colors: N". 0 = caller didn't compute it; viewer
    // omits the line.
    std::size_t total_unique_colors = 0;

    // Multi-frame palette fade animation. When non-empty, the viewer
    // emits per-frame palette VALUE arrays + a runtime that walks
    // copper1 once at startup to capture the address of every COLOR
    // MOVE's value word, then patches those addresses with per-frame
    // values every `fade_frame_hold_vbls` VBLs. Right-click exits.
    //
    // fade_per_frame_values[f] is the flat list of 16-bit color
    // values to write for frame f, in the same order the cop list
    // emits its COLOR MOVEs (per-row HI sweep then per-row LO sweep
    // for AGA; per-row sweep only for OCS). Frame 0 should match
    // the cop list's baked-in values (frame 0 of the fade IS the
    // source palette). fade_per_frame_values.size() = F frames.
    //
    // Restriction (v1): AGA banked palettes (>32 colors) are not yet
    // supported because the runtime cop-list scan would have to track
    // BPLCON3 bank/LOCT context per MOVE. Caller must guard with that
    // check before populating.
    std::vector<std::vector<std::uint16_t>> fade_per_frame_values;
    int fade_frame_hold_vbls = 3;
    bool fade_ping_pong = true;  // false → wrap last → first

    // Optional transparency mask, pre-packed into 1bpp big-endian bits
    // matching planes.bytes_per_row × planes.height. Empty span = no
    // mask emitted (the .h ends with just the bitplane arrays).
    //
    // Layout semantics mirror the raw .bpl writer's --mask-layout:
    //   "replicated" + interleaved: mask row is injected after EVERY
    //                                bitplane row inside the single
    //                                interleaved array (kingcon's
    //                                blitter A=image / B=mask shape).
    //   "appended" + interleaved:   one mask row appended after each
    //                                scanline's bitplane group.
    //   any layout + non-interleaved: emit a separate `<sym>_mask[]`
    //                                array of height × words-per-row.
    std::span<const std::uint8_t> mask_plane;
    std::string_view mask_layout;
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

}  // namespace png2amiga::cheader
