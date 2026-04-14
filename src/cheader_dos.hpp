#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace png2amiga::cheader_dos {

// ---------------------------------------------------------------------------
// C++ source output for IBM PC / DOS target modes
//
// Emits a self-contained .cpp file with `static constexpr uint8_t` arrays
// for the image data and hardware palette, plus metadata constants so the
// consumer can blit the data into VGA/EGA/CGA video RAM directly.
//
// Output layout:
//   <symbol>_WIDTH, <symbol>_HEIGHT, <symbol>_DEPTH, <symbol>_MODE (string)
//   static constexpr uint8_t <symbol>_data[N]    = { ... };
//   static constexpr uint8_t <symbol>_palette[M] = { ... };  // omitted for CGA 320/640
//
// Byte layouts:
//   EGA graphics (4-plane planar):
//     data    = 4-plane MSB-first bitplane data (plane 0 full, then 1..3)
//     palette = 16 × IrgbIRGB bytes (6-bit; load into ATC palette registers)
//   EGA text (char+attr cells):
//     data    = char+attr pairs (rows × cols × 2 bytes)
//     palette = 16 × IrgbIRGB bytes (image-adaptive 16-of-64 selection)
//   VGA chunky (Mode 13h / Mode X / Mode Y):
//     data    = 8bpp indices (row-major for 13h; 4 column-interleaved
//               planes concatenated for Mode X / Y)
//     palette = 256 × 3 bytes (6-bit R,G,B; write to port 0x3C9)
//   VGA planar (Mode 10h / 12h):
//     data    = 4-plane MSB-first bitplane data
//     palette = 16 × 3 bytes (6-bit R,G,B; ATC + DAC setup)
//   CGA 320/640 graphics:
//     data    = 16 KB banked CGAPIC layout (even rows 0x0000, odd 0x2000)
//     palette = (omitted — hardware-fixed, include mode-register value below)
//   CGA text (8x8):
//     data    = char+attr pairs
//     palette = (omitted — CGA DAC is fixed)
//   CGA composite:
//     data    = 16 KB banked frame with NTSC artifact pattern nibbles
//     palette = (omitted — interpreted by the monitor's composite decoder)
// ---------------------------------------------------------------------------

struct Options {
    std::string symbol_name = "image";

    // Text-mode viewer parameters. Required when `mode` is an
    // amiga::is_text_graphics(mode); ignored otherwise.
    //   font_data  — 256 × 32 bytes of VGA font-RAM-formatted glyph data.
    //                Caller pre-shifts the source font by the encoder's
    //                scanline_offset so that glyph bytes [0..cell_height-1]
    //                carry the right bits; bytes [cell_height..31] should
    //                be zero (VGA slot padding). Loaded via int 10h
    //                AX=1110h at viewer startup.
    //   cell_height — 1 or 2. Programmed into CRTC reg 0x09 (max-scan-line)
    //                 bits 0..4 as `cell_height - 1`.
    //   scan_lines_al — int 10h AH=12h BL=30h AL=N "select vertical
    //                   resolution for next mode set": 0=200, 1=350,
    //                   2=400. Use 0 for CGA-text (200 lines), 1 for
    //                   EGA-text (350 lines). The viewer always passes
    //                   BL=30h (subfunction id); AL carries the count.
    std::span<const std::uint8_t> font_data;
    std::uint8_t cell_height = 0;
    std::uint8_t scan_lines_al = 0;
};

// Emit a .cpp source string.
//
//   planes      — bitplane data for EGA graphics / VGA planar modes; may be
//                 empty for chunky/composite/text modes.
//   raw_frame   — mode-specific bytes for chunky/composite/text modes; may
//                 be empty for bitplane modes. Exactly one of planes.data /
//                 raw_frame is non-empty depending on the mode.
//   palette     — logical palette (linear Color3f). For EGA modes we encode
//                 each entry as IrgbIRGB; for VGA as 6-bit DAC triples.
//                 Ignored for CGA 320/640/composite and CGA text modes.
//   width/height — buffer dimensions (before any CRT stretch).
Result<std::string> generate(amiga::Mode mode,
                             std::size_t width,
                             std::size_t height,
                             const bitplane::BitplaneData& planes,
                             std::span<const std::uint8_t> raw_frame,
                             std::span<const Color3f> palette,
                             const Options& options = {});

Result<void> save(std::string_view path,
                  amiga::Mode mode,
                  std::size_t width,
                  std::size_t height,
                  const bitplane::BitplaneData& planes,
                  std::span<const std::uint8_t> raw_frame,
                  std::span<const Color3f> palette,
                  const Options& options = {});

// Pack chunky palette indices into the 16 KB banked CGAPIC frame that
// sits at 0xB8000 on a real CGA. Shared between the .raw writer and the
// DJGPP viewer generator.
//   cga_640:       1bpp mono, 8 pixels/byte MSB-first (fg = index != 0).
//   cga_320:       2bpp packed, 4 pixels/byte MSB-first.
//   cga_composite: indices are 0..15; each byte holds two 4-bit artifact
//                  nibbles via palette::cga_composite_pattern.
// Output is always 16384 bytes: even rows at offset 0x0000, odd rows at
// 0x2000, padding bytes zeroed.
std::vector<std::uint8_t>
pack_cga_banked(std::span<const std::uint8_t> indices,
                std::size_t width, std::size_t height,
                amiga::Mode mode);

} // namespace png2amiga::cheader_dos
