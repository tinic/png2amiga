#include "cheader_dos.hpp"

#include "palette.hpp"

#include <cctype>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace png2amiga::cheader_dos {

namespace {

std::string format_bytes(std::span<const std::uint8_t> data,
                         const char* indent = "    ",
                         std::size_t per_row = 16) {
    std::string out;
    out.reserve(data.size() * 6);
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i % per_row == 0) {
            if (i > 0) out += "\n";
            out += indent;
        }
        out += std::format("0x{:02X}", data[i]);
        if (i + 1 < data.size()) out += ", ";
    }
    out += "\n";
    return out;
}

const char* mode_string(amiga::Mode m) noexcept {
    switch (m) {
    case amiga::Mode::vga_13h:         return "vga-13h";
    case amiga::Mode::vga_modex:       return "vga-modex";
    case amiga::Mode::vga_modey:       return "vga-modey";
    case amiga::Mode::vga_10h:         return "vga-10h";
    case amiga::Mode::vga_12h:         return "vga-12h";
    case amiga::Mode::ega_320:         return "ega-320";
    case amiga::Mode::ega_640:         return "ega-640";
    case amiga::Mode::ega_hi:          return "ega-hi";
    case amiga::Mode::ega_text80x200:  return "ega-text80x200";
    case amiga::Mode::cga_320:         return "cga-320";
    case amiga::Mode::cga_640:         return "cga-640";
    case amiga::Mode::cga_composite:   return "cga-composite";
    case amiga::Mode::cga_text80x100:  return "cga-text80x100";
    default:                           return "unknown";
    }
}

// BIOS video mode number for int 10h AH=00.
int bios_mode_ax(amiga::Mode m) noexcept {
    using namespace amiga;
    switch (m) {
    case Mode::vga_13h:
    case Mode::vga_modex:
    case Mode::vga_modey:        return 0x0013;
    case Mode::vga_10h:          return 0x0010;
    case Mode::vga_12h:          return 0x0012;
    case Mode::ega_320:          return 0x000D;  // EGA 320x200x16 planar
    case Mode::ega_640:          return 0x000E;  // EGA 640x200x16 planar
    case Mode::ega_hi:           return 0x0010;  // EGA 640x350x16 planar
    case Mode::ega_text80x200:   return 0x0003;  // 80x25 color text (EGA, 200-line)
    case Mode::cga_320:          return 0x0004;
    case Mode::cga_640:          return 0x0006;
    case Mode::cga_composite:    return 0x0004;
    case Mode::cga_text80x100:   return 0x0003;  // CGA 80x25 color text (8x8)
    default:                     return 0x0003;
    }
}

std::vector<std::uint8_t> encode_palette(amiga::Mode mode,
                                         std::span<const Color3f> palette) {
    std::vector<std::uint8_t> out;
    if (mode == amiga::Mode::ega_320 || mode == amiga::Mode::ega_640) {
        // CGA-compat EGA 200-line graphics output (modes 0Dh / 0Eh).
        // The IBM EGA card gates the secondary r' and b' pins (ATC
        // bits 5, 3) off and routes b4 (secondary g') to the CGA
        // "intensity" pin; b2, b1, b0 drive R, G, B primary. Palette
        // slot i maps to kCgaHw[i] by construction, so ATC byte is the
        // IRGB index repacked: b4=I, b2=R, b1=G, b0=B. Verified on
        // 86Box IBM EGA + 5154 ECD via the ATCPROBE viewer.
        out.reserve(16);
        for (std::size_t i = 0; i < 16; ++i) {
            auto v = static_cast<std::uint8_t>(
                  (i & 0x07)          // R G B (low 3 bits)
                | ((i & 0x08) << 1)); // I → b4
            out.push_back(v);
        }
    } else if (amiga::is_ega(mode) || amiga::is_ega_text(mode)) {
        // Mode 10h / ega-hi and EGA text modes (350-line): card drives
        // all 6 ATC output pins, 5154 reads all of them — full 64-entry
        // IrgbIRGB gamut available. The viewer also populates the VGA
        // DAC with the canonical IrgbIRGB→RGB mapping so the same byte
        // values work when run on VGA / DOSBox.
        out.reserve(16);
        for (std::size_t i = 0; i < palette.size() && i < 16; ++i) {
            auto rrggbb = palette::linear_to_ega(palette[i]);
            out.push_back(palette::ega_to_hw(rrggbb));
        }
        while (out.size() < 16) out.push_back(0);
    } else if (amiga::is_vga(mode)) {
        // VGA (chunky + planar): 3-byte-per-color VGA DAC triples (6 bits
        // each). Viewer loads directly into DAC[0..N-1], ATC identity.
        std::size_t n = amiga::is_chunky(mode) ? 256 : 16;
        out.reserve(n * 3);
        for (std::size_t i = 0; i < n; ++i) {
            Color3f c = (i < palette.size()) ? palette[i] : Color3f{0, 0, 0};
            auto v = palette::linear_to_vga(c);
            out.push_back(static_cast<std::uint8_t>((v >> 16) & 0x3F));
            out.push_back(static_cast<std::uint8_t>((v >> 8)  & 0x3F));
            out.push_back(static_cast<std::uint8_t>( v        & 0x3F));
        }
    }
    return out;
}

