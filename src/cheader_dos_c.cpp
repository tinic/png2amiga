#include "cheader_dos_c.hpp"

#include <format>
#include <fstream>

namespace png2amiga::cheader_dos_c {

namespace {

std::string sanitize_symbol(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_')
            out += c;
        else
            out += '_';
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
        out.insert(out.begin(), '_');
    return out;
}

std::string emit_byte_array(std::span<const std::uint8_t> data,
                            std::size_t per_row = 16) {
    std::string out;
    out.reserve(data.size() * 6);
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i % per_row == 0) out += "    ";
        out += std::format("0x{:02X}", data[i]);
        if (i + 1 != data.size()) out += ',';
        out += ((i + 1) % per_row == 0 || i + 1 == data.size()) ? '\n' : ' ';
    }
    return out;
}

// Common preamble: mode-setting / palette / key-wait / far blit helpers.
// All BIOS calls via inline asm so no <dos.h> / libi86 dependency is
// required; works on vanilla ia16-elf-gcc + newlib.
//
// ia16-elf-gcc's ABI: DS and SS both point to the data segment on
// function entry, but DS is a scratch register within a function —
// the compiler is free to trash it between statements (commonly for
// IVT / BIOS-data-area access via `outb`'s DX setup, or zero DS for
// SS-relative stores). Inline asm can't declare DS as a clobber, so
// helpers like blit_seg MUST force DS = SS themselves before any
// DS:SI-relative instruction (MOVSB, LODSB, etc.). Ref gcc-ia16 #94.
constexpr std::string_view kPreamble = R"C_(#include <stddef.h>

/* int 10h AH=0 AL=mode: set BIOS video mode. */
static void __attribute__((unused)) set_mode(unsigned char mode) {
    __asm__ volatile ("int $0x10" :
                      : "a"((unsigned short)mode) : "cc", "memory");
}

/* int 10h AH=0Bh BH=subfunc BL=value: CGA palette / background register. */
static void __attribute__((unused)) cga_palette(unsigned char bh,
                                                unsigned char bl) {
    unsigned short ax = 0x0B00;
    unsigned short bx = ((unsigned short)bh << 8) | bl;
    __asm__ volatile ("int $0x10"
                      : "+a"(ax), "+b"(bx) : : "cc", "memory");
}

/* Port I/O helpers. Ports >= 0x100 require the DX-indirect form. */
static void __attribute__((unused)) outb(unsigned short port, unsigned char v) {
    __asm__ volatile ("outb %%al, %%dx"
                      : : "a"(v), "d"(port) : "cc", "memory");
}
static unsigned char __attribute__((unused)) inb(unsigned short port) {
    unsigned char v;
    __asm__ volatile ("inb %%dx, %%al" : "=a"(v) : "d"(port) : "cc");
    return v;
}
/* outw for 16-bit port writes (used to set 3C4/3CE indexed regs in one op). */
static void __attribute__((unused)) outw(unsigned short port, unsigned short v) {
    __asm__ volatile ("outw %%ax, %%dx"
                      : : "a"(v), "d"(port) : "cc", "memory");
}

/* int 16h AH=0: wait for key, returns AX (AH=scan code, AL=ASCII). */
static unsigned short wait_key(void) {
    unsigned short ax = 0x0000;
    __asm__ volatile ("int $0x16" : "+a"(ax) : : "cc", "memory");
    return ax;
}

/* Blit `size` bytes from our data segment (SS) to `dst_seg`:0 via REP MOVSB.
 * See preamble comment re: DS scratch-register discipline. */
static void __attribute__((unused))
blit_seg(unsigned short dst_seg, const unsigned char *src, unsigned short size)
{
    __asm__ volatile (
        "pushw %%ds\n\t"
        "pushw %%es\n\t"
        "pushw %%ss\n\t"
        "popw  %%ds\n\t"
        "movw  %%bx, %%es\n\t"
        "xorw  %%di, %%di\n\t"
        "cld\n\t"
        "rep movsb\n\t"
        "popw  %%es\n\t"
        "popw  %%ds\n\t"
        : "+S"(src), "+c"(size)
        : "b"(dst_seg)
        : "di", "cc", "memory");
}

/* Legacy CGA-specific blit (kept for modes whose setup code uses it). */
static void __attribute__((unused))
blit_b800(const unsigned char *src, unsigned short size) {
    blit_seg(0xB800, src, size);
}
)C_";

// Per-mode video setup. Returns the mode-setup code block (between
// set_mode() and the blit) and the BIOS mode number.
struct ModeSetup {
    int bios_mode;
    std::string setup;    // additional configuration after set_mode
    std::string palette;  // 0x3D9 / 0x3D8 register writes
};

