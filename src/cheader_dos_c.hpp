#pragma once

#include "amiga.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace png2amiga::cheader_dos_c {

// ---------------------------------------------------------------------------
// 16-bit real-mode C source output for tkchia's ia16-elf-gcc (targets 8086+).
//
// Sibling of cheader_dos.cpp but emits plain C with inline asm for BIOS
// calls and direct far-pointer writes to video memory — no DPMI, no libc
// I/O, no C++ runtime. Produces a single .c file compilable with:
//
//   ia16-elf-gcc -march=i8086 -Os -o viewer.exe viewer.c
//
// Intended floor: original IBM PC / XT (8088 at 4.77 MHz, 16 KB-640 KB
// RAM, CGA card, 5.25" 360 KB floppy). The .EXE is small enough (~20 KB
// for CGA graphics modes — 16 KB data + a few hundred bytes of code +
// newlib crt0) that multiple viewers fit on a single 360 KB disk.
//
// Current scope: CGA graphics modes (cga_320, cga_640, cga_composite)
// plus cga_text80x100 — 80×100 char+attr cells rendered via 6845 CRTC
// reprogramming (2-scan cells, 100-row vertical layout). 80x200 can't
// fit on real CGA (32 KB char+attr exceeds 16 KB VRAM). EGA/VGA
// targets stay on the DJGPP path via cheader_dos.cpp.
// ---------------------------------------------------------------------------

struct Options {
    std::string symbol_name = "image";

    // EGA text modes only. 256 × 32 bytes of VGA font-RAM-formatted
    // glyph data pre-shifted by the encoder's scanline_offset (so byte
    // `c*32 + s` holds the glyph slice that the CRTC will display at
    // scanline `s` of cell `c`). Loaded via int 10h AX=1110h at viewer
    // startup. Ignored for non-text modes and for CGA text (CGA has
    // no custom-font slot).
    std::span<const std::uint8_t> font_data = {};

    // CGA 320x200 4-color palette selection. Encodes directly into
    // port 0x3D9 (CGA mode-control register 2):
    //   bit 5: palette (0 = G/R/Brown, 1 = C/M/white)
    //   bit 4: intensity (0 = dim, 1 = bright)
    //   bits 3..0: background color (0..15)
    // Typical auto-selected values from cga_build_palette:
    //   p0_low  = 0x00  p0_high = 0x10
    //   p1_low  = 0x20  p1_high = 0x30
    // Only applies to cga_320. Ignored by cga_640 (fixed mono) and
    // cga_composite (palette irrelevant — NTSC decoder interprets
    // chroma patterns directly).
    std::uint8_t cga_mode_ctrl2 = 0x30;
};

// Emit a .c source string.
//
//   mode       — CGA graphics (cga_320 / cga_640 / cga_composite),
//                CGA text (cga_text80x100), or EGA graphics
//                (ega_320 / ega_640).
//   width/height — logical image dimensions.
//   raw_frame  — for CGA graphics: the 16 KB banked CGAPIC frame from
//                cheader_dos::pack_cga_banked. For CGA text: char+attr
//                pair bytes (16000 bytes for 80×100). For EGA planar:
//                the four bitplanes concatenated plane-sequentially
//                (plane 0 full, plane 1 full, ... plane 3).
//   palette    — EGA modes only: 16 bytes of ATC register values (the
//                IRGB-packed palette bytes that cheader_dos::encode_palette
//                emits for ega_320 / ega_640). Empty for CGA modes.
//   options    — symbol name + CGA-specific palette variant.
Result<std::string> generate(amiga::Mode mode,
                             std::size_t width,
                             std::size_t height,
                             std::span<const std::uint8_t> raw_frame,
                             std::span<const std::uint8_t> palette = {},
                             const Options& options = {});

Result<void> save(std::string_view path,
                  amiga::Mode mode,
                  std::size_t width,
                  std::size_t height,
                  std::span<const std::uint8_t> raw_frame,
                  std::span<const std::uint8_t> palette = {},
                  const Options& options = {});

} // namespace png2amiga::cheader_dos_c