// DJGPP viewer main() body — mode-specific. Uses only DJGPP runtime
// calls (<dpmi.h>, <pc.h>, <conio.h>, <sys/movedata.h>,
// <go32.h>). Blits the image, waits for a keypress, restores text mode.
std::string emit_main(amiga::Mode mode, std::size_t /*w*/, std::size_t /*h*/,
                       const std::string& sym,
                       std::size_t data_bytes,
                       std::size_t pal_bytes,
                       const Options& options) {
    using namespace amiga;
    std::string main_body;
    const int set_ax = bios_mode_ax(mode);

    // Common helper: set BIOS video mode.
    std::string helpers = std::format(
        "static void set_video_mode(int ax) {{\n"
        "    __dpmi_regs r;\n"
        "    memset(&r, 0, sizeof(r));\n"
        "    r.x.ax = ax;\n"
        "    __dpmi_int(0x10, &r);\n"
        "}}\n\n");

    std::string main_fn;

    if (mode == Mode::vga_13h) {
        // Chunky 320x200 @ 0xA0000.
        main_fn = std::format(
            "int main() {{\n"
            "    set_video_mode(0x{:04X});\n"
            "    // Load DAC palette (256 x 3 bytes, 6-bit).\n"
            "    outportb(0x3C8, 0);\n"
            "    for (int i = 0; i < {}; ++i) outportb(0x3C9, {}_palette[i]);\n"
            "    // Blit {} bytes to 0xA0000.\n"
            "    dosmemput({}_data, {}, 0xA0000);\n"
            "    while (!kbhit()) {{}}\n"
            "    getch();\n"
            "    set_video_mode(0x0003);\n"
            "    return 0;\n"
            "}}\n",
            set_ax, pal_bytes, sym, data_bytes, sym, data_bytes);
    }
    else if (is_vga(mode) && !is_chunky(mode)) {
        // VGA planar 10h/12h: 4 planes, plane-sequential. Use sequencer
        // map mask 0x3C4/5 index 0x02 to select plane for write.
        std::size_t plane_bytes = data_bytes / 4;
        main_fn = std::format(
            "int main() {{\n"
            "    set_video_mode(0x{:04X});\n"
            "    // Reset ATC to pass-through: palette register i → i.\n"
            "    for (int i = 0; i < 16; ++i) {{\n"
            "        inportb(0x3DA);           // reset ATC flip-flop\n"
            "        outportb(0x3C0, i);       // select palette reg i\n"
            "        outportb(0x3C0, i);       // set to pass-through (will be overridden below)\n"
            "    }}\n"
            "    // Load DAC (VGA 18-bit: palette[] holds 16 * 3 bytes).\n"
            "    outportb(0x3C8, 0);\n"
            "    for (int i = 0; i < {}; ++i) outportb(0x3C9, {}_palette[i]);\n"
            "    // Blit each of the 4 planes (plane-sequential in _data).\n"
            "    const unsigned plane_bytes = {};\n"
            "    for (int p = 0; p < 4; ++p) {{\n"
            "        outportw(0x3C4, 0x0002 | ((1u << p) << 8));  // map mask = 1<<p\n"
            "        dosmemput({}_data + p * plane_bytes, plane_bytes, 0xA0000);\n"
            "    }}\n"
            "    // Re-enable video output (ATC PAS bit).\n"
            "    inportb(0x3DA); outportb(0x3C0, 0x20);\n"
            "    while (!kbhit()) {{}}\n"
            "    getch();\n"
            "    set_video_mode(0x0003);\n"
            "    return 0;\n"
            "}}\n",
            set_ax, pal_bytes, sym, plane_bytes, sym);
    }
    else if (is_ega(mode)) {
        // EGA planar modes 0Dh / 0Eh / 10h. Universal setup for real
        // IBM EGA, 86Box IBM-EGA emulation, VGA-emulating-EGA, DOSBox:
        //
        //   Mode 10h (ega-hi, 21.85 kHz hsync): IBM EGA card drives all
        //     6 ATC output pins, 5154 reads all of them → full 16-of-64
        //     IrgbIRGB palette. Populate DAC[0..63] with IrgbIRGB→RGB
        //     mapping (safety on VGA; no-op on real EGA), load 16
        //     image-picked IrgbIRGB values via BIOS AH=10h AL=02h.
        //
        //   Modes 0Dh / 0Eh (ega-320 / ega-640, 15.75 kHz hsync): IBM
        //     EGA card physically gates off output pins 2 (r') and 7
        //     (b') in this signal mode — bits 5 and 3 of the ATC
        //     palette registers never reach the monitor. Only bits 0,
        //     1, 2, 4 drive pins (primary R, G, B, and g'-as-intensity).
        //     That gives 16 effective colours, exactly the kCgaHw
        //     palette. Our encoder already restricts its picks to
        //     kCgaHw, and we load ATC regs i=0..15 with the CGA-compat
        //     IrgbIRGB values (0x00..0x07, 0x10..0x17). To make VGA
        //     hardware produce the SAME visible colors (where the DAC
        //     decodes ATC values verbatim and has no pin-gating), we
        //     additionally load DAC slots 0x00..0x07 and 0x10..0x17
        //     with the 16 kCgaHw RGB values — so ATC→DAC→RGB yields
        //     the same 16 IRGB colours on VGA as pin-gated 5154 shows.
        bool cga_compat = (mode == amiga::Mode::ega_320 ||
                           mode == amiga::Mode::ega_640);
        std::size_t plane_bytes = data_bytes / 4;
        std::string dac_setup;
        if (cga_compat) {
            // Hardcoded 16-entry RGB table in VGA 6-bit DAC units (0..63).
            dac_setup =
                "    // On VGA, make DAC slots {0..7, 0x10..0x17} match the\n"
                "    // 16 IRGB colours the 5154 shows in 200-line mode. On\n"
                "    // real EGA the 0x3C9 writes are ignored (no DAC chip).\n"
                "    static const unsigned char cga_dac[16 * 3] = {\n"
                "         0,  0,  0,   0,  0, 42,   0, 42,  0,   0, 42, 42,\n"
                "        42,  0,  0,  42,  0, 42,  42, 21,  0,  42, 42, 42,\n"
                "        21, 21, 21,  21, 21, 63,  21, 63, 21,  21, 63, 63,\n"
                "        63, 21, 21,  63, 21, 63,  63, 63, 21,  63, 63, 63\n"
                "    };\n"
                "    for (int i = 0; i < 16; ++i) {\n"
                "        unsigned slot = (i < 8) ? i : (0x10 + (i - 8));\n"
                "        outportb(0x3C8, slot);\n"
                "        outportb(0x3C9, cga_dac[i*3 + 0]);\n"
                "        outportb(0x3C9, cga_dac[i*3 + 1]);\n"
                "        outportb(0x3C9, cga_dac[i*3 + 2]);\n"
                "    }\n";
        } else {
            dac_setup =
                "    // Populate DAC[0..63] with IrgbIRGB→RGB mapping\n"
                "    // (no-op on real EGA; needed on VGA so the ATC output's\n"
                "    // 6-bit value decodes to the right colour).\n"
                "    outportb(0x3C8, 0);\n"
                "    for (int v = 0; v < 64; ++v) {\n"
                "        int r = ((v >> 2) & 1) * 42 + ((v >> 5) & 1) * 21;\n"
                "        int g = ((v >> 1) & 1) * 42 + ((v >> 4) & 1) * 21;\n"
                "        int b = ((v >> 0) & 1) * 42 + ((v >> 3) & 1) * 21;\n"
                "        outportb(0x3C9, r);\n"
                "        outportb(0x3C9, g);\n"
                "        outportb(0x3C9, b);\n"
                "    }\n";
        }
        main_fn = std::format(
            "int main() {{\n"
            "    set_video_mode(0x{:04X});\n"
            "{}"
            "    // Load ATC palette regs 0..15 directly. BIOS int 10h\n"
            "    // AH=10h AL=02h was the earlier canonical path but the\n"
            "    // IBM EGA BIOS in mode 0Dh/0Eh turned out to permute the\n"
            "    // load order (picked colors landed at wrong indices,\n"
            "    // \"false-color\" effect). Direct port writes bypass that.\n"
            "    //   1. Read 0x3DA to reset the ATC flip-flop to INDEX mode.\n"
            "    //   2. Write index i with bit 5 = 0 (palette-access mode,\n"
            "    //      video output blanked).\n"
            "    //   3. Write palette value.\n"
            "    //   4. Flip-flop toggles back to INDEX; iterate.\n"
            "    //   5. Re-enable video: index = 0x20 (bit 5 set).\n"
            "    for (int i = 0; i < 16; ++i) {{\n"
            "        (void)inportb(0x3DA);\n"
            "        outportb(0x3C0, i);\n"
            "        outportb(0x3C0, {}_palette[i]);\n"
            "    }}\n"
            "    (void)inportb(0x3DA);\n"
            "    outportb(0x3C0, 0x20);  // re-enable video\n"
            "    // Blit each of the 4 planes via sequencer map mask.\n"
            "    const unsigned plane_bytes = {};\n"
            "    for (int p = 0; p < 4; ++p) {{\n"
            "        outportw(0x3C4, 0x0002 | ((1u << p) << 8));\n"
            "        dosmemput({}_data + p * plane_bytes, plane_bytes, 0xA0000);\n"
            "    }}\n"
            "    while (!kbhit()) {{}}\n"
            "    getch();\n"
            "    set_video_mode(0x0003);\n"
            "    return 0;\n"
            "}}\n",
            set_ax, dac_setup, sym, plane_bytes, sym);
    }
    else if (is_cga(mode) && !is_text_graphics(mode)) {
        // CGA 320/640/composite: banked 16 KB CGAPIC @ 0xB8000.
        // cga_320 selects palette+intensity via 0x3D9; cga_composite
        // toggles color-burst off via 0x3D8 bit 2.
        std::string mode_setup;
        if (mode == Mode::cga_320) {
            mode_setup = "    outportb(0x3D9, 0x30);  // palette 1 high (cyan/magenta/white)\n";
        } else if (mode == Mode::cga_composite) {
            // Mode 04h: BIOS already clears bit 2 (color burst = ON) so the
            // composite monitor sees the chroma signal and produces artifact
            // colors. This write is belt-and-braces in case we came from a
            // mode with bit 2 set.
            mode_setup = "    outportb(0x3D8, 0x0A);  // mode 04h with color burst ENABLED\n";
        }
        main_fn = std::format(
            "int main() {{\n"
            "    set_video_mode(0x{:04X});\n"
            "{}"
            "    // Blit banked frame to 0xB8000 ({} bytes).\n"
            "    dosmemput({}_data, {}, 0xB8000);\n"
            "    while (!kbhit()) {{}}\n"
            "    getch();\n"
            "    set_video_mode(0x0003);\n"
            "    return 0;\n"
            "}}\n",
            set_ax, mode_setup, data_bytes, sym, data_bytes);
    }
    else if (is_text_graphics(mode)) {
        // Text-mode graphics hack. Steps (order matters):
        //   1. Select target scan line count via int 10h AX=1200h BL=30/31
        //      (BIOS applies at next mode set). 30 = 200 for CGA-text,
        //      31 = 350 for EGA-text.
        //   2. Set mode 03 (80x25 color text). BIOS picks the default font
        //      for the selected scan line count.
        //   3. Load our custom shifted font (256 × 32 B) via int 10h
        //      AX=1110h, BH=32 bytes-per-char. This realizes the
        //      encoder's scanline_offset: the font bytes already hold the
        //      right slice of each glyph.
        //   4. Program CRTC max-scan-line (reg 0x09) = cell_height - 1 so
        //      each displayed row is 1 or 2 scanlines tall. The controller
        //      divides total scanlines (200 or 350) by cell_height to get
        //      the row count (100, 175, 200, or 350 — matching our data).
        //   5. Disable blink (CGA: mode reg bit 5; EGA/VGA: ATC reg 0x10
        //      bit 3) so the attr byte's bit 7 becomes a bg-intensity bit
        //      and all 16 bg colors are usable.
        //   6. For EGA-text: populate VGA DAC[0..63] with standard
        //      IrgbIRGB→RGB and push the encoder's 16 IrgbIRGB values to
        //      ATC palette regs 0..15. Same dance as EGA graphics modes.
        //   7. Blit char+attr bytes to 0xB8000.
        //
        // Caller must populate options.font_data (256*32), cell_height, and
        // scan_lines_al — an empty font_data falls back to the plain BIOS
        // behavior (no CRTC change) which will NOT match the encoded image.
        bool have_font = !options.font_data.empty() &&
                         options.cell_height > 0;
        std::string font_load;
        if (have_font) {
            // Two text-mode setup paths:
            //
            //   scan_lines_al == 0 (200-line, kCgaHw 16-color): stay at
            //     BIOS mode 03h default timing. Just load our custom
            //     font and set CRTC max-scan-line for row density. This
            //     is the CGA-style path and runs on everything (CGA,
            //     EGA, VGA) without scan-rate gymnastics.
            //
            //   scan_lines_al == 1 (350-line, full 64-color): try to get
            //     21.85 kHz text scan on both EGA and VGA. AX=1111h
            //     does an 8x14 ROM font load + CRTC recalc which hits
            //     350-line on VGA and proper-DIP'd IBM EGA. We also
            //     force Misc Output to the 350-line polarity
            //     (-VSYNC +HSYNC) for 5154 ECD monitors. Known broken
            //     on 86Box's IBM EGA emulation — that card seems to
            //     stay at 200-line regardless; works on VGA/DOSBox.
            bool scan_350 = (options.scan_lines_al == 1);
            std::string scan_setup = scan_350
                ? "    memset(&r, 0, sizeof(r)); r.x.ax = 0x1111;\n"
                  "    __dpmi_int(0x10, &r);  // 8x14 ROM font + CRTC recalc → 350-line\n"
                  "    // 350-line sync polarity: -VSYNC +HSYNC + 16 MHz EGA clock.\n"
                  "    outportb(0x3C2, 0xA7);\n"
                : "    // 200-line (BIOS mode 03 default) — no extra setup needed.\n";
            font_load = std::format(
                "    // Target: {}-scanline text mode.\n"
                "    __dpmi_regs r;\n"
                "    memset(&r, 0, sizeof(r)); r.x.ax = 0x0003;\n"
                "    __dpmi_int(0x10, &r);  // BIOS mode 03\n"
                "{}"
                "    // Install custom font (256 glyphs × 32 bytes each,\n"
                "    // scanline_offset already baked into the array).\n"
                "    dosmemput({}_font, sizeof({}_font), __tb);\n"
                "    memset(&r, 0, sizeof(r));\n"
                "    r.x.ax = 0x1110;   // Load user font, no CRTC recalc\n"
                "    r.h.bh = 32;       // bytes per char\n"
                "    r.h.bl = 0;        // font block 0 (primary)\n"
                "    r.x.cx = 256;      // all 256 chars\n"
                "    r.x.dx = 0;        // starting at char 0\n"
                "    r.x.es = __tb >> 4;\n"
                "    r.x.bp = __tb & 0xF;\n"
                "    __dpmi_int(0x10, &r);\n"
                "    // CRTC max-scan-line = cell_height - 1 (preserves bits 5..7).\n"
                "    outportb(0x3D4, 0x09);\n"
                "    outportb(0x3D5, (inportb(0x3D5) & 0xE0) | {});\n",
                scan_350 ? 350 : 200,
                scan_setup,
                sym, sym,
                options.cell_height - 1);
        } else {
            font_load = std::format(
                "    set_video_mode(0x{:04X});\n", set_ax);
        }

        std::string pal_load;
        if (is_ega_text(mode)) {
            pal_load = std::format(
                "    // Populate DAC[0..63] with IrgbIRGB→RGB mapping.\n"
                "    outportb(0x3C8, 0);\n"
                "    for (int v = 0; v < 64; ++v) {{\n"
                "        int r = ((v >> 2) & 1) * 42 + ((v >> 5) & 1) * 21;\n"
                "        int g = ((v >> 1) & 1) * 42 + ((v >> 4) & 1) * 21;\n"
                "        int b = ((v >> 0) & 1) * 42 + ((v >> 3) & 1) * 21;\n"
                "        outportb(0x3C9, r);\n"
                "        outportb(0x3C9, g);\n"
                "        outportb(0x3C9, b);\n"
                "    }}\n"
                "    // Load ATC palette regs 0..15 with our 16 IrgbIRGB.\n"
                "    for (int i = 0; i < 16; ++i) {{\n"
                "        inportb(0x3DA);\n"
                "        outportb(0x3C0, i);\n"
                "        outportb(0x3C0, {}_palette[i]);\n"
                "    }}\n"
                "    inportb(0x3DA); outportb(0x3C0, 0x20);\n",
                sym);
        }
        // Disable blink so attr bit 7 becomes bg intensity. Two mechanisms
        // exist and the right one depends on the actual card:
        //   EGA/VGA: ATC mode-control register 0x10, clear bit 3.
        //   CGA:    mode register 0x3D8, clear bit 5.
        // We detect EGA/VGA at runtime via int 10h AH=12h BL=10h (function
        // added by the EGA BIOS; on CGA the call returns BL unchanged).
        // A 386 (DJGPP's minimum) with a CGA-only card is rare but real
        // (e.g. early 386 upgrades in AT clones) — this makes the viewer
        // portable across the full DOS-era range.
        std::string disable_blink =
            "    // Detect EGA/VGA once so we pick the right blink-disable.\n"
            "    __dpmi_regs br; memset(&br, 0, sizeof(br));\n"
            "    br.h.ah = 0x12; br.h.bl = 0x10;  // EGA info\n"
            "    __dpmi_int(0x10, &br);\n"
            "    bool has_ega = (br.h.bl != 0x10);\n"
            "    if (has_ega) {\n"
            "        // EGA/VGA blink-off: ATC reg 0x10, clear bit 3.\n"
            "        inportb(0x3DA);\n"
            "        outportb(0x3C0, 0x10 | 0x20);\n"
            "        unsigned char v = inportb(0x3C1);\n"
            "        outportb(0x3C0, v & ~0x08);\n"
            "        inportb(0x3DA); outportb(0x3C0, 0x20);  // re-enable video\n"
            "    } else {\n"
            "        // CGA blink-off: mode reg 0x3D8, clear bit 5.\n"
            "        // Mode 03 default is 0x29 (80x25 text + video + blink);\n"
            "        // 0x09 keeps mode/video on, turns blink off.\n"
            "        outportb(0x3D8, 0x09);\n"
            "    }\n";
        // Blit strategy. Default text mode has CPU odd/even mapping — the
        // 32 KB window at 0xB8000 holds 16 KB of char codes (plane 0) and
        // 16 KB of attributes (plane 1) interleaved at even/odd bytes.
        // That's enough for modes up to 80×200 (32 000 interleaved bytes,
        // 16 000 cells). EGA-text 80×350 needs 28 000 cells × 2 = 56 000
        // bytes which doesn't fit — without the split you'd see the image
        // repeated/cut as the writes wrap or overflow.
        //
        // Solution for large cell counts: disable odd/even on CPU side,
        // deinterleave char+attr pairs into two separate arrays, and
        // write each plane via the sequencer's map-mask register. Each
        // plane then uses the full 32 KB CPU window, giving us 32 K cells
        // × 2 bytes = 64 KB effective — plenty for 80×350.
        std::size_t total_cells = data_bytes / 2;
        std::string blit;
        if (total_cells <= 16384) {
            blit = std::format(
                "    // Blit char+attr bytes to text-mode video RAM.\n"
                "    dosmemput({}_data, {}, 0xB8000);\n",
                sym, data_bytes);
        } else {
            blit = std::format(
                "    // {} cells > 16384 — standard CPU odd/even + CRTC word mode\n"
                "    // only addresses 16K plane bytes. Switch both to linear/byte\n"
                "    // mode so each plane becomes a flat 32 KB array matching our\n"
                "    // 28 000-byte writes and the CRTC's char-fetch offsets.\n"
                "    // Sequencer reg 0x04 bit 2 = 0 (CPU odd/even off).\n"
                "    outportb(0x3C4, 0x04);\n"
                "    outportb(0x3C5, inportb(0x3C5) & ~0x04);\n"
                "    // Graphics controller reg 0x05 bit 4 = 0 (odd/even read off).\n"
                "    outportb(0x3CE, 0x05);\n"
                "    outportb(0x3CF, inportb(0x3CF) & ~0x10);\n"
                "    // Graphics controller reg 0x06 bit 1 = 0 (linear CPU address).\n"
                "    outportb(0x3CE, 0x06);\n"
                "    outportb(0x3CF, inportb(0x3CF) & ~0x02);\n"
                "    // CRTC reg 0x17 bit 6 = 1 (byte mode), bit 0 = 0 (disable\n"
                "    // CGA memory-wrap compatibility). Default mode 03 has both\n"
                "    // inverted: word mode + CMS on, which caps plane addressing\n"
                "    // at ~16 KB and makes the CRTC read every second plane byte.\n"
                "    outportb(0x3D4, 0x17);\n"
                "    outportb(0x3D5, (inportb(0x3D5) | 0x40) & ~0x01);\n"
                "    // CRTC reg 0x13 (Offset) = 80 — in byte mode the row stride\n"
                "    // is the register value (bytes), not word-scaled. Default 40\n"
                "    // worked in word mode (40 words × 2 = 80 byte stride); now\n"
                "    // byte mode needs the raw 80.\n"
                "    outportw(0x3D4, 0x5013);  // idx 0x13, value 0x50 (=80)\n"
                "    // CRTC start address = 0 so display begins at plane offset 0\n"
                "    // regardless of whatever the previous program left in it.\n"
                "    outportw(0x3D4, 0x000C);\n"
                "    outportw(0x3D4, 0x000D);\n"
                "    // Deinterleave char+attr pairs.\n"
                "    static std::uint8_t chars[{}];\n"
                "    static std::uint8_t attrs[{}];\n"
                "    for (std::size_t i = 0; i < {}; ++i) {{\n"
                "        chars[i] = {}_data[i * 2];\n"
                "        attrs[i] = {}_data[i * 2 + 1];\n"
                "    }}\n"
                "    // Map mask = plane 0, write char codes.\n"
                "    outportw(0x3C4, 0x0102);\n"
                "    dosmemput(chars, {}, 0xB8000);\n"
                "    // Map mask = plane 1, write attribute bytes.\n"
                "    outportw(0x3C4, 0x0202);\n"
                "    dosmemput(attrs, {}, 0xB8000);\n",
                total_cells,
                total_cells, total_cells, total_cells, sym, sym,
                total_cells, total_cells);
        }
        main_fn = std::format(
            "int main() {{\n"
            "{}"   // font_load (either our full setup or just set_video_mode)
            "{}"   // disable_blink
            "{}"   // pal_load (empty for CGA)
            "{}"   // blit (odd/even or split-plane)
            "    while (!kbhit()) {{}}\n"
            "    getch();\n"
            "    set_video_mode(0x0003);\n"
            "    return 0;\n"
            "}}\n",
            font_load, disable_blink, pal_load, blit);
    }
    else if (mode == Mode::vga_modex || mode == Mode::vga_modey) {
        // Mode X (320x240) / Mode Y (320x200) — unchained 256-color VGA
        // with 4 column-interleaved planes (hardware has 256 KB of video
        // RAM but chunky mode 13h only addresses 64 KB of one plane).
        // Setup:
        //   1. Start in mode 13h (gets us 320 pixel horizontal timing,
        //      paged DAC, and blanks the chained-memory config).
        //   2. Load DAC[0..255] from our 256×3-byte palette.
        //   3. Unchain planes: sequencer mem-mode reg (0x3C4 idx 04) = 0x06
        //      (chain-4 OFF, odd/even OFF, extended-memory ON).
        //   4. Put CRTC in byte addressing (underline reg 0x14 = 0,
        //      mode-ctrl reg 0x17 = 0xE3) so successive video-mem bytes
        //      map to successive display columns within a plane.
        //   5. Mode X only — reprogram the vertical timing chain to 240
        //      lines (values from Abrash's Black Book). Mode Y keeps the
        //      200-line mode-13h CRTC timing so no further CRTC changes.
        //   6. Blit each of the 4 planes via sequencer map mask 0x02.
        //
        // Under each plane i, pixel (x,y) with x%4==i lives at byte
        // offset y * (W/4) + (x/4). Our encoder packs the planes in that
        // order in planes[0..], planes[1..], etc. back-to-back.
        std::size_t plane_bytes = data_bytes / 4;
        const char* crtc_reprogram = "";
        if (mode == Mode::vga_modex) {
            crtc_reprogram =
                "    // Unlock CRTC registers 0..7 for vertical timing edit.\n"
                "    outportb(0x3D4, 0x11);\n"
                "    outportb(0x3D5, inportb(0x3D5) & 0x7F);\n"
                "    // 240-line Mode X CRTC timing (Abrash, Black Book).\n"
                "    outportw(0x3D4, 0x0D06);  // vertical total\n"
                "    outportw(0x3D4, 0x3E07);  // overflow\n"
                "    outportw(0x3D4, 0x4109);  // max scan line + bit9 of disp end\n"
                "    outportw(0x3D4, 0xEA10);  // v retrace start\n"
                "    outportw(0x3D4, 0xAC11);  // v retrace end + write-protect off\n"
                "    outportw(0x3D4, 0xDF12);  // v display end (240 visible lines)\n"
                "    outportw(0x3D4, 0xE715);  // v blank start\n"
                "    outportw(0x3D4, 0x0616);  // v blank end\n";
        }
        main_fn = std::format(
            "int main() {{\n"
            "    set_video_mode(0x0013);\n"
            "    // Load VGA DAC (256 colors, 3 x 6-bit bytes each).\n"
            "    outportb(0x3C8, 0);\n"
            "    for (int i = 0; i < {}; ++i) outportb(0x3C9, {}_palette[i]);\n"
            "    // Unchain planes: sequencer memory mode = 0x06.\n"
            "    outportw(0x3C4, 0x0604);\n"
            "    // Byte CRTC addressing.\n"
            "    outportw(0x3D4, 0x0014);\n"
            "    outportw(0x3D4, 0xE317);\n"
            "{}"  // crtc_reprogram (Mode X only; empty for Mode Y)
            "    // Blit 4 column-interleaved planes via sequencer map mask.\n"
            "    const unsigned plane_bytes = {};\n"
            "    for (int p = 0; p < 4; ++p) {{\n"
            "        outportw(0x3C4, 0x0002 | ((1u << p) << 8));\n"
            "        dosmemput({}_data + p * plane_bytes, plane_bytes, 0xA0000);\n"
            "    }}\n"
            "    while (!kbhit()) {{}}\n"
            "    getch();\n"
            "    set_video_mode(0x0003);\n"
            "    return 0;\n"
            "}}\n",
            pal_bytes, sym, crtc_reprogram, plane_bytes, sym);
    }
    else {
        main_fn = "int main() { return 0; }  // unsupported mode\n";
    }

    return helpers + main_fn;
}

} // namespace