ModeSetup mode_setup(amiga::Mode m, std::uint8_t cga_mode_ctrl2) {
    using namespace amiga;
    ModeSetup s;
    switch (m) {
    case Mode::cga_320:
        s.bios_mode = 0x04;
        // Direct write to port 0x3D9 (CGA mode-control register 2) so
        // palette + intensity + background match exactly the variant the
        // encoder picked. BIOS int 10h AH=0Bh doesn't reliably set the
        // intensity bit across BIOSes, and the encoder's auto-selector
        // can pick any of the 4 variants (p0-low/high, p1-low/high).
        // Hardcoded 0x30 (palette 1 bright) would produce correct CGA
        // colors but with wrong pixel mapping if encoder chose p0.
        s.palette = std::format(
            "    /* CGA 0x3D9 mode-control 2: palette variant + bg the\n"
            "       encoder quantized against (auto-picked). */\n"
            "    outb(0x3D9, 0x{:02X});\n", cga_mode_ctrl2);
        break;
    case Mode::cga_640:
        s.bios_mode = 0x06;
        // Mode 6 is 640x200 mono; no palette to configure (fixed B&W on composite;
        // fg = text-attr foreground on RGB). BIOS mode-set sufficient.
        s.palette = "    /* CGA 640x200 mono: no palette register to configure. */\n";
        break;
    case Mode::cga_composite:
        s.bios_mode = 0x04;
        // Mode 4 = CGA 320x200 4-color. For composite NTSC artifact colors,
        // we keep the color-burst enabled (mode register 0x3D8 bit 2 = 0) so
        // the composite monitor's NTSC decoder picks up the chroma and
        // produces the 16-color artifact palette. BIOS mode-set already
        // leaves burst on; write it explicitly for idempotence.
        s.palette =
            "    /* CGA mode register 0x3D8: clear bit 2 (color burst = on). */\n"
            "    outb(0x3D8, 0x0A);\n";
        break;
    default:
        // Unsupported on the 16-bit path.
        s.bios_mode = -1;
        break;
    }
    return s;
}

} // namespace

namespace {

// cga_text80x100 viewer: 80 cols × 100 rows × 2 bytes = 16000 bytes
// linear in B800. Reprogram the 6845 from 80x25 (8-scan cells) to 80x100
// (2-scan cells) via the canonical LORES / FastDoom table that writes
// ALL 14 CRTC registers (not just the differences) — relying on BIOS
// mode-03 defaults is fragile because BIOS may leave cursor on, start-
// address non-zero, or subtly different H-timing. Same table as
// github.com/dschmenk/LORES/blob/main/SRC/LIB/LORES.C.
//
// Register values:
//   0 H total          0x71 (113) — 80-col H timing, same as mode 03
//   1 H displayed      0x50 (80)
//   2 H sync pos       0x59 (89)
//   3 H sync width     0x0F (15)
//   4 V total          0x7F (128 char rows)
//   5 V total adjust   0x06 (extra scanlines)
//   6 V displayed      0x64 (100 rows)
//   7 V sync pos       0x70 (112)
//   8 Interlace mode   0x02 (non-interlaced)
//   9 Max scan line    0x01 (2 scans / char row)
//  10 Cursor start     0x20 (cursor disabled — bit 5 = 1)
//  11 Cursor end       0x00
//  12 Start addr hi    0x00 — critical: BIOS may leave this non-zero
//  13 Start addr lo    0x00   and the CRTC would display garbage
//
// 0x3D8 bits: 0x01 = video OFF + 80-col during reprogram (avoids CGA
// snow and incomplete-latch artifacts); 0x09 = video ON + 80-col +
// blink OFF (attr bit 7 becomes bg intensity → 16 bg colors available).
std::string generate_cga_text(std::span<const std::uint8_t> char_attr,
                              std::size_t rows,
                              std::string_view sym) {
    std::string out;
    out.reserve(char_attr.size() * 6 + 2048);
    out += std::format(
        "/* Mode: cga-text80x{} (8x{} cells, 80x{} effective)\n"
        " * Symbol: {}\n"
        " */\n",
        rows, 200 / rows, rows, sym);
    out += kPreamble;
    out += std::format(
        "\n/* Char+attr pairs: byte 0 = char at (0,0), byte 1 = attr at (0,0), ... */\n"
        "static const unsigned char {}_data[{}] = {{\n",
        sym, char_attr.size());
    out += emit_byte_array(char_attr);
    out += "};\n\n";
    out += std::format(
        "/* Full 14-register 6845 CRTC init for 80x{} text (2-scan cells). */\n"
        "static const unsigned char crtc_init[14] = {{\n"
        "    0x71, 0x50, 0x59, 0x0F, 0x7F, 0x06, 0x64, 0x70,\n"
        "    0x02, 0x01, 0x20, 0x00, 0x00, 0x00\n"
        "}};\n\n"
        "int main(void) {{\n"
        "    set_mode(0x03);               /* BIOS 80x25 color text */\n"
        "    outb(0x3D8, 0x01);            /* video OFF, 80-col */\n"
        "    for (unsigned i = 0; i < 14; ++i) {{\n"
        "        outb(0x3D4, (unsigned char)i);\n"
        "        outb(0x3D5, crtc_init[i]);\n"
        "    }}\n"
        "    blit_b800({}_data, sizeof({}_data));\n"
        "    outb(0x3D8, 0x09);            /* video ON + blink OFF */\n"
        "    (void)wait_key();\n"
        "    set_mode(0x03);               /* BIOS mode-set restores 6845 */\n"
        "    return 0;\n"
        "}}\n",
        rows, sym, sym);
    return out;
}

} // namespace

