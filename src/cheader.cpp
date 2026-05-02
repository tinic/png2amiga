#include "cheader.hpp"
#include "color_space.hpp"
#include "palette.hpp"
#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace png2amiga::cheader {

namespace {

// Convert a symbol name to uppercase for #define guards and macros
std::string to_upper(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (auto c : s) {
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return result;
}

// Sanitize a symbol name: replace non-alphanumeric with underscore
std::string sanitize_symbol(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (auto c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result.push_back(c);
        } else {
            result.push_back('_');
        }
    }
    // Ensure it doesn't start with a digit
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0]))) {
        result.insert(result.begin(), '_');
    }
    return result;
}

// CAMG viewport mode word
std::uint32_t make_camg(amiga::Mode mode, bool hires, bool interlace, bool dpf) {
    std::uint32_t camg = 0;
    auto params = amiga::get_mode_params(mode);
    if (hires)           camg |= 0x8000;
    if (params.is_ham)   camg |= 0x0800;
    if (params.is_ehb)   camg |= 0x0080;
    if (interlace)       camg |= 0x0004;
    if (dpf)             camg |= 0x0400;  // DBLPF (dual playfield)
    return camg;
}

// Emit the strips copper list as a UWORD array. Used by both .h (data-
// only export) and .cpp (viewer init code). Caller chooses whether
// the declaration is `const` (external linkage, .h) or `static const`
// (file-scope, .cpp viewer); pass the literal `linkage` prefix.
//
// Includes the past-0xFF wrap marker patched into line 255's end-of-line
// WAIT when the list extends past line 255 + kVStart=44 — see the
// detailed comment at the original emit site for the timing rationale.
void emit_strips_copper_list(std::string& out,
                           const std::string& sym,
                           const CHeaderOptions& options,
                           std::string_view linkage) {
    auto& moves = *options.strips_line_moves;
    constexpr int kVStart = 44;

    out += std::format("// Strips copper list — {} (anchor=0x{:02X}, "
                       "total_planes={})\n",
                       options.strips_label.empty() ? "unnamed"
                                                  : options.strips_label,
                       options.strips_anchor_hpos,
                       options.strips_total_planes);

    std::size_t strips_total_words = 0;
    for (auto& row : moves) strips_total_words += row.size() * 2;
    bool needs_wrap_marker =
        (kVStart + static_cast<int>(moves.size()) > 256) && !moves.empty();
    if (strips_total_words == 0) strips_total_words = 2;

    out += std::format("{} UWORD {}_strips_copper_list[{}] = {{\n",
                       linkage, sym, strips_total_words);
    std::size_t emitted = 0;
    for (std::size_t y = 0; y < moves.size(); ++y) {
        auto& row = moves[y];
        int abs_vpos = kVStart + static_cast<int>(y);
        if (row.empty()) continue;
        bool patch_wrap_at_eol = needs_wrap_marker && abs_vpos == 255;
        out += "    ";
        for (std::size_t i = 0; i < row.size(); ++i) {
            auto& op = row[i];
            std::uint16_t w0 = 0, w1 = 0;
            if (op.kind == strips::ScapOpKind::kWait) {
                w0 = static_cast<std::uint16_t>(
                    (static_cast<unsigned>(op.vpos) << 8) |
                    (op.hpos & 0xFE) | 0x0001);
                w1 = 0xFFFE;
                if (patch_wrap_at_eol && i == row.size() - 1) {
                    w0 = 0xFFDF;
                }
            } else {
                auto reg = static_cast<unsigned>(op.reg & 0x1F);
                w0 = static_cast<std::uint16_t>(0x0180 + reg * 2);
                w1 = op.rgb_ocs;
            }
            out += std::format("0x{:04X},0x{:04X}", w0, w1);
            emitted += 2;
            if (emitted < strips_total_words) out += ",";
        }
        out += std::format("  /* y={} (vp={}) */\n", y, abs_vpos & 0xFF);
    }
    if (emitted == 0) out += "    0x0000,0x0000\n";
    out += "};\n\n";
    out += std::format("{} ULONG {}_strips_copper_words = {};\n\n",
                       linkage, sym, emitted);
}

} // namespace

// ---------------------------------------------------------------------------
// Generate C header content
// ---------------------------------------------------------------------------

Result<std::string> generate(const bitplane::BitplaneData& planes,
                             std::span<const Color3f> palette,
                             amiga::Mode mode,
                             const CHeaderOptions& options) {
    auto sym = sanitize_symbol(options.symbol_name);
    auto SYM = to_upper(sym);

    std::string out;
    out.reserve(planes.total_bytes() * 8);  // rough estimate

    // Header guard
    out += std::format("#ifndef {}_H\n", SYM);
    out += std::format("#define {}_H\n\n", SYM);

    // Type include for UWORD
    out += "/* Generated by png2amiga — do not edit */\n\n";
    out += "#ifndef UWORD\n";
    out += "#define UWORD unsigned short\n";
    out += "#endif\n";
    out += "#ifndef ULONG\n";
    out += "#define ULONG unsigned long\n";
    out += "#endif\n\n";

    // Metadata defines
    auto camg = make_camg(mode, options.hires, options.interlace, options.dpf);
    out += std::format("#define {}_WIDTH   {}\n", SYM, planes.width);
    out += std::format("#define {}_HEIGHT  {}\n", SYM, planes.height);
    out += std::format("#define {}_DEPTH   {}\n", SYM, planes.depth);
    out += std::format("#define {}_BPR     {}    /* bytes per row per plane */\n",
                       SYM, planes.bytes_per_row);
    out += std::format("#define {}_CAMG    0x{:04X}\n\n", SYM, camg);

    auto words_per_row = planes.bytes_per_row / 2;

    if (options.interleaved) {
        // Single interleaved array: all planes for row 0, then row 1, etc.
        out += std::format("const UWORD {}_planes[] = {{\n", sym);

        auto words_per_interleaved_row = words_per_row * planes.depth;
        auto total_words = words_per_interleaved_row * planes.height;
        std::size_t word_count = 0;

        for (std::size_t y = 0; y < planes.height; ++y) {
            out += std::format("    /* row {} */\n", y);
            for (std::size_t p = 0; p < planes.depth; ++p) {
                out += "    ";
                auto offset = planes.plane_row_offset(p, y);
                for (std::size_t w = 0; w < words_per_row; ++w) {
                    auto byte_off = offset + w * 2;
                    auto hi = static_cast<std::uint16_t>(planes.data[byte_off]);
                    auto lo = static_cast<std::uint16_t>(planes.data[byte_off + 1]);
                    auto word = static_cast<std::uint16_t>((hi << 8) | lo);
                    ++word_count;
                    out += std::format("0x{:04X}", word);
                    if (word_count < total_words) {
                        out += ",";
                    }
                }
                out += "\n";
            }
        }

        out += "};\n\n";
    } else {
        // Separate arrays per plane
        for (std::size_t p = 0; p < planes.depth; ++p) {
            out += std::format("const UWORD {}_plane{}[] = {{\n", sym, p);

            auto total_words = words_per_row * planes.height;
            std::size_t word_count = 0;

            for (std::size_t y = 0; y < planes.height; ++y) {
                out += "    ";
                auto offset = planes.plane_row_offset(p, y);
                for (std::size_t w = 0; w < words_per_row; ++w) {
                    auto byte_off = offset + w * 2;
                    auto hi = static_cast<std::uint16_t>(planes.data[byte_off]);
                    auto lo = static_cast<std::uint16_t>(planes.data[byte_off + 1]);
                    auto word = static_cast<std::uint16_t>((hi << 8) | lo);
                    ++word_count;
                    out += std::format("0x{:04X}", word);
                    if (word_count < total_words) {
                        out += ",";
                    }
                }
                out += "\n";
            }

            out += "};\n\n";
        }
    }

    // Palette as OCS 12-bit RGB values (0x0RGB)
    // For EHB, only write 32 base colors (hardware generates the rest)
    auto pal_count = palette.size();
    if (mode == amiga::Mode::ehb && pal_count > 32) {
        pal_count = 32;
    }

    out += std::format("#define {}_COLORS  {}\n\n", SYM, pal_count);
    out += std::format("const UWORD {}_palette[] = {{\n", sym);

    for (std::size_t i = 0; i < pal_count; ++i) {
        auto rgb12 = palette::linear_to_ocs(palette[i]);
        out += std::format("    0x{:04X}", rgb12);
        if (i + 1 < pal_count) {
            out += ",";
        }
        // Add sRGB comment for readability
        auto srgb = color_space::linear_to_srgb(palette[i]).clamped();
        auto r8 = static_cast<int>(std::lround(srgb.r * 255.0f));
        auto g8 = static_cast<int>(std::lround(srgb.g * 255.0f));
        auto b8 = static_cast<int>(std::lround(srgb.b * 255.0f));
        out += std::format("  /* #{:02X}{:02X}{:02X} */", r8, g8, b8);
        out += "\n";
    }

    out += "};\n\n";

    // Copper list: per-scanline register changes
    // Format: {register_index, color_value} pairs per scanline
    if (options.copper_changes && !options.copper_changes->empty()) {
        auto& changes = *options.copper_changes;
        auto cpl = options.copper_changes_per_line;
        auto height = changes.size();

        out += std::format("#define {}_COPPER_CHANGES  {}  "
                           "/* max register changes per scanline */\n\n",
                           SYM, cpl);

        // Struct for copper change entry
        out += std::format("struct {}_copper_entry {{\n", sym);
        out += "    UWORD reg;     /* palette register index */\n";
        out += "    UWORD color;   /* OCS 12-bit 0x0RGB */\n";
        out += "};\n\n";

        // Emit as [height][changes_per_line] array
        // Unused slots filled with {0xFFFF, 0x0000} as sentinel
        out += std::format("const struct {0}_copper_entry {0}_copper"
                           "[{1}][{2}] = {{\n",
                           sym, height, cpl);

        for (std::size_t y = 0; y < height; ++y) {
            out += "    { ";
            auto& line = changes[y];
            for (std::size_t s = 0; s < cpl; ++s) {
                if (s < line.size()) {
                    auto rgb12 = palette::linear_to_ocs(line[s].color);
                    out += std::format("{{{},"
                                       "0x{:04X}}}",
                                       line[s].reg, rgb12);
                } else {
                    out += "{0xFFFF,0x0000}";  // sentinel: no change
                }
                if (s + 1 < cpl) out += ", ";
            }
            out += " }";
            if (y + 1 < height) out += ",";
            out += "\n";
        }

        out += "};\n\n";
    }

    // strips copper list (data-only). The .cpp viewer also emits this
    // array under the same name, with `static const` linkage; here we
    // use `const` (external linkage) so user code that includes the
    // .h gets a definition it can pass to its own Copper installer.
    if (options.strips_line_moves && !options.strips_line_moves->empty()) {
        emit_strips_copper_list(out, sym, options, "const");
    }

    out += std::format("#endif /* {}_H */\n", SYM);

    return out;
}

// ---------------------------------------------------------------------------
// Save C header to file
// ---------------------------------------------------------------------------

Result<void> save(std::string_view path,
                  const bitplane::BitplaneData& planes,
                  std::span<const Color3f> palette,
                  amiga::Mode mode,
                  const CHeaderOptions& options) {
    auto content = generate(planes, palette, mode, options);
    if (!content) return std::unexpected{content.error()};

    auto path_str = std::string(path);
    std::ofstream file(path_str);
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to open file for writing: " + path_str,
        }};
    }

    file << *content;
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to write C header to: " + path_str,
        }};
    }

    return {};
}