Result<std::string> generate(amiga::Mode mode,
                             std::size_t width,
                             std::size_t height,
                             const bitplane::BitplaneData& planes,
                             std::span<const std::uint8_t> raw_frame,
                             std::span<const Color3f> palette,
                             const Options& options) {
    std::span<const std::uint8_t> data;
    if (!planes.data.empty()) {
        data = std::span{planes.data.data(), planes.data.size()};
    } else {
        data = raw_frame;
    }

    auto params = amiga::get_mode_params(mode);
    const auto& sym = options.symbol_name;
    auto pal_bytes = encode_palette(mode, palette);

    std::string SYM = sym;
    for (auto& c : SYM) c = static_cast<char>(std::toupper(c));

    std::string out;
    out.reserve(data.size() * 6 + pal_bytes.size() * 6 + 2048);

    out += "// ---------------------------------------------------------------\n";
    out += std::format("// Auto-generated by png2amiga — mode: {}\n",
                        mode_string(mode));
    out += std::format("// Buffer: {}x{}, depth: {} bitplane(s)\n",
                        width, height, params.bitplane_depth);
    out += "//\n";
    out += "// Compile with DJGPP (build-djgpp toolchain):\n";
    out += std::format("//   i586-pc-msdosdjgpp-g++ -O2 -o {}.exe {}.cpp\n",
                        sym, sym);
    out += "//\n";
    out += "// If you get `Load error: no DPMI - Get csdpmi*b.zip` when running\n";
    out += "// this on bare DOS (FreeDOS 1.3 boot, MS-DOS 6.22, etc.), bundle\n";
    out += "// CWSDPMI into the stub (from csdpmi7b.zip):\n";
    out += std::format("//   exe2coff {}.exe\n", sym);
    out += std::format("//   cat CWSDSTUB.EXE {} > {}.exe\n", sym, sym);
    out += "// Or run on Windows 95/98/ME, OS/2, or DOSBox-X (all provide DPMI).\n";
    out += "// Run on a real DOS machine or in DOSBox / PCem.\n";
    out += "// ---------------------------------------------------------------\n";
    out += "\n";
    out += "#include <cstdint>\n";
    out += "#include <cstring>\n";
    out += "#include <conio.h>      // kbhit, getch\n";
    out += "#include <pc.h>         // outportb, inportb, outportw\n";
    out += "#include <dpmi.h>       // __dpmi_int, __dpmi_regs\n";
    out += "#include <go32.h>\n";
    out += "#include <sys/movedata.h> // dosmemput\n";
    out += "\n";

    out += std::format("#define {}_MODE   \"{}\"\n", SYM, mode_string(mode));
    out += std::format("#define {}_WIDTH  {}\n", SYM, width);
    out += std::format("#define {}_HEIGHT {}\n", SYM, height);
    out += std::format("#define {}_DEPTH  {}\n", SYM, params.bitplane_depth);
    out += std::format("#define {}_DATA_BYTES {}\n", SYM, data.size());
    if (!pal_bytes.empty()) {
        out += std::format("#define {}_PALETTE_BYTES {}\n",
                            SYM, pal_bytes.size());
    }
    out += "\n";

    out += std::format("static const std::uint8_t {}_data[{}] = {{\n",
                        sym, data.size());
    out += format_bytes(data);
    out += "};\n\n";

    if (!pal_bytes.empty()) {
        out += std::format(
            "static const std::uint8_t {}_palette[{}] = {{\n",
            sym, pal_bytes.size());
        out += format_bytes(pal_bytes);
        out += "};\n\n";
    }

    // Text-mode font RAM image (256 glyphs × 32-byte VGA slots).
    // Bytes [0..cell_height-1] of each slot carry the image's bits; the
    // scanline_offset the encoder chose is already baked in by the caller.
    if (!options.font_data.empty()) {
        out += std::format(
            "static const std::uint8_t {}_font[{}] = {{\n",
            sym, options.font_data.size());
        out += format_bytes(options.font_data);
        out += "};\n\n";
    }

    out += emit_main(mode, width, height, sym, data.size(), pal_bytes.size(),
                     options);

    return out;
}