namespace {

// EGA planar viewer (ega_320 = mode 0Dh, ega_640 = mode 0Eh). On IBM
// EGA at 200-line scan the card gates off ATC bits 5 and 3 (secondary
// r'/b'), leaving only 16 IRGB colors — which is why main.cpp locks
// the palette to kCgaHw and cheader_dos::encode_palette produces
// CGA-compat IRGB bytes (b4=I, b2=R, b1=G, b0=B).
//
// For VGA hardware emulating mode 0Dh/0Eh, the DAC is ALSO populated
// with kCgaHw-equivalent RGB triples so the same ATC values produce
// the same on-screen colors. On real EGA the DAC writes are ignored
// (no DAC chip), so they're harmless.
//
// Target: -march=i80286 -Os (works on 286+, since EGA wasn't common
// below 286; will also compile with -march=i8086 if 8088 support is
// desired — 286 instructions are only used if gcc chooses to emit them).
std::string generate_ega_graphics(amiga::Mode mode,
                                  std::size_t width, std::size_t height,
                                  std::span<const std::uint8_t> planes,
                                  std::span<const std::uint8_t> palette,
                                  std::string_view sym) {
    std::size_t plane_bytes = planes.size() / 4;
    std::string out;
    out.reserve(planes.size() * 6 + 4096);
    out += std::format(
        "/* Mode: {} ({} x {} at 200-line, CGA-compat 16 IRGB)\n"
        " * Symbol: {}\n"
        " * Build: ia16-elf-gcc -march=i80286 -mcmodel=small -Os \\\n"
        " *             -o viewer.exe viewer.c\n"
        " * -mcmodel=small (separate code + data segments) is required\n"
        " * because 4 × plane-bytes of rodata won't fit the combined\n"
        " * code+data segment of the default tiny (.COM) model.\n"
        " */\n",
        mode == amiga::Mode::ega_320 ? "ega-320 (mode 0Dh)"
                                     : "ega-640 (mode 0Eh)",
        width, height, sym);
    out += kPreamble;
    // 4 plane arrays instead of one concatenated array. A single 64000-
    // byte array for ega_640 overflows the 16-bit data-segment limit
    // (can't fit code + stack + 64K rodata in one 64KB segment); per-
    // plane arrays stay under 16 KB each and leave room for everything.
    for (std::size_t p = 0; p < 4; ++p) {
        out += std::format(
            "\n/* EGA bitplane {}. */\n"
            "static const unsigned char {}_plane{}[{}] = {{\n",
            p, sym, p, plane_bytes);
        out += emit_byte_array(
            planes.subspan(p * plane_bytes, plane_bytes));
        out += "};\n";
    }
    out += std::format(
        "\nstatic const unsigned char *const {}_planes[4] = {{\n"
        "    {}_plane0, {}_plane1, {}_plane2, {}_plane3\n"
        "}};\n\n",
        sym, sym, sym, sym, sym);
    // 16-byte ATC palette (CGA-compat IRGB: b4=I, b2=R, b1=G, b0=B).
    out += std::format(
        "/* 16 ATC palette bytes (CGA-IRGB). */\n"
        "static const unsigned char {}_palette[16] = {{\n",
        sym);
    out += emit_byte_array(palette);
    out += "};\n\n";
    out += std::format(
        "int main(void) {{\n"
        "    set_mode(0x{:02X});\n"
        "    /* VGA DAC: make slots {{0..7, 0x10..0x17}} match the 16 kCgaHw\n"
        "     * RGB colors so ATC IRGB values render correctly on VGA. On\n"
        "     * real EGA there's no DAC and these writes are ignored. */\n"
        "    static const unsigned char cga_dac[48] = {{\n"
        "         0, 0, 0,   0, 0,42,   0,42, 0,   0,42,42,\n"
        "        42, 0, 0,  42, 0,42,  42,21, 0,  42,42,42,\n"
        "        21,21,21,  21,21,63,  21,63,21,  21,63,63,\n"
        "        63,21,21,  63,21,63,  63,63,21,  63,63,63\n"
        "    }};\n"
        "    for (unsigned i = 0; i < 16; ++i) {{\n"
        "        unsigned slot = (i < 8) ? i : (0x10 + (i - 8));\n"
        "        outb(0x3C8, (unsigned char)slot);\n"
        "        outb(0x3C9, cga_dac[i*3 + 0]);\n"
        "        outb(0x3C9, cga_dac[i*3 + 1]);\n"
        "        outb(0x3C9, cga_dac[i*3 + 2]);\n"
        "    }}\n"
        "    /* Load ATC palette regs 0..15 (video blanked while bit 5 = 0). */\n"
        "    for (unsigned i = 0; i < 16; ++i) {{\n"
        "        (void)inb(0x3DA);               /* reset ATC flip-flop */\n"
        "        outb(0x3C0, (unsigned char)i);   /* index */\n"
        "        outb(0x3C0, {}_palette[i]);      /* value */\n"
        "    }}\n"
        "    (void)inb(0x3DA);\n"
        "    outb(0x3C0, 0x20);                 /* re-enable video */\n"
        "    /* Blit 4 planes via sequencer map-mask (port 0x3C4 idx 2). */\n"
        "    for (unsigned p = 0; p < 4; ++p) {{\n"
        "        outw(0x3C4, (unsigned short)(0x0002 | ((1u << p) << 8)));\n"
        "        blit_seg(0xA000, {}_planes[p], {}u);\n"
        "    }}\n"
        "    (void)wait_key();\n"
        "    set_mode(0x03);\n"
        "    return 0;\n"
        "}}\n",
        mode == amiga::Mode::ega_320 ? 0x0D : 0x0E,
        sym, sym, plane_bytes);
    return out;
}

// VGA Mode 13h: 320×200 chunky, 8 bits/pixel, DAC-mapped to 256 colors.
// Linear at A000:0000, 64000 bytes exactly. Split into 2 × 32000-byte
// halves because a single 64000-byte const array + bss + stack blows
// past the small-model 64 KB data-segment limit; two halves leave
// plenty of headroom. Blit the halves back-to-back to A000:0000 and
// A000:7D00 (=32000).
std::string generate_vga_13h(std::size_t width, std::size_t height,
                             std::span<const std::uint8_t> pixels,
                             std::span<const std::uint8_t> palette_dac,
                             std::string_view sym) {
    constexpr std::size_t kHalf = 32000;
    std::string out;
    out.reserve(pixels.size() * 6 + 4096);
    out += std::format(
        "/* Mode: vga-13h (320x200x256, chunky, linear at A000:0)\n"
        " * Image: {} x {}\n"
        " * Symbol: {}\n"
        " * Build: ia16-elf-gcc -march=i80286 -mcmodel=small -Os \\\n"
        " *             -o viewer.exe viewer.c\n"
        " */\n",
        width, height, sym);
    out += kPreamble;
    out += std::format(
        "\n/* 320x200 chunky, first half (rows 0..99). */\n"
        "static const unsigned char {}_data0[{}] = {{\n",
        sym, kHalf);
    out += emit_byte_array(pixels.subspan(0, kHalf));
    out += "};\n\n";
    out += std::format(
        "/* 320x200 chunky, second half (rows 100..199). */\n"
        "static const unsigned char {}_data1[{}] = {{\n",
        sym, kHalf);
    out += emit_byte_array(pixels.subspan(kHalf, pixels.size() - kHalf));
    out += "};\n\n";
    out += std::format(
        "/* VGA DAC: 256 × 3 bytes (R,G,B), each 6-bit (0..63). */\n"
        "static const unsigned char {}_dac[{}] = {{\n",
        sym, palette_dac.size());
    out += emit_byte_array(palette_dac);
    out += "};\n\n";
    // A000:7D00 (offset 32000) addresses row 100 onwards. Since 0x7D00
    // is a byte offset < 64 KB, we can blit to segment 0xA000 + offset
    // 0x7D00 by either (a) using a different destination segment that
    // already advances by 32000 bytes = 0x7D00 = 2000 paragraphs, so
    // segment 0xA000 + 0x07D0 = 0xA7D0, or (b) doing a second blit_seg
    // to 0xA000 and fixing up DI. Easier: use segment 0xA7D0 (=0xA000
    // + 0x07D0) with DI=0 for the second half.
    out += std::format(
        "int main(void) {{\n"
        "    set_mode(0x13);\n"
        "    outb(0x3C8, 0);\n"
        "    for (unsigned i = 0; i < 768u; ++i)\n"
        "        outb(0x3C9, {}_dac[i]);\n"
        "    blit_seg(0xA000, {}_data0, {}u);\n"
        "    blit_seg(0xA7D0, {}_data1, {}u);  /* A000:7D00 = rows 100.. */\n"
        "    (void)wait_key();\n"
        "    set_mode(0x03);\n"
        "    return 0;\n"
        "}}\n",
        sym, sym, kHalf, sym, kHalf);
    return out;
}

// VGA Mode Y: 320×200 planar (8bpp chunky unchained into 4 column-
// interleaved 8K planes). Same palette+DAC as 13h; different memory
// layout lets us do page flipping from a single 64 KB VGA bank.
// Sequencer map-mask selects which plane the CPU writes. Total:
// 4 × 8000 = 32000 bytes, fits tiny model (.COM).
std::string generate_vga_modey(std::span<const std::uint8_t> planes,
                               std::span<const std::uint8_t> palette_dac,
                               std::string_view sym) {
    std::size_t plane_bytes = planes.size() / 4;
    std::string out;
    out.reserve(planes.size() * 6 + 4096);
    out += std::format(
        "/* Mode: vga-modey (320x200x256, unchained planar, Mode Y)\n"
        " * Symbol: {}\n"
        " * Build: ia16-elf-gcc -march=i80286 -Os -o viewer.exe viewer.c\n"
        " */\n",
        sym);
    out += kPreamble;
    for (std::size_t p = 0; p < 4; ++p) {
        out += std::format(
            "\n/* Mode-Y plane {}: every 4th pixel column (x % 4 == {}). */\n"
            "static const unsigned char {}_plane{}[{}] = {{\n",
            p, p, sym, p, plane_bytes);
        out += emit_byte_array(planes.subspan(p * plane_bytes, plane_bytes));
        out += "};\n";
    }
    out += std::format(
        "\nstatic const unsigned char *const {}_planes[4] = {{\n"
        "    {}_plane0, {}_plane1, {}_plane2, {}_plane3\n"
        "}};\n\n",
        sym, sym, sym, sym, sym);
    out += std::format(
        "static const unsigned char {}_dac[{}] = {{\n", sym, palette_dac.size());
    out += emit_byte_array(palette_dac);
    out += "};\n\n";
    out += std::format(
        "int main(void) {{\n"
        "    set_mode(0x13);                       /* 320x200x256 chunky */\n"
        "    /* Sequencer memory-mode (reg 4) = 0x06:\n"
        "     *   bit 3 = 0 → chain-4 OFF\n"
        "     *   bit 2 = 1 → sequential addressing (odd/even OFF)\n"
        "     *   bit 1 = 1 → extended memory enable */\n"
        "    outw(0x3C4, 0x0604);\n"
        "    /* Graphics Controller reg 5 (Mode): bit 4 = 0 → host odd/even\n"
        "     * read OFF. BIOS mode 13h leaves this ON, which splits the\n"
        "     * planes weirdly under CPU read/write. Preserve other bits. */\n"
        "    outb(0x3CE, 0x05);\n"
        "    outb(0x3CF, (unsigned char)(inb(0x3CF) & ~0x10));\n"
        "    /* Graphics Controller reg 6 (Misc): bit 1 = 0 → linear CPU\n"
        "     * addressing (no odd/even split). */\n"
        "    outb(0x3CE, 0x06);\n"
        "    outb(0x3CF, (unsigned char)(inb(0x3CF) & ~0x02));\n"
        "    /* CRTC underline (reg 0x14): bit 6 = 0 → doubleword OFF */\n"
        "    outb(0x3D4, 0x14); outb(0x3D5, 0x00);\n"
        "    /* CRTC mode control (reg 0x17) = 0xE3 → byte addressing,\n"
        "     * normal vertical timing (inherits mode-13h's 200-line). */\n"
        "    outb(0x3D4, 0x17); outb(0x3D5, 0xE3);\n"
        "    /* CRTC offset (reg 0x13) = 40: row stride = 40*2 = 80 bytes\n"
        "     * per plane per scanline (320 pixels / 4 planes). */\n"
        "    outb(0x3D4, 0x13); outb(0x3D5, 0x28);\n"
        "    /* Display start = 0. */\n"
        "    outb(0x3D4, 0x0C); outb(0x3D5, 0x00);\n"
        "    outb(0x3D4, 0x0D); outb(0x3D5, 0x00);\n"
        "    /* Load DAC. */\n"
        "    outb(0x3C8, 0);\n"
        "    for (unsigned i = 0; i < 768u; ++i)\n"
        "        outb(0x3C9, {}_dac[i]);\n"
        "    /* Blit 4 planes via sequencer map-mask. */\n"
        "    for (unsigned p = 0; p < 4; ++p) {{\n"
        "        outw(0x3C4, (unsigned short)(0x0002 | ((1u << p) << 8)));\n"
        "        blit_seg(0xA000, {}_planes[p], {}u);\n"
        "    }}\n"
        "    (void)wait_key();\n"
        "    set_mode(0x03);\n"
        "    return 0;\n"
        "}}\n",
        sym, sym, plane_bytes);
    return out;
}

// EGA text 80x200: 200 char rows × 80 cols × 2 bytes = 32000 bytes of
// char+attr, plus 8192 bytes of custom 8x14 font (pre-shifted so row 0
// of each glyph is the slice the encoder picked). Runs at 200-line scan
// (5153 CGA monitor compatible) — that gates ATC bits 5 and 3 off, so
// we use the same CGA-compat IRGB palette as ega_320/640.
//
// CRTC reprogramming from BIOS mode-03 (200-line, 25 rows × 8-scan):
//   reg 0x09 Max scan line        = 0 (1 scan per char row)
//   reg 0x06 V total low 8 bits   = 0x05 (for V total = 261 scanlines)
//   reg 0x07 Overflow             = OR 0x01 (V total bit 8 = 1)
//   reg 0x12 V disp end low 8     = 0xC7 (199 + 1 = 200 rows visible)
//   reg 0x10 V retrace start low  = 0xC8 (starts right after display)
//   reg 0x15 V blank start low    = 0xC7
//   reg 0x16 V blank end          = 0x04
// Keeps BIOS's horizontal timing + offset (40 = 80 bytes per plane per
// scanline, still right since 1 scan cell × 80 chars/row = 80 bytes).
std::string generate_ega_text_80x200(std::span<const std::uint8_t> char_attr,
                                     std::span<const std::uint8_t> font,
                                     std::span<const std::uint8_t> atc_palette,
                                     std::string_view sym) {
    std::string out;
    out.reserve(char_attr.size() * 6 + font.size() * 6 + 4096);
    out += std::format(
        "/* Mode: ega-text80x200 (EGA 80x200 at 200-line, kCgaHw 16 IRGB)\n"
        " * Symbol: {}\n"
        " * Build: ia16-elf-gcc -march=i80286 -mcmodel=small -Os \\\n"
        " *             -o viewer.exe viewer.c\n"
        " */\n",
        sym);
    out += kPreamble;
    out += std::format(
        "\n/* Char+attr pairs (byte 0 = char (0,0), byte 1 = attr (0,0), ...). */\n"
        "static const unsigned char {}_data[{}] = {{\n", sym, char_attr.size());
    out += emit_byte_array(char_attr);
    out += "};\n\n";
    out += std::format(
        "/* Custom 8x14 font, pre-shifted by encoder's scanline_offset. */\n"
        "static const unsigned char {}_font[{}] = {{\n", sym, font.size());
    out += emit_byte_array(font);
    out += "};\n\n";
    out += std::format(
        "/* 16 ATC palette bytes (CGA-compat IRGB: b4=I, b2=R, b1=G, b0=B). */\n"
        "static const unsigned char {}_palette[16] = {{\n", sym);
    out += emit_byte_array(atc_palette);
    out += "};\n\n";
    out += std::format(
        "int main(void) {{\n"
        "    /* EGA BIOS reads its DIP switches once at boot and stores the\n"
        "     * result in BIOS data area 0040:0088. Low nybble: 3/9 = ECD\n"
        "     * (350-line), 2/8 = CD (200-line). If we're on ECD, decrement\n"
        "     * the byte BEFORE calling mode-03 so BIOS programs the card\n"
        "     * for 200-line output (correct H/V timing, sync polarity, and\n"
        "     * palette registers — the 5154 ECD will auto-switch modes on\n"
        "     * the polarity change). Trick from reenigne.org blog:\n"
        "     * \"How to set 200-line text modes on EGA\".\n"
        "     * Restore the original byte on exit so the next program sees\n"
        "     * the real hardware. */\n"
        "    unsigned char saved_88;\n"
        "    __asm__ volatile (\n"
        "        \"pushw %%ds\\n\\t\"\n"
        "        \"xorw %%ax, %%ax\\n\\t\"\n"
        "        \"movw %%ax, %%ds\\n\\t\"\n"
        "        \"movb 0x488, %%al\\n\\t\"        /* read 0040:0088 */\n"
        "        \"movb %%al, %%ah\\n\\t\"          /* save original */\n"
        "        \"andb $0x0F, %%al\\n\\t\"\n"
        "        \"cmpb $0x03, %%al\\n\\t\"\n"
        "        \"je 11f\\n\\t\"\n"
        "        \"cmpb $0x09, %%al\\n\\t\"\n"
        "        \"jne 12f\\n\\t\"\n"
        "        \"11:\\n\\t\"\n"
        "        \"movb %%ah, %%al\\n\\t\"\n"
        "        \"decb %%al\\n\\t\"\n"
        "        \"movb %%al, 0x488\\n\\t\"\n"
        "        \"12:\\n\\t\"\n"
        "        \"movb %%ah, %%al\\n\\t\"           /* return original in AL */\n"
        "        \"popw %%ds\\n\\t\"\n"
        "        : \"=a\"(saved_88) : : \"cc\", \"memory\");\n"
        "    set_mode(0x03);                       /* BIOS configures 200-line mode 03 */\n"
        "    /* Load custom font via int 10h AX=1110h. ES:BP = font ptr.\n"
        "     * Use SS (not DS) as source segment: ia16-elf-gcc keeps DS as\n"
        "     * a scratch register, so at this point DS may be 0 (IVT) or\n"
        "     * any other value. SS always points at the data segment per\n"
        "     * the ABI. Without this, BIOS reads font bytes from IVT →\n"
        "     * all-zero garbage font → black screen. Also push/pop SI so\n"
        "     * gcc's input-register assumption survives int 10h. */\n"
        "    __asm__ volatile (\n"
        "        \"pushw %%es\\n\\t\"\n"
        "        \"pushw %%bp\\n\\t\"\n"
        "        \"pushw %%si\\n\\t\"\n"
        "        \"pushw %%ss\\n\\t\"\n"
        "        \"popw %%es\\n\\t\"\n"
        "        \"movw %%si, %%bp\\n\\t\"\n"
        "        \"movw $0x1110, %%ax\\n\\t\"\n"
        "        \"movw $0x0020, %%bx\\n\\t\"\n"
        "        \"movw $0x0100, %%cx\\n\\t\"\n"
        "        \"xorw %%dx, %%dx\\n\\t\"\n"
        "        \"int $0x10\\n\\t\"\n"
        "        \"popw %%si\\n\\t\"\n"
        "        \"popw %%bp\\n\\t\"\n"
        "        \"popw %%es\\n\\t\"\n"
        "        : : \"S\"({}_font) : \"ax\", \"bx\", \"cx\", \"dx\", \"cc\", \"memory\");\n"
        "    /* Populate DAC[0..7, 0x10..0x17] with the 16 kCgaHw RGB triples\n"
        "     * (6-bit values) so VGA-emulating-EGA renders the same IRGB\n"
        "     * colors. No-op on real EGA (no DAC chip). */\n"
        "    static const unsigned char cga_dac[48] = {{\n"
        "         0, 0, 0,   0, 0,42,   0,42, 0,   0,42,42,\n"
        "        42, 0, 0,  42, 0,42,  42,21, 0,  42,42,42,\n"
        "        21,21,21,  21,21,63,  21,63,21,  21,63,63,\n"
        "        63,21,21,  63,21,63,  63,63,21,  63,63,63\n"
        "    }};\n"
        "    for (unsigned i = 0; i < 16; ++i) {{\n"
        "        unsigned slot = (i < 8) ? i : (0x10 + (i - 8));\n"
        "        outb(0x3C8, (unsigned char)slot);\n"
        "        outb(0x3C9, cga_dac[i*3 + 0]);\n"
        "        outb(0x3C9, cga_dac[i*3 + 1]);\n"
        "        outb(0x3C9, cga_dac[i*3 + 2]);\n"
        "    }}\n"
        "    /* Load ATC palette regs 0..15 with CGA-compat IRGB values. */\n"
        "    for (unsigned i = 0; i < 16; ++i) {{\n"
        "        (void)inb(0x3DA);\n"
        "        outb(0x3C0, (unsigned char)i);\n"
        "        outb(0x3C0, {}_palette[i]);\n"
        "    }}\n"
        "    /* Disable blink via ATC mode-control reg 0x10 (bit 3 clear) so\n"
        "     * attr bit 7 becomes bg intensity — unlocks all 16 bg colors.\n"
        "     * Proper ATC read-then-write sequence: read with bit5=1 (video\n"
        "     * on), write with bit5=0 (palette-access mode). */\n"
        "    {{\n"
        "        unsigned char v;\n"
        "        (void)inb(0x3DA);\n"
        "        outb(0x3C0, 0x30);                 /* index 0x10 + bit5=1 READ */\n"
        "        v = inb(0x3C1);\n"
        "        (void)inb(0x3DA);\n"
        "        outb(0x3C0, 0x10);                 /* index 0x10 + bit5=0 WRITE */\n"
        "        outb(0x3C0, (unsigned char)(v & ~0x08));\n"
        "    }}\n"
        "    (void)inb(0x3DA);\n"
        "    outb(0x3C0, 0x20);                    /* re-enable video */\n"
        "    /* Cursor off (reg 0x0A bit 5 = 1). */\n"
        "    outb(0x3D4, 0x0A); outb(0x3D5, 0x20);\n"
        "    /* Max scan line (reg 9) = 1 → 2-scan cells → 100 char rows\n"
        "     * visible (200 scanlines / 2). We encode 200 rows in data so\n"
        "     * only the top half shows; this is the proven \"EGA 200-line\n"
        "     * text\" configuration per the Reenigne article. A true 1-scan\n"
        "     * 80×200 needs CRTC V-total reprogramming via the overflow\n"
        "     * register and is deferred. */\n"
        "    outb(0x3D4, 0x09); outb(0x3D5, 0x01);\n"
        "    /* Blit char+attr to B8000. Text mode has CPU odd/even mapping\n"
        "     * (even = plane 0 chars, odd = plane 1 attrs). */\n"
        "    blit_seg(0xB800, {}_data, (unsigned short)sizeof({}_data));\n"
        "    (void)wait_key();\n"
        "    /* Restore BIOS data area byte before mode-reset so subsequent\n"
        "     * programs see the real monitor config. */\n"
        "    __asm__ volatile (\n"
        "        \"pushw %%ds\\n\\t\"\n"
        "        \"xorw %%dx, %%dx\\n\\t\"\n"
        "        \"movw %%dx, %%ds\\n\\t\"\n"
        "        \"movb %%al, 0x488\\n\\t\"\n"
        "        \"popw %%ds\\n\\t\"\n"
        "        : : \"a\"(saved_88) : \"dx\", \"cc\", \"memory\");\n"
        "    set_mode(0x03);                       /* BIOS mode-set resets state */\n"
        "    return 0;\n"
        "}}\n",
        sym, sym, sym, sym);
    return out;
}

} // namespace

