#pragma once

#include "amiga.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace png2amiga::cheader_dos_c {

// ---------------------------------------------------------------------------
// 16-bit real-mode C source output for tkchia's ia16-elf-gcc (targets 8086+).
//
// Emits plain C with inline asm for BIOS calls and direct far-pointer writes
// to video memory — no DPMI, no libc I/O, no C++ runtime. Produces a single
// .c file compilable with:
//
//   ia16-elf-gcc -march=i80286 -mcmodel=small -Os -o viewer.exe viewer.c
//
// Intended floor: original IBM PC / XT (8088 at 4.77 MHz, 16 KB-640 KB
// RAM, CGA card, 5.25" 360 KB floppy). The .EXE is small enough (~20 KB
// for CGA graphics modes — 16 KB data + a few hundred bytes of code +
// newlib crt0) that multiple viewers fit on a single 360 KB disk.
//
// Supported modes: CGA graphics (cga_320, cga_640, cga_composite_hires),
// cga_text80x100, EGA graphics (ega_320, ega_640, ega_hi), VGA chunky
// 13h, VGA planar 10h / 12h.
// ---------------------------------------------------------------------------

struct Options {
    std::string symbol_name = "image";

    // CGA 320x200 4-color palette selection. Encodes directly into
    // port 0x3D9 (CGA mode-control register 2):
    //   bit 5: palette (0 = G/R/Brown, 1 = C/M/white)
    //   bit 4: intensity (0 = dim, 1 = bright)
    //   bits 3..0: background color (0..15)
    // Typical auto-selected values from cga_build_palette:
    //   p0_low  = 0x00  p0_high = 0x10
    //   p1_low  = 0x20  p1_high = 0x30
    // Only applies to cga_320 / cga_composite_hires. Ignored by cga_640
    // (fixed mono). cga_composite_hires stashes the chosen FG (0..15)
    // in the low nibble — written to 0x3D9 so the chroma decoder sees
    // it.
    std::uint8_t cga_mode_ctrl2 = 0x30;
};

// Emit a .c source string.
//
//   mode       — CGA graphics (cga_320 / cga_640 / cga_composite_hires),
//                CGA text (cga_text80x100), EGA graphics
//                (ega_320 / ega_640 / ega_hi), or VGA
//                (vga_13h / vga_10h / vga_12h).
//   width/height — logical image dimensions.
//   raw_frame  — for CGA graphics: the 16 KB banked CGAPIC frame from
//                pack_cga_banked. For CGA text: char+attr pair bytes
//                (16000 bytes for 80×100). For EGA / VGA planar: the
//                four bitplanes concatenated plane-sequentially. For
//                VGA 13h: chunky 8bpp indices.
//   palette    — EGA modes: 16 bytes of ATC register values.
//                VGA 13h: 768-byte DAC (256 × 3 RGB).
//                VGA planar: 48-byte DAC (16 × 3 RGB).
//                Empty for CGA modes.
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

// Pack chunky palette indices into the 16 KB banked CGAPIC frame that
// sits at 0xB8000 on a real CGA. Shared utility used by both .raw
// output and the viewer generator.
//   cga_640 / cga_composite_hires: 1bpp mono, 8 pixels/byte MSB-first
//                  (fg = index != 0).
//   cga_320:       2bpp packed, 4 pixels/byte MSB-first.
// Output always 16384 bytes: even rows at offset 0x0000, odd rows at
// 0x2000, padding zeroed.
std::vector<std::uint8_t> pack_cga_banked(std::span<const std::uint8_t> indices,
                                          std::size_t width,
                                          std::size_t height,
                                          amiga::Mode mode);

}  // namespace png2amiga::cheader_dos_c