Result<void> save(std::string_view path,
                  amiga::Mode mode,
                  std::size_t width,
                  std::size_t height,
                  const bitplane::BitplaneData& planes,
                  std::span<const std::uint8_t> raw_frame,
                  std::span<const Color3f> palette,
                  const Options& options) {
    auto text = generate(mode, width, height, planes, raw_frame, palette,
                          options);
    if (!text) return std::unexpected{text.error()};
    std::ofstream file((std::string(path)));
    if (!file) {
        return std::unexpected{Error{ErrorCode::write_failed,
            std::format("Failed to open for write: {}", path)}};
    }
    file << *text;
    return {};
}

std::vector<std::uint8_t>
pack_cga_banked(std::span<const std::uint8_t> indices,
                std::size_t width, std::size_t height,
                amiga::Mode mode) {
    bool is_mono = (mode == amiga::Mode::cga_640);
    bool is_composite = amiga::is_composite(mode);
    auto buffer_width = is_composite ? std::size_t{320} : width;
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
                    byte = static_cast<std::uint8_t>(
                        (byte << 1) | (indices[y * width + x] != 0 ? 1 : 0));
                }
            } else if (is_composite) {
                auto p0 = palette::cga_composite_pattern(
                    indices[y * width + bx * 2]);
                auto p1 = palette::cga_composite_pattern(
                    indices[y * width + bx * 2 + 1]);
                byte = static_cast<std::uint8_t>((p0 << 4) | p1);
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

} // namespace png2amiga::cheader_dos