Result<std::string>
generate(amiga::Mode mode,
         std::size_t width,
         std::size_t height,
         std::span<const std::uint8_t> raw_frame,
         std::span<const std::uint8_t> palette,
         const Options& options) {
    using namespace amiga;
    auto sym = sanitize_symbol(options.symbol_name);

    if (mode == Mode::cga_text80x100) {
        if (raw_frame.size() != 80 * 100 * 2) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("cheader_dos_c: cga_text80x100 expects 16000 "
                            "char+attr bytes, got {}", raw_frame.size())}};
        }
        return generate_cga_text(raw_frame, 100, sym);
    }
    if (mode == Mode::ega_text80x200) {
        if (raw_frame.size() != 80 * 200 * 2) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("cheader_dos_c: ega_text80x200 expects 32000 "
                            "char+attr bytes, got {}", raw_frame.size())}};
        }
        if (palette.size() != 16) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                "cheader_dos_c: ega_text80x200 needs 16-byte ATC palette"}};
        }
        if (options.font_data.size() != 256 * 32) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                "cheader_dos_c: ega_text80x200 needs 8192-byte font_data "
                "(256 glyphs × 32 bytes)"}};
        }
        return generate_ega_text_80x200(raw_frame, options.font_data,
                                        palette, sym);
    }
    if (mode == Mode::vga_13h) {
        if (raw_frame.size() != 64000) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("cheader_dos_c: vga_13h expects 64000 pixels, "
                            "got {}", raw_frame.size())}};
        }
        if (palette.size() != 768) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                "cheader_dos_c: vga_13h needs 768-byte DAC palette (256×3)"}};
        }
        return generate_vga_13h(width, height, raw_frame, palette, sym);
    }
    if (mode == Mode::vga_modey) {
        if (raw_frame.size() != 64000) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("cheader_dos_c: vga_modey expects 64000 plane "
                            "bytes, got {}", raw_frame.size())}};
        }
        if (palette.size() != 768) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                "cheader_dos_c: vga_modey needs 768-byte DAC palette"}};
        }
        return generate_vga_modey(raw_frame, palette, sym);
    }
    if (mode == Mode::ega_320 || mode == Mode::ega_640) {
        std::size_t expected = (mode == Mode::ega_320)
            ? 4 * 320 * 200 / 8    // 4 planes × 8000 bytes
            : 4 * 640 * 200 / 8;   // 4 planes × 16000 bytes
        if (raw_frame.size() != expected) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("cheader_dos_c: {} expects {} plane bytes, got {}",
                    mode == Mode::ega_320 ? "ega_320" : "ega_640",
                    expected, raw_frame.size())}};
        }
        if (palette.size() != 16) {
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                "cheader_dos_c: EGA modes need 16-byte ATC palette"}};
        }
        return generate_ega_graphics(mode, width, height, raw_frame,
                                     palette, sym);
    }
    if (mode != Mode::cga_320 && mode != Mode::cga_640 &&
        mode != Mode::cga_composite) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "cheader_dos_c: supported modes are cga_320 / cga_640 / "
            "cga_composite / cga_text80x100 / ega_320 / ega_640 / "
            "vga_13h / vga_modey"}};
    }
    if (raw_frame.size() != 16384) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("cheader_dos_c: expected 16 KB banked frame, got {}",
                        raw_frame.size())}};
    }
    auto setup = mode_setup(mode, options.cga_mode_ctrl2);
    const char* mode_name =
        (mode == Mode::cga_320)       ? "cga-320 (320x200x4)" :
        (mode == Mode::cga_640)       ? "cga-640 (640x200 mono)" :
        /*composite*/                   "cga-composite (160x200x16 NTSC)";

    std::string out;
    out.reserve(raw_frame.size() * 6 + 2048);
    out += std::format(
        "/* Mode: {}\n"
        " * Image: {} x {}\n"
        " * Symbol: {}\n"
        " */\n",
        mode_name, width, height, sym);
    out += kPreamble;
    out += std::format(
        "\nstatic const unsigned char {}_data[16384] = {{\n", sym);
    out += emit_byte_array(raw_frame);
    out += "};\n\n";
    out += std::format(
        "int main(void) {{\n"
        "    set_mode(0x{:02X});\n"
        "{}"
        "    blit_b800({}_data, sizeof({}_data));\n"
        "    (void)wait_key();\n"
        "    set_mode(0x03);  /* back to 80x25 text */\n"
        "    return 0;\n"
        "}}\n",
        setup.bios_mode, setup.palette, sym, sym);
    return out;
}

Result<void>
save(std::string_view path,
     amiga::Mode mode,
     std::size_t width,
     std::size_t height,
     std::span<const std::uint8_t> raw_frame,
     std::span<const std::uint8_t> palette,
     const Options& options) {
    auto src = generate(mode, width, height, raw_frame, palette, options);
    if (!src) return std::unexpected{src.error()};
    std::ofstream f{std::string{path}};
    if (!f) return std::unexpected{Error{
        ErrorCode::write_failed,
        std::format("cannot open {} for writing", path)}};
    f << *src;
    return {};
}

} // namespace png2amiga::cheader_dos_c
