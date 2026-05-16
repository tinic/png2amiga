#include "cheader_dos_c.hpp"
#include "palette.hpp"

#include <format>
#include <fstream>

namespace png2amiga::cheader_dos_c {

namespace {

std::string sanitize_symbol(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            out += c;
        else
            out += '_';
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out.insert(out.begin(), '_');
    return out;
}

std::string emit_byte_array(std::span<const std::uint8_t> data, std::size_t per_row = 16) {
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

/* Read from a port and DISCARD the result, declaring %ax as a real
 * clobber. Workaround for a gcc optimization where `(void)inb(p)`
 * with the "=a" variant lets gcc assume %al is preserved across the
 * asm — which silently breaks any loop that keeps its counter in %al,
 * because `inb %dx, %al` actually writes %al with the port value.
 * Symptom: ATC palette registers receive random status-byte bits
 * instead of the intended palette index/value; viewer hangs in a
 * random-walk loop until AL happens to hit the termination check. */
static void __attribute__((unused)) inb_discard(unsigned short port) {
    __asm__ volatile ("inb %%dx, %%al"
                      : : "d"(port) : "ax", "cc");
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

/* Blit `size` bytes from a __far source (seg:off) to `dst_seg`:0 via
 * REP MOVSB. Used for ega_hi / other >64 KB modes where the plane data
 * lives in its own far segment (via the .farrodata.*NN linker bins).
 * The __far pointer is passed as a 32-bit long — seg in high 16 bits,
 * offset in low 16 bits — so we load DS:SI directly from it. */
static void __attribute__((unused))
blit_seg_far(unsigned short dst_seg, const unsigned char __far *src,
             unsigned short size) {
    unsigned long src_fp = (unsigned long)src;
    unsigned short src_off = (unsigned short)src_fp;
    unsigned short src_seg = (unsigned short)(src_fp >> 16);
    __asm__ volatile (
        "pushw %%ds\n\t"
        "pushw %%es\n\t"
        "movw  %%bx, %%es\n\t"
        "movw  %%dx, %%ds\n\t"      /* DS = source segment */
        "xorw  %%di, %%di\n\t"
        "cld\n\t"
        "rep movsb\n\t"
        "popw  %%es\n\t"
        "popw  %%ds\n\t"
        : "+S"(src_off), "+c"(size)
        : "b"(dst_seg), "d"(src_seg)
        : "di", "cc", "memory");
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
        s.palette = std::format("    /* CGA 0x3D9 mode-control 2: palette variant + bg the\n"
                                "       encoder quantized against (auto-picked). */\n"
                                "    outb(0x3D9, 0x{:02X});\n",
                                cga_mode_ctrl2);
        break;
    case Mode::cga_640:
        s.bios_mode = 0x06;
        // Mode 6 is 640x200 mono; no palette to configure (fixed B&W on composite;
        // fg = text-attr foreground on RGB). BIOS mode-set sufficient.
        s.palette = "    /* CGA 640x200 mono: no palette register to configure. */\n";
        break;
    case Mode::cga_composite:
        s.bios_mode = 0x04;
        // Mode 4 = CGA 320x200 4-color, composite output. Two register
        // writes: 0x3D8 to keep colour-burst on (NTSC chroma decoder
        // produces the artifact palette), and 0x3D9 to pin the palette
        // variant + intensity + bg the encoder quantized against. The
        // BIOS leaves whatever the previous mode set in 0x3D9 — without
        // an explicit write, MartyPC and real hardware interpret the
        // 2bpp framebuffer through the wrong 4-colour palette.
        s.palette = std::format(
            "    /* CGA mode register 0x3D8: clear bit 2 (color burst = on). */\n"
            "    outb(0x3D8, 0x0A);\n"
            "    /* CGA 0x3D9: palette variant + intensity + bg from the\n"
            "       encoder's auto-picked variant. */\n"
            "    outb(0x3D9, 0x{:02X});\n",
            cga_mode_ctrl2);
        break;
    case Mode::cga_composite_hires:
        s.bios_mode = 0x06;
        // Mode 6 = CGA 640x200 1bpp, composite output. 0x3D8 bit 4
        // (0x10) selects hires graphics; bit 1 = graphics; bit 3 =
        // video enable; bit 2 = 0 keeps colour-burst on. 0x3D9 low
        // nibble = foreground colour (RGBI 0..15). BG is fixed at
        // black on CGA mode 6 (hardware can't override).
        s.palette = std::format(
            "    /* CGA mode register 0x3D8: hires graphics, colour burst on. */\n"
            "    outb(0x3D8, 0x1A);\n"
            "    /* CGA 0x3D9: foreground colour (low nibble) for mode 6. */\n"
            "    outb(0x3D9, 0x{:02X});\n",
            cga_mode_ctrl2 & 0x0F);
        break;
    default:
        // Unsupported on the 16-bit path.
        s.bios_mode = -1;
        break;
    }
    return s;
}

}  // namespace

namespace {

// cga_text80x100 / 80x50 / 80x25 viewer: 80 cols × {100, 50, 25} rows ×
// 2 bytes = {16000, 8000, 4000} bytes linear in B800. Reprogram the 6845
// to use {2, 4, 8}-scanline cells via a canonical LORES / FastDoom-style
// table that writes ALL 14 CRTC registers (not just the differences) —
// relying on BIOS mode-03 defaults is fragile because BIOS may leave
// cursor on, start-address non-zero, or subtly different H-timing.
//
// Register layout (constant H-timing, V-timing scales with cell_h to
// keep the 200-line / 60 Hz NTSC frame intact):
//   0  H total          0x71      H timing identical to BIOS mode 03
//   1  H displayed      0x50      (80-column).
//   2  H sync pos       0x59
//   3  H sync width     0x0F
//   4  V total          (256 / cell_h) − 1     scaled char-row count
//   5  V total adjust   0x06                   trailing scanlines
//   6  V displayed      rows                   visible char rows
//   7  V sync pos       224 / cell_h           vsync at scan 224
//   8  Interlace mode   0x02                   non-interlaced
//   9  Max scan line    cell_h − 1             scanlines per cell minus 1
//   10 Cursor start     0x20                   cursor disabled (bit 5)
//   11 Cursor end       0x00
//   12 Start addr hi    0x00                   BIOS may leave non-zero
//   13 Start addr lo    0x00
//
// Total scanlines (canonical NTSC frame): (V_total + 1) × cell_h
// + V_total_adj = 256 + 6 = 262 — matches the original CGA mode 03.
//
// Per-mode register values (cell_h shown in the 4-scan / 8-scan rows):
//   80x100 (cell_h=2): V_total=0x7F  V_disp=0x64  V_sync=0x70  Max=0x01
//   80x50  (cell_h=4): V_total=0x3F  V_disp=0x32  V_sync=0x38  Max=0x03
//   80x25  (cell_h=8): V_total=0x1F  V_disp=0x19  V_sync=0x1C  Max=0x07
//                                                  ^- standard BIOS mode 03
//
// 0x3D8 bits: 0x01 = video OFF + 80-col during reprogram (avoids CGA
// snow and incomplete-latch artifacts); 0x09 = video ON + 80-col +
// blink OFF (attr bit 7 becomes bg intensity → 16 bg colors available).
std::string generate_cga_text(std::span<const std::uint8_t> char_attr,
                              std::size_t cols,
                              std::size_t rows,
                              std::string_view sym) {
    const std::size_t cell_h = 200 / rows;
    const unsigned v_total = static_cast<unsigned>(256u / cell_h - 1u);
    const unsigned v_displayed = static_cast<unsigned>(rows) & 0xFFu;
    const unsigned v_sync_pos = static_cast<unsigned>(224u / cell_h);
    const unsigned max_scan = static_cast<unsigned>(cell_h - 1) & 0xFFu;

    // 6845 horizontal regs differ between 80-col and 40-col CRTC modes
    // (BIOS-init values for modes 03h vs 01h).
    const bool is_40col = (cols == 40);
    const unsigned h_total = is_40col ? 0x38u : 0x71u;
    const unsigned h_displayed = is_40col ? 0x28u : 0x50u;
    const unsigned h_sync_pos = is_40col ? 0x2Du : 0x59u;
    const unsigned h_sync_width = is_40col ? 0x0Au : 0x0Fu;
    // 0x3D8 mode-control bits:
    //   bit 0 (0x01): 80-col high-res; clear = 40-col
    //   bit 3 (0x08): video output enable
    // Reprogram phase: video off + correct column width.
    const unsigned mode_off = is_40col ? 0x00u : 0x01u;
    const unsigned mode_on = is_40col ? 0x08u : 0x09u;
    // BIOS mode 0x01 = 40x25 color text; 0x03 = 80x25 color text.
    const unsigned bios_mode = is_40col ? 0x01u : 0x03u;

    std::string out;
    out.reserve(char_attr.size() * 6 + 2048);
    out += std::format("/* Mode: cga-text{}x{} (8x{} cells, {}x{} effective)\n"
                       " * Symbol: {}\n"
                       " */\n",
                       cols,
                       rows,
                       cell_h,
                       cols,
                       rows,
                       sym);
    out += kPreamble;
    out += std::format(
        "\n/* Char+attr pairs: byte 0 = char at (0,0), byte 1 = attr at (0,0), ... */\n"
        "static const unsigned char {}_data[{}] = {{\n",
        sym,
        char_attr.size());
    out += emit_byte_array(char_attr);
    out += "};\n\n";
    out += std::format(
        "/* Full 14-register 6845 CRTC init for {}x{} text ({}-scan cells). */\n"
        "static const unsigned char crtc_init[14] = {{\n"
        "    0x{:02X}, 0x{:02X}, 0x{:02X}, 0x{:02X}, 0x{:02X}, 0x06, 0x{:02X}, 0x{:02X},\n"
        "    0x02, 0x{:02X}, 0x20, 0x00, 0x00, 0x00\n"
        "}};\n\n"
        "int main(void) {{\n"
        "    set_mode(0x{:02X});               /* BIOS {}x25 color text */\n"
        "    outb(0x3D8, 0x{:02X});            /* video OFF, {}-col */\n"
        "    for (unsigned i = 0; i < 14; ++i) {{\n"
        "        outb(0x3D4, (unsigned char)i);\n"
        "        outb(0x3D5, crtc_init[i]);\n"
        "    }}\n"
        "    blit_b800({}_data, sizeof({}_data));\n"
        "    outb(0x3D8, 0x{:02X});            /* video ON + blink OFF */\n"
        "    (void)wait_key();\n"
        "    set_mode(0x03);               /* BIOS mode-set restores 6845 */\n"
        "    return 0;\n"
        "}}\n",
        cols,
        rows,
        cell_h,
        h_total,
        h_displayed,
        h_sync_pos,
        h_sync_width,
        v_total,
        v_displayed,
        v_sync_pos,
        max_scan,
        bios_mode,
        cols,
        mode_off,
        cols,
        sym,
        sym,
        mode_on);
    return out;
}

}  // namespace

namespace {

// EGA planar viewer (ega_320 = mode 0Dh, ega_640 = mode 0Eh, ega_hi =
// mode 10h). At 200-line scan (0Dh/0Eh) the card gates ATC bits 5 and 3
// off, leaving only 16 IRGB colors — main.cpp locks to kCgaHw and emits
// CGA-compat IRGB bytes. At 350-line (10h) all 6 ATC output pins drive,
// so the palette is image-adaptive 16-of-64 via ega_histogram.
//
// Memory layout:
//   ega_320: 4 × 8000 bytes  = 32 KB total → tiny model
//   ega_640: 4 × 16000 bytes = 64 KB      → small model (2 × 32KB data segs)
//   ega_hi : 4 × 28000 bytes = 112 KB     → small model + far data sections
//                                           (one 64KB "bin" per plane via the
//                                           .farrodata.*NN linker-script
//                                           trick in ia16-elf-gcc)
std::string generate_ega_graphics(amiga::Mode mode,
                                  std::size_t width,
                                  std::size_t height,
                                  std::span<const std::uint8_t> planes,
                                  std::span<const std::uint8_t> palette,
                                  std::string_view sym) {
    std::size_t plane_bytes = planes.size() / 4;
    // Planes >28 KB total don't fit in one data segment — route via far
    // bins. ega_hi (112 KB) always does; ega_320/640 stay in near data.
    bool far_planes = planes.size() > 60000;
    std::string out;
    out.reserve(planes.size() * 6 + 4096);
    const char* mode_label = (mode == amiga::Mode::ega_320)   ? "ega-320 (mode 0Dh, 200-line)"
                             : (mode == amiga::Mode::ega_640) ? "ega-640 (mode 0Eh, 200-line)"
                                                              : "ega-hi (mode 10h, 350-line)";
    out += std::format("/* Mode: {} ({} x {})\n"
                       " * Symbol: {}\n"
                       " * Build: ia16-elf-gcc -march=i80286 -mcmodel=small -Os \\\n"
                       " *             -o viewer.exe viewer.c\n"
                       " */\n",
                       mode_label,
                       width,
                       height,
                       sym);
    out += kPreamble;
    // Plane storage. Near (default rodata) for ega_320/640 — both fit in
    // one 64 KB data segment. Far bins for ega_hi — 4 × 28 KB exceeds
    // 64 KB so we route each plane into its own 64 KB "bin" via named
    // sections .farrodata.<sym>_NN where NN is 00/01/02/03 octal.
    // Access is through far pointers loaded into DS:SI by the blit asm.
    for (std::size_t p = 0; p < 4; ++p) {
        if (far_planes) {
            out += std::format("\n/* EGA bitplane {} (far, linker-script bin {:02o}). */\n"
                               "__attribute__((section(\".farrodata.{}_{:02o}\")))\n"
                               "const __far unsigned char {}_plane{}[{}] = {{\n",
                               p,
                               p,
                               sym,
                               p,
                               sym,
                               p,
                               plane_bytes);
        } else {
            out += std::format("\n/* EGA bitplane {}. */\n"
                               "static const unsigned char {}_plane{}[{}] = {{\n",
                               p,
                               sym,
                               p,
                               plane_bytes);
        }
        out += emit_byte_array(planes.subspan(p * plane_bytes, plane_bytes));
        out += "};\n";
    }
    if (far_planes) {
        out += std::format("\nstatic const unsigned char __far *const {}_planes[4] = {{\n"
                           "    {}_plane0, {}_plane1, {}_plane2, {}_plane3\n"
                           "}};\n\n",
                           sym,
                           sym,
                           sym,
                           sym,
                           sym);
    } else {
        out += std::format("\nstatic const unsigned char *const {}_planes[4] = {{\n"
                           "    {}_plane0, {}_plane1, {}_plane2, {}_plane3\n"
                           "}};\n\n",
                           sym,
                           sym,
                           sym,
                           sym,
                           sym);
    }
    // 16-byte ATC palette (CGA-compat IRGB: b4=I, b2=R, b1=G, b0=B).
    out += std::format("/* 16 ATC palette bytes (CGA-IRGB). */\n"
                       "static const unsigned char {}_palette[16] = {{\n",
                       sym);
    out += emit_byte_array(palette);
    out += "};\n\n";
    out += std::format("int main(void) {{\n"
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
                       "        inb_discard(0x3DA);               /* reset ATC flip-flop */\n"
                       "        outb(0x3C0, (unsigned char)i);   /* index */\n"
                       "        outb(0x3C0, {}_palette[i]);      /* value */\n"
                       "    }}\n"
                       "    inb_discard(0x3DA);\n"
                       "    outb(0x3C0, 0x20);                 /* re-enable video */\n"
                       "    /* Blit 4 planes via sequencer map-mask (port 0x3C4 idx 2). */\n"
                       "    for (unsigned p = 0; p < 4; ++p) {{\n"
                       "        outw(0x3C4, (unsigned short)(0x0002 | ((1u << p) << 8)));\n"
                       "        {}(0xA000, {}_planes[p], {}u);\n"
                       "    }}\n"
                       "    (void)wait_key();\n"
                       "    set_mode(0x03);\n"
                       "    return 0;\n"
                       "}}\n",
                       (mode == amiga::Mode::ega_320)   ? 0x0D
                       : (mode == amiga::Mode::ega_640) ? 0x0E
                                                        : 0x10,
                       sym,
                       far_planes ? "blit_seg_far" : "blit_seg",
                       sym,
                       plane_bytes);
    return out;
}

// VGA Mode 13h: 320×200 chunky, 8 bits/pixel, DAC-mapped to 256 colors.
// Linear at A000:0000, 64000 bytes exactly. Split into 2 × 32000-byte
// halves because a single 64000-byte const array + bss + stack blows
// past the small-model 64 KB data-segment limit; two halves leave
// plenty of headroom. Blit the halves back-to-back to A000:0000 and
// A000:7D00 (=32000).
std::string generate_vga_13h(std::size_t width,
                             std::size_t height,
                             std::span<const std::uint8_t> pixels,
                             std::span<const std::uint8_t> palette_dac,
                             std::string_view sym) {
    constexpr std::size_t kHalf = 32000;
    std::string out;
    out.reserve(pixels.size() * 6 + 4096);
    out += std::format("/* Mode: vga-13h (320x200x256, chunky, linear at A000:0)\n"
                       " * Image: {} x {}\n"
                       " * Symbol: {}\n"
                       " * Build: ia16-elf-gcc -march=i80286 -mcmodel=small -Os \\\n"
                       " *             -o viewer.exe viewer.c\n"
                       " */\n",
                       width,
                       height,
                       sym);
    out += kPreamble;
    out += std::format("\n/* 320x200 chunky, first half (rows 0..99). */\n"
                       "static const unsigned char {}_data0[{}] = {{\n",
                       sym,
                       kHalf);
    out += emit_byte_array(pixels.subspan(0, kHalf));
    out += "};\n\n";
    out += std::format("/* 320x200 chunky, second half (rows 100..199). */\n"
                       "static const unsigned char {}_data1[{}] = {{\n",
                       sym,
                       kHalf);
    out += emit_byte_array(pixels.subspan(kHalf, pixels.size() - kHalf));
    out += "};\n\n";
    out += std::format("/* VGA DAC: 256 × 3 bytes (R,G,B), each 6-bit (0..63). */\n"
                       "static const unsigned char {}_dac[{}] = {{\n",
                       sym,
                       palette_dac.size());
    out += emit_byte_array(palette_dac);
    out += "};\n\n";
    // A000:7D00 (offset 32000) addresses row 100 onwards. Since 0x7D00
    // is a byte offset < 64 KB, we can blit to segment 0xA000 + offset
    // 0x7D00 by either (a) using a different destination segment that
    // already advances by 32000 bytes = 0x7D00 = 2000 paragraphs, so
    // segment 0xA000 + 0x07D0 = 0xA7D0, or (b) doing a second blit_seg
    // to 0xA000 and fixing up DI. Easier: use segment 0xA7D0 (=0xA000
    // + 0x07D0) with DI=0 for the second half.
    out += std::format("int main(void) {{\n"
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
                       sym,
                       sym,
                       kHalf,
                       sym,
                       kHalf);
    return out;
}

// VGA planar 16-color (mode 10h on VGA, mode 12h). Unlike ega_hi these
// drive the VGA's 18-bit DAC instead of ATC IrgbIRGB, so palette is
// 16 × 3 RGB bytes. ATC registers stay at BIOS pass-through (reg i = i).
//
//   vga_10h: 640×350 → 4 × 28 KB = 112 KB → far bins
//   vga_12h: 640×480 → 4 × 38.4 KB = 153.6 KB → far bins
std::string generate_vga_planar(amiga::Mode mode,
                                std::size_t width,
                                std::size_t height,
                                std::span<const std::uint8_t> planes,
                                std::span<const std::uint8_t> dac48,
                                std::string_view sym) {
    std::size_t plane_bytes = planes.size() / 4;
    // Two splits for oversized planes: per-array max is 32767 bytes
    // (gcc-ia16 signed-16 limit). vga_12h's 38400 bytes per plane gets
    // halved into 2 × 19200-byte sub-arrays. ega/vga 10h (28000 bytes
    // per plane) fits as a single array.
    bool split = plane_bytes > 32000;
    std::size_t half = plane_bytes / 2;  // only used when split
    std::size_t half_para = half / 16;   // paragraph count for seg math
    const char* mode_label = (mode == amiga::Mode::vga_10h) ? "vga-10h (640x350x16 planar)"
                                                            : "vga-12h (640x480x16 planar)";
    std::string out;
    out.reserve(planes.size() * 6 + 4096);
    out += std::format("/* Mode: {} ({} x {})\n"
                       " * Symbol: {}\n"
                       " * Build: ia16-elf-gcc -march=i80286 -mcmodel=small -Os \\\n"
                       " *             -o viewer.exe viewer.c\n"
                       " */\n",
                       mode_label,
                       width,
                       height,
                       sym);
    out += kPreamble;
    // Emit plane arrays. 4 planes, each either 1 array (~28K) or 2 halves
    // (~19K each, routed to separate far bins).
    std::size_t bin = 0;
    for (std::size_t p = 0; p < 4; ++p) {
        if (split) {
            for (std::size_t h = 0; h < 2; ++h) {
                std::size_t off = p * plane_bytes + h * half;
                std::size_t sz = half;
                out += std::format("\n/* Plane {} half {} (far, bin {:02o}). */\n"
                                   "__attribute__((section(\".farrodata.{}_{:02o}\")))\n"
                                   "const __far unsigned char {}_plane{}h{}[{}] = {{\n",
                                   p,
                                   h,
                                   bin,
                                   sym,
                                   bin,
                                   sym,
                                   p,
                                   h,
                                   sz);
                out += emit_byte_array(planes.subspan(off, sz));
                out += "};\n";
                ++bin;
            }
        } else {
            out += std::format("\n/* Plane {} (far, bin {:02o}). */\n"
                               "__attribute__((section(\".farrodata.{}_{:02o}\")))\n"
                               "const __far unsigned char {}_plane{}[{}] = {{\n",
                               p,
                               bin,
                               sym,
                               bin,
                               sym,
                               p,
                               plane_bytes);
            out += emit_byte_array(planes.subspan(p * plane_bytes, plane_bytes));
            out += "};\n";
            ++bin;
        }
    }
    // Plane pointer array at file scope (matches ega_hi structure).
    // Non-split modes only — split case dereferences each half inline.
    if (!split) {
        out += std::format("\nstatic const unsigned char __far *const {}_planes[4] = {{\n"
                           "    {}_plane0, {}_plane1, {}_plane2, {}_plane3\n"
                           "}};\n",
                           sym,
                           sym,
                           sym,
                           sym,
                           sym);
    }
    out += std::format("\n/* VGA DAC: 16 × 3 bytes (R,G,B, each 6-bit 0..63). */\n"
                       "static const unsigned char {}_dac[48] = {{\n",
                       sym);
    out += emit_byte_array(dac48);
    out += "};\n\n";
    out += std::format("int main(void) {{\n"
                       "    set_mode(0x{:02X});\n"
                       "    /* ATC palette pass-through (reg i = i) so raw 4-bit pixel\n"
                       "     * value → DAC index. */\n"
                       "    for (unsigned i = 0; i < 16; ++i) {{\n"
                       "        inb_discard(0x3DA);\n"
                       "        outb(0x3C0, (unsigned char)i);\n"
                       "        outb(0x3C0, (unsigned char)i);\n"
                       "    }}\n"
                       "    inb_discard(0x3DA);\n"
                       "    outb(0x3C0, 0x20);\n"
                       "    /* Load 16 × 3-byte DAC triples into slots 0..15. */\n"
                       "    outb(0x3C8, 0);\n"
                       "    for (unsigned i = 0; i < 48u; ++i)\n"
                       "        outb(0x3C9, {}_dac[i]);\n",
                       (mode == amiga::Mode::vga_10h) ? 0x10 : 0x12,
                       sym);
    // Blit loop: per plane set map mask, then 1 or 2 blits.
    if (split) {
        // Each half blitted to A000:0 and A000:(half) via segment fixup
        // (seg = 0xA000 + half/16, offset = 0).
        out += std::format("    for (unsigned p = 0; p < 4; ++p) {{\n"
                           "        outw(0x3C4, (unsigned short)(0x0002 | ((1u << p) << 8)));\n"
                           "        switch (p) {{\n");
        for (std::size_t p = 0; p < 4; ++p) {
            out += std::format("        case {}: blit_seg_far(0xA000, {}_plane{}h0, {}u);\n"
                               "                 blit_seg_far((unsigned short)(0xA000 + {}u),\n"
                               "                              {}_plane{}h1, {}u); break;\n",
                               p,
                               sym,
                               p,
                               half,
                               half_para,
                               sym,
                               p,
                               half);
        }
        out += "        }\n    }\n";
    } else {
        out += std::format("    for (unsigned p = 0; p < 4; ++p) {{\n"
                           "        outw(0x3C4, (unsigned short)(0x0002 | ((1u << p) << 8)));\n"
                           "        blit_seg_far(0xA000, {}_planes[p], {}u);\n"
                           "    }}\n",
                           sym,
                           plane_bytes);
    }
    out += "    (void)wait_key();\n"
           "    set_mode(0x03);\n"
           "    return 0;\n"
           "}\n";
    return out;
}

}  // namespace

Result<std::string> generate(amiga::Mode mode,
                             std::size_t width,
                             std::size_t height,
                             std::span<const std::uint8_t> raw_frame,
                             std::span<const std::uint8_t> palette,
                             const Options& options) {
    using namespace amiga;
    auto sym = sanitize_symbol(options.symbol_name);

    // cga_composite_text80x{100,200} borrow the cga_text viewer (CRTC
    // reprogram, mode 03 entry, blink-off mode 0x09 — same hardware
    // setup); row count is encoded into the mode name.
    bool is_composite_text = (mode == Mode::cga_composite_text80x100 ||
                              mode == Mode::cga_composite_text80x200);
    if (is_cga_text(mode) || is_composite_text) {
        std::size_t expected_cols = 80;
        std::size_t expected_rows;
        if (mode == Mode::cga_composite_text80x100)      expected_rows = 100;
        else if (mode == Mode::cga_composite_text80x200) expected_rows = 200;
        else {
            expected_cols = cga_text_cols(mode);
            expected_rows = cga_text_rows(mode);
        }
        std::size_t expected_bytes = expected_cols * expected_rows * 2;
        // 80x200 (32 KB) overflows the 16 KB CGA video RAM — refuse to
        // emit a viewer that would be silently broken (would display
        // the first 16 KB and read garbage from BIOS data after).
        // Every other variant (including the new 40x200 / 40x100) fits.
        bool fits = is_composite_text ? (expected_bytes <= 16384u)
                                       : cga_text_fits_vram(mode);
        if (!fits) {
            return std::unexpected{
                Error{ErrorCode::unsupported_mode,
                      std::format("cheader_dos_c: cga-text{}x{} buffer ({} bytes) "
                                  "overflows the 16384-byte CGA video RAM. Use "
                                  "this mode only for png/preview analysis.",
                                  expected_cols,
                                  expected_rows,
                                  expected_bytes)}};
        }
        if (raw_frame.size() != expected_bytes) {
            return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                         std::format("cheader_dos_c: cga-text{}x{} expects {} "
                                                     "char+attr bytes, got {}",
                                                     expected_cols,
                                                     expected_rows,
                                                     expected_bytes,
                                                     raw_frame.size())}};
        }
        return generate_cga_text(raw_frame, expected_cols, expected_rows, sym);
    }
    if (mode == Mode::vga_13h) {
        if (raw_frame.size() != 64000) {
            return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                         std::format("cheader_dos_c: vga_13h expects 64000 pixels, "
                                                     "got {}",
                                                     raw_frame.size())}};
        }
        if (palette.size() != 768) {
            return std::unexpected{
                Error{ErrorCode::invalid_dimensions,
                      "cheader_dos_c: vga_13h needs 768-byte DAC palette (256×3)"}};
        }
        return generate_vga_13h(width, height, raw_frame, palette, sym);
    }
    if (mode == Mode::ega_320 || mode == Mode::ega_640 || mode == Mode::ega_hi) {
        std::size_t expected = (mode == Mode::ega_320) ? 4 * 320 * 200 / 8 :  // 32000
                                   (mode == Mode::ega_640) ? 4 * 640 * 200 / 8
                                                           :  // 64000
                                   4 * 640 * 350 / 8;         // 112000 (ega_hi)
        if (raw_frame.size() != expected) {
            return std::unexpected{
                Error{ErrorCode::invalid_dimensions,
                      std::format("cheader_dos_c: {} expects {} plane bytes, got {}",
                                  (mode == Mode::ega_320)   ? "ega_320"
                                  : (mode == Mode::ega_640) ? "ega_640"
                                                            : "ega_hi",
                                  expected,
                                  raw_frame.size())}};
        }
        if (palette.size() != 16) {
            return std::unexpected{
                Error{ErrorCode::invalid_dimensions,
                      "cheader_dos_c: EGA planar modes need 16-byte ATC palette"}};
        }
        return generate_ega_graphics(mode, width, height, raw_frame, palette, sym);
    }
    if (mode == Mode::vga_10h || mode == Mode::vga_12h) {
        std::size_t expected = (mode == Mode::vga_10h) ? 4 * 640 * 350 / 8   // 112000
                                                       : 4 * 640 * 480 / 8;  // 153600
        if (raw_frame.size() != expected) {
            return std::unexpected{
                Error{ErrorCode::invalid_dimensions,
                      std::format("cheader_dos_c: {} expects {} plane bytes, got {}",
                                  (mode == Mode::vga_10h) ? "vga_10h" : "vga_12h",
                                  expected,
                                  raw_frame.size())}};
        }
        if (palette.size() != 48) {
            return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                         "cheader_dos_c: VGA planar modes need 48-byte DAC palette "
                                         "(16 × 3 RGB)"}};
        }
        return generate_vga_planar(mode, width, height, raw_frame, palette, sym);
    }
    if (mode != Mode::cga_320 && mode != Mode::cga_640 &&
        mode != Mode::cga_composite && mode != Mode::cga_composite_hires) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
                                     "cheader_dos_c: supported modes are cga_320 / cga_640 / "
                                     "cga_composite / cga_composite_hires / cga_text80x100 / "
                                     "ega_320 / ega_640 / ega_hi / vga_13h / vga_10h / vga_12h"}};
    }
    if (raw_frame.size() != 16384) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("cheader_dos_c: expected 16 KB banked frame, got {}", raw_frame.size())}};
    }
    auto setup = mode_setup(mode, options.cga_mode_ctrl2);
    const char* mode_name = (mode == Mode::cga_320) ? "cga-320 (320x200x4)"
                            : (mode == Mode::cga_640)
                                ? "cga-640 (640x200 mono)"
                            : (mode == Mode::cga_composite_hires)
                                ? "cga-composite-hires (640x200 1bpp NTSC)"
                                :
                                /*composite*/ "cga-composite (320x200x4 NTSC)";

    std::string out;
    out.reserve(raw_frame.size() * 6 + 2048);
    out += std::format("/* Mode: {}\n"
                       " * Image: {} x {}\n"
                       " * Symbol: {}\n"
                       " */\n",
                       mode_name,
                       width,
                       height,
                       sym);
    out += kPreamble;
    out += std::format("\nstatic const unsigned char {}_data[16384] = {{\n", sym);
    out += emit_byte_array(raw_frame);
    out += "};\n\n";
    out += std::format("int main(void) {{\n"
                       "    set_mode(0x{:02X});\n"
                       "{}"
                       "    blit_b800({}_data, sizeof({}_data));\n"
                       "    (void)wait_key();\n"
                       "    set_mode(0x03);  /* back to 80x25 text */\n"
                       "    return 0;\n"
                       "}}\n",
                       setup.bios_mode,
                       setup.palette,
                       sym,
                       sym);
    return out;
}

