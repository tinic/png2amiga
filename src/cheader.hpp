#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "copper.hpp"
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

    // Copper: per-scanline register changes (nullptr = no copper)
    const std::vector<std::vector<copper::CopperChange>>* copper_changes = nullptr;
    std::size_t copper_changes_per_line = 0;
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