// ===========================================================================
// Standalone AmigaOS viewer .cpp generator
//
// Produces a single self-contained C++ file with inline image data,
// palette, copper list, and a main() that takes over the system,
// sets up the display, and waits for left mouse button.
//
// Compatible with vscode-amiga-debug toolchain (m68k-amiga-elf-gcc 15).
// ===========================================================================

Result<std::string> generate_viewer(const bitplane::BitplaneData& planes,
                                    std::span<const Color3f> palette,
                                    amiga::Mode mode,
                                    const CHeaderOptions& options) {
    auto sym = sanitize_symbol(options.symbol_name);
    auto params = amiga::get_mode_params(mode);
    auto width = planes.width;
    auto height = planes.height;
    auto depth = planes.depth;
    auto bpr = planes.bytes_per_row;
    auto is_ham = params.is_ham;

    bool is_hires = options.hires;
    auto camg = make_camg(mode, is_hires, options.interlace, options.dpf);

    // Fade-in is only supported for progressive non-HAM displays. Interlace
    // would need a separate fade loop per field; HAM's modify-R/G/B bits
    // carry absolute values so fading the base palette would corrupt pixels.
    bool do_fade = options.fade_in && !options.interlace && !is_ham;

    // Palette count (EHB: only 32 base colors)
    auto pal_count = palette.size();
    if (mode == amiga::Mode::ehb && pal_count > 32) pal_count = 32;

    std::string out;
    out.reserve(planes.total_bytes() * 8 + 8192);

    // Header
    out += std::format("// Generated by png2amiga {} — AmigaOS viewer\n",
                       png2amiga::version);
    out += "// https://www.png2amiga.app\n";
    out += "//\n";
    out += "// Compile: m68k-amiga-elf-gcc -m68000 -Ofast -nostdlib -o out.elf "
           "this.cpp support/gcc8_a_support.s support/gcc8_c_support.c\n";
    out += "//          elf2hunk out.elf out.exe\n";
    out += "//\n";
    {
        // Build a human-readable mode label that mirrors the runtime
        // exit message (e.g. "HAM6 + Sliced", "EHB + Strips",
        // "lores 6bpl + DPF + Strips").
        bool _has_cop  = options.copper_changes && !options.copper_changes->empty();
        bool _has_scap = options.strips_line_moves && !options.strips_line_moves->empty();
        std::string mode_label;
        if (params.is_ham) {
            mode_label = std::format("HAM{}", depth);
        } else if (params.is_ehb) {
            mode_label = "EHB";
        } else if (is_hires) {
            mode_label = std::format("hires {}bpl", depth);
        } else {
            mode_label = std::format("lores {}bpl", depth);
        }
        if (options.dpf)       mode_label += " + DPF";
        if (options.interlace) mode_label += " + lace";
        if (_has_scap)         mode_label += " + Strips";
        else if (_has_cop)     mode_label += " + Sliced";
        const char* chipset_str = options.aga ? "AGA" : "OCS";
        out += std::format("// Image:    {}x{}, {} ({}-bit palette)\n",
                           width, height, chipset_str,
                           options.aga ? 24 : 12);
        out += std::format("// Mode:     {}\n", mode_label);
        out += std::format("// Bitplane: {} bytes ({} planes, {} bytes/row)\n",
                           planes.total_bytes(), depth, bpr);
        std::size_t visible_pal_count = options.dpf
            ? (std::size_t{1} << (depth / 2))
            : pal_count;
        out += std::format("// Palette:  {} colors\n", visible_pal_count);
        if (options.total_unique_colors > 0) {
            out += std::format("// Colors:   {} unique\n",
                               options.total_unique_colors);
        }
        if (_has_cop)
            out += std::format("// Sliced:      {} swaps/line max\n",
                               options.copper_changes_per_line);
        if (_has_scap) {
            std::size_t strips_words = 0;
            for (auto& row : *options.strips_line_moves)
                strips_words += row.size() * 2;
            out += std::format("// Strips:     {} bytes copper list\n",
                               strips_words * 2);
        }
        out += std::format("// CAMG:     0x{:04X}\n", camg);
    }
    out += "// Click left mouse button to exit.\n\n";

    out += "#include \"support/gcc8_c_support.h\"\n";
    out += "#include <proto/exec.h>\n";
    out += "#include <proto/dos.h>\n";
    out += "#include <proto/graphics.h>\n";
    out += "#include <exec/execbase.h>\n";
    out += "#include <graphics/gfxbase.h>\n";
    out += "#include <graphics/view.h>\n";
    out += "#include <hardware/custom.h>\n";
    out += "#include <hardware/dmabits.h>\n\n";

    // System globals
    out += "// --- AmigaOS system pointers ---\n";
    out += "struct ExecBase *SysBase;\n";
    out += "volatile struct Custom *custom;  // $DFF000 custom chip registers\n";
    out += "struct GfxBase *GfxBase;\n\n";

    // System backup/restore
    out += "// --- Saved system state (restored on exit) ---\n";
    out += "static UWORD SystemInts, SystemDMA, SystemADKCON;\n";
    out += "static volatile APTR VBR = 0;  // Vector Base Register (68010+)\n";
    out += "static APTR SystemIrq;\n";
    out += "struct View *ActiView;\n\n";

    // Lace field-swap handler — file-scope (not a lambda) so the
    // __attribute__((interrupt)) attribute reliably emits RTE rather than
    // RTS, and so cop_odd/cop_even references resolve at the same scope
    // as the handler itself. The captureless-lambda pattern caused field 2
    // to receive zero copper instructions on m68k-amiga-elf-gcc 14.
    if (options.interlace) {
        out += "// Lace field-swap globals (set in main, read in VBL handler).\n";
        out += "static volatile ULONG cop_odd, cop_even;\n";
        out += "static __attribute__((interrupt)) void VblHandler() {\n";
        out += "    volatile struct Custom* c = (volatile struct Custom*)0xdff000;\n";
        out += "    c->intreq = (1<<5); c->intreq = (1<<5);  // ack VBL\n";
        out += "    if (*(volatile UWORD*)0xdff004 & 0x8000)\n";
        out += "        c->cop1lc = cop_even;\n";
        out += "    else\n";
        out += "        c->cop1lc = cop_odd;\n";
        out += "}\n\n";
    }

    out += "// Read VBR via Supervisor mode (movec VBR,d0)\n";
    out += "static __attribute__((interrupt)) void SupervisorGetVBR() {\n";
    out += "    __asm__ volatile(\".short 0x4e7a, 0x0801\");\n";
    out += "}\n\n";

    out += "static APTR GetVBR() {\n";
    out += "    APTR vbr = 0;\n";
    out += "    if (SysBase->AttnFlags & AFF_68010)\n";
    out += "        vbr = (APTR)Supervisor((ULONG (*)())SupervisorGetVBR);\n";
    out += "    return vbr;\n";
    out += "}\n\n";

    out += "// Wait for vertical blank (PAL line 311)\n";
    out += "void WaitVbl() {\n";
    out += "    while ((*(volatile ULONG*)0xDFF004 & 0x1ff00) != (311<<8)) {}\n";
    out += "    while ((*(volatile ULONG*)0xDFF004 & 0x1ff00) == (311<<8)) {}\n";
    out += "}\n\n";

    out += "// Read left mouse button from CIA-A port A\n";
    out += "__attribute__((always_inline)) inline short MouseLeft() {\n";
    out += "    return !((*(volatile UBYTE*)0xbfe001) & 64);\n";
    out += "}\n\n";

    out += "// Take over the system: disable OS, DMA, interrupts, save state\n";
    out += "void TakeSystem() {\n";
    out += "    Forbid();                    // prevent task switching\n";
    out += "    SystemADKCON = custom->adkconr; // save audio/disk control\n";
    out += "    SystemInts = custom->intenar;    // save interrupt enable bits\n";
    out += "    SystemDMA = custom->dmaconr;     // save DMA enable bits\n";
    out += "    ActiView = GfxBase->ActiView;    // save current Intuition view\n";
    out += "    LoadView(0); WaitTOF(); WaitTOF(); // flush Intuition display\n";
    out += "    WaitVbl(); WaitVbl();\n";
    out += "    OwnBlitter();                    // exclusive blitter access\n";
    out += "    while (*(volatile UWORD*)&custom->dmaconr & (1<<14)) {} // wait blitter done\n";
    out += "    Disable();                       // mask all CPU interrupts\n";
    out += "    custom->intena = 0x7fff;         // disable all interrupt sources\n";
    out += "    custom->intreq = 0x7fff;         // clear pending interrupts\n";
    out += "    custom->dmacon = 0x7fff;         // disable all DMA channels\n";
    out += "    for (int a = 0; a < 32; a++) custom->color[a] = 0; // black palette\n";
    out += "    WaitVbl(); WaitVbl();\n";
    out += "    VBR = GetVBR();                  // read vector base (68010+)\n";
    out += "    SystemIrq = *(volatile APTR*)(((UBYTE*)VBR) + 0x6c); // save level 3 IRQ\n";
    out += "}\n\n";

    out += "// Restore the system: re-enable OS, DMA, interrupts\n";
    out += "void FreeSystem() {\n";
    out += "    WaitVbl();\n";
    out += "    while (*(volatile UWORD*)&custom->dmaconr & (1<<14)) {}\n";
    out += "    custom->intena = 0x7fff;\n";
    out += "    custom->intreq = 0x7fff;\n";
    out += "    custom->dmacon = 0x7fff;\n";
    // Reset BPLCON state to neutral defaults before re-installing the\n
    // system copper. Stops any leftover viewer-mode bits (DBLPF, HAM,\n
    // HIRES, LACE, AGA PF2OF/BPLAM) from briefly bleeding into the\n
    // restored display in the window between dmacon=off and the system\n
    // copper's first cycle. BPLCON3/4 writes are ignored on plain OCS.\n
    out += "    custom->bplcon0 = 0x0200;  // COLOR enable, BPU=0, no DBLPF/HAM/HIRES/LACE\n";
    out += "    custom->bplcon1 = 0;\n";
    out += "    custom->bplcon2 = 0;\n";
    out += "    custom->bplcon3 = 0x0C00;  // BANK=0, LOCT=0, PF2OF=011 (AGA default)\n";
    out += "    custom->bplcon4 = 0x0011;  // BPLAM=0, sprite bases (AGA default)\n";
    out += "    *(volatile APTR*)(((UBYTE*)VBR) + 0x6c) = SystemIrq;\n";
    out += "    custom->cop1lc = (ULONG)GfxBase->copinit;\n";
    out += "    custom->cop2lc = (ULONG)GfxBase->LOFlist;\n";
    out += "    custom->copjmp1 = 0x7fff;\n";
    out += "    custom->intena = SystemInts | 0x8000;\n";
    out += "    custom->dmacon = SystemDMA | 0x8000;\n";
    out += "    custom->adkcon = SystemADKCON | 0x8000;\n";
    out += "    while (*(volatile UWORD*)&custom->dmaconr & (1<<14)) {}\n";
    out += "    DisownBlitter();\n";
    out += "    Enable();\n";
    out += "    LoadView(ActiView); WaitTOF(); WaitTOF();\n";
    out += "    Permit();\n";
    out += "}\n\n";

    // --- Image data ---
    // Always use line-interleaved layout (plane 0 row 0, plane 1 row 0, ...
    // plane N row 0, plane 0 row 1, ...). Better DRAM page locality than
    // planar — consecutive plane fetches stay on the same DRAM row, which
    // matters most under FMODE=3 with many bitplanes.
    //
    // FMODE=3 (64-bit DMA fetch) is needed when bandwidth is tight:
    //   - Hires >4 planes (always)
    //   - Lores >6 planes WITH copper
    // Computed up here so it gates both the data array's 8-byte alignment
    // and the BPLxMOD/FMODE register writes later in the copper list.
    bool has_copper = options.copper_changes && !options.copper_changes->empty();
    bool has_scap = options.strips_line_moves && !options.strips_line_moves->empty();
    // strips supplies its own per-line copper ops and replaces the sliced
    // emission path. They never compose in the same viewer.
    if (has_scap) has_copper = false;
    bool is_lace = options.interlace;
    bool need_fmode3 = ((depth > 6) && has_copper) ||
                       (is_hires && depth > 4);

    out += std::format("// {}x{}, {} bitplanes, CAMG 0x{:04X}\n",
                       width, height, depth, camg);
    // 8-byte alignment when FMODE=3 (64-bit DMA fetch); 2-byte otherwise.
    auto align = need_fmode3 ? 8 : 2;
    auto words_per_row = bpr / 2;

    // Multi-frame batch viewer flag: switches the data layout from
    // line-interleaved (single image, fast DMA) to plane-sequential
    // per-frame so we can omit zero planes entirely and let the runtime
    // supply a shared zero chip-RAM buffer for them. Plane-sequential
    // layout uses BPLxMOD=0 (or -8 for FMODE=3) since each plane is
    // contiguous bpr*height bytes; no inter-plane skip needed between
    // rows. The single-frame path keeps the original interleaved layout.
    auto n_extra = options.extra_frame_planes.size();
    bool is_batch = n_extra > 0;
    std::size_t n_frames_total = is_batch ? n_extra + 1 : 1;

    // Detect planes that are all-zero across every frame in the batch.
    // Those get omitted from the source (huge savings for DPF where one
    // playfield is unused) and the runtime points BPLxPT for those plane
    // indices at a single shared zero buffer.
    auto plane_all_zero = [](const bitplane::BitplaneData& bp, std::size_t p) {
        for (std::size_t y = 0; y < bp.height; ++y) {
            auto off = bp.plane_row_offset(p, y);
            for (std::size_t b = 0; b < bp.bytes_per_row; ++b)
                if (bp.data[off + b]) return false;
        }
        return true;
    };
    std::vector<bool> plane_zero(depth, false);
    if (is_batch) {
        for (std::size_t p = 0; p < depth; ++p) {
            bool z = plane_all_zero(planes, p);
            for (std::size_t fi = 0; fi < n_extra && z; ++fi)
                if (!plane_all_zero(options.extra_frame_planes[fi], p))
                    z = false;
            plane_zero[p] = z;
        }
    }
    bool any_zero_plane = std::ranges::any_of(plane_zero,
                                              [](bool z) { return z; });

    auto emit_plane_seq = [&](std::size_t fi, const bitplane::BitplaneData& bp,
                              std::size_t p) {
        // One plane-sequential UWORD array: just this plane's bpr*height
        // bytes serialised row-by-row. Word count = words_per_row*height.
        auto wpr = bp.bytes_per_row / 2;
        auto total = wpr * bp.height;
        std::size_t wc = 0;
        out += std::format("static const UWORD {}_f{}_p{}[]"
                           " __attribute__((aligned({})))"
                           " __attribute__((section(\".MEMF_CHIP\"))) = {{\n",
                           sym, fi, p, align);
        for (std::size_t y = 0; y < bp.height; ++y) {
            out += "    ";
            auto offset = bp.plane_row_offset(p, y);
            for (std::size_t w = 0; w < wpr; ++w) {
                auto byte_off = offset + w * 2;
                auto hi = static_cast<std::uint16_t>(bp.data[byte_off]);
                auto lo = static_cast<std::uint16_t>(bp.data[byte_off + 1]);
                auto word = static_cast<std::uint16_t>((hi << 8) | lo);
                ++wc;
                out += std::format("0x{:04X}", word);
                if (wc < total) out += ",";
            }
            out += "\n";
        }
        out += "};\n";
    };

    if (is_batch) {
        // Plane-sequential per-frame emission. Each frame fi gets one
        // array per non-zero plane: <sym>_fI_pJ. Zero planes are NOT
        // emitted; the runtime's shared zero buffer covers them.
        if (any_zero_plane) {
            out += "/* Plane mask (all-zero across every frame, NULL in "
                   "_frame_planes[][] = use shared runtime zero buffer): ";
            for (std::size_t p = 0; p < depth; ++p)
                out += plane_zero[p] ? '0' : '1';
            out += " */\n\n";
        }
        for (std::size_t fi = 0; fi < n_frames_total; ++fi) {
            const auto& bp = (fi == 0) ? planes
                                       : options.extra_frame_planes[fi - 1];
            for (std::size_t p = 0; p < depth; ++p) {
                if (plane_zero[p]) continue;
                emit_plane_seq(fi, bp, p);
            }
        }
        out += "\n";
        // Pointer table: per frame, per plane. NULL = use the shared
        // runtime zero buffer (alloc'd and patched in below).
        out += std::format("static const UBYTE* {}_frame_planes[{}][{}] = {{\n",
                           sym, n_frames_total, depth);
        for (std::size_t fi = 0; fi < n_frames_total; ++fi) {
            out += "    {";
            for (std::size_t p = 0; p < depth; ++p) {
                if (plane_zero[p]) out += " 0";
                else out += std::format(" (const UBYTE*){}_f{}_p{}",
                                        sym, fi, p);
                if (p + 1 < depth) out += ",";
            }
            out += " }";
            if (fi + 1 < n_frames_total) out += ",";
            out += "\n";
        }
        out += "};\n\n";
    } else {
        // Single-frame: original line-interleaved blob (best DRAM page
        // locality for fast DMA). One static array, all planes inlined.
        out += std::format("static const UWORD {}_planes[]"
                           " __attribute__((aligned({})))"
                           " __attribute__((section(\".MEMF_CHIP\"))) = {{\n",
                           sym, align);
        auto total_words = words_per_row * depth * height;
        std::size_t word_count = 0;
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t p = 0; p < depth; ++p) {
                out += "    ";
                auto offset = planes.plane_row_offset(p, y);
                for (std::size_t w = 0; w < words_per_row; ++w) {
                    auto byte_off = offset + w * 2;
                    auto hi = static_cast<std::uint16_t>(planes.data[byte_off]);
                    auto lo = static_cast<std::uint16_t>(planes.data[byte_off + 1]);
                    auto word = static_cast<std::uint16_t>((hi << 8) | lo);
                    ++word_count;
                    out += std::format("0x{:04X}", word);
                    if (word_count < total_words) out += ",";
                }
                out += "\n";
            }
        }
        out += "};\n\n";
    }

    // --- Palette ---
    out += std::format("static const UWORD {}_palette[] = {{\n", sym);
    for (std::size_t i = 0; i < pal_count; ++i) {
        if (options.aga) {
            out += std::format("    0x{:04X}", palette::linear_to_aga_hilo(palette[i]).hi);
        } else {
            out += std::format("    0x{:04X}", palette::linear_to_ocs(palette[i]));
        }
        if (i + 1 < pal_count) out += ",";
        out += "\n";
    }
    out += "};\n\n";
    if (options.aga) {
        out += std::format("static const UWORD {}_palette_lo[] = {{\n", sym);
        for (std::size_t i = 0; i < pal_count; ++i) {
            out += std::format("    0x{:04X}", palette::linear_to_aga_hilo(palette[i]).lo);
            if (i + 1 < pal_count) out += ",";
            out += "\n";
        }
        out += "};\n\n";
    }

    // --- Copper list (if present) ---
    if (has_copper) {
        auto cpl = options.copper_changes_per_line;

        // For interlace, we rebuild the per-row change lists: each field
        // visits every other row (field 1: 0,2,4,...; field 2: 1,3,5,...),
        // so row y's effective transition is from row y-2 — not the default
        // row y-1 sequential encoding. We pick the K registers whose color
        // magnitude differs most between scanline_palettes[y-2] and [y].
        std::vector<std::vector<copper::CopperChange>> lace_changes;
        const std::vector<std::vector<copper::CopperChange>>* changes_ptr =
            options.copper_changes;
        if (is_lace && options.copper_scanline_palettes
                && !options.copper_scanline_palettes->empty()) {
            auto& pals = *options.copper_scanline_palettes;
            auto H = pals.size();
            lace_changes.resize(H);
            // Base palette (written at frame start for both fields).
            std::vector<Color3f> base_pal(palette.begin(), palette.end());
            for (std::size_t y = 0; y < H; ++y) {
                // For each field's pre-display:
                //   Field 1 loads base palette then applies row 0's diffs.
                //   Field 2 loads base palette then applies row 1's diffs.
                // So rows 0 and 1 both diff against the base palette.
                // Rows >=2 diff against row y-2 (the previous row in the same
                // field — field 1: 0,2,4,...; field 2: 1,3,5,...).
                const auto& pal_prev = (y < 2) ? base_pal : pals[y - 2];
                auto& pal_cur = pals[y];
                auto n_regs = std::min(pal_prev.size(), pal_cur.size());
                // Candidates: every register whose color changed.
                struct Cand {
                    std::uint8_t reg;
                    float dist;
                    Color3f color;
                };
                std::vector<Cand> cands;
                cands.reserve(n_regs);
                for (std::size_t r = 0; r < n_regs; ++r) {
                    auto dr = pal_cur[r].r - pal_prev[r].r;
                    auto dg = pal_cur[r].g - pal_prev[r].g;
                    auto db = pal_cur[r].b - pal_prev[r].b;
                    auto d2 = dr*dr + dg*dg + db*db;
                    if (d2 > 0.0f) {
                        cands.push_back({static_cast<std::uint8_t>(r),
                                         d2, pal_cur[r]});
                    }
                }
                // Keep top-K by distance.
                if (cands.size() > cpl) {
                    std::partial_sort(cands.begin(),
                                      cands.begin() + static_cast<std::ptrdiff_t>(cpl),
                                      cands.end(),
                                      [](auto& a, auto& b) { return a.dist > b.dist; });
                    cands.resize(cpl);
                }
                // Sort by register for stable bank emission.
                std::sort(cands.begin(), cands.end(),
                          [](auto& a, auto& b) { return a.reg < b.reg; });
                auto& out_changes = lace_changes[y];
                out_changes.reserve(cands.size());
                for (auto& c : cands) {
                    copper::CopperChange ch{};
                    ch.reg = c.reg;
                    ch.color = c.color;
                    out_changes.push_back(ch);
                }
            }
            changes_ptr = &lace_changes;
        }
        auto& changes = *changes_ptr;
        auto ch_height = changes.size();

        // Variable-length per-scanline storage. Each line emits only its
        // *real* hi/lo entries; per-line count arrays say how many. The
        // viewer walks running pointers advanced by the count per line.
        // Drops the prior fixed [H][cpl] grid + 0xFFFF sentinel scheme,
        // which wasted bytes on padding and nibble-skip placeholders.

        // Precompute per-line counts (skip slots flagged skip_hi / skip_lo)
        std::vector<std::size_t> n_hi(ch_height), n_lo(ch_height);
        for (std::size_t y = 0; y < ch_height; ++y) {
            for (auto& ch : changes[y]) {
                if (!(options.aga && ch.skip_hi)) ++n_hi[y];
                if (!(options.aga && ch.skip_lo)) ++n_lo[y];
            }
        }

        out += std::format("// Copper: {} changes/line max, {} scanlines\n",
                           cpl, ch_height);
        out += "struct CopperEntry { UWORD reg; UWORD color; };\n";

        // Per-line count arrays
        auto emit_count_array = [&](const char* suffix,
                                    const std::vector<std::size_t>& counts) {
            out += std::format("static const UBYTE {}_copper_n_{}[{}] = {{\n    ",
                               sym, suffix, ch_height);
            for (std::size_t y = 0; y < ch_height; ++y) {
                out += std::format("{}", counts[y]);
                if (y + 1 < ch_height) out += ",";
                if ((y + 1) % 16 == 0) out += "\n    ";
            }
            out += "\n};\n\n";
        };
        emit_count_array("hi", n_hi);
        if (options.aga) emit_count_array("lo", n_lo);

        // Per-line prefix-sum offset arrays. Enables O(1) random access
        // into the flat entry array for a given image row — needed for
        // interlace, where field 1 walks even rows (0, 2, 4, ...) and
        // field 2 walks odd rows (1, 3, 5, ...), so the two fields can't
        // share a single sequentially-advancing pointer.
        auto emit_offset_array = [&](const char* suffix,
                                     const std::vector<std::size_t>& counts) {
            out += std::format("static const USHORT {}_copper_off_{}[{}] = {{\n    ",
                               sym, suffix, ch_height);
            std::size_t acc = 0;
            for (std::size_t y = 0; y < ch_height; ++y) {
                out += std::format("{}", acc);
                if (y + 1 < ch_height) out += ",";
                if ((y + 1) % 16 == 0) out += "\n    ";
                acc += counts[y];
            }
            out += "\n};\n\n";
        };
        emit_offset_array("hi", n_hi);
        if (options.aga) emit_offset_array("lo", n_lo);

        // Flat hi-entry array
        std::size_t total_hi = 0;
        for (auto v : n_hi) total_hi += v;
        out += std::format("static const struct CopperEntry {}_copper_hi[{}] = {{\n",
                           sym, std::max(total_hi, std::size_t{1}));
        if (total_hi == 0) {
            out += "    {0,0}\n";  // dummy entry to avoid zero-sized array
        } else {
            std::size_t emitted = 0;
            for (std::size_t y = 0; y < ch_height; ++y) {
                if (n_hi[y] == 0) continue;
                out += "    ";
                for (auto& ch : changes[y]) {
                    if (options.aga && ch.skip_hi) continue;
                    auto hi = options.aga
                        ? palette::linear_to_aga_hilo(ch.color).hi
                        : palette::linear_to_ocs(ch.color);
                    out += std::format("{{{},0x{:04X}}}", ch.reg, hi);
                    ++emitted;
                    if (emitted < total_hi) out += ",";
                }
                out += std::format("  /* y={} */\n", y);
            }
        }
        out += "};\n\n";

        // Flat lo-entry array (AGA only)
        if (options.aga) {
            std::size_t total_lo = 0;
            for (auto v : n_lo) total_lo += v;
            out += std::format("static const struct CopperEntry {}_copper_lo[{}] = {{\n",
                               sym, std::max(total_lo, std::size_t{1}));
            if (total_lo == 0) {
                out += "    {0,0}\n";
            } else {
                std::size_t emitted = 0;
                for (std::size_t y = 0; y < ch_height; ++y) {
                    if (n_lo[y] == 0) continue;
                    out += "    ";
                    for (auto& ch : changes[y]) {
                        if (options.aga && ch.skip_lo) continue;
                        auto aga = palette::linear_to_aga_hilo(ch.color);
                        out += std::format("{{{},0x{:04X}}}", ch.reg, aga.lo);
                        ++emitted;
                        if (emitted < total_lo) out += ",";
                    }
                    out += std::format("  /* y={} */\n", y);
                }
            }
            out += "};\n\n";
        }
    }

    // --- Copper list builder helper ---
    // --- strips raw copper list (calibration / mid-line palette mode) ---
    //
    // line_moves[y] is a per-image-row sequence of (WAIT|MOVE) ops. Emit
    // them as a flat UWORD array of (cmd, data) pairs in chip RAM. The
    // viewer installs this list verbatim — no slot table indirection, no
    // per-line count arrays. Probe ADFs use this to map HPOS to display
    // pixel x; the production planner emits the same shape.
    if (has_scap) {
        // Shared with generate() (.h emitter). Viewer keeps `static const`
        // linkage so the array stays in this translation unit; .h uses
        // plain `const` so user code that includes the .h sees the
        // definition with external linkage.
        emit_strips_copper_list(out, sym, options, "static const");
    }

    out += "// Write bitplane pointer registers into a copper list.\n";
    out += "// BPL1PTH = $DFF0E0, each pointer pair is 4 bytes apart.\n";
    out += "static inline USHORT* copSetPlanes(USHORT* cl, "
           "const UBYTE** planes, int n) {\n";
    out += "    for (int i = 0; i < n; i++) {\n";
    out += "        ULONG a = (ULONG)planes[i];\n";
    out += "        UWORD reg = 0x00E0 + i * 4;  // BPLxPTH\n";
    out += "        *cl++ = reg;\n";
    out += "        *cl++ = (UWORD)(a >> 16);\n";
    out += "        *cl++ = reg + 2;  // BPLxPTL\n";
    out += "        *cl++ = (UWORD)a;\n";
    out += "    }\n";
    out += "    return cl;\n";
    out += "}\n\n";

    if (do_fade) {
        // Precomputed nibble×step/15 table (256 bytes). No multiply needed.
        out += "static const UBYTE fadeLUT[256] = {\n";
        for (int nibble = 0; nibble < 16; ++nibble) {
            out += "    ";
            for (int step = 0; step < 16; ++step) {
                out += std::format("{}", nibble * step / 15);
                if (nibble < 15 || step < 15) out += ",";
            }
            out += "\n";
        }
        out += "};\n";
        out += "// Copy copper list while fading color register values.\n";
        out += "// Single pass — no separate CopyMem needed.\n";
        out += "static void fadeBuild(const USHORT* src, USHORT* dst, int step) {\n";
        out += "    const UBYTE* lut = &fadeLUT[step];\n";
        out += "    while (!(src[0] == 0xFFFF && src[1] == 0xFFFE)) {\n";
        out += "        dst[0] = src[0];\n";
        out += "        if ((src[0] & 1) == 0 && src[0] >= 0x0180 && src[0] <= 0x01BE) {\n";
        out += "            UWORD c = src[1];\n";
        out += "            dst[1] = (UWORD)((lut[((c>>8)&0xF)<<4]<<8) | "
               "(lut[((c>>4)&0xF)<<4]<<4) | lut[(c&0xF)<<4]);\n";
        out += "        } else dst[1] = src[1];\n";
        out += "        src += 2; dst += 2;\n";
        out += "    }\n";
        out += "    dst[0] = 0xFFFF; dst[1] = 0xFFFE;\n";
        out += "}\n\n";
    }

    // --- main() ---
    out += "// ========================================================================\n";
    out += "// Entry point: take system, build copper list, display, wait for click\n";
    out += "// ========================================================================\n";
    out += "int main() {\n";
    out += "    SysBase = *((struct ExecBase**)4UL);\n";
    out += "    custom = (struct Custom*)0xdff000;\n";
    out += "    GfxBase = (struct GfxBase*)OpenLibrary("
           "(CONST_STRPTR)\"graphics.library\", 0);\n";
    out += "    if (!GfxBase) return 0;\n\n";

    // Check if AGA is required but not present.
    //
    // ChipRevBits0 is normally populated by SetPatch in the Workbench
    // startup-sequence, but our viewer may be launched straight from a
    // bootblock/trackloader with no Workbench up — in that case the
    // field is 0 even on real AGA hardware. We call SetChipRev() once
    // ourselves to force-populate the field and use its return value
    // directly (it's the same chiprev bits, returned for convenience).
    // SetChipRev() exists from graphics.library v39 onwards.
    //
    // We check the ALICE bit, not LISA. Alice is the AGA Agnus
    // replacement and is the defining AGA chip — GFXF_AA_ALICE is set
    // on every AGA machine (A1200/A4000/CD32). Canonical refs:
    //   https://jvaltane.kapsi.fi/amiga/howtocode/aga.html
    //   http://www.lysator.liu.se/amiga/code/guide/howtocode/text/AGA
    bool requires_aga = (pal_count > 32) || (depth > 6) ||
                        (is_hires && depth > 4);
    if (requires_aga) {
        out += "    // AGA detection: force-populate via SetChipRev, check Alice bit\n";
        out += "    ULONG chiprev = 0;\n";
        out += "    if (GfxBase->LibNode.lib_Version >= 39) {\n";
        out += "        chiprev = SetChipRev(SETCHIPREV_BEST);\n";
        out += "    }\n";
        out += "    if (GfxBase->LibNode.lib_Version < 39 ||\n";
        out += "        !(chiprev & GFXF_AA_ALICE)) {\n";
        out += "        struct Library* DOSBase = OpenLibrary("
               "(CONST_STRPTR)\"dos.library\", 0);\n";
        out += "        if (DOSBase) {\n";
        out += "            Write(Output(), (APTR)"
               "\"This image requires AGA (A1200/A4000).\\n\", 42);\n";
        out += "            Delay(150);\n";
        out += "            CloseLibrary(DOSBase);\n";
        out += "        }\n";
        out += "        CloseLibrary((struct Library*)GfxBase);\n";
        out += "        return 0;\n";
        out += "    }\n\n";
    }

    out += "    TakeSystem();\n";
    out += "    WaitVbl();\n\n";

    // Calculate copper list size: display setup + bitplane ptrs + colors + copper changes + end.
    // strips appends one UWORD pair per ScapMove = 4 bytes each. The past-0xFF
    // wrap is patched into line 255's existing end-of-line WAIT, so no extra
    // bytes are needed.
    std::size_t strips_total_bytes = 0;
    if (has_scap) {
        for (auto& row : *options.strips_line_moves)
            strips_total_bytes += row.size() * 4;
    }
    auto cop_size = 128 + depth * 4 * 2 + pal_count * 4 + strips_total_bytes;
    if (pal_count > 32) {
        // AGA >32: double writes (LOCT high + low) + BPLCON3 per bank + reset
        auto num_banks = (pal_count + 31) / 32;
        cop_size += pal_count * 4;  // double the color writes (LOCT=1 pass)
        cop_size += static_cast<std::size_t>((num_banks * 2 + 1) * 4);  // BPLCON3 switches
    } else if (options.aga) {
        // AGA <=32: LOCT=1 pass + BPLCON3 switches (2 for LOCT on/off)
        cop_size += pal_count * 4 + 8;
    } else if (options.dpf) {
        // OCS-targeted DPF on AGA: extra LOCT=1 pass + 2 BPLCON3 switches
        // + BPLCON4 + explicit FMODE write.
        cop_size += pal_count * 4 + 16;
    }
    if (has_copper) {
        cop_size += height * options.copper_changes_per_line * 8 + height * 4;
        if (options.aga) {
            // AGA LOCT per scanline: BPLCON3 LOCT=1 + N color writes + BPLCON3 LOCT=0
            cop_size += height * (8 + options.copper_changes_per_line * 4);
            if (pal_count > 32) {
                // Extra bank switches per scanline for hi pass
                cop_size += height * 8;
            }
        }
        // Line 0 copper changes emitted separately (before display)
        cop_size += options.copper_changes_per_line * 4;
        if (options.aga)
            cop_size += 8 + options.copper_changes_per_line * 4;
    }
    cop_size += 128;  // padding for blank-below, 256-boundary WAITs, end markers

    // MEMF_CLEAR: zero the tail past our end-sentinel. Empirically the
    // canonical halt-WAIT (0xFFFF, 0xFFFE) does not actually halt copper
    // on every chip / chipset state — without zero-fill, copper walks past
    // the sentinel into uninitialized chip RAM, can hit a stray bit
    // pattern that parses as `MOVE → VPOSR` (CDANG-blocked, undefined),
    // and ends up corrupting the field. Zero-fill makes the runaway path
    // a stream of harmless `MOVE 0 → BLTDDAT` until the next vblank's
    // auto-COPJMP1 properly enters the swapped list.
    out += std::format("    USHORT* copper1 = (USHORT*)AllocMem({}, MEMF_CHIP | MEMF_CLEAR);\n",
                       cop_size);
    out += "    USHORT* cl = copper1;\n\n";

    // Display setup — use standard Amiga display parameters
    auto y_start = 44;
    // For interlace: display window is per-field (half total height)
    // disp_height no longer needed — using fixed PAL diwstop (0x2CC1)

    // (need_fmode3 already computed up at the data array section so it could
    // gate the data array's 8-byte alignment.)

    // --- Display window and data fetch timing ---
    // DDFSTRT/DDFSTOP: when the chipset starts/stops fetching bitplane data each line.
    // FMODE=3 (64-bit fetch) needs earlier start (-8) for the wider DMA fetch alignment.
    out += "    // Data fetch start/stop — controls which horizontal pixels have bitplane data\n";
    if (is_hires && need_fmode3) {
        out += "    *cl++ = offsetof(struct Custom, ddfstrt); *cl++ = 0x0034;  // hires FMODE=3\n";
        out += "    *cl++ = offsetof(struct Custom, ddfstop); *cl++ = 0x00D4;\n";
    } else if (is_hires) {
        out += "    *cl++ = offsetof(struct Custom, ddfstrt); *cl++ = 0x003C;  // hires standard\n";
        out += "    *cl++ = offsetof(struct Custom, ddfstop); *cl++ = 0x00D4;\n";
    } else {
        auto lores_ddfstrt = need_fmode3 ? 0x0030 : 0x0038;
        out += std::format("    *cl++ = offsetof(struct Custom, ddfstrt); *cl++ = 0x{:04X};  // lores{}\n",
                           lores_ddfstrt, need_fmode3 ? " FMODE=3" : "");
        out += "    *cl++ = offsetof(struct Custom, ddfstop); *cl++ = 0x00D0;\n";
    }

    // DIWSTRT/DIWSTOP: display window — the visible rectangle on screen.
    // VSTART=44 (PAL standard), HSTART=0x81. DIWSTOP=0x2CC1 covers full PAL height.
    out += "    // Display window start/stop — visible area on screen\n";
    out += std::format("    *cl++ = offsetof(struct Custom, diwstrt); "
                       "*cl++ = ({}<<8)|0x81;  // VSTART={}, HSTART=$81\n",
                       y_start, y_start);
    {
        // Close DIWSTOP V at exactly the image bottom so the display window
        // ends there. Stops bitplane DMA at the hardware level — much more
        // reliable than waiting for a copper bplcon0=0 MOVE to fight FMODE=3
        // timing in the blank-below area (HAM8 lace AGA failed without this).
        // diwstop V encoding: V8 = ~V7. Using the low byte of last_line gives
        // the correct stop V because last_line >= 256 sets V7=0 → V8=1
        // → total = 256+byte; last_line < 256 has V7=1 → V8=0 → total = byte.
        auto field_lines = is_lace ? height / 2 : height;
        auto last_line = y_start + static_cast<int>(field_lines);
        auto stop_v = last_line & 0xFF;
        out += std::format("    *cl++ = offsetof(struct Custom, diwstop); "
                           "*cl++ = ({}<<8)|0xC1;  // VSTOP={}, HSTOP=$C1\n",
                           stop_v, last_line);
    }

    // FMODE: AGA fetch mode. 0=16-bit (OCS compatible), 3=64-bit (needed
    // for >6 plane modes and hires-5+). For DPF we explicitly zero FMODE
    // so an AGA host running an OCS-targeted DPF viewer doesn't inherit a
    // non-zero state from prior software (e.g. SetPatch leaving FMODE=1).
    if (need_fmode3) {
        out += "    *cl++ = 0x01fc; *cl++ = 0x0003;  // FMODE: 64-bit DMA fetch\n";
    } else if (options.dpf) {
        out += "    *cl++ = 0x01fc; *cl++ = 0x0000;  // FMODE: 16-bit DMA fetch (OCS-compat)\n";
    }

    // --- BPLCON0: main display control register ---
    // Bit 15: HIRES (640px mode)
    // Bit 14-12: BPU2-0 (bitplane count, 1-7)
    // Bit 11: HAM (Hold-And-Modify mode)
    // Bit 9: COLOR (color composite output enable, always set)
    // Bit 4: BPU3 (8th bitplane on AGA)
    // Bit 2: LACE (interlace enable)
    auto bpu = static_cast<int>(depth) & 7;
    auto bpu3 = (depth == 8) ? (1 << 4) : 0;
    auto bplcon0 = (bpu << 12) | bpu3 | (1 << 9);
    if (is_ham) bplcon0 |= (1 << 11);
    if (is_hires) bplcon0 |= (1 << 15);
    if (is_lace) bplcon0 |= (1 << 2);
    if (options.dpf) bplcon0 |= (1 << 10);  // DBLPF (dual playfield)

    // Emit BPLCON0 as OR of named parts
    std::string bplcon0_expr;
    if (is_hires) bplcon0_expr += "(1<<15)/*HIRES*/|";
    bplcon0_expr += std::format("({}<<12)/*BPU*/", bpu);
    if (is_ham) bplcon0_expr += "|(1<<11)/*HAM*/";
    if (options.dpf) bplcon0_expr += "|(1<<10)/*DBLPF*/";
    bplcon0_expr += "|(1<<9)/*COLOR*/";
    if (bpu3) bplcon0_expr += "|(1<<4)/*BPU3=8planes*/";
    if (is_lace) bplcon0_expr += "|(1<<2)/*LACE*/";

    out += std::format("    *cl++ = offsetof(struct Custom, bplcon0); "
                       "*cl++ = {};  // 0x{:04X}\n", bplcon0_expr, bplcon0);
    out += "    *cl++ = offsetof(struct Custom, bplcon1); *cl++ = 0;  // scroll offset = 0\n";

    // BPLCON2: playfield priority and KILLEHB
    // Bit 9: KILLEHB — prevents 6-plane non-EHB from triggering Extra Half-Brite
    //                  (not applicable in DPF mode — bit 9 is unused there)
    // Bit 6: PF2PRI = 0 (PF1 in front of PF2). With DPF and PF1 zeroed,
    //        leaving PF2 behind means transparent PF1 lets PF2 show through.
    auto bplcon2 = (depth == 6 && !params.is_ehb && !is_ham && !options.dpf)
                       ? 0x0200 : 0x0000;
    if (bplcon2)
        out += std::format("    *cl++ = offsetof(struct Custom, bplcon2); "
                           "*cl++ = 0x{:04X};  // KILLEHB\n", bplcon2);
    else
        out += "    *cl++ = offsetof(struct Custom, bplcon2); *cl++ = 0x0000;\n";

    // BPLCON3: AGA palette bank select (bank 0, LOCT=0). In dual-playfield
    // mode the PF2OF field (bits 12-10) selects PF2's color-register
    // offset; on AGA the field defaults to 000 (= +0, no offset) and must
    // be set explicitly even for OCS-compatible behavior.
    //   000 = +0  101 = +32
    //   001 = +2  110 = +64
    //   010 = +4  111 = +128
    //   011 = +8 (OCS-compatible, 3-plane PF2 -> regs 8..15)
    //   100 = +16 (4-plane PF2 -> regs 16..31, AGA DPF)
    // Verified empirically on A1200: with PF2OF=000 the image rendered via
    // regs 0..7 instead of 8..15, hiding our shifted PF2 palette behind
    // the zeroed PF1 slots.
    {
        unsigned bplcon3 = 0;
        if (options.dpf) {
            bplcon3 |= (depth == 8) ? 0x1000  // PF2OF=100, +16
                                    : 0x0C00; // PF2OF=011, +8
        }
        out += std::format("    *cl++ = 0x0106; *cl++ = 0x{:04X};  // BPLCON3"
                           "{}\n",
                           bplcon3,
                           options.dpf
                               ? (depth == 8 ? " (DPF, PF2 +16)"
                                             : " (DPF, PF2 +8)")
                               : ": bank 0, no LOCT");
    }
    // BPLCON4: AGA bitplane XOR mask + sprite color base. Default state
    // varies (Workbench may leave non-zero); zero it explicitly so
    // OCS-targeted viewers run cleanly on AGA hardware.
    if (options.dpf) {
        out += "    *cl++ = 0x010C; *cl++ = 0x0011;  // BPLCON4: BPLAM=0, sprite bases\n";
    }
    out += "\n";

    // --- Bitplane modulo ---
    // Single-frame (line-interleaved): each plane's pointer skips past
    // other planes' rows after consuming bpr bytes of its own row.
    //   Progressive: mod = bpr * (depth - 1)
    //   Interlace:   mod = bpr * (depth*2 - 1)   (skip the other field too)
    // Batch (plane-sequential): each plane is a contiguous bpr*height
    // block, so the next row is already at +bpr — no skip, mod = 0.
    // FMODE=3 reads 8 bytes past the visible area as overread, so the
    // pointer has advanced bpr+8 instead of bpr — subtract 8.
    int mod = is_batch
        ? -(need_fmode3 ? 8 : 0)
        : static_cast<int>(bpr)
              * (static_cast<int>(depth) * (is_lace ? 2 : 1) - 1)
              - (need_fmode3 ? 8 : 0);
    out += std::format("    *cl++ = offsetof(struct Custom, bpl1mod); "
                       "*cl++ = {};\n", mod);
    out += std::format("    *cl++ = offsetof(struct Custom, bpl2mod); "
                       "*cl++ = {};\n\n", mod);

    if (is_batch) {
        // Batch: BPLxPT comes from <sym>_frame_planes[frame][plane].
        // NULL slots get patched at runtime to a shared zero buffer that
        // is allocated once in chip RAM (size bpr*height) so unused
        // playfields cost only one buffer instead of one per frame.
        if (any_zero_plane) {
            out += std::format("    UBYTE* zero_plane = (UBYTE*)AllocMem("
                               "{}, MEMF_CHIP | MEMF_CLEAR);\n",
                               bpr * height);
            out += "    if (!zero_plane) { FreeSystem(); return 0; }\n";
            out += std::format("    for (int fi = 0; fi < {}; fi++)\n",
                               n_frames_total);
            out += std::format("        for (int p = 0; p < {}; p++)\n", depth);
            out += std::format("            if (!{}_frame_planes[fi][p])\n",
                               sym);
            out += std::format("                {}_frame_planes[fi][p] = "
                               "zero_plane;\n", sym);
        }
        out += std::format("    const UBYTE* planes[{}];\n", depth);
        out += std::format("    for (int i = 0; i < {}; i++)\n", depth);
        out += std::format("        planes[i] = {}_frame_planes[0][i];\n", sym);
        out += "    USHORT* bpl_slot = cl;\n";
        out += std::format("    cl = copSetPlanes(cl, planes, {});\n\n", depth);
    } else {
        out += std::format("    const UBYTE* planes[{}];\n", depth);
        out += std::format("    for (int i = 0; i < {}; i++)\n", depth);
        out += std::format("        planes[i] = (const UBYTE*){}_planes + {} * i;\n",
                           sym, bpr);
        out += std::format("    cl = copSetPlanes(cl, planes, {});\n\n", depth);
    }

    // Set palette via copper list.
    // AGA >32 colors: BPLCON3 bank switching.
    // For HAM modes: skip LOCT write — the nibble-copy (0xN → 0xNN)
    //   matches what the encoder computes (palette::quantize_to_ocs).
    // For standard modes: LOCT double-write zeroes low nibbles to prevent
    //   the auto-copy artifact that makes bright colors overshoot.
    if (pal_count > 32) {
        auto num_banks = (pal_count + 31) / 32;
        out += std::format("    // AGA palette: {} colors, {} banks\n",
                           pal_count, num_banks);
        for (std::size_t bank = 0; bank < static_cast<std::size_t>(num_banks); ++bank) {
            auto base = bank * 32;
            auto count = std::min(std::size_t{32}, pal_count - base);
            auto bank_bits = static_cast<unsigned>(bank << 13);

            // High nibbles (LOCT=0)
            out += std::format("    *cl++ = 0x0106; *cl++ = 0x{:04X};"
                               "  // bank {}\n", bank_bits, bank);
            out += std::format("    for (int i = 0; i < {}; i++) {{\n", count);
            out += "        *cl++ = offsetof(struct Custom, color) + i * 2;\n";
            out += std::format("        *cl++ = {}_palette[{} + i];\n", sym, base);
            out += "    }\n";

            // Low nibbles (LOCT=1) — skip during fade, full precision on final step
            if (options.aga) {
                    out += std::format("    *cl++ = 0x0106; *cl++ = 0x{:04X};"
                                   "  // bank {}, LOCT=1\n",
                                   bank_bits | 0x0200, bank);
                out += std::format("    for (int i = 0; i < {}; i++) {{\n", count);
                out += "        *cl++ = offsetof(struct Custom, color) + i * 2;\n";
                out += std::format("        *cl++ = {}_palette_lo[{} + i];\n", sym, base);
                out += "    }\n";
                }
        }
        // Reset to bank 0, LOCT=0
        out += "    *cl++ = 0x0106; *cl++ = 0x0000;\n\n";
    } else {
        // <=32 colors: write directly (no bank switching)
        out += std::format("    for (int i = 0; i < {}; i++) {{\n", pal_count);
        out += "        *cl++ = offsetof(struct Custom, color) + i * 2;\n";
        out += std::format("        *cl++ = {}_palette[i];\n", sym);
        out += "    }\n";
        if (options.aga) {
            // During fade: skip LOCT (12-bit precision is fine for a 16-step fade)
            // On final step or no-fade: write full 24-bit lo nibbles. In
            // DPF the BPLCON3 write must preserve PF2OF or PF2 collapses
            // back to register 0 (= no offset).
            unsigned pf2of = options.dpf ? ((depth == 8) ? 0x1000 : 0x0C00)
                                         : 0x0000;
            out += std::format("    *cl++ = 0x0106; *cl++ = 0x{:04X};"
                               "  // LOCT=1\n",
                               pf2of | 0x0200);
            out += std::format("    for (int i = 0; i < {}; i++) {{\n", pal_count);
            out += "        *cl++ = offsetof(struct Custom, color) + i * 2;\n";
            out += std::format("        *cl++ = {}_palette_lo[i];\n", sym);
            out += "    }\n";
            out += std::format("    *cl++ = 0x0106; *cl++ = 0x{:04X};"
                               "  // LOCT=0\n",
                               pf2of);
        } else if (options.dpf) {
            // OCS-targeted DPF viewer running on AGA: do an LOCT=1 pass
            // with the same OCS 12-bit values so each color channel nibble
            // gets replicated (0xN -> 0xNN), matching how OCS hardware
            // expands 12-bit colors to 24-bit. Without this, the low
            // nibbles inherit prior state and colors look wrong (often
            // black) on AGA. Preserve PF2OF in BPLCON3 across the LOCT
            // toggle so PF2 keeps its color offset.
            unsigned pf2of = (depth == 8) ? 0x1000 : 0x0C00;
            out += std::format("    *cl++ = 0x0106; *cl++ = 0x{:04X};"
                               "  // LOCT=1 (AGA-compat)\n",
                               pf2of | 0x0200);
            out += std::format("    for (int i = 0; i < {}; i++) {{\n", pal_count);
            out += "        *cl++ = offsetof(struct Custom, color) + i * 2;\n";
            out += std::format("        *cl++ = {}_palette[i];\n", sym);
            out += "    }\n";
            out += std::format("    *cl++ = 0x0106; *cl++ = 0x{:04X};"
                               "  // LOCT=0, restore PF2OF\n",
                               pf2of);
        }
        out += "\n";
    }

    // Copper changes per scanline
    bool aga_banks = options.aga && (pal_count > 32);

    // Emit a block of palette changes from the precomputed per-row
    // tables. Accepts the destination list variable name ("cl" or "cl2")
    // and a C expression that evaluates to the image-row index — so
    // interlace can have field 1 reference even rows and field 2
    // reference odd rows, sharing the same static tables.
    auto emit_copper_changes = [&](const std::string& cl_var,
                                   const std::string& row_expr,
                                   bool use_aga_banks) {
        if (!has_copper) return;
        out += "        {\n";
        out += std::format(
            "          const struct CopperEntry* p_hi = "
            "{0}_copper_hi + {0}_copper_off_hi[{1}];\n",
            sym, row_expr);
        if (options.aga) {
            out += std::format(
                "          const struct CopperEntry* p_lo = "
                "{0}_copper_lo + {0}_copper_off_lo[{1}];\n",
                sym, row_expr);
        }
        out += std::format(
            "          int n_hi = {}_copper_n_hi[{}];\n", sym, row_expr);
        if (options.aga)
            out += std::format(
                "          int n_lo = {}_copper_n_lo[{}];\n", sym, row_expr);

        if (use_aga_banks) {
            out += "          int cur_bank = -1;\n";
            out += "          for (int s = 0; s < n_hi; s++) {\n";
            out += "            UWORD reg = p_hi[s].reg;\n";
            out += "            int bank = reg / 32;\n";
            out += "            if (bank != cur_bank) {\n";
            out += std::format("                *{0}++ = 0x0106;\n", cl_var);
            out += std::format("                *{0}++ = (UWORD)(bank << 13);\n", cl_var);
            out += "                cur_bank = bank;\n";
            out += "            }\n";
            out += std::format("            *{0}++ = offsetof(struct Custom, color)"
                               " + (reg % 32) * 2;\n", cl_var);
            out += std::format("            *{0}++ = p_hi[s].color;\n", cl_var);
            out += "          }\n";
            out += "          cur_bank = -1;\n";
            out += "          for (int s = 0; s < n_lo; s++) {\n";
            out += "            UWORD reg = p_lo[s].reg;\n";
            out += "            int bank = reg / 32;\n";
            out += "            if (bank != cur_bank) {\n";
            out += std::format("                *{0}++ = 0x0106;\n", cl_var);
            out += std::format("                *{0}++ = (UWORD)((bank << 13) | 0x0200);\n", cl_var);
            out += "                cur_bank = bank;\n";
            out += "            }\n";
            out += std::format("            *{0}++ = offsetof(struct Custom, color)"
                               " + (reg % 32) * 2;\n", cl_var);
            out += std::format("            *{0}++ = p_lo[s].color;\n", cl_var);
            out += "          }\n";
        } else if (options.aga) {
            // BPLCON3 LOCT toggle inside the per-scanline copper. In DPF
            // mode the write must keep PF2OF set or PF2 will collapse to
            // register 0 starting from the line that ran this code.
            unsigned pf2of = options.dpf ? ((depth == 8) ? 0x1000 : 0x0C00)
                                         : 0x0000;
            out += "          for (int s = 0; s < n_hi; s++) {\n";
            out += std::format("            *{0}++ = offsetof(struct Custom, color) + p_hi[s].reg * 2;\n", cl_var);
            out += std::format("            *{0}++ = p_hi[s].color;\n", cl_var);
            out += "          }\n";
            out += std::format("          *{0}++ = 0x0106; *{0}++ = 0x{1:04X};  // LOCT=1\n",
                               cl_var, pf2of | 0x0200);
            out += "          for (int s = 0; s < n_lo; s++) {\n";
            out += std::format("            *{0}++ = offsetof(struct Custom, color) + p_lo[s].reg * 2;\n", cl_var);
            out += std::format("            *{0}++ = p_lo[s].color;\n", cl_var);
            out += "          }\n";
            out += std::format("          *{0}++ = 0x0106; *{0}++ = 0x{1:04X};  // LOCT=0\n",
                               cl_var, pf2of);
        } else {
            out += "          for (int s = 0; s < n_hi; s++) {\n";
            out += std::format("            *{0}++ = offsetof(struct Custom, color) + p_hi[s].reg * 2;\n", cl_var);
            out += std::format("            *{0}++ = p_hi[s].color;\n", cl_var);
            out += "          }\n";
        }
        out += "        }\n";
    };

    if (has_copper) {
        // Write palette changes right after the last bitplane fetch completes.
        // Encoded byte 0xDD = binary 11011101, which puts the copper WAIT H
        // comparator at 0x6E * 2 = 0xDC (= 220 color clocks). For hires
        // FMODE=3, DDFSTOP is 0xD4 = 212 and the 64-bit fetch overread
        // extends ~8 CCK past DDFSTOP, so the last fetch completes right
        // around 220. Waking at 220 reclaims the ~2 CCK of dead time that
        // the previous H=0xDE/222 position left on the table — visible in
        // the e9k-debugger copper overlay. For non-FMODE=3 modes the fetch
        // ends even earlier (no overread), so 0xDD is still safe.
        // Line 0: write changes before display starts (no WAIT).
        // In interlace mode, field 1 renders even image rows (0, 2, 4, ...)
        // so row 0's changes go here; field 2 renders odd rows so its
        // first pre-display write is row 1's changes (emitted in cl2).
        // Both lace and progressive need this seed: the encoder builds row
        // y's diff vs row y-1 (progressive) or row y-2 (lace), so the
        // very first row of each field must be applied against the base
        // palette before any per-line WAIT'd diff cascades from it.
        out += "    // Line 0 copper changes (before display)\n";
        emit_copper_changes("cl", "0", aga_banks);

        // Per-scanline copper palette changes. One WAIT per visible
        // scanline of the field. For non-interlace each VPOS line maps
        // 1:1 to an image row. For interlace, field 1 maps VPOS y-1+ys
        // to image row 2*(y-1)+2 (i.e. y scans rows 2,4,...), because
        // line 0 was written pre-display; field 2 scans odd rows 1,3,...
        // starting from pre-display row 1.
        out += "    // Per-scanline copper palette changes (end of previous line)\n";
        // In interlace, the Agnus VPOS counter runs at 2x the per-field rate
        // seen by diwstrt. diwstrt V=y_start places display start at physical
        // VPOS y_start*2 (field 1) / y_start*2+1 (field 2), and each image row
        // consumes 2 VPOS within a field. Progressive uses 1 VPOS per row.
        auto per_line_loop = [&](const std::string& cl_var,
                                 const std::string& row_expr,
                                 std::size_t rows_in_field,
                                 int vpos_step,
                                 int vpos_first) {
            out += std::format("    for (int y = 1; y < {}; y++) {{\n",
                               rows_in_field);
            out += std::format("        USHORT line = (y - 1) * {} + {};\n",
                               vpos_step, vpos_first);
            // Interlace needs the WAIT HP pushed later (0xE3 = HPOS 226) —
            // with 0xDD (220) the MOVEs land inside the last ~16 lores pixels
            // of the scanline still being drawn. Progressive keeps 0xDD.
            // Special case: at line==255 use 0xFFDF instead — this specific
            // pattern activates the copper's "past 0xFF" state so subsequent
            // WAITs with wrapped vp values (vp=256, 258 ...) match correctly.
            auto hp_byte = is_lace ? "0xE3" : "0xDD";
            out += std::format("        if (line == 255) {{\n"
                               "            *{0}++ = 0xFFDF;\n"
                               "        }} else {{\n"
                               "            *{0}++ = ((line & 0xFF) << 8) | {1};\n"
                               "        }}\n",
                               cl_var, hp_byte);
            out += std::format("        *{0}++ = 0xfffe;\n", cl_var);
            emit_copper_changes(cl_var, row_expr, aga_banks);
            out += "    }\n\n";
        };
        if (is_lace) {
            per_line_loop("cl", "(y * 2)", height / 2, 1, y_start);
        } else {
            per_line_loop("cl", "y", height, 1, y_start);
        }
    }

    // strips per-line copper ops: copy the static strips list into the live
    // copper buffer verbatim. Each entry is a UWORD pair, and the encoder
    // already inserted the past-0xFF marker if needed, so the viewer just
    // streams it out.
    if (has_scap) {
        out += "    // Strips per-line copper ops (raw WAIT/MOVE pairs)\n";
        out += std::format(
            "    for (ULONG i = 0; i < {0}_strips_copper_words; i++)\n"
            "        *cl++ = {0}_strips_copper_list[i];\n\n", sym);
    }

    // Blank below image: 0 bitplanes, keep LACE if interlaced
    auto blank_expr = is_lace
        ? "(1<<9)/*COLOR*/|(1<<2)/*LACE*/"
        : "(1<<9)/*COLOR*/";
    {
        auto vpos_stride = 1;
        auto vpos_y_start = y_start;
        auto field_lines = is_lace
            ? static_cast<int>(height / 2)
            : static_cast<int>(height);
        auto last_line = vpos_y_start + field_lines * vpos_stride;
        auto loop_max_line = vpos_y_start + (field_lines - 2) * vpos_stride;
        // If the per-line loop already emitted 0xFFDF (loop reached line=255
        // or beyond), don't emit it here — a second 0xFFDF after we've passed
        // vp=0xFF hangs forever. Only emit in blank-below when loop is too
        // short to have emitted one itself.
        // The strips path bakes its own 0xFFDF marker into the static table
        // when needed (see strips_copper_list emitter); suppress here so we
        // don't double-emit and hang past vp=0xFF.
        bool strips_emitted_wrap = has_scap && (last_line >= 256);
        if (last_line >= 256 && (!has_copper || loop_max_line < 255)
            && !strips_emitted_wrap) {
            out += "    *cl++ = 0xFFDF; *cl++ = 0xFFFE;"
                   "  // cross 256 boundary\n";
        }
        out += std::format("    *cl++ = ({}<<8)|1; *cl++ = 0xfffe;"
                           "  // WAIT line {}\n",
                           last_line >= 256 ? (last_line & 0xFF) : last_line,
                           last_line);
        out += std::format("    *cl++ = offsetof(struct Custom, bplcon0); "
               "*cl++ = {};  // 0 planes, blank below image\n", blank_expr);
    }

    // End copper list
    out += "    *cl++ = 0xffff; *cl++ = 0xfffe;\n\n";

    // Fade: scan copper list for color register offsets, then patch per frame
    if (do_fade) {
        out += "    ULONG cop_len = (ULONG)cl - (ULONG)copper1 + 4;\n";
        // Precompute all 16 fade steps as separate copper lists in chip RAM.
        // During fade, just swap cop1lc — zero CPU chip RAM access per frame.
        out += "    USHORT* fade_cops[16] = {0};\n";
        out += "    // Single alloc for all fade copies, carve into slices\n";
        out += "    int fade_steps = 0;\n";
        out += "    USHORT* fade_block = 0;\n";
        out += std::format("    {{ ULONG avail = AvailMem(MEMF_CHIP | MEMF_LARGEST);\n");
        out += std::format("      int want = (int)(avail / {});\n", cop_size);
        out += "      if (want > 15) want = 15;\n";
        out += "      if (want > 0) {\n";
        out += std::format("          fade_block = (USHORT*)AllocMem((ULONG)want * {}, MEMF_CHIP);\n",
                           cop_size);
        out += "          if (fade_block) fade_steps = want;\n";
        out += "      }\n";
        out += "      for (int s = 0; s < fade_steps; s++)\n";
        out += std::format("          fade_cops[s] = (USHORT*)((UBYTE*)fade_block + (ULONG)s * {});\n",
                           cop_size);
        out += "    }\n";
        out += "    fade_cops[fade_steps] = copper1;  // last = full brightness\n";
        out += "    // Build faded copies in a single pass (no CopyMem needed)\n";
        out += "    for (int s = 0; s < fade_steps; s++) {\n";
        out += "        int step = s * 15 / fade_steps;  // distribute 0..14 evenly\n";
        out += "        fadeBuild(copper1, fade_cops[s], step);\n";
        out += "    }\n";
        out += "    fade_steps++;  // include the full-brightness entry\n";
        // Fade-in
        out += "    // Fade-in: one cop1lc write per frame\n";
        out += "    custom->cop1lc = (ULONG)fade_cops[0];\n";
        out += "    custom->copjmp1 = 0x7fff;\n";
        out += "    custom->dmacon = DMAF_BLITTER;\n";
        out += "    WaitVbl();  // sync so copper starts at frame top\n";
        out += "    custom->dmacon = DMAF_SETCLR | DMAF_MASTER | "
               "DMAF_RASTER | DMAF_COPPER;\n";
        out += "    for (int fade = 1; fade < fade_steps; fade++) {\n";
        out += "        WaitVbl();\n";
        out += "        custom->cop1lc = (ULONG)fade_cops[fade];\n";
        out += "    }\n\n";
    }

    // For interlace: build a second copper list for the even field
    // and a vblank interrupt handler that swaps cop1lc based on LOF bit.
    if (is_lace) {
        out += std::format("    USHORT* copper2 = (USHORT*)AllocMem({}, MEMF_CHIP | MEMF_CLEAR);\n",
                           cop_size);
        out += "    USHORT* cl2 = copper2;\n\n";

        // Rebuild even field copper list
        if (is_hires) {
            auto h_ddf = need_fmode3 ? 0x0034 : 0x003C;
            out += std::format("    *cl2++ = offsetof(struct Custom, ddfstrt); *cl2++ = 0x{:04X};\n", h_ddf);
            out += "    *cl2++ = offsetof(struct Custom, ddfstop); *cl2++ = 0x00D4;\n";
        } else {
            out += "    *cl2++ = offsetof(struct Custom, ddfstrt); *cl2++ = 0x0038;\n";
            out += "    *cl2++ = offsetof(struct Custom, ddfstop); *cl2++ = 0x00D0;\n";
        }
        out += std::format("    *cl2++ = offsetof(struct Custom, diwstrt); "
                           "*cl2++ = ({}<<8)|0x81;\n", y_start);
        {
            auto field_lines = height / 2;
            auto last_line = y_start + static_cast<int>(field_lines);
            auto stop_v = last_line & 0xFF;
            out += std::format("    *cl2++ = offsetof(struct Custom, diwstop); "
                               "*cl2++ = ({}<<8)|0xC1;  // VSTOP={}\n",
                               stop_v, last_line);
        }
        if (need_fmode3)
            out += "    *cl2++ = 0x01fc; *cl2++ = 0x0003;\n";
        out += std::format("    *cl2++ = offsetof(struct Custom, bplcon0); "
                           "*cl2++ = {};  // 0x{:04X}\n", bplcon0_expr, bplcon0);
        out += "    *cl2++ = offsetof(struct Custom, bplcon1); *cl2++ = 0;\n";
        out += std::format("    *cl2++ = offsetof(struct Custom, bplcon2); *cl2++ = 0x{:04X};\n",
                           bplcon2);

        // Interleaved interlace: mod skips other field's interleaved row,
        // -8 compensates FMODE=3 overread.
        auto lace_mod = static_cast<int>(bpr) * (static_cast<int>(depth) * 2 - 1)
                      - (need_fmode3 ? 8 : 0);
        out += std::format("    *cl2++ = offsetof(struct Custom, bpl1mod); "
                           "*cl2++ = {};\n", lace_mod);
        out += std::format("    *cl2++ = offsetof(struct Custom, bpl2mod); "
                           "*cl2++ = {};\n", lace_mod);
        // Even field starts one interleaved row (depth*bpr) into the data,
        // plus the per-plane offset within that row.
        out += std::format("    const UBYTE* planes_even[{}];\n", depth);
        out += std::format("    for (int i = 0; i < {}; i++)\n", depth);
        out += std::format("        planes_even[i] = (const UBYTE*){}_planes"
                           " + {} * i + {};\n",
                           sym, bpr, bpr * depth);
        out += std::format("    cl2 = copSetPlanes(cl2, planes_even, {});\n", depth);
        // Field 2 palette init — must mirror field 1's setup so AGA banks
        // and LOCT bytes are loaded for HAM8 etc. Otherwise field 2's color
        // registers retain stale values from field 1's per-line writes.
        if (pal_count > 32) {
            auto num_banks = (pal_count + 31) / 32;
            for (std::size_t bank = 0; bank < static_cast<std::size_t>(num_banks); ++bank) {
                auto base = bank * 32;
                auto count = std::min(std::size_t{32}, pal_count - base);
                auto bank_bits = static_cast<unsigned>(bank << 13);

                out += std::format("    *cl2++ = 0x0106; *cl2++ = 0x{:04X};"
                                   "  // bank {}\n", bank_bits, bank);
                out += std::format("    for (int i = 0; i < {}; i++) {{\n", count);
                out += "        *cl2++ = offsetof(struct Custom, color) + i * 2;\n";
                out += std::format("        *cl2++ = {}_palette[{} + i];\n", sym, base);
                out += "    }\n";

                if (options.aga) {
                    out += std::format("    *cl2++ = 0x0106; *cl2++ = 0x{:04X};"
                                       "  // bank {}, LOCT=1\n",
                                       bank_bits | 0x0200, bank);
                    out += std::format("    for (int i = 0; i < {}; i++) {{\n", count);
                    out += "        *cl2++ = offsetof(struct Custom, color) + i * 2;\n";
                    out += std::format("        *cl2++ = {}_palette_lo[{} + i];\n", sym, base);
                    out += "    }\n";
                }
            }
            out += "    *cl2++ = 0x0106; *cl2++ = 0x0000;\n\n";
        } else {
            out += std::format("    for (int i = 0; i < {}; i++) {{\n", pal_count);
            out += "        *cl2++ = offsetof(struct Custom, color) + i * 2;\n";
            out += std::format("        *cl2++ = {}_palette[i];\n", sym);
            out += "    }\n";
            if (options.aga) {
                unsigned pf2of = options.dpf
                    ? ((depth == 8) ? 0x1000 : 0x0C00) : 0x0000;
                out += std::format("    *cl2++ = 0x0106; *cl2++ = 0x{:04X};"
                                   "  // LOCT=1\n", pf2of | 0x0200);
                out += std::format("    for (int i = 0; i < {}; i++) {{\n", pal_count);
                out += "        *cl2++ = offsetof(struct Custom, color) + i * 2;\n";
                out += std::format("        *cl2++ = {}_palette_lo[i];\n", sym);
                out += "    }\n";
                out += std::format("    *cl2++ = 0x0106; *cl2++ = 0x{:04X};"
                                   "  // LOCT=0\n", pf2of);
            }
        }

        // Field 2 row-1 seed (before display): mirrors field 1's row-0
        // seed. Field 2 renders odd image rows; row 1 diffs against the
        // base palette in the encoder's lace-rebuild scheme, and rows 3,
        // 5, ... cascade from it. Without this, the per-line loop's first
        // iteration writes row 3's diffs against a row-1 state that was
        // never applied, breaking the cascade for the entire field.
        if (has_copper) {
            out += "    // Field 2 line 1 copper changes (before display)\n";
            emit_copper_changes("cl2", "1", aga_banks);
        }

        // Field 2 per-scanline palette changes (odd image rows 1, 3, 5, ...)
        if (has_copper) {
            out += "    // Field 2 per-scanline palette changes\n";
            out += std::format("    for (int y = 1; y < {}; y++) {{\n", height / 2);
            out += std::format("        USHORT line = (y - 1) + {};\n",
                               y_start);
            out += "        if (line == 255) {\n";
            out += "            *cl2++ = 0xFFDF;  // activates past-0xFF state\n";
            out += "        } else {\n";
            out += "            *cl2++ = ((line & 0xFF) << 8) | 0xE3;\n";
            out += "        }\n";
            out += "        *cl2++ = 0xfffe;\n";
            emit_copper_changes("cl2", "(y * 2 + 1)", aga_banks);
            out += "    }\n\n";
        }
        // Blank below image in even field
        {
            auto field_lines = static_cast<int>(height / 2);
            auto last_line = y_start + field_lines;
            auto loop_max_line = y_start + field_lines - 2;
            if (last_line >= 256 && (!has_copper || loop_max_line < 255)) {
                out += "    *cl2++ = 0xFFDF; *cl2++ = 0xFFFE;"
                       "  // cross 256 boundary\n";
            }
            out += std::format("    *cl2++ = ({}<<8)|1; *cl2++ = 0xfffe;"
                               "  // WAIT line {}\n",
                               last_line >= 256 ? (last_line & 0xFF) : last_line,
                               last_line);
            out += std::format("    *cl2++ = offsetof(struct Custom, bplcon0); "
                   "*cl2++ = {};\n", blank_expr);
        }
        out += "    *cl2++ = 0xffff; *cl2++ = 0xfffe;\n\n";

        // Set the file-scope swap pointers used by VblHandler.
        out += "    // Field-swap pointers for VblHandler (declared at file scope)\n";
        out += "    cop_odd = (ULONG)copper1;\n";
        out += "    cop_even = (ULONG)copper2;\n\n";
    }

    // Turn off floppy motor
    out += "    // Turn off floppy motor (select DF0, motor off, deselect)\n";
    out += "    *(volatile UBYTE*)0xbfd100 &= ~(1<<3);  // select DF0\n";
    out += "    *(volatile UBYTE*)0xbfd100 |=  (1<<7);   // motor off\n";
    out += "    *(volatile UBYTE*)0xbfd100 |=  (1<<3);   // deselect DF0\n\n";

    // Start display (fade-in mode activates inside the fade loop)
    if (!do_fade) {
        out += "    custom->cop1lc = (ULONG)copper1;\n";
        if (is_lace) {
            out += "    custom->cop2lc = (ULONG)copper2;\n";
        }
        out += "    custom->dmacon = DMAF_BLITTER;\n";
        out += "    custom->copjmp1 = 0x7fff;\n";
        out += "    custom->dmacon = DMAF_SETCLR | DMAF_MASTER | "
               "DMAF_RASTER | DMAF_COPPER;\n\n";
    }

    if (is_lace) {
        // Vblank handler swaps cop1lc based on LOF (Long Frame) bit
        // in VPOSR (bit 15 of $DFF004). Installed via plain function
        // pointer; see VblHandler at file scope.
        out += "    // Install vblank interrupt for interlace field switching\n";
        out += "    *(volatile APTR*)(((UBYTE*)VBR) + 0x6c) = (APTR)VblHandler;\n";
        out += "    custom->intena = 0x8000 | (1<<5);  // enable VBL interrupt\n";
        out += "    Enable();  // allow CPU to service interrupts\n\n";
    }

    // Wait for left mouse button. Multi-frame batch viewer: each click
    // advances to the next frame (wraps around); right-click exits.
    if (n_extra > 0) {
        out += std::format("    // Multi-frame click-cycle: {} frames total\n",
                           n_extra + 1);
        out += "    int frame = 0;\n";
        out += "    short prev = 0;\n";
        out += "    for (;;) {\n";
        out += "        WaitVbl();\n";
        out += "        // Right-click (CIA-A port A bit 2 via POTGOR bit 10) exits.\n";
        out += "        if (((*(volatile UWORD*)0xdff016) & (1<<10)) == 0) break;\n";
        out += "        short cur = MouseLeft();\n";
        out += "        if (cur && !prev) {\n";
        out += std::format("            frame++; if (frame >= {}) frame = 0;\n",
                           n_extra + 1);
        out += std::format("            const UBYTE* fp[{}];\n", depth);
        out += std::format("            for (int i = 0; i < {}; i++) fp[i] = "
                           "{}_frame_planes[frame][i];\n", depth, sym);
        out += std::format("            (void)copSetPlanes(bpl_slot, fp, {});\n",
                           depth);
        out += "        }\n";
        out += "        prev = cur;\n";
        out += "    }\n\n";
    } else {
        out += "    while (!MouseLeft()) { WaitVbl(); }\n\n";
    }

    // Fade-out: reverse, then disable display and free
    if (do_fade) {
        out += "    // Fade-out\n";
        out += "    for (int fade = fade_steps - 2; fade >= 0; fade--) {\n";
        out += "        WaitVbl();\n";
        out += "        custom->cop1lc = (ULONG)fade_cops[fade];\n";
        out += "    }\n";
        out += "    WaitVbl();\n";
        out += "    custom->dmacon = DMAF_COPPER | DMAF_RASTER;\n";
        out += std::format("    if (fade_block) FreeMem(fade_block, (ULONG)(fade_steps-1) * {});\n\n",
                           cop_size);
    }

    // Cleanup
    if (is_lace) {
        out += "    Disable();  // re-disable before FreeSystem restores state\n";
    }
    out += "    FreeSystem();\n";
    out += std::format("    FreeMem(copper1, {});\n", cop_size);
    if (is_lace) {
        out += std::format("    FreeMem(copper2, {});\n", cop_size);
    }
    out += "    CloseLibrary((struct Library*)GfxBase);\n\n";

    // Print exit message with image stats
    {
        auto chipset_str = options.aga ? "AGA" : "OCS";
        auto total_bytes = planes.total_bytes();
        bool has_cop  = options.copper_changes && !options.copper_changes->empty();
        // has_scap is already declared in the outer scope (line 416).

        // Mode label: combines the base mode (HAM/EHB/lores/hires) with
        // any active modifiers (DPF, sliced, strips, hires, interlace) into a
        // single human-readable string. Order: BASE [+ DPF] [+ sliced|strips].
        auto mode_params = amiga::get_mode_params(mode);
        std::string mode_label;
        if (mode_params.is_ham) {
            mode_label = std::format("HAM{}", planes.depth);
        } else if (mode_params.is_ehb) {
            mode_label = "EHB";
        } else if (options.hires) {
            mode_label = std::format("hires {}bpl", planes.depth);
        } else {
            mode_label = std::format("lores {}bpl", planes.depth);
        }
        if (options.dpf)       mode_label += " + DPF";
        if (options.interlace) mode_label += " + lace";
        if (has_scap)          mode_label += " + Strips";
        else if (has_cop)      mode_label += " + Sliced";

        // Build the message as a C string literal. Each line starts with
        // two spaces and a label-padded prefix so the values align in a
        // single visual column.
        std::string msg;
        msg += "\\n";
        msg += "\\033[1;33m";  // bold yellow banner
        msg += "                 ___            _\\n";
        msg += "  _ __ _ _  __ _|_  )__ _ _ __ (_)__ _ __ _\\n";
        msg += " | '_ \\\\ ' \\\\/ _` |/ // _` | '  \\\\| / _` / _` |\\n";
        msg += " | .__/_||_\\\\__, /___\\\\__,_|_|_|_|_\\\\__, \\\\__,_|\\n";
        msg += " |_|       |___/                 |___/\\n";
        msg += "\\033[0m";
        msg += "\\n";
        msg += std::format("  Mode:     {}\\n", mode_label);
        msg += std::format("  Display:  {}x{}, {} ({}-bit palette)\\n",
                           planes.width, planes.height, chipset_str,
                           options.aga ? 24 : 12);
        // Visible palette count: for DPF the palette span is the
        // COLOR00..15 register layout (16 entries) but PF2 only uses
        // 1 << (depth/2) of them. Report the visible count so the
        // viewer matches the CLI / web Palette: line.
        std::size_t visible_pal = options.dpf
            ? (std::size_t{1} << (planes.depth / 2))
            : palette.size();
        msg += std::format("  Palette:  {} colors\\n", visible_pal);
        if (options.total_unique_colors > 0) {
            msg += std::format("  Colors:   {} unique\\n",
                               options.total_unique_colors);
        }
        msg += std::format("  Bitplane: {} bytes\\n", total_bytes);
        if (has_cop)
            msg += std::format("  Sliced:      {} swaps/line max\\n",
                               options.copper_changes_per_line);
        if (has_scap) {
            std::size_t strips_words = 0;
            for (auto& row : *options.strips_line_moves)
                strips_words += row.size() * 2;
            msg += std::format("  Strips:     {} bytes copper list\\n",
                               strips_words * 2);
        }
        msg += "\\n";
        msg += "  \\033[36mhttps://www.png2amiga.app\\033[0m\\n";
        msg += "\\n";

        // Use DOSBase->Write(Output(), msg, len) via proto/dos.h
        // The viewer already has access to AmigaOS headers.
        out += "    // Exit message\n";
        out += "    { struct Library* DOSBase = OpenLibrary((CONST_STRPTR)\"dos.library\", 0);\n";
        out += "      if (DOSBase) {\n";
        out += "        static const char msg[] = \"" + msg + "\";\n";
        out += "        BPTR fh = Output();\n";
        out += std::format("        if (fh) Write(fh, (APTR)msg, {});\n",
                           // Count actual bytes (unescape \\n → \n, \\033 → \033, \\\\ → \\)
                           [&]() {
                               std::size_t len = 0;
                               for (std::size_t i = 0; i < msg.size(); ++i) {
                                   if (msg[i] == '\\' && i + 1 < msg.size()) {
                                       if (msg[i+1] == 'n' || msg[i+1] == '\\') { ++i; }
                                       else if (msg[i+1] == '0') { i += 3; }  // \033
                                   }
                                   ++len;
                               }
                               return len;
                           }());
        out += "        CloseLibrary(DOSBase);\n";
        out += "    } }\n\n";
    }

    out += "    return 0;\n";
    out += "}\n";

    return out;
}

Result<void> save_viewer(std::string_view path,
                         const bitplane::BitplaneData& planes,
                         std::span<const Color3f> palette,
                         amiga::Mode mode,
                         const CHeaderOptions& options) {
    auto content = generate_viewer(planes, palette, mode, options);
    if (!content) return std::unexpected{content.error()};

    auto path_str = std::string(path);
    std::ofstream file(path_str);
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to open file for writing: " + path_str,
        }};
    }

    file << *content;
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to write viewer source to: " + path_str,
        }};
    }

    return {};
}

} // namespace png2amiga::cheader