Result<void> save(std::string_view path,
                  amiga::Mode mode,
                  std::size_t width,
                  std::size_t height,
                  std::span<const std::uint8_t> raw_frame,
                  std::span<const std::uint8_t> palette,
                  const Options& options) {
    auto src = generate(mode, width, height, raw_frame, palette, options);
    if (!src) return std::unexpected{src.error()};
    std::ofstream f{std::string{path}};
    if (!f)
        return std::unexpected{
            Error{ErrorCode::write_failed, std::format("cannot open {} for writing", path)}};
    f << *src;
    return {};
}

std::vector<std::uint8_t> pack_cga_banked(std::span<const std::uint8_t> indices,
                                          std::size_t width,
                                          std::size_t height,
                                          amiga::Mode mode) {
    // cga_composite_hires is 1bpp at 640 wide — packs as monochrome.
    bool is_mono = (mode == amiga::Mode::cga_640 ||
                    mode == amiga::Mode::cga_composite_hires);
    bool is_composite_lores = (mode == amiga::Mode::cga_composite);
    auto buffer_width = is_composite_lores ? std::size_t{320} : width;
    auto row_bytes = buffer_width / (is_mono ? 8 : 4);
    std::vector<std::uint8_t> buf(16384, 0);
    for (std::size_t y = 0; y < height; ++y) {
        std::size_t bank_base = (y & 1) ? 0x2000u : 0x0000u;
        auto row_offset = bank_base + (y >> 1) * row_bytes;
        for (std::size_t bx = 0; bx < row_bytes; ++bx) {
            std::uint8_t byte = 0;
            if (is_mono) {
                for (std::size_t p = 0; p < 8; ++p) {
                    auto x = bx * 8 + p;
                    byte = static_cast<std::uint8_t>((byte << 1) |
                                                     (indices[y * width + x] != 0 ? 1 : 0));
                }
            } else if (is_composite_lores) {
                // 320 px × 2bpp = 4 pixels per byte, same layout as
                // real CGA mode 04. Indices are 0..3 selecting one of
                // the active palette variant's 4 RGBI colours.
                for (std::size_t p = 0; p < 4; ++p) {
                    auto idx = indices[y * width + bx * 4 + p] & 0x3;
                    byte = static_cast<std::uint8_t>((byte << 2) | idx);
                }
            } else {
                for (std::size_t p = 0; p < 4; ++p) {
                    auto x = bx * 4 + p;
                    auto idx = indices[y * width + x] & 0x3;
                    byte = static_cast<std::uint8_t>((byte << 2) | idx);
                }
            }
            buf[row_offset + bx] = byte;
        }
    }
    return buf;
}

}  // namespace png2amiga::cheader_dos_c
