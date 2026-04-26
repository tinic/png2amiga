#include "amiga.hpp"
#include "api.hpp"
#include "bitplane.hpp"
#include "cheader.hpp"
#include "cheader_dos_c.hpp"
#include "degas.hpp"
#include "color_space.hpp"
#include "cga_text.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "dither_tuning.hpp"
#include "scap.hpp"
#include "ham.hpp"
#include "iff.hpp"
#include "palette.hpp"
#include "palette_io.hpp"
#include "palette_locks.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"
#include "version.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <print>
#include <unordered_set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using namespace png2amiga;

// Exit codes following sysexits.h conventions so build systems can
// distinguish failure categories (CMake's RESULT_VARIABLE, Make's $?).
//   0  success
//   1  internal error / encode failure
//   64 EX_USAGE     — bad CLI arguments / unsupported option combo
//   66 EX_NOINPUT   — input file missing or unreadable
//   73 EX_CANTCREAT — output file write failed
namespace exit_code {
inline constexpr int ok          = 0;
inline constexpr int internal    = 1;
inline constexpr int usage       = 64;
inline constexpr int no_input    = 66;
inline constexpr int cant_create = 73;
}  // namespace exit_code

// Status-output gating. Set in main() right after parse_args returns so
// cli_status() can no-op silently in quiet mode. Two booleans (vs. a
// pointer to Config) so this works without forward declarations.
bool g_quiet = false;
bool g_json  = false;

bool is_quiet() { return g_quiet; }

// Encode-result snapshot for JSON status output. Populated lazily by
// the per-mode branches as soon as the relevant numbers are known so
// the end-of-main JSON emitter can produce a single object without
// re-parsing internal state. Floats default to NaN to mark "not set".
struct JsonResult {
    int   width  = 0;
    int   height = 0;
    int   depth  = 0;
    int   colors = 0;
    float psnr   = std::numeric_limits<float>::quiet_NaN();
} g_json_result;

// Wrapper around std::println for human status output to stdout. No-op
// when --quiet / --json is set. Errors should NOT use this — they go
// straight to stderr regardless of flag state.
template <class... Args>
void cli_status(std::format_string<Args...> fmt, Args&&... args) {
    if (is_quiet()) return;
    std::println(fmt, std::forward<Args>(args)...);
}

// Human-readable byte size: "51120 B", "49.9 K", "1.2 M".
std::string fmt_size(int bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    if (bytes < 1024 * 1024)
        return std::format("{:.1f} K", static_cast<double>(bytes) / 1024.0);
    return std::format("{:.1f} M", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// Canonical "Encoded:" status line. ALL CLI encoders should funnel
// through this so disk/chip numbers stay correct as new modes add data.
// Caller supplies the encoder's outputs:
//   plane_bytes      bitplane data size
//   palette_size     base palette entry count
//   aga              AGA chipset (palette / copper costs 2× the OCS price)
//   cap_grid_entries CAP planner's height × cpl (0 if no CAP)
//   scap_op_count    SCAP planner's total WAIT/MOVE ops (0 if no SCAP)
//   max_moves        worst-case copper MOVEs per scanline (for chip-RAM)
//   height           image height (used in chip-RAM per-line list math)
// Pass `avg_cap` only when the mode actually has CAP — otherwise the
// "0.0 avg CAP/line" suffix is misleading.
void cli_print_encoded(int depth, int plane_bytes, int palette_size,
                       bool aga, int cap_grid_entries, int scap_op_count,
                       int height, int max_moves, int num_colors,
                       std::optional<float> avg_cap,
                       double quant_error, float psnr) {
    auto sb = api::compute_size_breakdown(plane_bytes, palette_size, aga,
                                          cap_grid_entries, scap_op_count,
                                          height, max_moves);
    if (avg_cap) {
        cli_status("Encoded: {} bitplanes, disk: {}, chip: {}, {} colors, "
                   "{:.1f} avg CAP/line, error: {:.4f}, PSNR: {:.2f} dB",
                   depth, fmt_size(sb.disk_bytes), fmt_size(sb.chip_bytes),
                   num_colors, *avg_cap, quant_error, psnr);
    } else {
        cli_status("Encoded: {} bitplanes, disk: {}, chip: {}, {} colors, "
                   "error: {:.4f}, PSNR: {:.2f} dB",
                   depth, fmt_size(sb.disk_bytes), fmt_size(sb.chip_bytes),
                   num_colors, quant_error, psnr);
    }
}

// Emit a Make-format depfile so CMake's add_custom_command(... DEPFILE)
// triggers a rebuild when --palette / external inputs change. Format:
//   <output>: <input1> <input2> ...
// Only call after a successful encode so a partial depfile doesn't poison
// the build.
void write_depfile(const std::string& depfile_path,
                   const std::string& output_path,
                   std::span<const std::string_view> inputs) {
    std::ofstream f(depfile_path);
    if (!f) return;  // Soft failure — depfile is advisory; don't fail the build
    f << output_path << ":";
    for (auto& in : inputs) {
        if (!in.empty()) f << " " << in;
    }
    f << "\n";
}

int count_unique_colors(const Image& img) {
    std::unordered_set<std::uint32_t> seen;
    for (std::size_t y = 0; y < img.height(); ++y)
        for (std::size_t x = 0; x < img.width(); ++x) {
            auto srgb = color_space::linear_to_srgb(img[x, y]).clamped();
            auto r = static_cast<std::uint32_t>(srgb.r * 255.0f + 0.5f);
            auto g = static_cast<std::uint32_t>(srgb.g * 255.0f + 0.5f);
            auto b = static_cast<std::uint32_t>(srgb.b * 255.0f + 0.5f);
            seen.insert((r << 16) | (g << 8) | b);
        }
    return static_cast<int>(seen.size());
}

// ---------------------------------------------------------------------------
// Pad bitplane data to mode display width (for viewer .cpp export only)
// ---------------------------------------------------------------------------

void pad_planes_to_mode(bitplane::BitplaneData& planes, amiga::Mode mode,
                        bool hires = false) {
    auto display_w = (hires || amiga::get_mode_params(mode).is_hires)
        ? std::size_t{640} : amiga::default_width(mode);
    if (planes.width == display_w) return;
    auto old_bpr = planes.bytes_per_row;
    auto new_bpr = ((display_w + 15) / 16) * 2;
    auto depth = planes.depth;
    auto height = planes.height;
    std::vector<std::uint8_t> padded(depth * height * new_bpr, 0);
    auto copy_bpr = std::min(old_bpr, new_bpr);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t p = 0; p < depth; ++p) {
            auto src_off = planes.plane_row_offset(p, y);
            std::size_t dst_off;
            if (planes.layout == bitplane::Layout::interleaved)
                dst_off = y * depth * new_bpr + p * new_bpr;
            else
                dst_off = p * height * new_bpr + y * new_bpr;
            std::copy_n(planes.data.data() + src_off, copy_bpr,
                        padded.data() + dst_off);
        }
    }
    planes.data = std::move(padded);
    planes.width = display_w;
    planes.bytes_per_row = new_bpr;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// Build-system integration flags (--quiet, --json, --depfile, --list-modes).
// Centralised so the CLI dispatcher can route status output through one
// helper instead of sprinkling `if (config.quiet)` everywhere.
struct Config {
    std::string input_path;
    std::string output_path;           // .png for preview, .iff for ILBM, .h for C header
    bool quiet = false;                // suppress all stdout status (errors → stderr)
    bool json = false;                 // emit JSON status object instead of human text
    std::string depfile;               // Make-format depfile path (empty = disabled)
    bool list_modes = false;           // emit mode catalog (human or JSON) and exit 0

    // Bitplane layout override for raw/.h export. nullopt = auto per mode
    // (Amiga → line-interleaved, DOS planar → plane-sequential, Atari →
    // word-interleaved). Setting this overrides the auto choice. Affects
    // the byte order in the encoded output; doesn't touch IFF (which has
    // its own body-storage convention).
    std::optional<bitplane::Layout> layout_override;

    // Skip image scaling — output uses source dimensions verbatim.
    // Compatible with .h, .iff, .raw, .pal, .png. .cpp/.c viewer is
    // rejected because the viewer code assumes fixed Amiga screen modes.
    bool no_scale = false;
    amiga::Mode mode = amiga::Mode::lores;
    bool hires = false;                // compound mode hires override
    bool interlace = false;            // LACE bit in CAMG
    std::size_t depth = 5;
    bool depth_explicit = false;  // true if user passed --depth on the CLI
    preprocess::Settings preprocess{};
    std::optional<std::size_t> width;
    std::optional<std::size_t> height;
    bool match_range = false;

    // HAM encoding
    std::size_t ham_beam = 16;

    // HAM triple-pixel refinement post-pass. 0 = off, 16 default
    // (sweet spot — plateau past that).
    std::size_t ham_triple = 16;

    // HAM greedy encoder (skip DP beam search, ~20× faster, ~1 dB worse).
    bool ham_fast = false;

    // CAP best-quality planner. Multi-candidate × all-slot search + joint
    // base-palette refinement. HAM6 + copper and HAM8 + copper only —
    // indexed copper modes ignore this flag (their planner is already
    // mature). Adds ~+0.5-2 dB PSNR for ~4-5× the CAP-encoding cost.
    // Off by default — opt-in for offline / final exports.
    bool cap_best = false;

    // Palette diversity (ham_convert-style). 0 = off, 1-5 = progressively
    // aggressive removal of near-duplicate palette entries, re-seeded from
    // poorly-served image regions.
    int palette_diversity = 0;

    // Quantizer selection (empty = auto: OCS brute-force for OCS, median-cut
    // for AGA). "pnn" uses Pairwise Nearest Neighbor (experimental).
    std::string quantizer;

    // Dithering
    dither::Method dither_method = dither::Method::floyd_steinberg;
    bool dither_explicit = false;       // true if user passed --dither
    bool dither_strength_explicit = false;
    bool error_clamp_explicit = false;  // true if user passed --error-clamp
    float dither_strength = 1.0f;
    float error_clamp = 0.35f;
    std::string cga_text_metric = "blur";

    // Chipset
    std::optional<amiga::Chipset> chipset;  // empty = auto-detect from mode

    // Palette
    std::string palette_file;            // load palette from file (empty = auto)

    // C header
    std::string symbol_name;           // base name for C symbols (default: derived from output)

    // Transparency
    float alpha_threshold = 0.0f;      // offset from 0.5 midpoint (-0.5..0.5)
    dither::Method alpha_dither = dither::Method::none;  // none = use threshold
    float alpha_dither_strength = 1.0f; // strength for alpha dither (independent)

    // Palette refinement
    int refine_iterations = 4;         // dither-aware palette refinement iterations (0=off)

    // Copper palette
    bool copper = false;               // per-scanline palette changes
    int copper_changes = 0;            // 0 = auto (based on chipset/depth)
    bool fade_in = false;              // 16-step fade-in from black

    // CGA-specific options
    int cga_palette = 3;               // 0=P0-low, 1=P0-high, 2=P1-low, 3=P1-high (default)
    int cga_bg = 0;                    // background color index (0..15 in master palette)
    bool cga_auto_palette = true;      // try all palettes, pick lowest-error

    // PAR handling: for modes with non-square hardware pixels (VGA 13h 1.2
    // tall, EGA 640x200 2x tall, etc.), --native-par preserves source aspect
    // on the target hardware display by letterboxing/pillarboxing into the
    // fixed buffer. Without the flag, modes with fixed dimensions stretch
    // source to fill the buffer (may look squished on CRT).
    bool native_par = false;
    bool reserve_color0 = true;        // reserve index 0 for black (border)

    // Dual playfield: encode image into PF2, leave PF1 zeroed, palette
    // shifted into upper color registers (8-15 OCS / 16-31 AGA).
    bool dual_playfield = false;

    // SCAP calibration probe (DPF-only). "" disables. "a"/"b"/"c"/"d"
    // selects which probe synthesizes the test viewer. Phase 1 only
    // implements probe A (OCS DPF slot HPOS sweep).
    std::string scap_probe;

    // SCAP encoder: mid-line palette swaps inside the displayed area, with
    // up to slot.capacity MOVEs per slot. OCS DPF lores only (Phase 1).
    bool scap = false;
    bool scap_debug = false;

    // Mask export
    std::string mask_path;             // output path for transparency mask
    bool mask_invert = false;          // invert mask polarity

    // Cropping
    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;                    // 0 = no crop
    int crop_h = 0;
    bool crop_auto = false;            // auto-crop to mode aspect ratio

    // Output
    bool preview = false;              // show terminal image preview (iTerm2)

    // Palette index manipulation
    std::vector<api::LockSpec> locks;  // --lock-index <id> <rgbhex>
    std::vector<api::PinSpec>  pins;   // --pin-index-at <id> <x> <y>
};

void print_version() {
    cli_status("png2amiga {}", png2amiga::version);
}

void print_usage() {
    std::println(stderr,
        "png2amiga {}\n"
        "\n"
        "Usage: png2amiga [options] input.[png|jpg|webp] [-o output.[png|iff|h|raw|pal|pi1|pi2|pi3]]\n"
        "\n",
        png2amiga::version);
    std::println(stderr,
        "Modes:\n"
        "  --mode <mode>                   Graphics mode (default: lores)\n"
        "    Amiga:\n"
        "         lores, lores-lace, hires, hires-lace,\n"
        "         ham6, ham6-lace, ham6-hires, ham6-hires-lace,\n"
        "         ham8, ham8-lace, ham8-hires, ham8-hires-lace,\n"
        "         ehb, ehb-lace\n"
        "    Atari ST/STE:\n"
        "         stf-low, stf-med, stf-hi, ste-low, ste-med, ste-hi\n"
        "    IBM PC VGA:\n"
        "         vga-13h     (320x200, 256 colors, 8bpp chunky, 18-bit DAC)\n"
        "         vga-10h     (640x350, 16 colors, 4-plane planar, 18-bit DAC)\n"
        "         vga-12h     (640x480, 16 colors, 4-plane planar, square pixels)\n"
        "    IBM PC EGA (16 colors from fixed 64-color IrgbIRGB gamut):\n"
        "         ega-320     (320x200)\n"
        "         ega-640     (640x200)\n"
        "         ega-hi      (640x350)\n"
        "    IBM PC CGA:\n"
        "         cga-320       (320x200, 4 colors, fixed palettes via --cga-palette)\n"
        "         cga-640       (640x200, 2 colors monochrome)\n"
        "         cga-composite (160x200 effective, 16 colors via NTSC artifacting)\n"
        "    Text-mode graphics (AREA 5150 style glyph matching):\n"
        "         cga-text80x100  (CGA 8x8 font, 2-scanline cells, 80x100 cells)\n"
        "  --depth <1-8>                   Bitplane depth (default: 5)\n"
        "  --chipset ocs|aga               OCS 12-bit / AGA 24-bit (default: auto)\n"
        "  --dual-playfield, --dpf         Dual playfield: encode image into PF2\n"
        "                                  (upper color regs 8-15 OCS / 16-31 AGA)\n"
        "                                  with PF1 (foreground) zeroed. Forces\n"
        "                                  depth=3 (OCS) or 4 (AGA).\n"
        "\n"
        "CAP — Copper-Augmented Palette (per-line swaps):\n"
        "  --cap                           Per-scanline palette swaps via the copper,\n"
        "                                  picked greedily by OKLab error reduction.\n"
        "                                  (Legacy alias: --copper)\n"
        "  --cap-changes <0-16>            CAP swaps per line (0 = auto, picks the\n"
        "                                  worst-case K that fits the 14-MOVE\n"
        "                                  budget; auto mode also tries K+1..K+3).\n"
        "                                  (Legacy alias: --copper-changes)\n"
        "  --cap-best                      Slower (~4-5×) CAP planner: multi-candidate\n"
        "                                  slot search + joint base-palette refinement.\n"
        "                                  HAM6 + CAP and HAM8 + CAP only — the indexed\n"
        "                                  CAP planner (lores/hires/EHB) is already\n"
        "                                  mature and the refinement gives ≤+0.10 dB\n"
        "                                  there. +0.5 to +2 dB PSNR on HAM. Off by\n"
        "                                  default.\n"
        "\n"
        "SCAP — Super CAP (mid-line swaps):\n"
        "  --scap                          Mid-line palette swaps inside the displayed\n"
        "                                  area, on top of CAP's per-line evolution.\n"
        "                                  19 MOVEs per scanline at 16-lores-px stride;\n"
        "                                  slot HPOS table calibrated against real OCS\n"
        "                                  hardware. OCS lores only; pair with --dpf\n"
        "                                  (3-plane PF2, 8 base colors) or --mode ehb\n"
        "                                  (32 base + 32 half-brite).\n"
        "  --scap-probe <a|b|c|d>          [developer] DPF SCAP calibration probe.\n"
        "                                  Synthesizes a viewer that sweeps mid-line\n"
        "                                  MOVE slots on real hardware to discover the\n"
        "                                  slot table. Probe A only (OCS DPF) for now.\n"
        "  --scap-debug                    [developer] SCAP slot-tuning debug bundle:\n"
        "                                  forces base-palette MOVEs to 0x0000 and\n"
        "                                  paints yellow PF1 ruler markers at every\n"
        "                                  4/8/16 px. Pair with examples/ramps.png.\n"
        "\n"
        "HAM encoding:\n"
        "  --ham-beam <1-256>              Beam width for DP search (default: 48)\n"
        "  --ham-triple <0-256>            Triple-pixel refinement beam after main DP\n"
        "                                  (default: 16; 0 = disable). Catches fringe\n"
        "                                  artefacts the 1-pixel beam misses, ~+0.5-1 dB.\n"
        "  --ham-fast                      Greedy HAM encoder (no DP beam search).\n"
        "                                  ~15× faster, ~0.04 dB PSNR cost. For\n"
        "                                  realtime / batch / preview workflows.\n"
        "\n"
        "DOS-specific:\n"
        "  --native-par                    Preserve source aspect on fixed-buffer DOS\n"
        "                                  hardware (letterbox/pillarbox)\n"
        "  --cga-palette <p>               CGA 320 palette variant: p0-low, p0-high,\n"
        "                                  p1-low, p1-high (default: auto-pick best)\n"
        "  --cga-bg <0..15>                CGA background color (master palette index,\n"
        "                                  default: 0/black)\n"
        "\n"
        "Dithering:\n"
        "  --dither <method>               none|bayer2x2|bayer4x4|bayer8x8|\n"
        "                                  checker|clustered-dot|\n"
        "                                  h2x4|v4x2|bayer4x2|bayer2x4|\n"
        "                                  line2|line-checker|line4|line8|\n"
        "                                  vline2|vline-checker|vline4|vline8|\n"
        "                                  halftone8x8|diagonal8x8|spiral5x5|\n"
        "                                  hex8x8|hex5x5|blue-noise|\n"
        "                                  ign|white-noise|r2|crosshatch|\n"
        "                                  radial|value-noise|\n"
        "                                  floyd-steinberg|atkinson|sierra-lite|\n"
        "                                  stucki|jarvis|ostromoukhov\n"
        "                                  (default: floyd-steinberg)\n"
        "  --dither-strength <float>       Dither amount 0.0-1.0 (default: 1.0)\n"
        "  --error-clamp <float>           Max error per channel, squared internally\n"
        "                                  (default: 0.35; useful range 0.0-1.0)\n"
        "  --cga-text-metric mse|blur      CGA text mode: per-cell error metric.\n"
        "                                    blur = Pappas-Neuhoff perceptual halftoning\n"
        "                                           (default; ignores --dither).\n"
        "                                    mse  = per-pixel OKLab MSE\n"
        "                                           (pairs with --dither <method>).\n"
        "  --refine <0-32>                Dither-aware palette refinement iterations\n"
        "                                  (default: 4, 0 = off)\n"
        "\n"
        "Palette:\n"
        "  --palette <file>                Load palette from file (GIMP .gpl, IFF, hex text)\n"
        "  --quantizer <name>              auto (default) | median-cut | ocs-bruteforce |\n"
        "                                  pnn — auto picks ocs-bruteforce on OCS,\n"
        "                                  median-cut on AGA, PNN for HAM8/AGA-deep.\n"
        "  --palette-diversity <0-9>       Remove near-duplicate palette entries (experimental)\n"
        "  --no-reserve-color0             Don't reserve palette index 0 for black\n"
        "                                  (gives the encoder one extra image colour;\n"
        "                                  loses transparency / Amiga border colour 0)\n"
        "\n"
        "Image processing:\n"
        "  --brightness <float>            Brightness -1.0 to 1.0 (default: 0.0)\n"
        "  --contrast <float>              Contrast 0.0-3.0 (default: 1.0)\n"
        "  --saturation <float>            Saturation 0.0-3.0 (default: 1.0)\n"
        "  --gamma <float>                 Gamma 0.1-8.0 (default: 1.0)\n"
        "  --hue-shift <float>             Hue rotation -180 to 180 (default: 0)\n"
        "  --sharpen <float>               Sharpen/blur -1.0 to 2.0 (default: 0.0)\n"
        "  --black-point <float>           Black point 0.0-0.5 (default: 0.0)\n"
        "  --white-point <float>           White point 0.0-0.5 (default: 0.0)\n"
        "  --match-range                   Match image range to palette\n"
        "  --width <int>                   Override output width\n"
        "  --height <int>                  Override output height\n"
        "\n"
        "Transparency (color 0 = transparent when input has alpha):\n"
        "  --alpha-threshold <-0.5..0.5>   Offset from 0.5 midpoint (default: 0)\n"
        "  --alpha-dither <method>         Dither alpha (e.g. checker, bayer4x4)\n"
        "  --alpha-dither-strength <float> Alpha dither strength (default: 1.0)\n"
        "  --mask <file>                   Export transparency mask (.png/.iff/.raw)\n"
        "  --mask-invert                   Invert mask (1=transparent, 0=opaque)\n"
        "\n"
        "Cropping:\n"
        "  --crop <x,y,w,h>               Manual crop region (pixels)\n"
        "  --crop-auto                     Auto-crop to mode aspect ratio (center)\n"
        "\n"
        "Palette index manipulation (lores/hires/EHB/Atari only):\n"
        "  --lock-index <id> <rgbhex>      Lock palette slot to a specific color\n"
        "                                    e.g. --lock-index 0 000000 (force black at 0)\n"
        "                                    repeatable; locks override implicit color 0 = black\n"
        "  --pin-index-at <id> <x> <y>     After dither, swap whatever index pixel (x,y)\n"
        "                                    landed at with slot <id>. Repeatable.\n"
        "                                    Pin targets must not be locked.\n"
        "\n"
        "C header output:\n"
        "  --symbol <name>                 Base symbol name (default: from filename)\n"
        "  --fade-in                       Viewer fades in from black over 16 steps\n"
        "                                  on entry, fades out on exit. Non-HAM,\n"
        "                                  non-interlace only.\n"
        "\n"
        "Bitplane export:\n"
        "  --layout <which>                Bitplane byte order in .raw and .h output:\n"
        "                                    auto              (default; mode-specific)\n"
        "                                    interleaved       (Amiga DMA order; rows\n"
        "                                                       interleave plane bytes)\n"
        "                                    standard          (plane-sequential; for\n"
        "                                                       paint programs / boot\n"
        "                                                       blocks; aliases:\n"
        "                                                       --non-interleaved,\n"
        "                                                       --planar)\n"
        "                                    word-interleaved  (Atari ST native)\n"
        "                                  Doesn't affect .iff (uses its own format).\n"
        "  --non-interleaved, --planar     Shortcut for --layout standard\n"
        "  --interleaved                   Shortcut for --layout interleaved\n"
        "  --no-scale                      Skip scaling — output uses source PNG\n"
        "                                  dimensions verbatim. Compatible with .h,\n"
        "                                  .iff, .raw, .pal, .png. NOT compatible with\n"
        "                                  .cpp/.c viewer (needs fixed screen modes).\n"
        "\n"
        "Build-system integration:\n"
        "  -q, --quiet                     Suppress all stdout status; errors still go\n"
        "                                  to stderr. Useful in CMake/Make/Ninja builds.\n"
        "  --json                          Emit JSON status object instead of human text.\n"
        "                                  Implies --quiet for non-JSON output.\n"
        "  --depfile <path>                Write a Make-format depfile listing the input\n"
        "                                  PNG and any external palette file. Used by\n"
        "                                  CMake's add_custom_command(... DEPFILE) so a\n"
        "                                  --palette change triggers a rebuild.\n"
        "  --list-modes                    Print supported modes and exit (pair with\n"
        "                                  --json for machine-readable catalog).\n"
        "\n"
        "Exit codes (sysexits.h-style):\n"
        "  0  success\n"
        "  1  internal error / encode failure\n"
        "  64 EX_USAGE     — bad CLI args / unsupported option combo\n"
        "  66 EX_NOINPUT   — input file missing or unreadable\n"
        "  73 EX_CANTCREAT — output write failed\n"
        "\n"
        "Output:\n"
        "  --preview                       Show iTerm2 inline image preview\n"
        "  .png extension -> preview PNG image\n"
        "  .iff extension -> IFF ILBM Amiga image file\n"
        "  .h extension   -> C header with UWORD bitplane arrays\n"
        "  .raw extension -> raw interleaved bitplane data (no header)\n"
        "  .pal extension -> OCS 12-bit palette (2 bytes/color, big-endian 0x0RGB)\n"
        "  .pi1/.pi2/.pi3 -> Atari Degas image (requires Atari ST/STE mode)");
}

Result<dither::Method> parse_dither_method(std::string_view s) {
    if (s == "none") return dither::Method::none;
    if (s == "bayer2x2") return dither::Method::bayer2x2;
    if (s == "bayer4x4") return dither::Method::bayer4x4;
    if (s == "bayer8x8") return dither::Method::bayer8x8;
    if (s == "checker") return dither::Method::checker;
    if (s == "h2x4") return dither::Method::h2x4;
    if (s == "clustered-dot") return dither::Method::clustered_dot;
    if (s == "line2") return dither::Method::line2;
    if (s == "vline2") return dither::Method::vline2;
    if (s == "vline-checker") return dither::Method::vline_checker;
    if (s == "vline4") return dither::Method::vline4;
    if (s == "vline8") return dither::Method::vline8;
    if (s == "line-checker") return dither::Method::line_checker;
    if (s == "line4") return dither::Method::line4;
    if (s == "v4x2") return dither::Method::v4x2;
    if (s == "bayer4x2") return dither::Method::bayer4x2;
    if (s == "bayer2x4") return dither::Method::bayer2x4;
    if (s == "line8") return dither::Method::line8;
    if (s == "halftone8x8") return dither::Method::halftone8x8;
    if (s == "diagonal8x8") return dither::Method::diagonal8x8;
    if (s == "spiral5x5") return dither::Method::spiral5x5;
    if (s == "hex8x8") return dither::Method::hex8x8;
    if (s == "hex5x5") return dither::Method::hex5x5;
    if (s == "blue-noise") return dither::Method::blue_noise;
    if (s == "floyd-steinberg") return dither::Method::floyd_steinberg;
    if (s == "atkinson") return dither::Method::atkinson;
    if (s == "sierra-lite") return dither::Method::sierra_lite;
    if (s == "stucki") return dither::Method::stucki;
    if (s == "jarvis") return dither::Method::jarvis;
    if (s == "ostromoukhov") return dither::Method::ostromoukhov;
    if (s == "gilbert") return dither::Method::gilbert;
    if (s == "ign") return dither::Method::ign;
    if (s == "white-noise") return dither::Method::white_noise;
    if (s == "r2") return dither::Method::r2_sequence;
    if (s == "crosshatch") return dither::Method::crosshatch;
    if (s == "radial") return dither::Method::radial;
    if (s == "value-noise") return dither::Method::value_noise;
    return std::unexpected{Error{ErrorCode::unsupported_mode,
        "Unknown dither method: " + std::string(s)}};
}

Result<Config> parse_args(int argc, char* argv[]) {
    Config config;
    int positional = 0;

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view(argv[i]);

        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        }

        if (arg == "--version" || arg == "-V") {
            print_version();
            std::exit(0);
        }

        if (arg == "--match-range") {
            config.match_range = true;
            continue;
        }

        if (arg == "--preview") {
            config.preview = true;
            continue;
        }


        if (arg == "--no-reserve-color0") {
            config.reserve_color0 = false;
            continue;
        }

        if (arg == "--crop-auto") {
            config.crop_auto = true;
            continue;
        }

        if (arg == "--cap" || arg == "--copper") {
            // --copper kept as legacy alias for --cap.
            config.copper = true;
            continue;
        }

        if (arg == "--ham-fast") {
            config.ham_fast = true;
            continue;
        }

        if (arg == "--cap-best" || arg == "--ham-cap-best") {
            // --ham-cap-best kept as legacy alias.
            config.cap_best = true;
            continue;
        }

        if (arg == "--fade-in") {
            config.fade_in = true;
            continue;
        }

        if (arg == "--quiet" || arg == "-q") {
            config.quiet = true;
            continue;
        }

        if (arg == "--json") {
            config.json = true;
            // JSON implies quiet — no human text mixed with the JSON object.
            config.quiet = true;
            continue;
        }

        if (arg == "--list-modes") {
            config.list_modes = true;
            continue;
        }

        if (arg == "--depfile" && i + 1 < argc) {
            config.depfile = std::string(argv[++i]);
            continue;
        }

        if (arg == "--no-scale") {
            config.no_scale = true;
            continue;
        }

        // --layout interleaved|standard. Also --non-interleaved as a
        // shortcut for the plane-sequential variant most paint programs
        // and bootblocks expect.
        if (arg == "--layout" && i + 1 < argc) {
            std::string_view v = argv[++i];
            if (v == "interleaved")
                config.layout_override = bitplane::Layout::interleaved;
            else if (v == "standard" || v == "non-interleaved" ||
                     v == "planar" || v == "plane-sequential")
                config.layout_override = bitplane::Layout::standard;
            else if (v == "word-interleaved" || v == "atari")
                config.layout_override = bitplane::Layout::word_interleaved;
            else if (v != "auto") {
                return std::unexpected{Error{ErrorCode::invalid_dimensions,
                    std::format("Unknown --layout '{}' (use interleaved, "
                                "standard, word-interleaved, or auto)", v)}};
            }
            continue;
        }
        if (arg == "--non-interleaved" || arg == "--planar") {
            config.layout_override = bitplane::Layout::standard;
            continue;
        }
        if (arg == "--interleaved") {
            config.layout_override = bitplane::Layout::interleaved;
            continue;
        }

        if (arg == "--native-par") {
            config.native_par = true;
            continue;
        }

        if (arg == "--dual-playfield" || arg == "--dpf") {
            config.dual_playfield = true;
            continue;
        }

        if (arg.starts_with("--scap-probe=")) {
            config.scap_probe = std::string(arg.substr(13));
            continue;
        }
        if (arg == "--scap-probe" && i + 1 < argc) {
            config.scap_probe = std::string(argv[++i]);
            continue;
        }
        if (arg == "--scap") {
            config.scap = true;
            // SCAP defaults to DPF (the production mode). The lores 5bpp
            // investigation path needs --depth 5 + no --dpf — let that
            // through by NOT auto-enabling DPF here. The post-parse fixup
            // below auto-enables DPF only when the user didn't pick a
            // 5bpp lores configuration.
            continue;
        }
        if (arg == "--scap-debug") {
            config.scap_debug = true;
            continue;
        }

        if (arg == "--mask-invert") {
            config.mask_invert = true;
            continue;
        }

        if (arg == "--cga-palette" && i + 1 < argc) {
            auto v = std::string_view(argv[++i]);
            if      (v == "0-low"  || v == "p0-low")  config.cga_palette = 0;
            else if (v == "0-high" || v == "p0-high") config.cga_palette = 1;
            else if (v == "1-low"  || v == "p1-low")  config.cga_palette = 2;
            else if (v == "1-high" || v == "p1-high") config.cga_palette = 3;
            else if (v == "auto") { config.cga_auto_palette = true; continue; }
            else return std::unexpected{Error{ErrorCode::unsupported_mode,
                std::format("Unknown CGA palette: {}", v)}};
            config.cga_auto_palette = false;
            continue;
        }
        if (arg == "--cga-bg" && i + 1 < argc) {
            auto d = std::atoi(argv[++i]);
            if (d < 0 || d > 15) return std::unexpected{Error{
                ErrorCode::unsupported_mode,
                "--cga-bg must be 0..15 (master palette index)"}};
            config.cga_bg = d;
            continue;
        }

        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            config.output_path = std::string(argv[++i]);
            continue;
        }

        // --lock-index <id> <rgbhex>: lock palette slot to a specific color.
        if (arg == "--lock-index" && i + 2 < argc) {
            int idx = std::atoi(argv[++i]);
            if (idx < 0 || idx > 255) {
                return std::unexpected{Error{ErrorCode::invalid_depth,
                    "--lock-index id must be 0..255"}};
            }
            auto hex = std::string(argv[++i]);
            // Strip leading '#' or '0x'/'0X'
            if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
            if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
                hex.erase(0, 2);
            // Expand 3-digit hex to 6 (e.g. "f0c" -> "ff00cc")
            if (hex.size() == 3) {
                std::string expanded;
                for (char c : hex) { expanded.push_back(c); expanded.push_back(c); }
                hex = expanded;
            }
            if (hex.size() != 6) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--lock-index color must be a 3- or 6-digit hex value (e.g. f0c or ff00cc)"}};
            }
            auto parse_byte = [](std::string_view s) -> int {
                int v = 0;
                for (char c : s) {
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= (c - '0');
                    else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
                    else return -1;
                }
                return v;
            };
            int r = parse_byte(std::string_view(hex).substr(0, 2));
            int g = parse_byte(std::string_view(hex).substr(2, 2));
            int b = parse_byte(std::string_view(hex).substr(4, 2));
            if (r < 0 || g < 0 || b < 0) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--lock-index color must be a hex value (e.g. ff00cc)"}};
            }
            config.locks.push_back({idx, r, g, b});
            continue;
        }

        // --pin-index-at <id> <x> <y>: pin palette slot to source pixel.
        if (arg == "--pin-index-at" && i + 3 < argc) {
            int idx = std::atoi(argv[++i]);
            int x = std::atoi(argv[++i]);
            int y = std::atoi(argv[++i]);
            if (idx < 0 || idx > 255 || x < 0 || y < 0) {
                return std::unexpected{Error{ErrorCode::invalid_depth,
                    "--pin-index-at id must be 0..255 and x/y >= 0"}};
            }
            config.pins.push_back({idx, x, y});
            continue;
        }

        // (--alpha-dither moved to value-args below)

        if (arg.starts_with("--") && i + 1 < argc) {
            auto val = std::string_view(argv[++i]);

            if (arg == "--mode") {
                auto v = std::string(val);
                // Decompose compound modes: extract base mode + hires/lace flags
                bool mode_hires = v.find("hires") != std::string::npos;
                bool mode_lace = v.size() > 4 && v.find("-lace") != std::string::npos;
                if (v == "lores") config.mode = amiga::Mode::lores;
                else if (v == "lores-lace") config.mode = amiga::Mode::lores_interlace;
                else if (v == "hires") config.mode = amiga::Mode::hires;
                else if (v == "hires-lace") config.mode = amiga::Mode::hires_interlace;
                else if (v.starts_with("ham6")) config.mode = amiga::Mode::ham6;
                else if (v.starts_with("ham8")) config.mode = amiga::Mode::ham8;
                else if (v.starts_with("ehb")) config.mode = amiga::Mode::ehb;
                else if (v == "stf-low") config.mode = amiga::Mode::stf_low;
                else if (v == "stf-med") config.mode = amiga::Mode::stf_med;
                else if (v == "ste-low") config.mode = amiga::Mode::ste_low;
                else if (v == "ste-med") config.mode = amiga::Mode::ste_med;
                else if (v == "stf-hi") config.mode = amiga::Mode::stf_hi;
                else if (v == "ste-hi") config.mode = amiga::Mode::ste_hi;
                else if (v == "vga" || v == "vga-13h")
                    config.mode = amiga::Mode::vga_13h;
                else if (v == "vga-10h" || v == "vga-640x350")
                    config.mode = amiga::Mode::vga_10h;
                else if (v == "vga-12h" || v == "vga-hires" ||
                         v == "vga-640x480")
                    config.mode = amiga::Mode::vga_12h;
                else if (v == "ega" || v == "ega-320")
                    config.mode = amiga::Mode::ega_320;
                else if (v == "ega-640")
                    config.mode = amiga::Mode::ega_640;
                else if (v == "ega-hi" || v == "ega-640x350")
                    config.mode = amiga::Mode::ega_hi;
                else if (v == "cga" || v == "cga-320")
                    config.mode = amiga::Mode::cga_320;
                else if (v == "cga-640" || v == "cga-mono")
                    config.mode = amiga::Mode::cga_640;
                else if (v == "cga-composite" || v == "cga-comp" ||
                         v == "cga-16")
                    config.mode = amiga::Mode::cga_composite;
                else if (v == "cga-text80x100" || v == "cga-text-1k" ||
                         v == "cga-80x100")
                    config.mode = amiga::Mode::cga_text80x100;
                else return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown mode: " + v}};
                // Apply compound mode overrides + set flags from built-in modes
                if (mode_hires) { config.hires = true; if (!config.width) config.width = 640; }
                auto mp = amiga::get_mode_params(config.mode);
                config.interlace = mode_lace || mp.is_interlaced;
                // config.hires drives Amiga-style preview vertical doubling
                // (Amiga hires has 1:2 pixels). Don't set it for DOS modes —
                // they use their own `par` for preview aspect and shouldn't
                // go through Amiga's integer-scale preview path.
                bool dos_mode = amiga::is_vga(config.mode) ||
                                amiga::is_ega(config.mode) ||
                                amiga::is_cga(config.mode);
                if (!dos_mode) config.hires = config.hires || mp.is_hires;
            }
            else if (arg == "--depth") {
                int d = std::atoi(std::string(val).c_str());
                if (d < 1 || d > 8) {
                    return std::unexpected{Error{ErrorCode::invalid_depth,
                        "--depth must be 1..8"}};
                }
                config.depth = static_cast<std::size_t>(d);
                config.depth_explicit = true;
            }
            else if (arg == "--chipset") {
                if (val == "ocs") config.chipset = amiga::Chipset::ocs;
                else if (val == "aga") config.chipset = amiga::Chipset::aga;
                else return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown chipset: " + std::string(val) + " (use ocs or aga)"}};
            }
            else if (arg == "--ham-beam") {
                config.ham_beam = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
                if (config.ham_beam < 1) config.ham_beam = 1;
                if (config.ham_beam > 256) config.ham_beam = 256;
            }
            else if (arg == "--ham-triple") {
                config.ham_triple = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
                if (config.ham_triple > 256) config.ham_triple = 256;
            }
            else if (arg == "--palette-diversity") {
                config.palette_diversity = std::atoi(std::string(val).c_str());
                if (config.palette_diversity < 0) config.palette_diversity = 0;
                if (config.palette_diversity > 9) config.palette_diversity = 9;
            }
            else if (arg == "--quantizer") {
                config.quantizer = std::string(val);
                if (config.quantizer != "" && config.quantizer != "auto" &&
                    config.quantizer != "median-cut" &&
                    config.quantizer != "ocs-bruteforce" &&
                    config.quantizer != "pnn") {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Unknown quantizer: " + config.quantizer +
                        " (use auto, median-cut, ocs-bruteforce, pnn)"}};
                }
            }
            else if (arg == "--cap-changes" || arg == "--copper-changes") {
                // --copper-changes kept as legacy alias for --cap-changes.
                config.copper_changes = std::atoi(std::string(val).c_str());
            }
            else if (arg == "--weight-l") {
                color_space::WEIGHT_L = std::stof(std::string(val));
            }
            else if (arg == "--weight-a") {
                color_space::WEIGHT_A = std::stof(std::string(val));
            }
            else if (arg == "--weight-b") {
                color_space::WEIGHT_B = std::stof(std::string(val));
            }
            else if (arg == "--dither") {
                auto m = parse_dither_method(val);
                if (!m) return std::unexpected{m.error()};
                config.dither_method = *m;
                config.dither_explicit = true;
            }
            else if (arg == "--dither-strength") {
                config.dither_strength = std::stof(std::string(val));
                config.dither_strength_explicit = true;
            }
            else if (arg == "--error-clamp") {
                config.error_clamp = std::stof(std::string(val));
                config.error_clamp_explicit = true;
            }
            else if (arg == "--cga-text-metric") {
                config.cga_text_metric = std::string(val);
            }
            else if (arg == "--refine") {
                config.refine_iterations = std::stoi(std::string(val));
            }
            else if (arg == "--brightness") {
                config.preprocess.brightness = std::stof(std::string(val));
            }
            else if (arg == "--contrast") {
                config.preprocess.contrast = std::stof(std::string(val));
            }
            else if (arg == "--saturation") {
                config.preprocess.saturation = std::stof(std::string(val));
            }
            else if (arg == "--gamma") {
                config.preprocess.gamma = std::stof(std::string(val));
            }
            else if (arg == "--hue-shift") {
                config.preprocess.hue_shift = std::stof(std::string(val));
            }
            else if (arg == "--sharpen") {
                config.preprocess.sharpen = std::stof(std::string(val));
            }
            else if (arg == "--black-point") {
                config.preprocess.black_point = std::stof(std::string(val));
            }
            else if (arg == "--white-point") {
                config.preprocess.white_point = std::stof(std::string(val));
            }
            else if (arg == "--width") {
                int v = std::atoi(std::string(val).c_str());
                if (v <= 0 || v > 32768) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "--width must be 1..32768"}};
                }
                config.width = static_cast<std::size_t>(v);
            }
            else if (arg == "--height") {
                int v = std::atoi(std::string(val).c_str());
                if (v <= 0 || v > 32768) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "--height must be 1..32768"}};
                }
                config.height = static_cast<std::size_t>(v);
            }
            else if (arg == "--symbol") {
                config.symbol_name = std::string(val);
            }
            else if (arg == "--alpha-threshold") {
                config.alpha_threshold = std::stof(std::string(val));
            }
            else if (arg == "--alpha-dither") {
                auto m = parse_dither_method(val);
                if (!m) return std::unexpected{m.error()};
                config.alpha_dither = *m;
            }
            else if (arg == "--alpha-dither-strength") {
                config.alpha_dither_strength = std::stof(std::string(val));
            }
            else if (arg == "--palette") {
                config.palette_file = std::string(val);
            }
            else if (arg == "--mask") {
                config.mask_path = std::string(val);
            }
            else if (arg == "--crop") {
                // Parse "x,y,w,h" format
                auto s = std::string(val);
                int parsed[4]{};
                int idx = 0;
                std::size_t pos = 0;
                while (pos < s.size() && idx < 4) {
                    auto comma = s.find(',', pos);
                    auto token = s.substr(pos, comma - pos);
                    parsed[idx++] = std::atoi(token.c_str());
                    pos = (comma == std::string::npos) ? s.size() : comma + 1;
                }
                if (idx != 4) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Crop format must be x,y,w,h (e.g. 10,20,300,200)"}};
                }
                if (parsed[0] < 0 || parsed[1] < 0 ||
                    parsed[2] <= 0 || parsed[3] <= 0) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Crop x/y must be >= 0 and w/h must be > 0"}};
                }
                config.crop_x = parsed[0];
                config.crop_y = parsed[1];
                config.crop_w = parsed[2];
                config.crop_h = parsed[3];
            }
            else {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown option: " + std::string(arg)}};
            }
        }
        else if (arg.starts_with("--")) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "Unknown option: " + std::string(arg)}};
        }
        else {
            if (positional == 0) config.input_path = std::string(arg);
            else if (positional == 1) config.output_path = std::string(arg);
            ++positional;
        }
    }

    if (config.input_path.empty() && config.scap_probe.empty() &&
        !config.list_modes) {
        print_usage();
        std::exit(exit_code::usage);
    }

    // SCAP post-parse fixup. Three supported configurations:
    //   * DPF: --scap --dpf, OCS lores depth=3, 8 PF2 colours.
    //   * EHB: --scap --mode ehb, 32 base + 32 hardware half-brite.
    // No auto-promotion — the user must opt into one of the supported
    // SCAP regimes explicitly. The CLI block below errors out otherwise.

    return config;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    return s.substr(s.size() - suffix.size()) == suffix;
}

// Derive a C symbol name from a filename path
std::string derive_symbol_name(std::string_view path) {
    // Extract filename without directory
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos)
        path = path.substr(slash + 1);
    auto backslash = path.rfind('\\');
    if (backslash != std::string_view::npos)
        path = path.substr(backslash + 1);

    // Remove extension
    auto dot = path.rfind('.');
    if (dot != std::string_view::npos)
        path = path.substr(0, dot);

    // Sanitize: replace non-alphanumeric with underscore
    std::string result;
    result.reserve(path.size());
    for (auto c : path) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty()) return "image";
    if (std::isdigit(static_cast<unsigned char>(result[0])))
        result.insert(result.begin(), '_');
    return result;
}

// Crop an image to a sub-region
Result<Image> crop_image(const Image& src,
                         std::size_t cx, std::size_t cy,
                         std::size_t cw, std::size_t ch) {
    if (cx + cw > src.width() || cy + ch > src.height() || cw == 0 || ch == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Crop region {}x{}+{}+{} exceeds image {}x{}",
                        cw, ch, cx, cy, src.width(), src.height()),
        }};
    }
    Image dst(cw, ch);
    for (std::size_t y = 0; y < ch; ++y) {
        for (std::size_t x = 0; x < cw; ++x) {
            dst[x, y] = src[cx + x, cy + y];
        }
    }
    return dst;
}

// Auto-crop region selection is inlined into the pipeline so the
// transparency mask can be sampled from the same region.

// ---------------------------------------------------------------------------
// iTerm2 inline image display
// ---------------------------------------------------------------------------

constexpr auto base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        auto a = data[i], b = data[i + 1], c = data[i + 2];
        out += base64_chars[(a >> 2) & 0x3F];
        out += base64_chars[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
        out += base64_chars[((b & 0x0F) << 2) | ((c >> 6) & 0x03)];
        out += base64_chars[c & 0x3F];
        i += 3;
    }
    if (i < data.size()) {
        auto a = data[i];
        out += base64_chars[(a >> 2) & 0x3F];
        if (i + 1 < data.size()) {
            auto b = data[i + 1];
            out += base64_chars[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
            out += base64_chars[((b & 0x0F) << 2)];
        } else {
            out += base64_chars[((a & 0x03) << 4)];
            out += '=';
        }
        out += '=';
    }
    return out;
}

void iterm2_display(const Image& image, unsigned scale = 2) {
    auto png = png_io::encode(image);
    if (!png) return;
    auto encoded = base64_encode(*png);
    cli_status("\033]1337;File=inline=1;size={};width={}px;height={}px:{}\a",
                 png->size(), image.width() * scale, image.height() * scale,
                 encoded);
}

const char* dither_name(dither::Method m) {
    switch (m) {
    case dither::Method::none: return "none";
    case dither::Method::bayer2x2: return "bayer2x2";
    case dither::Method::bayer4x4: return "bayer4x4";
    case dither::Method::bayer8x8: return "bayer8x8";
    case dither::Method::checker: return "checker";
    case dither::Method::h2x4: return "h2x4";
    case dither::Method::clustered_dot: return "clustered-dot";
    case dither::Method::line2: return "line2";
    case dither::Method::vline2: return "vline2";
    case dither::Method::vline_checker: return "vline-checker";
    case dither::Method::vline4: return "vline4";
    case dither::Method::vline8: return "vline8";
    case dither::Method::line_checker: return "line-checker";
    case dither::Method::line4: return "line4";
    case dither::Method::v4x2: return "v4x2";
    case dither::Method::bayer4x2: return "bayer4x2";
    case dither::Method::bayer2x4: return "bayer2x4";
    case dither::Method::line8: return "line8";
    case dither::Method::halftone8x8: return "halftone8x8";
    case dither::Method::diagonal8x8: return "diagonal8x8";
    case dither::Method::spiral5x5: return "spiral5x5";
    case dither::Method::hex8x8: return "hex8x8";
    case dither::Method::hex5x5: return "hex5x5";
    case dither::Method::blue_noise: return "blue-noise";
    case dither::Method::floyd_steinberg: return "floyd-steinberg";
    case dither::Method::atkinson: return "atkinson";
    case dither::Method::sierra_lite: return "sierra-lite";
    case dither::Method::stucki: return "stucki";
    case dither::Method::jarvis: return "jarvis";
    case dither::Method::ostromoukhov: return "ostromoukhov";
    case dither::Method::gilbert: return "gilbert";
    case dither::Method::ign: return "ign";
    case dither::Method::white_noise: return "white-noise";
    case dither::Method::r2_sequence: return "r2";
    case dither::Method::crosshatch: return "crosshatch";
    case dither::Method::radial: return "radial";
    case dither::Method::value_noise: return "value-noise";
    }
    return "unknown";
}

// Determine effective chipset: auto-detect from mode if not specified.
// Modes requiring >6 bitplanes (HAM8) force AGA regardless.
// All other modes default to OCS but can be overridden with --chipset aga
// (e.g., HAM6 on AGA gives 24-bit base palette precision).
amiga::Chipset effective_chipset(const Config& cfg) {
    auto params = amiga::get_mode_params(cfg.mode);
    bool requires_aga = params.bitplane_depth > 6;
    if (requires_aga) return amiga::Chipset::aga;
    if (cfg.chipset.has_value()) return *cfg.chipset;
    return amiga::Chipset::ocs;
}

// Quantize palette: OCS uses brute-force, AGA uses median-cut
// Build a K-color palette from a discrete 64-entry EGA histogram using
// k-means++-style weighted seeding, then one round of weighted-mean refine
// with snap-back to the gamut. Guarantees K distinct EGA entries (as long
// as the image actually contains K distinct EGA colors after snap — usually
// true for anything richer than a logo).
//
// Much better than running continuous median-cut then snapping: the
// continuous centroids of clustered pixels often collapse onto the same
// EGA entry, wasting slots. This algorithm picks directly in gamut space.
Palette ega_histogram_quantize(const Image& image, std::size_t K) {
    return quantize::ega_histogram(image, K);
}

// Original inline implementation kept here for reference only. The live
// version lives in quantize.cpp so both main.cpp and api.cpp can use it.
#if 0
Palette ega_histogram_quantize_inline(const Image& image, std::size_t K) {
    std::array<std::uint64_t, 64> hist{};
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            auto e = palette::linear_to_ega(image[x, y]);
            hist[e]++;
        }
    }
    // Precompute OKLab for each of the 64 gamut entries (for perceptual d²).
    std::array<color_space::OKLab, 64> gamut_lab;
    std::array<Color3f, 64> gamut_rgb;
    for (std::size_t i = 0; i < 64; ++i) {
        gamut_rgb[i] = palette::ega_to_linear(static_cast<std::uint8_t>(i));
        gamut_lab[i] = color_space::linear_to_oklab(gamut_rgb[i]);
    }

    std::vector<std::uint8_t> picked;
    picked.reserve(K);

    // Seed 1: highest-frequency non-zero bucket.
    {
        std::uint64_t best_count = 0; std::uint8_t best = 0;
        for (std::size_t i = 0; i < 64; ++i) if (hist[i] > best_count) {
            best_count = hist[i]; best = static_cast<std::uint8_t>(i);
        }
        picked.push_back(best);
    }

    // Seed 2..K: weighted by (count × min_d²_to_existing_picks).
    // This is the k-means++ seeding rule adapted for weighted histograms.
    while (picked.size() < K) {
        std::array<double, 64> score{};
        double total = 0;
        for (std::size_t i = 0; i < 64; ++i) {
            if (hist[i] == 0) continue;
            double min_d = std::numeric_limits<double>::infinity();
            for (auto p : picked) {
                if (p == i) { min_d = 0; break; }
                auto& a = gamut_lab[i]; auto& b = gamut_lab[p];
                double dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                double d = dL*dL + da*da + db*db;
                if (d < min_d) min_d = d;
            }
            score[i] = static_cast<double>(hist[i]) * min_d;
            total += score[i];
        }
        if (total <= 0) break;  // no more distinct colors to pick
        // Deterministic: pick max-score bucket.
        std::uint8_t best = 0; double best_s = -1;
        for (std::size_t i = 0; i < 64; ++i) if (score[i] > best_s) {
            best_s = score[i]; best = static_cast<std::uint8_t>(i);
        }
        picked.push_back(best);
    }

    // Lloyd refinement in EGA space: iteratively reassign image pixels to
    // nearest picked slot, then replace each slot with the EGA gamut entry
    // that best serves its assigned pixels (weighted by pixel count). This
    // pulls seeds inward from outliers onto clusters that actually have mass.
    constexpr int kMaxIters = 16;
    for (int iter = 0; iter < kMaxIters; ++iter) {
        // Per-slot pixel counts and OKLab centroids.
        struct Acc { double L{}, a{}, b{}; double w{}; };
        std::vector<Acc> acc(picked.size());
        // Assign each non-empty bucket to nearest picked slot.
        for (std::size_t i = 0; i < 64; ++i) {
            if (hist[i] == 0) continue;
            float best_d = std::numeric_limits<float>::infinity();
            std::size_t best_k = 0;
            for (std::size_t k = 0; k < picked.size(); ++k) {
                auto& a = gamut_lab[i]; auto& b = gamut_lab[picked[k]];
                float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                float d = dL*dL + da*da + db*db;
                if (d < best_d) { best_d = d; best_k = k; }
            }
            auto w = static_cast<double>(hist[i]);
            acc[best_k].L += static_cast<double>(gamut_lab[i].L) * w;
            acc[best_k].a += static_cast<double>(gamut_lab[i].a) * w;
            acc[best_k].b += static_cast<double>(gamut_lab[i].b) * w;
            acc[best_k].w += w;
        }
        // For each slot with mass, find the unused EGA gamut entry closest
        // to the centroid in oklab. "Unused" here means not already picked
        // by another slot in this iteration — keeps slots distinct.
        std::vector<std::uint8_t> new_picked(picked.size());
        std::array<bool, 64> taken{};
        bool changed = false;
        // Process slots with most mass first so popular clusters get their
        // ideal gamut entry; empty clusters get leftovers.
        std::vector<std::size_t> order(picked.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](auto a, auto b) { return acc[a].w > acc[b].w; });
        for (auto k : order) {
            if (acc[k].w == 0) {
                // No mass — keep original pick but only if still available.
                if (!taken[picked[k]]) {
                    new_picked[k] = picked[k]; taken[picked[k]] = true;
                    continue;
                }
            }
            auto cent = (acc[k].w > 0)
                ? color_space::OKLab{
                      static_cast<float>(acc[k].L / acc[k].w),
                      static_cast<float>(acc[k].a / acc[k].w),
                      static_cast<float>(acc[k].b / acc[k].w)}
                : gamut_lab[picked[k]];
            std::uint8_t best = 0; float best_d = std::numeric_limits<float>::infinity();
            for (std::size_t g = 0; g < 64; ++g) {
                if (taken[g]) continue;
                auto& gl = gamut_lab[g];
                float dL = cent.L - gl.L, da = cent.a - gl.a, db = cent.b - gl.b;
                float d = dL*dL + da*da + db*db;
                if (d < best_d) { best_d = d; best = static_cast<std::uint8_t>(g); }
            }
            new_picked[k] = best;
            taken[best] = true;
            if (best != picked[k]) changed = true;
        }
        picked = new_picked;
        if (!changed) break;
    }

    Palette pal;
    pal.name = "ega";
    pal.colors.reserve(picked.size());
    for (auto p : picked) pal.colors.push_back(gamut_rgb[p]);
    return pal;
}
#endif

Result<Palette> auto_quantize(const Image& image, std::size_t max_colors,
                              amiga::Chipset chipset,
                              int palette_diversity = 0,
                              std::string_view quantizer = {},
                              amiga::Mode mode = amiga::Mode::lores) {
    // EGA palette selection:
    //
    //   Mode 10h (ega-hi, 640×350 @ 21.85 kHz): the IBM EGA card enables
    //     all 6 ATC output pins and the 5154 ECD reads all of them —
    //     16 colors chosen from the full 64-entry IrgbIRGB gamut via
    //     the dedicated histogram quantizer.
    //
    //   Modes 0Dh / 0Eh (ega-320 / ega-640 @ 15.75 kHz): CGA-compat.
    //     The IBM EGA card gates off the r' (secondary red, bit 5) and
    //     b' (secondary blue, bit 3) pins in this output mode; the 5154
    //     only reads RGB primary + g' as intensity. Empirically on
    //     86Box + real-IBM-EGA emulation, image-picked palette values
    //     with bit 5 or 3 set get those bits zeroed before reaching the
    //     monitor, so 64-gamut colors collapse onto a red-heavy subset.
    //     → Restrict to the 16 IRGB colors (kCgaHw); the encoder then
    //     only picks values where bits 5 and 3 happen to be zero, which
    //     the 5154 displays correctly in 200-line mode.
    if (amiga::is_ega(mode) && quantizer != "median-cut" && quantizer != "pnn") {
        if (mode == amiga::Mode::ega_320 || mode == amiga::Mode::ega_640) {
            Palette pal;
            pal.name = "ega-irgb16";
            pal.colors.reserve(16);
            for (auto hex : palette::kCgaHw) {
                pal.colors.push_back(
                    color_space::srgb_hex_to_linear(hex));
            }
            return pal;
        }
        return ega_histogram_quantize(image, max_colors);
    }
    // PNN with chipset-aware snap: OCS → OCS-snapped PNN, AGA → continuous.
    if (quantizer == "pnn") {
        auto pal = quantize::pnn_quantize(image.pixels(), max_colors,
                                          palette_diversity,
                                          /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
        return pal;
    }
    quantize::Algorithm algo;
    if (quantizer == "median-cut") {
        algo = quantize::Algorithm::median_cut;
    } else if (quantizer == "ocs-bruteforce") {
        algo = quantize::Algorithm::ocs_bruteforce;
    } else {
        // Auto: OCS brute-force is tuned for the 4096-color OCS gamut;
        // EGA/VGA have different gamuts and benefit from median-cut's
        // gamut-agnostic clustering. AGA has continuous color so median-cut too.
        bool use_median = chipset == amiga::Chipset::aga ||
                          amiga::is_ega(mode) ||
                          amiga::is_vga(mode);
        algo = use_median
            ? quantize::Algorithm::median_cut
            : quantize::Algorithm::ocs_bruteforce;
    }
    return quantize::quantize(image, max_colors, algo, palette_diversity);
}

// Snap palette to chipset/mode precision
void snap_palette(Palette& pal, amiga::Chipset chipset, amiga::Mode mode) {
    if (amiga::is_stf(mode)) {
        for (auto& c : pal.colors)
            c = palette::quantize_to_stf(c);
    } else if (amiga::is_vga(mode)) {
        for (auto& c : pal.colors)
            c = palette::quantize_to_vga(c);
    } else if (amiga::is_ega(mode)) {
        for (auto& c : pal.colors)
            c = palette::quantize_to_ega(c);
    } else if (chipset != amiga::Chipset::aga) {
        for (auto& c : pal.colors)
            c = palette::quantize_to_ocs(c);
    }
}

// Scale preview image for correct pixel aspect ratio.
// hires: double vertically (tall pixels). lores_interlace: double horizontally.
Image scale_preview(const Image& src, std::size_t sx, std::size_t sy) {
    if (sx == 1 && sy == 1) return src;
    auto dw = src.width() * sx;
    auto dh = src.height() * sy;
    Image dst(dw, dh);
    for (std::size_t y = 0; y < dh; ++y) {
        for (std::size_t x = 0; x < dw; ++x) {
            dst[x, y] = src[x / sx, y / sy];
        }
    }
    return dst;
}

// Nearest-neighbor resample to arbitrary non-integer dimensions. Preserves
// the pixel-art look while letting us apply non-integer PAR corrections
// (e.g. VGA 13h's 5:6 ratio, EGA-hi's 7:10). `dst_w`/`dst_h` give the
// exact output dimensions.
Image scale_preview_nn(const Image& src, std::size_t dst_w, std::size_t dst_h) {
    Image dst(dst_w, dst_h);
    if (dst_w == 0 || dst_h == 0) return dst;
    auto sw = src.width();
    auto sh = src.height();
    for (std::size_t y = 0; y < dst_h; ++y) {
        auto sy = std::min(sh - 1, (y * sh) / dst_h);
        for (std::size_t x = 0; x < dst_w; ++x) {
            auto sx = std::min(sw - 1, (x * sw) / dst_w);
            dst[x, y] = src[sx, sy];
        }
    }
    return dst;
}

// Compute preview dimensions for a PAR-aware mode: scale horizontally by
// `base_scale` (typically 2 for pixel art detail, 1 for already-large
// buffers like 640x200 text modes), then compute vertical so that the
// PNG matches the CRT's display aspect.
//   display_aspect = W × par / H
//   want preview_w / preview_h = display_aspect
std::pair<std::size_t, std::size_t>
preview_dims_for_par(std::size_t w, std::size_t h, double par,
                     std::size_t base_scale = 2) {
    if (par <= 0 || w == 0 || h == 0) return {w, h};
    auto preview_w = w * base_scale;
    auto preview_h = static_cast<std::size_t>(std::lround(
        static_cast<double>(preview_w) * static_cast<double>(h) /
        (static_cast<double>(w) * par)));
    if (preview_h == 0) preview_h = h;
    return {preview_w, preview_h};
}

// Scale transparency mask for preview
std::vector<bool> scale_mask(const std::vector<bool>& mask,
                             std::size_t w, std::size_t h,
                             std::size_t sx, std::size_t sy) {
    if (sx == 1 && sy == 1) return mask;
    auto dw = w * sx;
    auto dh = h * sy;
    std::vector<bool> dst(dw * dh);
    for (std::size_t y = 0; y < dh; ++y) {
        for (std::size_t x = 0; x < dw; ++x) {
            dst[y * dw + x] = mask[(y / sy) * w + (x / sx)];
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// Save transparency mask to file (PNG, IFF, or raw 1-bitplane)
// ---------------------------------------------------------------------------

void save_mask(std::string_view path, const std::vector<bool>& tmask,
               std::size_t w, std::size_t h, bool invert, bool interlace) {
    if (tmask.empty()) {
        std::println(stderr, "Mask: no transparency in source image");
        return;
    }

    // Build B/W image from mask
    // Default: white (1) = opaque, black (0) = transparent
    // Inverted: white (1) = transparent, black (0) = opaque
    auto build_image = [&]() {
        Image img(w, h);
        for (std::size_t i = 0; i < w * h; ++i) {
            bool transparent = (i < tmask.size()) && tmask[i];
            bool white = invert ? transparent : !transparent;
            float v = white ? 1.0f : 0.0f;
            img.pixels()[i] = Color3f{v, v, v};
        }
        return img;
    };

    auto build_indices = [&]() {
        std::vector<std::uint8_t> indices(w * h, 0);
        for (std::size_t i = 0; i < w * h; ++i) {
            bool transparent = (i < tmask.size()) && tmask[i];
            bool set = invert ? transparent : !transparent;
            indices[i] = set ? 1 : 0;
        }
        return indices;
    };

    auto path_str = std::string(path);

    if (ends_with(path, ".png")) {
        auto img = build_image();
        auto result = png_io::save(path, img);
        if (!result) {
            std::println(stderr, "Mask PNG write error: {}", result.error().message);
            return;
        }
        cli_status("Mask:   {} ({}x{} PNG)", path, w, h);
    } else if (ends_with(path, ".iff") || ends_with(path, ".ilbm")) {
        auto indices = build_indices();
        auto planes = bitplane::encode(indices, w, h, 1);
        if (!planes) {
            std::println(stderr, "Mask encode error: {}", planes.error().message);
            return;
        }
        std::vector<Color3f> mask_palette = {
            Color3f{0.0f, 0.0f, 0.0f},
            Color3f{1.0f, 1.0f, 1.0f},
        };
        iff::IffOptions iff_opts;
        iff_opts.interlace = interlace;
        auto result = iff::save_ilbm(path, *planes, mask_palette,
                                     amiga::Mode::lores, iff_opts);
        if (!result) {
            std::println(stderr, "Mask IFF write error: {}", result.error().message);
            return;
        }
        cli_status("Mask:   {} ({}x{} IFF, 1 bitplane)", path, w, h);
    } else if (ends_with(path, ".raw")) {
        auto indices = build_indices();
        auto planes = bitplane::encode(indices, w, h, 1);
        if (!planes) {
            std::println(stderr, "Mask encode error: {}", planes.error().message);
            return;
        }
        std::ofstream file(path_str, std::ios::binary);
        if (!file) {
            std::println(stderr, "Mask: failed to open {}", path);
            return;
        }
        file.write(reinterpret_cast<const char*>(planes->data.data()),
                   static_cast<std::streamsize>(planes->data.size()));
        cli_status("Mask:   {} ({}x{} raw, {} bytes)", path, w, h,
                     planes->data.size());
    } else {
        std::println(stderr, "Mask: unsupported extension for '{}' (use .png, .iff, or .raw)", path);
    }
}

// Save preview PNG with pixel aspect scaling and optional transparency
// Display preview in terminal and optionally save to file
// Save raw bitplane data + palette + copper changes to a binary file.
void save_raw(std::string_view path,
              const bitplane::BitplaneData& planes,
              std::span<const Color3f> palette,
              amiga::Chipset chipset,
              const std::vector<std::vector<copper::CopperChange>>* copper = nullptr,
              std::size_t cpl = 0) {
    bool aga = (chipset == amiga::Chipset::aga);
    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) { std::println(stderr, "Failed to open: {}", path_str); return; }

    // Bitplanes
    file.write(reinterpret_cast<const char*>(planes.data.data()),
               static_cast<std::streamsize>(planes.data.size()));

    // Palette (big-endian 0x0RGB)
    for (auto& c : palette) {
        auto hi = aga ? palette::linear_to_aga_hilo(c).hi : palette::linear_to_ocs(c);
        auto buf = std::array<std::uint8_t, 2>{
            static_cast<std::uint8_t>(hi >> 8), static_cast<std::uint8_t>(hi & 0xFF)};
        file.write(reinterpret_cast<const char*>(buf.data()), 2);
    }
    if (aga) {
        for (auto& c : palette) {
            auto lo = palette::linear_to_aga_hilo(c).lo;
            auto buf = std::array<std::uint8_t, 2>{
                static_cast<std::uint8_t>(lo >> 8), static_cast<std::uint8_t>(lo & 0xFF)};
            file.write(reinterpret_cast<const char*>(buf.data()), 2);
        }
    }

    // Copper changes (UWORD reg + UWORD color per entry, cpl entries per line)
    if (copper && !copper->empty()) {
        for (auto& line : *copper) {
            for (std::size_t s = 0; s < cpl; ++s) {
                std::array<std::uint8_t, 4> buf;
                if (s < line.size()) {
                    auto hi = aga ? palette::linear_to_aga_hilo(line[s].color).hi
                                  : palette::linear_to_ocs(line[s].color);
                    buf = {0, line[s].reg,
                           static_cast<std::uint8_t>(hi >> 8),
                           static_cast<std::uint8_t>(hi & 0xFF)};
                } else {
                    buf = {0xFF, 0xFF, 0x00, 0x00};
                }
                file.write(reinterpret_cast<const char*>(buf.data()), 4);
            }
        }
        if (aga) {
            for (auto& line : *copper) {
                for (std::size_t s = 0; s < cpl; ++s) {
                    std::array<std::uint8_t, 4> buf;
                    if (s < line.size()) {
                        auto lo = palette::linear_to_aga_hilo(line[s].color).lo;
                        buf = {0, line[s].reg,
                               static_cast<std::uint8_t>(lo >> 8),
                               static_cast<std::uint8_t>(lo & 0xFF)};
                    } else {
                        buf = {0xFF, 0xFF, 0x00, 0x00};
                    }
                    file.write(reinterpret_cast<const char*>(buf.data()), 4);
                }
            }
        }
    }

    auto total = static_cast<std::size_t>(file.tellp());
    cli_status("Raw:    {} ({} bytes)", path, total);
}

// CGA raw output: hardware-layout memory dump (16384 bytes total).
// cga_320: 2bpp packed, 80 bytes/row × 200 rows. Two-bank memory layout:
//   0x0000..0x1F3F = even rows (0, 2, 4, ..., 198)
//   0x1F40..0x1FFF = 192 bytes padding
//   0x2000..0x3F3F = odd rows (1, 3, 5, ..., 199)
//   0x3F40..0x3FFF = 192 bytes padding
// cga_640: 1bpp packed, same banked layout. fg pixels are index != 0.
// cga_composite: logical 160x200 16-color image, each logical pixel encoded
//   as a 4-bit pattern across two adjacent 2bpp pixels in the 320x200 buffer.
//   The composite monitor's NTSC decoder interprets the pattern as one of
//   16 colors. Same banked layout as cga_320.
// This is the classic CGAPIC format; loads directly into 0xB8000 video RAM.
void save_raw_cga(std::string_view path,
                  std::span<const std::uint8_t> indices,
                  std::size_t width, std::size_t height,
                  amiga::Mode mode) {
    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) { std::println(stderr, "Failed to open: {}", path_str); return; }

    bool is_mono = (mode == amiga::Mode::cga_640);
    bool is_composite = (mode == amiga::Mode::cga_composite);
    // In composite mode `width` is the logical 160 wide; the packed buffer
    // stores 320 columns of 2bpp.
    auto buffer_width = is_composite ? 320 : width;
    auto row_bytes = buffer_width / (is_mono ? 8 : 4);

    std::vector<std::uint8_t> buf(16384, 0);

    for (std::size_t y = 0; y < height; ++y) {
        std::size_t bank_base = (y & 1) ? 0x2000u : 0x0000u;
        auto row_offset = bank_base + (y >> 1) * row_bytes;
        for (std::size_t bx = 0; bx < row_bytes; ++bx) {
            std::uint8_t byte = 0;
            if (is_mono) {
                // 8 pixels per byte, MSB first.
                for (std::size_t p = 0; p < 8; ++p) {
                    auto x = bx * 8 + p;
                    auto idx = indices[y * width + x];
                    byte = static_cast<std::uint8_t>(
                        (byte << 1) | (idx != 0 ? 1 : 0));
                }
            } else if (is_composite) {
                // Each byte holds 2 composite pixels (4 packed 2bpp pixels =
                // two 4-bit patterns). Each logical pixel's 0..15 color index
                // becomes a 4-bit pattern MSB-first in its half of the byte.
                auto p0 = palette::cga_composite_pattern(
                    indices[y * width + bx * 2]);
                auto p1 = palette::cga_composite_pattern(
                    indices[y * width + bx * 2 + 1]);
                byte = static_cast<std::uint8_t>((p0 << 4) | p1);
            } else {
                // 4 pixels per byte, 2bpp MSB first.
                for (std::size_t p = 0; p < 4; ++p) {
                    auto x = bx * 4 + p;
                    auto idx = indices[y * width + x] & 0x3;
                    byte = static_cast<std::uint8_t>(
                        (byte << 2) | idx);
                }
            }
            buf[row_offset + bx] = byte;
        }
    }
    file.write(reinterpret_cast<const char*>(buf.data()),
               static_cast<std::streamsize>(buf.size()));

    cli_status("Raw:    {} ({} bytes, CGA {})", path, buf.size(),
                 is_mono      ? "mono 640x200 (1bpp banked)"
               : is_composite ? "composite 160x200x16 (2bpp banked, NTSC artifact)"
                              : "320x200 (2bpp banked)");
}

// EGA raw output: 4-plane planar (IBM byte-per-row layout, planes interleaved
// at MSB-first bit packing — same as our Amiga non-interleaved BitplaneData)
// + EGA hardware palette (1 byte per color, IrgbIRGB 6-bit format). This
// matches what you'd DMA into EGA video RAM (0xA0000) with plane selects.
void save_raw_ega(std::string_view path,
                  const bitplane::BitplaneData& planes,
                  std::span<const Color3f> palette) {
    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) { std::println(stderr, "Failed to open: {}", path_str); return; }

    // Bitplane data (4 planes, standard MSB-first layout).
    file.write(reinterpret_cast<const char*>(planes.data.data()),
               static_cast<std::streamsize>(planes.data.size()));

    // EGA hardware palette (16 bytes, IrgbIRGB encoding, 6 bits each).
    for (auto& c : palette) {
        auto rrggbb = palette::linear_to_ega(c);
        auto hw_byte = palette::ega_to_hw(rrggbb);
        file.write(reinterpret_cast<const char*>(&hw_byte), 1);
    }

    auto total = static_cast<std::size_t>(file.tellp());
    cli_status("Raw:    {} ({} bytes, EGA 4-plane planar)", path, total);
}

// VGA hires planar raw output (Mode 10h / 12h): 4-plane MSB-first bitplanes
// + 16-color DAC palette (3 bytes each, 6-bit values). Same bitplane layout
// as save_raw_ega, but with VGA's programmable 18-bit DAC instead of the
// fixed EGA IrgbIRGB 6-bit encoding.
void save_raw_vga_planar(std::string_view path,
                         const bitplane::BitplaneData& planes,
                         std::span<const Color3f> palette,
                         amiga::Mode mode) {
    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) { std::println(stderr, "Failed to open: {}", path_str); return; }

    file.write(reinterpret_cast<const char*>(planes.data.data()),
               static_cast<std::streamsize>(planes.data.size()));

    // 16 × 3 bytes, each channel 0..63, ready for DAC writes at 0x3C9.
    for (auto& c : palette) {
        auto rgb18 = palette::linear_to_vga(c);
        std::array<std::uint8_t, 3> buf{
            static_cast<std::uint8_t>((rgb18 >> 16) & 0x3F),
            static_cast<std::uint8_t>((rgb18 >> 8) & 0x3F),
            static_cast<std::uint8_t>(rgb18 & 0x3F),
        };
        file.write(reinterpret_cast<const char*>(buf.data()), 3);
    }

    auto total = static_cast<std::size_t>(file.tellp());
    cli_status("Raw:    {} ({} bytes, VGA {})", path, total,
                 mode == amiga::Mode::vga_10h ? "Mode 10h 4-plane planar"
                                              : "Mode 12h 4-plane planar");
}

// VGA Mode 13h raw output: chunky 8bpp indices + 6-bit DAC palette
// (3 bytes/color), ready to feed into port 0x3C9 via `outp` loop.
void save_raw_vga(std::string_view path,
                  std::span<const std::uint8_t> indices,
                  std::size_t /*width*/, std::size_t /*height*/,
                  std::span<const Color3f> palette,
                  amiga::Mode /*mode*/) {
    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) { std::println(stderr, "Failed to open: {}", path_str); return; }

    file.write(reinterpret_cast<const char*>(indices.data()),
               static_cast<std::streamsize>(indices.size()));

    for (auto& c : palette) {
        auto rgb18 = palette::linear_to_vga(c);
        std::array<std::uint8_t, 3> buf{
            static_cast<std::uint8_t>((rgb18 >> 16) & 0x3F),
            static_cast<std::uint8_t>((rgb18 >> 8) & 0x3F),
            static_cast<std::uint8_t>(rgb18 & 0x3F),
        };
        file.write(reinterpret_cast<const char*>(buf.data()), 3);
    }

    auto total = static_cast<std::size_t>(file.tellp());
    cli_status("Raw:    {} ({} bytes, VGA Mode 13h chunky)", path, total);
}

Result<void> save_preview(std::string_view path, const Image& preview,
                          bool has_trans, const std::vector<bool>& mask,
                          amiga::Mode mode, bool hires = false,
                          bool interlace = false) {
    // DOS modes (EGA, VGA, CGA) have non-integer PAR (e.g. VGA 13h = 5:6
    // tall pixel). Use nearest-neighbor resampling to the display-aspect
    // preview dimensions, so the PNG shows the image at roughly 4:3 as
    // the CRT would. Amiga/Atari modes keep the existing integer-scale
    // path which handles their 1:1/1:2/2:1 PARs exactly.
    if (amiga::is_vga(mode) || amiga::is_ega(mode) || amiga::is_cga(mode)) {
        auto params = amiga::get_mode_params(mode);
        // Text modes render their cells at 640x200 (logical display); no
        // need for 2x upscale since that's already comfortable viewing size.
        std::size_t base_scale = amiga::is_cga_text(mode) ? 1u : 2u;
        auto [pw, ph] = preview_dims_for_par(preview.width(), preview.height(),
                                             static_cast<double>(params.par),
                                             base_scale);
        auto scaled = scale_preview_nn(preview, pw, ph);
        if (has_trans) {
            // Mask resample (bool → bool via nearest).
            std::vector<bool> scaled_mask(pw * ph, false);
            for (std::size_t y = 0; y < ph; ++y) {
                auto sy = std::min(preview.height() - 1,
                                   (y * preview.height()) / ph);
                for (std::size_t x = 0; x < pw; ++x) {
                    auto sx = std::min(preview.width() - 1,
                                       (x * preview.width()) / pw);
                    scaled_mask[y * pw + x] =
                        mask[sy * preview.width() + sx];
                }
            }
            return png_io::save(path, scaled, scaled_mask);
        }
        return png_io::save(path, scaled);
    }

    bool is_hires = hires;
    bool is_lace = interlace;
    std::size_t sx = is_hires ? 1 : 2;
    std::size_t sy = is_lace ? 1 : 2;
    if (is_hires && is_lace) { sx = 1; sy = 1; }
    auto scaled = scale_preview(preview, sx, sy);
    if (has_trans) {
        auto scaled_mask = scale_mask(mask, preview.width(), preview.height(),
                                      sx, sy);
        return png_io::save(path, scaled, scaled_mask);
    }
    return png_io::save(path, scaled);
}

// Show preview in terminal (iTerm2 inline image protocol)
void show_terminal_preview(const Image& preview, amiga::Mode mode,
                           bool hires = false, bool interlace = false) {
    // DOS modes use PAR-aware nearest-neighbor (same logic as save_preview).
    if (amiga::is_vga(mode) || amiga::is_ega(mode) || amiga::is_cga(mode)) {
        auto params = amiga::get_mode_params(mode);
        std::size_t base_scale = amiga::is_cga_text(mode) ? 1u : 2u;
        auto [pw, ph] = preview_dims_for_par(preview.width(), preview.height(),
                                             static_cast<double>(params.par),
                                             base_scale);
        iterm2_display(scale_preview_nn(preview, pw, ph));
        return;
    }
    bool is_hires = hires;
    bool is_lace = interlace;
    std::size_t sx = is_hires ? 1 : 2;
    std::size_t sy = is_lace ? 1 : 2;
    if (is_hires && is_lace) { sx = 1; sy = 1; }
    auto scaled = scale_preview(preview, sx, sy);
    iterm2_display(scaled);
}

// CLI progress reporter. Throttles to ~20Hz redraw, writes "\rEncoding NN.N%
// [stage]\033[K" to stderr when it's a TTY (no-op otherwise so piped runs
// don't get carriage returns in their stderr capture). Thread-safe — encoder
// workers may invoke it concurrently. Final tick prints a newline so the
// next CLI status line ("Encoded: ...") starts cleanly.
std::function<void(float, std::string_view)> make_cli_progress_reporter() {
    if (!isatty(fileno(stderr))) return {};
    auto state = std::make_shared<std::pair<std::mutex,
        std::chrono::steady_clock::time_point>>();
    state->second = std::chrono::steady_clock::time_point{};
    return [state](float p, std::string_view stage) {
        std::lock_guard lk(state->first);
        auto now = std::chrono::steady_clock::now();
        bool final_tick = (stage == "done") || p >= 1.0f;
        if (!final_tick &&
            now - state->second < std::chrono::milliseconds(50)) {
            return;
        }
        state->second = now;
        float pct = std::clamp(p, 0.0f, 1.0f) * 100.0f;
        std::fprintf(stderr, "\rEncoding... %5.1f%% [%.*s]\033[K",
                     static_cast<double>(pct),
                     static_cast<int>(stage.size()), stage.data());
        if (final_tick) std::fputc('\n', stderr);
        std::fflush(stderr);
    };
}

} // namespace

int main(int argc, char* argv[]) {
    auto config = parse_args(argc, argv);
    if (!config) {
        std::println(stderr, "Error: {}", config.error().message);
        return exit_code::usage;
    }
    g_quiet = config->quiet;
    g_json  = config->json;

    // --no-scale + .cpp/.c viewer is incoherent: the generated viewer
    // sets BPLCON0/DDFSTRT/DDFSTOP/BPLxMOD for a specific Amiga screen
    // mode and assumes the bitplane buffer matches the mode's display
    // window. Arbitrary source dimensions would mis-display. Reject
    // at startup so the user gets a clear error before any work.
    auto _ends_cpp_or_c = [](std::string_view s) {
        return s.size() >= 4 &&
               (s.substr(s.size() - 4) == ".cpp" ||
                s.substr(s.size() - 2) == ".c");
    };
    if (config->no_scale && _ends_cpp_or_c(config->output_path)) {
        std::println(stderr, "Error: --no-scale is incompatible with .cpp/.c "
                             "viewer output. The generated AmigaOS viewer "
                             "expects fixed Amiga screen dimensions; use "
                             "--no-scale only with .h, .iff, .raw, .pal, "
                             "or .png output.");
        return exit_code::usage;
    }

    // --list-modes: emit the mode catalog and exit. Useful for build
    // systems probing supported modes without running an encode.
    if (config->list_modes) {
        if (config->json) {
            std::print("{{\n  \"version\": \"{}\",\n  \"modes\": [\n",
                       png2amiga::version);
            const char* modes[] = {
                "lores", "lores-lace", "hires", "hires-lace",
                "ham6", "ham6-lace", "ham6-hires", "ham6-hires-lace",
                "ham8", "ham8-lace", "ham8-hires", "ham8-hires-lace",
                "ehb", "ehb-lace",
                "stf-low", "stf-med", "stf-hi",
                "ste-low", "ste-med", "ste-hi",
                "vga-13h", "vga-10h", "vga-12h",
                "ega-320", "ega-640", "ega-hi",
                "cga-320", "cga-640", "cga-composite", "cga-text80x100",
            };
            for (std::size_t i = 0; i < std::size(modes); ++i) {
                std::print("    \"{}\"{}\n", modes[i],
                           i + 1 < std::size(modes) ? "," : "");
            }
            cli_status("  ]\n}}");
        } else {
            cli_status("png2amiga {} — supported modes:", png2amiga::version);
            cli_status("  Amiga:    lores, lores-lace, hires, hires-lace, ham6, ham8, ehb (+ -lace, -hires variants)");
            cli_status("  Atari:    stf-low, stf-med, stf-hi, ste-low, ste-med, ste-hi");
            cli_status("  IBM PC:   vga-13h/10h/12h, ega-320/640/hi, cga-320/640/composite, cga-text80x100");
        }
        return exit_code::ok;
    }

    // --- SCAP calibration probe path ---
    // Synthesize the probe image + per-line copper ops directly and emit a
    // viewer .cpp/.adf, bypassing the normal load-image-and-encode pipeline.
    // Output extension picks .cpp (default) / .h, run through build-amiga.sh
    // afterwards to produce an ADF.
    if (!config->scap_probe.empty()) {
        if (config->output_path.empty()) {
            std::println(stderr,
                         "Error: --scap-probe requires -o <output.cpp>");
            return 1;
        }
        auto probe = config->scap_probe;
        scap::ScapResult res;
        if (probe == "a") {
            auto r = scap::make_scap_probe_a_dpf_ocs(320, 256);
            if (!r) { std::println(stderr, "Probe A error: {}",
                                   r.error().message); return 1; }
            res = *std::move(r);
        } else if (probe == "b" || probe == "c" || probe == "d") {
            std::println(stderr, "Error: --scap-probe {} not implemented yet "
                                 "(needs Probe A slot data first)", probe);
            return 1;
        } else {
            std::println(stderr,
                         "Error: --scap-probe value must be a|b|c|d, got '{}'",
                         probe);
            return 1;
        }

        // Pad palette to 16 entries (DPF emit needs the full upper-register
        // bank populated even when most are black).
        if (res.palette.size() < 16)
            res.palette.resize(16, Color3f{0.0f, 0.0f, 0.0f});

        cheader::CHeaderOptions ch_opts;
        ch_opts.symbol_name = config->symbol_name.empty()
            ? std::string("scap_") + res.probe_label
            : config->symbol_name;
        ch_opts.aga = false;            // OCS DPF for Phase 1
        ch_opts.dpf = true;
        ch_opts.fade_in = false;
        ch_opts.scap_line_moves = &res.line_moves;
        ch_opts.scap_label = res.probe_label;
        ch_opts.scap_anchor_hpos = res.slot_table.line_gate_hpos;
        ch_opts.scap_total_planes = res.slot_table.total_planes;

        cli_status("SCAP probe: {} ({}x{}, {} planes, {} per-line ops)",
                     res.probe_label,
                     res.planes.width, res.planes.height,
                     res.planes.depth,
                     res.line_moves.empty() ? 0 : res.line_moves[0].size());

        if (ends_with(config->output_path, ".cpp") ||
            ends_with(config->output_path, ".c")) {
            auto r = cheader::save_viewer(config->output_path, res.planes,
                                          res.palette, amiga::Mode::lores,
                                          ch_opts);
            if (!r) {
                std::println(stderr, "Viewer write error: {}",
                             r.error().message);
                return 1;
            }
            cli_status("Viewer: {}", config->output_path);
        } else if (ends_with(config->output_path, ".h")) {
            auto r = cheader::save(config->output_path, res.planes,
                                   res.palette, amiga::Mode::lores,
                                   ch_opts);
            if (!r) {
                std::println(stderr, "Header write error: {}",
                             r.error().message);
                return 1;
            }
            cli_status("Header: {}", config->output_path);
        } else {
            std::println(stderr,
                         "Error: --scap-probe output must be .cpp/.c/.h");
            return 1;
        }
        return 0;
    }

    // --- Validate mode combinations ---
    // (copper + interlace is supported: field 1 gets scanline_palettes
    // for even image rows, field 2 for odd rows. Each field's copper
    // list emits WAITs at the same VPOS values; only the referenced
    // row differs.)

    // --- Validate mode + depth against chipset ---
    {
        auto cs = effective_chipset(*config);
        auto cs_name = (cs == amiga::Chipset::aga) ? "AGA" : "OCS";

        if (config->depth < 1 || config->depth > 8) {
            std::println(stderr, "Error: depth must be 1-8, got {}",
                         config->depth);
            return 1;
        }

        // HAM modes with explicit --chipset ocs that need AGA
        if (amiga::is_ham(config->mode) && config->chipset.has_value() &&
            *config->chipset == amiga::Chipset::ocs) {
            auto ham_bp = amiga::get_mode_params(config->mode).bitplane_depth;
            if (ham_bp > amiga::max_depth(amiga::Chipset::ocs)) {
                std::println(stderr,
                    "Error: HAM{} requires AGA (>6 bitplanes), cannot use OCS",
                    ham_bp);
                return 1;
            }
        }

        // Check depth against mode+chipset limits
        // (HAM/EHB have fixed depths so --depth is ignored for them,
        //  but for standard modes the user's depth must be valid)
        if (!amiga::is_ham(config->mode) && config->mode != amiga::Mode::ehb &&
            !amiga::is_atari(config->mode) && !amiga::is_vga(config->mode) &&
            !amiga::is_ega(config->mode) && !amiga::is_cga(config->mode)) {
            auto max_d = amiga::max_user_depth(config->mode, cs);
            if (config->depth > max_d) {
                auto mp = amiga::get_mode_params(config->mode);
                const char* mode_name = mp.is_hires ? "hires" : "lores";
                std::println(stderr,
                    "Error: {} {} supports max {} bitplanes ({} colors), got {}",
                    cs_name, mode_name, max_d, std::size_t{1} << max_d,
                    config->depth);
                return 1;
            }
        }
    }

    // Load input image
    auto image = png_io::load(config->input_path);
    if (!image) {
        std::println(stderr, "Error loading {}: {}", config->input_path,
                     image.error().message);
        return exit_code::no_input;
    }

    // Compute target dimensions from source aspect ratio
    auto params = amiga::get_mode_params(config->mode);
    auto src_w = image->width();
    auto src_h = image->height();
    auto src_aspect = static_cast<double>(src_w) / static_cast<double>(src_h);

    bool interlace = config->interlace || params.is_interlaced;
    // Only force even height for interlace (fields must be equal).
    auto round_h = [interlace](double v) -> std::size_t {
        auto r = static_cast<std::size_t>(std::lround(v));
        if (interlace) return (r + 1) & ~std::size_t{1};
        return r;
    };

    // Pixel aspect ratio: PAR = pixel_width / pixel_height, from ModeParams.
    // hires: 0.5 (tall pixels); lores_interlace: 2.0 (wide); VGA 13h: 0.833
    // (CRT pixel 1.2x taller than wide). Integer scale_x/scale_y can't
    // represent VGA's 5:6, so we use an explicit float field.
    auto par = static_cast<double>(params.par);
    // For compound lace modes (e.g. ham6-lace), base mode isn't interlaced
    // but config->interlace is true — adjust PAR for double vertical resolution
    if (config->interlace && !params.is_interlaced) par *= 2.0;

    std::size_t target_w, target_h;
    if (config->no_scale) {
        // --no-scale: emit at the source PNG's exact dimensions.
        // The encoder's downstream scaling step becomes a no-op since
        // image dims already match target. Useful for asset pipelines
        // that pre-scale and want the converter to act as a pure
        // pixel-to-bitplane translator.
        target_w = src_w;
        target_h = src_h;
    } else if (config->width && config->height) {
        // Both specified: use as-is (user explicitly wants this aspect)
        target_w = *config->width;
        target_h = *config->height;
    } else if (config->width) {
        target_w = *config->width;
        // Adjust PAR when width differs from mode default (e.g. ham6 at 640px = hires)
        auto w_par = (target_w != params.screen_width && params.screen_width > 0)
            ? par * static_cast<double>(params.screen_width) / static_cast<double>(target_w)
            : par;
        target_h = round_h(static_cast<double>(target_w) * w_par / src_aspect);
    } else if (config->height) {
        target_h = *config->height;
        target_w = static_cast<std::size_t>(
            std::lround(static_cast<double>(target_h) * src_aspect / par));
    } else {
        // Use mode default width. For lores modes, don't upscale if source
        // is smaller. For hires, always use 640 (that's the point of hires).
        target_w = params.is_hires
            ? params.screen_width
            : std::min(params.screen_width, src_w);
        target_h = round_h(static_cast<double>(target_w) * par / src_aspect);
        // Fixed-buffer modes (Atari, VGA, CGA): by default stretch-fit the
        // source to the full buffer. With --native-par, preserve source
        // aspect on hardware — letterbox (reduce target_h) or pillarbox
        // (reduce target_w) to fit inside screen_height. Later padding
        // expands the encoded image to full buffer dimensions with black.
        if (params.screen_height > 0) {
            bool is_fixed_buffer = amiga::is_atari(config->mode) ||
                                   amiga::is_vga(config->mode) ||
                                   amiga::is_cga(config->mode) ||
                                   amiga::is_ega(config->mode);
            if (is_fixed_buffer && !config->native_par) {
                target_h = params.screen_height;  // stretch to fill
            } else if (target_h > params.screen_height) {
                // Horizontal-fit overflows vertically — use vertical-fit
                // (pillarbox) instead so source aspect is preserved.
                target_h = params.screen_height;
                target_w = static_cast<std::size_t>(std::lround(
                    static_cast<double>(target_h) * src_aspect / par));
                if (target_w > params.screen_width) {
                    target_w = params.screen_width;
                }
            }
        }
    }

    cli_status("Input:  {}x{}", image->width(), image->height());

    // Determine the effective source crop region BEFORE building the mask,
    // so we can sample alpha from the cropped region rather than the full
    // source. Otherwise the mask references pixels the image will no
    // longer contain after crop.
    std::size_t crop_x = 0, crop_y = 0;
    std::size_t crop_w = image->width(), crop_h = image->height();
    bool will_crop = false;
    if (config->crop_w > 0 && config->crop_h > 0) {
        auto cx = static_cast<std::size_t>(config->crop_x);
        auto cy = static_cast<std::size_t>(config->crop_y);
        auto cw = static_cast<std::size_t>(config->crop_w);
        auto ch = static_cast<std::size_t>(config->crop_h);
        if (cx + cw > image->width() || cy + ch > image->height() || cw == 0 || ch == 0) {
            std::println(stderr, "Crop region {}x{}+{}+{} exceeds image {}x{}",
                         cw, ch, cx, cy, image->width(), image->height());
            return 1;
        }
        crop_x = cx; crop_y = cy; crop_w = cw; crop_h = ch;
        will_crop = true;
    } else if (config->crop_auto) {
        auto target_ratio = static_cast<double>(target_w) / static_cast<double>(target_h);
        auto src_ratio = static_cast<double>(image->width()) /
                         static_cast<double>(image->height());
        if (src_ratio > target_ratio) {
            crop_h = image->height();
            crop_w = static_cast<std::size_t>(
                static_cast<double>(image->height()) * target_ratio + 0.5);
        } else {
            crop_w = image->width();
            crop_h = static_cast<std::size_t>(
                static_cast<double>(image->width()) / target_ratio + 0.5);
        }
        crop_x = (image->width() - crop_w) / 2;
        crop_y = (image->height() - crop_h) / 2;
        will_crop = (crop_w != image->width() || crop_h != image->height());
    }

    // Compute transparency mask from source alpha BEFORE crop/scale
    // (crop and scale lose the alpha channel). Bilinearly sample the
    // (possibly cropped) source region into target resolution, then
    // dither/threshold at target so the dither pattern matches output pixels.
    std::vector<bool> transparency_mask;
    bool has_transparency = false;

    if (image->has_alpha()) {
        // Scale the cropped source region to target resolution (bilinear).
        std::vector<float> scaled_alpha(target_w * target_h);
        auto last_x = crop_x + crop_w - 1;
        auto last_y = crop_y + crop_h - 1;
        for (std::size_t y = 0; y < target_h; ++y) {
            auto fy = static_cast<double>(crop_y) +
                      (static_cast<double>(y) + 0.5) *
                          static_cast<double>(crop_h) /
                          static_cast<double>(target_h) - 0.5;
            if (fy < static_cast<double>(crop_y)) fy = static_cast<double>(crop_y);
            auto sy = std::min(static_cast<std::size_t>(fy), last_y);
            auto sy1 = std::min(sy + 1, last_y);
            auto ty = static_cast<float>(fy - static_cast<double>(sy));

            for (std::size_t x = 0; x < target_w; ++x) {
                auto fx = static_cast<double>(crop_x) +
                          (static_cast<double>(x) + 0.5) *
                              static_cast<double>(crop_w) /
                              static_cast<double>(target_w) - 0.5;
                if (fx < static_cast<double>(crop_x)) fx = static_cast<double>(crop_x);
                auto sx = std::min(static_cast<std::size_t>(fx), last_x);
                auto sx1 = std::min(sx + 1, last_x);
                auto tx = static_cast<float>(fx - static_cast<double>(sx));

                float a00 = image->alpha_at(sx,  sy);
                float a10 = image->alpha_at(sx1, sy);
                float a01 = image->alpha_at(sx,  sy1);
                float a11 = image->alpha_at(sx1, sy1);
                float a = a00 * (1-tx) * (1-ty) + a10 * tx * (1-ty)
                        + a01 * (1-tx) * ty     + a11 * tx * ty;
                scaled_alpha[y * target_w + x] = a;
            }
        }

        // Apply threshold or dither at target resolution
        transparency_mask.resize(target_w * target_h);

        if (config->alpha_dither != dither::Method::none) {
            auto adm = config->alpha_dither;
            for (std::size_t y = 0; y < target_h; ++y) {
                for (std::size_t x = 0; x < target_w; ++x) {
                    float a = scaled_alpha[y * target_w + x];
                    float threshold = dither::ordered_threshold(adm, x, y);
                    transparency_mask[y * target_w + x] =
                        (a + threshold * config->alpha_dither_strength) < (0.5f + config->alpha_threshold);
                }
            }
        } else {
            for (std::size_t y = 0; y < target_h; ++y) {
                for (std::size_t x = 0; x < target_w; ++x) {
                    transparency_mask[y * target_w + x] =
                        scaled_alpha[y * target_w + x] < (0.5f + config->alpha_threshold);
                }
            }
        }

        for (auto v : transparency_mask) {
            if (v) { has_transparency = true; break; }
        }

        if (has_transparency) {
            cli_status("Alpha:  {} transparent pixels ({})",
                std::count(transparency_mask.begin(),
                           transparency_mask.end(), true),
                config->alpha_dither != dither::Method::none
                    ? dither_name(config->alpha_dither) : "threshold");
        }
    }

    // Apply the crop region we already validated above.
    if (will_crop) {
        auto cropped = crop_image(*image, crop_x, crop_y, crop_w, crop_h);
        if (!cropped) {
            std::println(stderr, "Crop error: {}", cropped.error().message);
            return 1;
        }
        if (config->crop_w > 0 && config->crop_h > 0) {
            cli_status("Crop:   {}x{}+{}+{} -> {}x{}",
                         crop_w, crop_h, crop_x, crop_y,
                         cropped->width(), cropped->height());
        } else {
            cli_status("Crop:   auto {}x{} -> {}x{} (center, {:g}:{:g} aspect)",
                         image->width(), image->height(),
                         cropped->width(), cropped->height(),
                         static_cast<double>(target_w),
                         static_cast<double>(target_h));
        }
        image = *std::move(cropped);
    }

    // Display actual bitplane depth (HAM/EHB override user's --depth)
    auto actual_depth = config->depth;
    if (amiga::is_ham(config->mode))
        actual_depth = amiga::get_mode_params(config->mode).bitplane_depth;
    else if (config->mode == amiga::Mode::ehb)
        actual_depth = 6;
    else if (amiga::is_atari(config->mode) || amiga::is_vga(config->mode) ||
             amiga::is_ega(config->mode) || amiga::is_cga(config->mode)) {
        // Atari/DOS modes have their bitplane depth baked into the hardware
        // (ST Low=4, VGA 13h=8, EGA=4, CGA 320=2, etc.), so --depth makes
        // no sense. Reject the user override rather than silently ignoring it.
        if (config->depth_explicit) {
            std::println(stderr,
                "Error: --depth is not configurable for this mode "
                "(it's fixed at {} by the target hardware)",
                amiga::get_mode_params(config->mode).bitplane_depth);
            return 1;
        }
        actual_depth = amiga::get_mode_params(config->mode).bitplane_depth;
        config->depth = actual_depth;
    }
    cli_status("Target: {}x{} @ {} bitplanes", target_w, target_h, actual_depth);

    // Scale
    if (image->width() != target_w || image->height() != target_h) {
        // Interlace needs even height. If only 1 row short, pad instead of
        // resampling the entire image (avoids blur from bicubic).
        if (image->width() == target_w && target_h == image->height() + 1) {
            Image padded(target_w, target_h);
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < target_w; ++x)
                    padded[x, y] = (*image)[x, y];
            for (std::size_t x = 0; x < target_w; ++x)
                padded[x, target_h - 1] = (*image)[x, image->height() - 1];
            image = std::move(padded);
        } else {
            auto scaled = scale::bicubic(*image, target_w, target_h);
            if (!scaled) {
                std::println(stderr, "Scale error: {}", scaled.error().message);
                return 1;
            }
            image = *std::move(scaled);
        }
    }

    // Fixed-buffer modes (Atari, VGA, EGA, CGA): center the image in the
    // hardware frame if either dimension is smaller than the buffer.
    // Vertical-only for Atari legacy, both axes for DOS modes so --native-par
    // pillarboxing is written to the actual hardware buffer width.
    if (params.screen_height > 0 &&
        (image->height() < params.screen_height ||
         image->width() < params.screen_width)) {
        auto w = image->width();
        auto h = image->height();
        auto fw = std::max(image->width(), params.screen_width);
        auto fh = std::max(image->height(), params.screen_height);
        Image padded(fw, fh);
        auto x_off = (fw - w) / 2;
        auto y_off = (fh - h) / 2;
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x)
                padded[x + x_off, y + y_off] = (*image)[x, y];
        if (has_transparency) {
            std::vector<bool> new_mask(fw * fh, true);
            for (std::size_t y = 0; y < h; ++y)
                for (std::size_t x = 0; x < w; ++x)
                    new_mask[(y + y_off) * fw + (x + x_off)] =
                        transparency_mask[y * w + x];
            transparency_mask = std::move(new_mask);
        }
        cli_status("Center: {}x{} -> {}x{} (pad)", w, h, fw, fh);
        image = std::move(padded);
        target_w = fw;
        target_h = fh;
    }

    // Preprocess
    preprocess::apply(*image, config->preprocess);

    auto chipset = effective_chipset(*config);
    cli_status("Chipset: {}", chipset == amiga::Chipset::aga ? "AGA (24-bit)" : "OCS (12-bit)");

    // Dual playfield: encoded image lives in PF2 of a 2N-plane display, with
    // PF1 (foreground) zeroed and the palette shifted into the upper color
    // registers. Force the encoder to depth 3 (OCS) / 4 (AGA) before any
    // mode-specific branch picks it up.
    bool use_dpf_std = config->dual_playfield &&
                       !amiga::is_ham(config->mode) &&
                       config->mode != amiga::Mode::ehb &&
                       !amiga::is_atari(config->mode) &&
                       !amiga::is_vga(config->mode) &&
                       !amiga::is_ega(config->mode) &&
                       !amiga::is_cga(config->mode);
    if (use_dpf_std) {
        config->depth = (chipset == amiga::Chipset::aga) ? 4 : 3;
    }

    // AGA depth 6 in standard mode: set KILLEHB to prevent hardware
    // from triggering EHB. This allows a true 64-color indexed palette.

    // --- Text-mode graphics (glyph-matched, CGA or EGA font) ---
    if (amiga::is_cga_text(config->mode)) {
        // Force transparent source pixels to black before glyph matching,
        // so logos / sprites with alpha cutouts don't contribute stray
        // RGBA-composite colors to the encoded output.
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        // Build the 16-color fg/bg candidate palette.
        //   CGA text: fixed IRGB master palette (kCgaHw) — CGA has no
        //             programmable DAC in text mode.
        //   EGA text: image-adaptive 16-of-64 from the IrgbIRGB gamut via
        //             the EGA histogram quantizer, same as EGA graphics
        //             modes. The 16 attribute-register slots on real EGA
        //             hardware get loaded with these colors at startup.
        std::vector<Color3f> text_pal;
        text_pal.reserve(16);
        // CGA text uses the kCgaHw 16-color IRGB master palette.
        for (std::size_t i = 0; i < 16; ++i) {
            text_pal.push_back(
                color_space::srgb_hex_to_linear(palette::kCgaHw[i]));
        }
        // Resolve metric and decide whether to pre-dither. `mse` (default)
        // pairs with pixel-level dither so the glyph matcher sees
        // discrete candidates. `blur` and `pca` need the continuous
        // source — pre-dither would collapse the precision they need.
        auto cga_metric =
            config->cga_text_metric == "mse" ? cga_text::Metric::mse
                                             : cga_text::Metric::blur;
        Image dithered(image->width(), image->height());
        if (cga_metric != cga_text::Metric::mse ||
            config->dither_method == dither::Method::none) {
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x)
                    dithered[x, y] = (*image)[x, y];
        } else {
            auto dith = dither::apply(*image, text_pal, {
                .method = config->dither_method,
                .strength = config->dither_strength,
                .error_clamp = config->error_clamp,
                .serpentine = true,
            });
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x)
                    dithered[x, y] = text_pal[dith.indices[y * image->width() + x]];
        }
        // Real CGA hardware has no custom-font slot (unlike EGA/VGA), so
        // the viewer can only render ROM glyph scanlines 0..(cell_h-1).
        // When emitting a 16-bit C viewer (.c → ia16-elf-gcc / 8088 XT)
        // for a CGA text mode, constrain the encoder to offset=0.
        int fixed_offset = -1;
        if (amiga::is_cga_text(config->mode) &&
            ends_with(config->output_path, ".c"))
            fixed_offset = 0;
        auto res = cga_text::encode(dithered, config->mode, {}, text_pal,
                                    fixed_offset, cga_metric);
        if (!res) {
            std::println(stderr, "CGA text encode error: {}",
                         res.error().message);
            return 1;
        }
        // Render once: used for stats line, terminal preview, and any
        // PNG output.
        auto preview = cga_text::render(*res);
        cli_status("Encoded: {} cells ({}x{}), {} bytes, {} colors, "
                     "error: {:.2f}, CRTC scanline offset: {}",
                     res->cols * res->rows, res->cols, res->rows,
                     res->data.size(), count_unique_colors(preview),
                     res->total_error, res->scanline_offset);

        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".raw")) {
                std::ofstream of(std::string(config->output_path),
                                 std::ios::binary);
                of.write(reinterpret_cast<const char*>(res->data.data()),
                         static_cast<std::streamsize>(res->data.size()));
                cli_status("Raw:    {} ({} bytes, CGA text {}x{} char+attr"
                             "; blink must be CLEARED in mode reg 0x3D8 bit 5 "
                             "so attr bit 7 becomes bg intensity, enabling "
                             "all 16 bg colors)",
                             config->output_path, res->data.size(),
                             res->cols, res->rows);
            } else if (ends_with(config->output_path, ".c") &&
                       amiga::is_cga_text(config->mode)) {
                // 16-bit C text viewer for real CGA (ia16-elf-gcc, 8088+).
                // CRTC reprogramming handled inside cheader_dos_c; ROM font
                // used as-is (scanline_offset forced to 0 at encode time).
                cheader_dos_c::Options opts{
                    .symbol_name = config->symbol_name.empty()
                        ? derive_symbol_name(config->output_path)
                        : config->symbol_name};
                auto result = cheader_dos_c::save(
                    config->output_path, config->mode,
                    res->cols, res->rows,
                    std::span{res->data.data(), res->data.size()},
                    {}, opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Viewer: {} (DOS/ia16-elf 16-bit)",
                             config->output_path);
            } else {
                std::vector<bool> empty_mask;
                auto result = save_preview(config->output_path, preview,
                                           false, empty_mask,
                                           config->mode, false, false);
                if (!result) {
                    std::println(stderr, "PNG write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("PNG:    {}", config->output_path);
            }
        }

        if (config->preview)
            show_terminal_preview(preview, config->mode, false, false);
        return 0;
    }

    // --- HAM modes ---
    if (amiga::is_ham(config->mode)) {
        if (!config->locks.empty() || !config->pins.empty()) {
            std::println(stderr, "Error: --lock-index / --pin-index-at "
                                 "are not supported in HAM modes "
                                 "(palette is dynamic per pixel)");
            return 1;
        }
        if (config->fade_in) {
            std::println(stderr, "Error: --fade-in is not supported in HAM modes "
                                 "(HAM modify bits carry absolute R/G/B values, "
                                 "so fading the base palette would corrupt the image)");
            return 1;
        }
        auto ham_params = amiga::get_mode_params(config->mode);
        // Both HAM6 and HAM8 default to Floyd-Steinberg. HAM8's 6-bit
        // MODIFY still introduces visible banding on smooth gradients
        // (~4 sRGB-value-per-step), and FS improves blurred PSNR by
        // +0.1 to +0.7 dB on all test images. User can disable with
        // `--dither none`.
        auto ham_default_dither = dither::Method::floyd_steinberg;
        auto ham_dither = config->dither_explicit
            ? config->dither_method : ham_default_dither;
        cli_status("Mode:   HAM{} (beam: {}, dither: {})",
                     ham_params.bitplane_depth,
                     config->ham_beam,
                     dither_name(ham_dither));

        ham::HamOptions ham_opts;
        ham_opts.beam_width = config->ham_beam;
        ham_opts.dither_method = ham_dither;
        auto ham_tune = dither_tuning::defaults_for(dither_tuning::Context{
            .mode    = config->mode,
            .depth   = static_cast<int>(config->depth),
            .dpf     = config->dual_playfield,
            .scap    = false,
            .copper  = config->copper,
            .chipset = chipset,
        });
        ham_opts.dither_strength = config->dither_strength_explicit
                                   ? config->dither_strength : ham_tune.strength;
        ham_opts.error_clamp = config->error_clamp_explicit
                              ? config->error_clamp : ham_tune.error_clamp;
        ham_opts.palette_diversity = config->palette_diversity;
        ham_opts.quantizer = config->quantizer;
        ham_opts.triple_beam = config->ham_triple;
        ham_opts.greedy = config->ham_fast;
        // cap_best gates HAM4/5/7 — only HAM6 and HAM8 are eligible.
        bool ham_eligible_for_cap_best =
            (ham_params.bitplane_depth == 6 ||
             ham_params.bitplane_depth == 8);
        ham_opts.cap_best = config->cap_best && ham_eligible_for_cap_best;
        ham_opts.on_progress = make_cli_progress_reporter();
        ham_opts.skip_initial_swap_rows = config->interlace ? 2 : 0;

        // Force transparent pixels to black before HAM encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        Result<ham::HamResult> ham_result = config->copper
            ? ham::encode_ham_copper(*image, config->mode, chipset, ham_opts,
                                   config->hires, static_cast<std::size_t>(config->copper_changes))
            : ham::encode_ham(*image, config->mode, chipset, ham_opts);

        if (!ham_result) {
            std::println(stderr, "HAM encode error: {}", ham_result.error().message);
            return 1;
        }

        // Render preview first so we can count unique colors for the stats line.
        auto data_bits = ham_result->planes.depth - 2;
        auto ham_preview = !ham_result->scanline_palettes.empty()
            ? ham::render_ham_copper(ham_result->planes,
                                    ham_result->scanline_palettes, data_bits)
            : ham::render_ham(ham_result->planes,
                             ham_result->base_palette, data_bits);
        int ham_unique = ham_preview ? count_unique_colors(*ham_preview) : 0;

        float ham_psnr = ham_preview
            ? color_space::compute_psnr_blurred(image->pixels(),
                                                ham_preview->pixels(),
                                                image->width(), image->height())
            : 0.0f;
        if (config->copper && !ham_result->copper_changes.empty()) {
            std::size_t total_ch = 0;
            for (auto& ch : ham_result->copper_changes) total_ch += ch.size();
            float avg_ch = ham_result->copper_changes.size() > 0
                ? static_cast<float>(total_ch) / static_cast<float>(ham_result->copper_changes.size())
                : 0.0f;
            int ham_cap_entries = static_cast<int>(
                ham_result->copper_changes.size() *
                ham_result->changes_per_line);
            cli_print_encoded(
                static_cast<int>(ham_result->planes.depth),
                static_cast<int>(ham_result->planes.total_bytes()),
                static_cast<int>(ham_result->base_palette.size()),
                chipset == amiga::Chipset::aga,
                ham_cap_entries, /*scap=*/0,
                static_cast<int>(ham_result->planes.height),
                static_cast<int>(ham_result->changes_per_line),
                ham_unique,
                std::optional<float>{avg_ch},
                static_cast<double>(ham_result->total_error), ham_psnr);
        } else {
            cli_print_encoded(
                static_cast<int>(ham_result->planes.depth),
                static_cast<int>(ham_result->planes.total_bytes()),
                static_cast<int>(ham_result->base_palette.size()),
                chipset == amiga::Chipset::aga,
                /*cap=*/0, /*scap=*/0,
                static_cast<int>(ham_result->planes.height),
                /*max_moves=*/0,
                ham_unique, std::nullopt,
                static_cast<double>(ham_result->total_error), ham_psnr);
        }

        if (ham_preview)
            if (config->preview) show_terminal_preview(*ham_preview, config->mode, config->hires, config->interlace);

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;
                iff_opts.has_transparency = has_transparency;
                if (!ham_result->scanline_palettes.empty()) {
                    iff_opts.scanline_palettes = &ham_result->scanline_palettes;
                }

                auto result = iff::save_ilbm(
                    config->output_path, ham_result->planes,
                    ham_result->base_palette, config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}", result.error().message);
                    return 1;
                }
                cli_status("IFF:    {}", config->output_path);
            } else if (ends_with(config->output_path, ".h")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                if (!ham_result->copper_changes.empty()) {
                    ch_opts.copper_changes = &ham_result->copper_changes;
                    ch_opts.copper_changes_per_line = ham_result->changes_per_line;
                    if (!ham_result->scanline_palettes.empty())
                        ch_opts.copper_scanline_palettes = &ham_result->scanline_palettes;
                }

                auto result = cheader::save(
                    config->output_path, ham_result->planes,
                    ham_result->base_palette, config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "C header write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Header: {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                if (!ham_result->copper_changes.empty()) {
                    ch_opts.copper_changes = &ham_result->copper_changes;
                    ch_opts.copper_changes_per_line = ham_result->changes_per_line;
                    if (!ham_result->scanline_palettes.empty())
                        ch_opts.copper_scanline_palettes = &ham_result->scanline_palettes;
                }

                pad_planes_to_mode(ham_result->planes, config->mode, config->hires);
                auto result = cheader::save_viewer(
                    config->output_path, ham_result->planes,
                    ham_result->base_palette, config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Viewer: {}", config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                save_raw(config->output_path, ham_result->planes,
                         ham_result->base_palette, chipset,
                         config->copper ? &ham_result->copper_changes : nullptr,
                         ham_result->changes_per_line);
            } else if (ends_with(config->output_path, ".pal")) {
                auto result = palette_io::save_ocs_palette(
                    config->output_path, ham_result->base_palette);
                if (!result) {
                    std::println(stderr, "Palette write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Pal:    {} ({} colors, {} bytes)",
                             config->output_path,
                             ham_result->base_palette.size(),
                             ham_result->base_palette.size() * 2);
            } else {
                // Use the preview rendered earlier for the stats line.
                if (!ham_preview) {
                    std::println(stderr, "Render error: {}", ham_preview.error().message);
                    return 1;
                }
                auto result = save_preview(config->output_path, *ham_preview,
                                           has_transparency, transparency_mask,
                                           config->mode, config->hires, config->interlace);
                if (!result) {
                    std::println(stderr, "PNG write error: {}", result.error().message);
                    return 1;
                }
                cli_status("PNG:    {}", config->output_path);
            }
        }

        // Mask export (HAM mode)
        if (!config->mask_path.empty())
            save_mask(config->mask_path, transparency_mask,
                      target_w, target_h, config->mask_invert, config->interlace);

        return 0;
    }

    // --- EHB mode ---
    // Skip when --scap is on so the SCAP-EHB investigation block below
    // gets to handle it.
    if (config->mode == amiga::Mode::ehb && !config->scap) {
        // EHB is always 6 bitplanes, 32 base + 32 half-brightness
        auto ehb_depth = std::size_t{6};

        // Force transparent pixels to black before encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        // --- EHB with copper ---
        if (config->copper) {
            if (!config->locks.empty() || !config->pins.empty()) {
                std::println(stderr, "Error: --lock-index / --pin-index-at "
                                     "are not supported with EHB + --copper");
                return 1;
            }

            dither::Settings dith;
            dith.method = config->dither_method;
            auto ehb_cap_tune = dither_tuning::defaults_for(dither_tuning::Context{
                .mode    = config->mode,
                .depth   = static_cast<int>(config->depth),
                .dpf     = config->dual_playfield,
                .scap    = false,
                .copper  = true,
                .chipset = chipset,
            });
            dith.strength = config->dither_strength_explicit
                            ? config->dither_strength : ehb_cap_tune.strength;
            dith.error_clamp = config->error_clamp_explicit
                              ? config->error_clamp : ehb_cap_tune.error_clamp;

            cli_status("Dither: {} (strength: {:.2f})",
                         dither_name(dith.method), dith.strength);

            // Copper encoder optimizes 32 base colors per scanline (depth=5)
            // Interlace: skip swaps on rows 0 and 1 (each field's first
            // displayed line must show base palette only).
            std::size_t skip_initial = config->interlace ? 2 : 0;
            auto copper_result = copper::encode_copper(*image, 5, dith, chipset,
                static_cast<std::size_t>(config->copper_changes),
                nullptr, true, {}, config->palette_diversity,
                skip_initial, config->interlace, /*is_ehb=*/true,
                make_cli_progress_reporter());
            if (!copper_result) {
                std::println(stderr, "Copper encode error: {}",
                             copper_result.error().message);
                return 1;
            }
            cli_status("Mode:   EHB + CAP ({} changes/line, max {} MOVEs/line)",
                         copper_result->changes_per_line,
                         copper_result->max_moves_per_line);

            // Re-dither each scanline against its 64-color EHB palette
            auto w = image->width();
            auto h = image->height();
            std::vector<std::uint8_t> all_indices(w * h);
            float total_error = 0.0f;

            bool use_ordered = dither::is_ordered(dith.method) &&
                               dith.method != dither::Method::none;
            bool use_diffusion = !use_ordered &&
                                 dith.method != dither::Method::none;
            std::vector<color_space::OKLab> err_buf;
            if (use_diffusion) err_buf.resize(w * h);

            for (std::size_t y = 0; y < h; ++y) {
                auto& base32 = copper_result->scanline_palettes[y];
                Palette bp;
                bp.colors.assign(base32.begin(), base32.end());
                auto ehb64 = palette::make_ehb_palette(bp.colors);

                auto row = image->row(y);
                std::vector<color_space::OKLab> pal_lab(ehb64.colors.size());
                for (std::size_t i = 0; i < ehb64.colors.size(); ++i)
                    pal_lab[i] = color_space::linear_to_oklab(ehb64.colors[i]);

                for (std::size_t x = 0; x < w; ++x) {
                    auto pixel_lab = color_space::linear_to_oklab(row[x]);
                    if (!err_buf.empty()) {
                        auto& e = err_buf[y * w + x];
                        auto ec = dith.error_clamp;
                        pixel_lab.L += std::clamp(e.L, -ec, ec);
                        pixel_lab.a += std::clamp(e.a, -ec, ec);
                        pixel_lab.b += std::clamp(e.b, -ec, ec);
                    }
                    if (use_ordered) {
                        float thr = dither::ordered_threshold(dith.method, x, y);
                        pixel_lab.L += thr * dith.strength * 0.15f;
                        pixel_lab.a += thr * dith.strength * 0.03f;
                        pixel_lab.b += thr * dith.strength * 0.03f;
                    }
                    float best_d = std::numeric_limits<float>::max();
                    std::uint8_t best_k = 0;
                    for (std::size_t k = 0; k < pal_lab.size(); ++k) {
                        float dL = pixel_lab.L - pal_lab[k].L;
                        float da = pixel_lab.a - pal_lab[k].a;
                        float db = pixel_lab.b - pal_lab[k].b;
                        float d = dL * dL + da * da + db * db;
                        if (d < best_d) { best_d = d; best_k = static_cast<std::uint8_t>(k); }
                    }
                    all_indices[y * w + x] = best_k;
                    total_error += best_d;
                    if (use_diffusion) {
                        auto& cl = pal_lab[best_k];
                        color_space::OKLab qe = {
                            (pixel_lab.L - cl.L) * dith.strength,
                            (pixel_lab.a - cl.a) * dith.strength,
                            (pixel_lab.b - cl.b) * dith.strength};
                        auto kernel = dither::error_diffusion_kernel(dith.method);
                        for (auto& [kdx, kdy, kw] : kernel) {
                            auto nx = static_cast<std::ptrdiff_t>(x) + kdx;
                            auto ny = static_cast<std::ptrdiff_t>(y) + kdy;
                            if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                                ny >= 0 && static_cast<std::size_t>(ny) < h) {
                                auto& e = err_buf[static_cast<std::size_t>(ny) * w +
                                                  static_cast<std::size_t>(nx)];
                                e.L += qe.L * kw;
                                e.a += qe.a * kw;
                                e.b += qe.b * kw;
                            }
                        }
                    }
                }
            }

            if (has_transparency) {
                for (std::size_t i = 0; i < transparency_mask.size() && i < all_indices.size(); ++i)
                    if (transparency_mask[i]) all_indices[i] = 0;
            }

            auto planes = bitplane::encode(all_indices, w, h, ehb_depth);
            if (!planes) {
                std::println(stderr, "Encode error: {}", planes.error().message);
                return 1;
            }

            // Per-scanline preview
            Image rendered(w, h);
            for (std::size_t y = 0; y < h; ++y) {
                auto& base32 = copper_result->scanline_palettes[y];
                Palette bp;
                bp.colors.assign(base32.begin(), base32.end());
                auto ehb64 = palette::make_ehb_palette(bp.colors);
                for (std::size_t x = 0; x < w; ++x) {
                    auto idx = all_indices[y * w + x];
                    if (idx < ehb64.colors.size())
                        rendered[x, y] = ehb64.colors[idx];
                }
            }

            float ehb_psnr = color_space::compute_psnr_blurred(
                image->pixels(), rendered.pixels(), w, h);
            int ehb_cap_entries = static_cast<int>(
                h * copper_result->changes_per_line);
            cli_print_encoded(
                static_cast<int>(planes->depth),
                static_cast<int>(planes->total_bytes()),
                static_cast<int>(copper_result->base_palette.size()),
                /*aga=*/false,
                ehb_cap_entries, /*scap=*/0,
                static_cast<int>(h),
                static_cast<int>(copper_result->max_moves_per_line),
                static_cast<int>(count_unique_colors(rendered)),
                std::optional<float>{copper_result->avg_changes_per_line},
                static_cast<double>(total_error), ehb_psnr);

            if (config->preview) show_terminal_preview(rendered, config->mode, config->hires, config->interlace);

            // Use base palette for CMAP
            std::vector<Color3f> cmap_palette = copper_result->base_palette;

            if (!config->output_path.empty()) {
                if (ends_with(config->output_path, ".png")) {
                    auto result = save_preview(config->output_path, rendered,
                                               has_transparency, transparency_mask,
                                               config->mode, config->hires, config->interlace);
                    if (!result) {
                        std::println(stderr, "PNG write error: {}", result.error().message);
                        return 1;
                    }
                    cli_status("PNG:    {}", config->output_path);
                } else if (ends_with(config->output_path, ".cpp") ||
                           ends_with(config->output_path, ".c")) {
                    cheader::CHeaderOptions ch_opts;
                    ch_opts.symbol_name = config->symbol_name.empty()
                        ? derive_symbol_name(config->output_path)
                        : config->symbol_name;
                    ch_opts.hires = config->hires;
                    ch_opts.interlace = config->interlace;
                    ch_opts.fade_in = config->fade_in;
                    ch_opts.copper_changes = &copper_result->scanline_changes;
                    ch_opts.copper_changes_per_line = copper_result->changes_per_line;
                    ch_opts.copper_scanline_palettes = &copper_result->scanline_palettes;

                    pad_planes_to_mode(planes.value(), config->mode, config->hires);
                    auto result = cheader::save_viewer(
                        config->output_path, planes.value(),
                        cmap_palette, config->mode, ch_opts);
                    if (!result) {
                        std::println(stderr, "Viewer write error: {}", result.error().message);
                        return 1;
                    }
                    cli_status("Viewer: {}", config->output_path);
                } else if (ends_with(config->output_path, ".iff") ||
                           ends_with(config->output_path, ".ilbm")) {
                    // EHB+CAP IFF: emit 6-plane bitplane data + CMAP
                    // (32 base colours; EHB hardware derives the
                    // half-brites at display time) + CAMG with EHB
                    // flag set + PCHG with the per-line base palette
                    // evolution. RECOIL / DPaint / ViewTek read this
                    // as a normal EHB ILBM and apply the per-line
                    // changes themselves.
                    iff::IffOptions iff_opts;
                    iff_opts.hires = config->hires;
                    iff_opts.interlace = config->interlace;
                    iff_opts.has_transparency = has_transparency;
                    if (!copper_result->scanline_palettes.empty()) {
                        iff_opts.scanline_palettes =
                            &copper_result->scanline_palettes;
                    }
                    auto result = iff::save_ilbm(
                        config->output_path, planes.value(),
                        cmap_palette, config->mode, iff_opts);
                    if (!result) {
                        std::println(stderr, "IFF write error: {}",
                                     result.error().message);
                        return exit_code::cant_create;
                    }
                    cli_status("IFF:    {} ({} bytes)",
                                 config->output_path,
                                 planes->total_bytes());
                } else if (ends_with(config->output_path, ".h")) {
                    // .h header for EHB+CAP: bitplanes + base CMAP +
                    // copper change data, no viewer init code.
                    cheader::CHeaderOptions ch_opts;
                    ch_opts.symbol_name = config->symbol_name.empty()
                        ? derive_symbol_name(config->output_path)
                        : config->symbol_name;
                    ch_opts.hires = config->hires;
                    ch_opts.interlace = config->interlace;
                    ch_opts.copper_changes = &copper_result->scanline_changes;
                    ch_opts.copper_changes_per_line =
                        copper_result->changes_per_line;
                    ch_opts.copper_scanline_palettes =
                        &copper_result->scanline_palettes;
                    pad_planes_to_mode(planes.value(), config->mode,
                                       config->hires);
                    auto result = cheader::save(
                        config->output_path, planes.value(),
                        cmap_palette, config->mode, ch_opts);
                    if (!result) {
                        std::println(stderr, "Header write error: {}",
                                     result.error().message);
                        return exit_code::cant_create;
                    }
                    cli_status("Header: {}", config->output_path);
                } else {
                    std::println(stderr, "EHB copper: only .png, .iff, "
                                         ".h, and .cpp/.c output supported");
                    return exit_code::usage;
                }
            }

            // Mask export (EHB copper mode)
            if (!config->mask_path.empty())
                save_mask(config->mask_path, transparency_mask,
                          target_w, target_h, config->mask_invert, config->interlace);

            return 0;
        }

        // --- EHB without copper ---
        cli_status("Mode:   EHB (Extra Half-Brite)");

        // Validate locks/pins for EHB (target must be 0-31)
        if (auto v = palette_locks::validate_locks(config->locks, 32); !v) {
            std::println(stderr, "{}", v.error().message);
            return 1;
        }
        bool reserve_zero_ehb = !config->palette_file.empty() ? false : config->reserve_color0;
        if (auto v = palette_locks::validate_pins(config->pins, config->locks, 32,
                                                  image->width(), image->height(),
                                                  reserve_zero_ehb); !v) {
            std::println(stderr, "{}", v.error().message);
            return 1;
        }

        Palette base_pal;
        std::vector<bool> base_locked(32, false);
        if (!config->palette_file.empty()) {
            auto loaded = palette_io::load_palette(config->palette_file);
            if (!loaded) {
                std::println(stderr, "Palette load error: {}",
                             loaded.error().message);
                return 1;
            }
            base_pal = *std::move(loaded);
            if (base_pal.colors.size() > 32)
                base_pal.colors.resize(32);
            snap_palette(base_pal, chipset, config->mode);
            // Apply locks on top of user palette
            for (auto& lock : config->locks) {
                auto idx = static_cast<std::size_t>(lock.index);
                if (idx < base_pal.colors.size()) {
                    base_pal.colors[idx] = palette_locks::to_color(lock, chipset, config->mode);
                    base_locked[idx] = true;
                }
            }
            cli_status("Palette: {} colors loaded from {}",
                         base_pal.size(), config->palette_file);
        } else {
            // 32 base colors total: locks + reserved black at 0 + quantized fill
            auto qcount = palette_locks::quant_count(32, config->locks, true);
            auto quantized = auto_quantize(*image, qcount, chipset, config->palette_diversity, config->quantizer);
            if (!quantized) {
                std::println(stderr, "Quantize error: {}",
                             quantized.error().message);
                return 1;
            }
            auto assembled = palette_locks::assemble_locked_palette(
                *quantized, config->locks, 32, true, chipset, config->mode);
            base_pal = std::move(assembled.palette);
            base_locked = std::move(assembled.locked);
        }

        // Build full 64-color EHB palette
        auto ehb_pal = palette::make_ehb_palette(base_pal.colors);

        if (config->match_range) {
            preprocess::match_palette_range(*image, ehb_pal);
        }

        // Dither against all 64 colors
        dither::Settings dith;
        dith.method = config->dither_method;
        auto ehb_tune = dither_tuning::defaults_for(dither_tuning::Context{
            .mode    = config->mode,
            .depth   = static_cast<int>(config->depth),
            .dpf     = config->dual_playfield,
            .scap    = false,
            .copper  = false,
            .chipset = chipset,
        });
        dith.strength = config->dither_strength_explicit
                        ? config->dither_strength : ehb_tune.strength;
        dith.error_clamp = config->error_clamp_explicit
                          ? config->error_clamp : ehb_tune.error_clamp;

        // Note: dither-aware refinement is skipped for EHB because the
        // hardware-derived half-brite colors (sRGB DAC halving) create a
        // non-linear constraint that the linear centroid approach can't
        // capture correctly.

        cli_status("Palette: {} base + {} half-brite = {} colors",
                     base_pal.size(), base_pal.size(), ehb_pal.size());
        cli_status("Dither: {} (strength: {:.2f})",
                     dither_name(dith.method), dith.strength);

        auto dither_result = dither::apply(*image, ehb_pal.colors, dith);

        // Apply EHB pin-index swaps. Pins act on the BASE 32 only;
        // half-brite copies (32-63) auto-track via re-derivation.
        for (auto& pin : config->pins) {
            auto target = static_cast<std::size_t>(pin.index);
            auto pixel_offset = static_cast<std::size_t>(pin.y) * image->width() +
                                static_cast<std::size_t>(pin.x);
            if (pixel_offset >= dither_result.indices.size()) {
                std::println(stderr, "--pin-index-at {}: pixel out of bounds",
                             pin.index);
                return 1;
            }
            auto src = static_cast<std::size_t>(dither_result.indices[pixel_offset]);
            if (src >= 32) {
                std::println(stderr,
                    "--pin-index-at {}: source pixel ({},{}) dithered to "
                    "half-brite slot {} (EHB pins must land on a base color, slots 0-31)",
                    pin.index, pin.x, pin.y, src);
                return 1;
            }
            if (src == target) { base_locked[target] = true; continue; }
            if (base_locked[target]) {
                std::println(stderr, "--pin-index-at {}: target is locked", pin.index);
                return 1;
            }
            std::swap(base_pal.colors[src], base_pal.colors[target]);
            auto src_u8 = static_cast<std::uint8_t>(src);
            auto tgt_u8 = static_cast<std::uint8_t>(target);
            auto src_hb = static_cast<std::uint8_t>(src + 32);
            auto tgt_hb = static_cast<std::uint8_t>(target + 32);
            for (auto& idx : dither_result.indices) {
                if (idx == src_u8) idx = tgt_u8;
                else if (idx == tgt_u8) idx = src_u8;
                else if (idx == src_hb) idx = tgt_hb;
                else if (idx == tgt_hb) idx = src_hb;
            }
            base_locked[target] = true;
            ehb_pal = palette::make_ehb_palette(base_pal.colors);
        }

        // Encode to 6 bitplanes
        auto planes = bitplane::encode(dither_result.indices,
                                       image->width(), image->height(),
                                       ehb_depth);
        if (!planes) {
            std::println(stderr, "Encode error: {}", planes.error().message);
            return 1;
        }

        std::vector<Color3f> full_palette(ehb_pal.colors.begin(),
                                          ehb_pal.colors.end());

        // Render preview
        auto preview = bitplane::render(*planes, full_palette);
        if (!preview) {
            std::println(stderr, "Render error: {}", preview.error().message);
            return 1;
        }

        float ehb_psnr = color_space::compute_psnr_blurred(
            image->pixels(), preview->pixels(),
            image->width(), image->height());
        cli_print_encoded(
            static_cast<int>(planes->depth),
            static_cast<int>(planes->total_bytes()),
            static_cast<int>(ehb_pal.colors.size()),
            /*aga=*/false,
            /*cap=*/0, /*scap=*/0,
            static_cast<int>(planes->height), /*max_moves=*/0,
            static_cast<int>(count_unique_colors(*preview)),
            std::nullopt,
            static_cast<double>(dither_result.total_error), ehb_psnr);

        if (config->preview) show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;

                // IFF writer will trim palette to 32 base colors for EHB
                auto result = iff::save_ilbm(
                    config->output_path, planes.value(), full_palette,
                    config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("IFF:    {} ({} bytes)",
                             config->output_path, planes->total_bytes());
            } else if (ends_with(config->output_path, ".h")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;

                auto result = cheader::save(
                    config->output_path, planes.value(), full_palette,
                    config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "C header write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Header: {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                cheader::CHeaderOptions ch_opts2;
                ch_opts2.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts2.hires = config->hires;
                ch_opts2.interlace = config->interlace;
                ch_opts2.aga = (chipset == amiga::Chipset::aga);
                ch_opts2.fade_in = config->fade_in;

                pad_planes_to_mode(planes.value(), config->mode, config->hires);
                auto result2 = cheader::save_viewer(
                    config->output_path, planes.value(), full_palette,
                    config->mode, ch_opts2);
                if (!result2) {
                    std::println(stderr, "Viewer write error: {}",
                                 result2.error().message);
                    return 1;
                }
                cli_status("Viewer: {}", config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                save_raw(config->output_path, planes.value(),
                         full_palette, chipset);
            } else if (ends_with(config->output_path, ".pal")) {
                auto result = palette_io::save_ocs_palette(
                    config->output_path, full_palette);
                if (!result) {
                    std::println(stderr, "Palette write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Pal:    {} ({} colors, {} bytes)",
                             config->output_path, full_palette.size(),
                             full_palette.size() * 2);
            } else {
                auto result = save_preview(config->output_path, *preview,
                                           has_transparency, transparency_mask,
                                           config->mode, config->hires, config->interlace);
                if (!result) {
                    std::println(stderr, "PNG write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("PNG:    {}", config->output_path);
            }
        }

        // Mask export (EHB mode)
        if (!config->mask_path.empty())
            save_mask(config->mask_path, transparency_mask,
                      target_w, target_h, config->mask_invert, config->interlace);

        return 0;
    }

    // --- Copper palette mode ---
    if (config->copper) {
        if (!config->pins.empty()) {
            std::println(stderr, "Error: --pin-index-at "
                                 "is not supported with --copper");
            return 1;
        }
        // IFF output with copper is now supported via the PCHG chunk.

        // Force transparent pixels to black before encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        dither::Settings dith;
        dith.method = config->dither_method;
        auto cap_tune = dither_tuning::defaults_for(dither_tuning::Context{
            .mode    = config->mode,
            .depth   = static_cast<int>(config->depth),
            .dpf     = config->dual_playfield,
            .scap    = false,
            .copper  = true,
            .chipset = chipset,
        });
        dith.strength = config->dither_strength_explicit
                        ? config->dither_strength : cap_tune.strength;
        dith.error_clamp = config->error_clamp_explicit
                          ? config->error_clamp : cap_tune.error_clamp;

        cli_status("Dither: {} (strength: {:.2f})",
                     dither_name(dith.method), dith.strength);

        // Build locked slot list from --lock-index specs
        std::vector<std::pair<std::size_t, Color3f>> copper_locks;
        for (auto& lock : config->locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            copper_locks.emplace_back(idx,
                palette_locks::to_color(lock, chipset, config->mode));
        }

        std::size_t skip_initial_lace = config->interlace ? 2 : 0;
        auto copper_result = copper::encode_copper(*image, config->depth, dith, chipset,
            static_cast<std::size_t>(config->copper_changes), nullptr,
            config->reserve_color0, copper_locks, config->palette_diversity,
            skip_initial_lace, config->interlace, /*is_ehb=*/false,
            make_cli_progress_reporter());
        if (!copper_result) {
            std::println(stderr, "Copper encode error: {}",
                         copper_result.error().message);
            return 1;
        }
        // Print actual cpl after orchestration (auto mode may have stretched
        // or fallen back).
        cli_status("Mode:   CAP ({} changes/line, max {} MOVEs/line)",
                     copper_result->changes_per_line,
                     copper_result->max_moves_per_line);

        // Apply transparency mask: transparent pixels → index 0
        if (has_transparency) {
            auto decoded = bitplane::decode(copper_result->planes);
            if (decoded) {
                for (std::size_t i = 0; i < decoded->size() && i < transparency_mask.size(); ++i) {
                    if (transparency_mask[i]) (*decoded)[i] = 0;
                }
                auto re_encoded = bitplane::encode(*decoded,
                    copper_result->planes.width, copper_result->planes.height,
                    copper_result->planes.depth);
                if (re_encoded) copper_result->planes = *std::move(re_encoded);
            }
        }

        // Render preview BEFORE any DPF expansion: render_copper decodes
        // a combined index from all planes, which would land on
        // non-contiguous slots once PF1 zeros are interleaved.
        auto preview = copper::render_copper(copper_result->planes,
                                             copper_result->scanline_palettes);
        if (!preview) {
            std::println(stderr, "Render error: {}", preview.error().message);
            return 1;
        }

        // Dual-playfield expansion (copper path): expand to 2N planes with
        // PF1 zeroed, prepend pf2_base zero entries to base + per-scanline
        // palettes, and shift each copper write target register up by
        // pf2_base so writes land in the upper color registers (8-15 OCS /
        // 16-31 AGA).
        if (use_dpf_std) {
            auto expanded = bitplane::expand_to_dpf_pf2(copper_result->planes);
            if (!expanded) {
                std::println(stderr, "DPF expand error: {}",
                             expanded.error().message);
                return 1;
            }
            copper_result->planes = *std::move(expanded);
            auto pf2_base = std::size_t{1} << (copper_result->planes.depth / 2);
            auto shift_palette = [&](std::vector<Color3f>& p) {
                std::vector<Color3f> shifted(pf2_base, Color3f{0, 0, 0});
                shifted.insert(shifted.end(), p.begin(), p.end());
                p = std::move(shifted);
            };
            shift_palette(copper_result->base_palette);
            for (auto& pal : copper_result->scanline_palettes)
                shift_palette(pal);
            for (auto& line : copper_result->scanline_changes)
                for (auto& ch : line)
                    ch.reg = static_cast<std::uint8_t>(ch.reg + pf2_base);
            copper_result->num_colors += pf2_base;
        }

        // Use base palette for IFF CMAP
        std::vector<Color3f> cmap_palette = copper_result->base_palette;

        float cop_psnr = color_space::compute_psnr_blurred(
            image->pixels(), preview->pixels(),
            image->width(), image->height());
        int cop_cap_entries = static_cast<int>(
            copper_result->planes.height * copper_result->changes_per_line);
        cli_print_encoded(
            static_cast<int>(copper_result->planes.depth),
            static_cast<int>(copper_result->planes.total_bytes()),
            static_cast<int>(copper_result->base_palette.size()),
            chipset == amiga::Chipset::aga,
            cop_cap_entries, /*scap=*/0,
            static_cast<int>(copper_result->planes.height),
            static_cast<int>(copper_result->max_moves_per_line),
            static_cast<int>(count_unique_colors(*preview)),
            std::optional<float>{copper_result->avg_changes_per_line},
            static_cast<double>(copper_result->total_error), cop_psnr);

        if (config->preview) show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;
                iff_opts.has_transparency = has_transparency;
                iff_opts.scanline_palettes = &copper_result->scanline_palettes;
                iff_opts.dpf = use_dpf_std;

                auto result = iff::save_ilbm(
                    config->output_path, copper_result->planes,
                    cmap_palette, config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("IFF:    {} ({} bytes)",
                             config->output_path,
                             copper_result->planes.total_bytes());
            } else if (ends_with(config->output_path, ".h")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.dpf = use_dpf_std;
                ch_opts.copper_changes = &copper_result->scanline_changes;
                ch_opts.copper_changes_per_line = copper_result->changes_per_line;

                auto result = cheader::save(
                    config->output_path, copper_result->planes,
                    cmap_palette, config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "C header write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Header: {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                cheader::CHeaderOptions ch_opts2;
                ch_opts2.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts2.hires = config->hires;
                ch_opts2.interlace = config->interlace;
                ch_opts2.aga = (chipset == amiga::Chipset::aga);
                ch_opts2.fade_in = config->fade_in;
                ch_opts2.dpf = use_dpf_std;
                ch_opts2.copper_changes = &copper_result->scanline_changes;
                ch_opts2.copper_changes_per_line = copper_result->changes_per_line;

                pad_planes_to_mode(copper_result->planes, config->mode, config->hires);
                auto result2 = cheader::save_viewer(
                    config->output_path, copper_result->planes,
                    cmap_palette, config->mode, ch_opts2);
                if (!result2) {
                    std::println(stderr, "Viewer write error: {}",
                                 result2.error().message);
                    return 1;
                }
                cli_status("Viewer: {}", config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                save_raw(config->output_path, copper_result->planes,
                         cmap_palette, chipset,
                         &copper_result->scanline_changes,
                         copper_result->changes_per_line);
            } else {
                auto result = save_preview(config->output_path, *preview,
                                           has_transparency, transparency_mask,
                                           config->mode, config->hires, config->interlace);
                if (!result) {
                    std::println(stderr, "PNG write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("PNG:    {}", config->output_path);
            }
        }

        // Mask export (copper mode)
        if (!config->mask_path.empty())
            save_mask(config->mask_path, transparency_mask,
                      target_w, target_h, config->mask_invert, config->interlace);

        return 0;
    }

    // --- SCAP (mid-line palette swaps) ---
    // Two paths:
    //   * DPF (production): --scap + --dpf + OCS lores depth=3 → 8 PF2
    //     colours, full cpp viewer export.
    //   * Lores 5bpp (investigation): --scap + OCS lores depth=5 (no
    //     --dpf) → 32 colours, single playfield, PNG preview only,
    //     no copper-list cpp output yet.
    if (config->scap) {
        bool scap_ehb = config->mode == amiga::Mode::ehb;
        bool scap_dpf = use_dpf_std && config->mode == amiga::Mode::lores;
        if ((!scap_dpf && !scap_ehb) || chipset != amiga::Chipset::ocs ||
            config->interlace) {
            std::println(stderr,
                "Error: --scap requires OCS (no interlace) with either "
                "--dpf lores depth=3 or --mode ehb.");
            return 1;
        }
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        dither::Settings scap_dith;
        scap_dith.method = config->dither_method;
        // Per-mode dither defaults from dither_tuning.cpp's empirical
        // sweep — see that file for grid + numbers. Explicit user
        // values via --dither-strength / --error-clamp always override.
        auto tune = dither_tuning::defaults_for(dither_tuning::Context{
            .mode    = config->mode,
            .depth   = static_cast<int>(config->depth),
            .dpf     = config->dual_playfield,
            .scap    = true,
            .copper  = true,    // SCAP layers on top of CAP since b0be343
            .chipset = chipset,
        });
        scap_dith.error_clamp = config->error_clamp_explicit
                                ? config->error_clamp : tune.error_clamp;
        scap_dith.strength    = config->dither_strength_explicit
                                ? config->dither_strength : tune.strength;
        auto scap_progress = make_cli_progress_reporter();
        auto scap_res =
            scap_ehb
            ? scap::encode_scap_ehb_ocs(
                *image,
                static_cast<int>(image->width()),
                static_cast<int>(image->height()),
                config->reserve_color0,
                scap_dith,
                static_cast<std::size_t>(config->copper_changes),
                config->palette_diversity,
                config->scap_debug,
                scap_progress)
            : scap::encode_scap_dpf_ocs(
                *image,
                static_cast<int>(image->width()),
                static_cast<int>(image->height()),
                config->reserve_color0,
                scap_dith,
                config->scap_debug,
                static_cast<std::size_t>(config->copper_changes),
                config->palette_diversity,
                scap_progress);
        if (!scap_res) {
            std::println(stderr, "SCAP encode error: {}",
                         scap_res.error().message);
            return 1;
        }
        float scap_psnr = color_space::compute_psnr_blurred(
            image->pixels(), scap_res->rendered.pixels(),
            image->width(), image->height());
        const char* scap_label = scap_ehb
            ? "OCS EHB 6bpp investigation"
            : "OCS DPF";
        cli_status("Mode:   SCAP ({}, {} slots, {:.1f} useful swaps/line)",
                     scap_label,
                     scap_res->slot_table.slots.size(),
                     scap_res->avg_changes_per_line);
        cli_status("Copper load: hblank avg {:.1f} (max {}), visible avg "
                     "{:.1f} (max {}), total avg {:.1f} (max {}/line)",
                     scap_res->avg_hblank_moves_per_line,
                     scap_res->max_hblank_moves_per_line,
                     scap_res->avg_visible_moves_per_line,
                     scap_res->max_visible_moves_per_line,
                     scap_res->avg_total_moves_per_line,
                     scap_res->max_moves_per_line);
        int scap_ops = 0;
        for (auto& moves : scap_res->line_moves) scap_ops += static_cast<int>(moves.size());
        cli_print_encoded(
            static_cast<int>(scap_res->planes.depth),
            static_cast<int>(scap_res->planes.total_bytes()),
            static_cast<int>(scap_res->palette.size()),
            /*aga=*/false,
            /*cap_grid_entries=*/0,
            scap_ops,
            static_cast<int>(scap_res->rendered.height()),
            static_cast<int>(scap_res->max_moves_per_line),
            static_cast<int>(count_unique_colors(scap_res->rendered)),
            std::optional<float>{scap_res->avg_changes_per_line},
            static_cast<double>(scap_res->total_error), scap_psnr);

        if (config->preview)
            show_terminal_preview(scap_res->rendered, config->mode,
                                  config->hires, config->interlace);

        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".png")) {
                auto r = save_preview(config->output_path,
                                      scap_res->rendered,
                                      has_transparency, transparency_mask,
                                      config->mode, config->hires,
                                      config->interlace);
                if (!r) {
                    std::println(stderr, "PNG write error: {}",
                                 r.error().message);
                    return 1;
                }
                cli_status("PNG:    {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c") ||
                       ends_with(config->output_path, ".h")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.aga = false;
                ch_opts.dpf = scap_dpf;     // false for EHB SCAP
                ch_opts.fade_in = false;
                ch_opts.scap_line_moves = &scap_res->line_moves;
                ch_opts.scap_label = scap_ehb ? "scap_ehb_ocs"
                                              : "scap_dpf_ocs";
                ch_opts.scap_anchor_hpos =
                    scap_res->slot_table.line_gate_hpos;
                ch_opts.scap_total_planes =
                    scap_res->slot_table.total_planes;
                pad_planes_to_mode(scap_res->planes, config->mode,
                                   config->hires);
                bool is_h = ends_with(config->output_path, ".h");
                auto r = is_h
                    ? cheader::save(
                        config->output_path, scap_res->planes,
                        scap_res->palette, config->mode, ch_opts)
                    : cheader::save_viewer(
                        config->output_path, scap_res->planes,
                        scap_res->palette, config->mode, ch_opts);
                if (!r) {
                    std::println(stderr, "{} write error: {}",
                                 is_h ? "Header" : "Viewer",
                                 r.error().message);
                    return exit_code::cant_create;
                }
                cli_status("{}: {}", is_h ? "Header" : "Viewer",
                           config->output_path);
            } else {
                std::println(stderr,
                    "SCAP output: only .png, .h, and .cpp/.c supported");
                return exit_code::usage;
            }
        }
        return 0;
    }

    // --- Standard bitplane modes ---

    // Force transparent pixels to black before quantization/encoding
    if (has_transparency) {
        for (std::size_t i = 0; i < transparency_mask.size(); ++i)
            if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
    }

    // Build palette
    auto max_colors = std::size_t{1} << config->depth;
    auto is_atari_std = amiga::is_atari(config->mode);
    bool user_pal_std = !config->palette_file.empty();
    auto reserve_zero_std = !user_pal_std && config->reserve_color0 &&
                             (has_transparency || !is_atari_std);

    // Validate locks/pins
    if (auto v = palette_locks::validate_locks(config->locks, max_colors); !v) {
        std::println(stderr, "{}", v.error().message);
        return 1;
    }
    if (auto v = palette_locks::validate_pins(config->pins, config->locks, max_colors,
                                              image->width(), image->height(),
                                              reserve_zero_std); !v) {
        std::println(stderr, "{}", v.error().message);
        return 1;
    }

    Palette pal;
    std::vector<bool> std_locked(max_colors, false);

    // CGA: build the fixed 4- or 2-color palette (auto-select variant if asked).
    if (amiga::is_cga(config->mode) && !user_pal_std) {
        if (amiga::is_composite(config->mode)) {
            // Fixed 16-entry composite artifact palette. No variants to pick
            // from — the NTSC decoder produces these colors from the 4-bit
            // patterns regardless of any CGA register setting.
            auto pal16 = palette::cga_composite_palette();
            pal.colors.assign(pal16.begin(), pal16.end());
            cli_status("Palette: CGA composite, 16 colors (NTSC artifact, old CGA)");
        } else if (config->mode == amiga::Mode::cga_320) {
            palette::CgaPalette best = palette::CgaPalette::p1_high;
            if (config->cga_auto_palette) {
                // Try all 4 variants; pick the one whose 4-color palette best
                // covers the image (sum of per-pixel nearest-OKLab-distance).
                float best_err = std::numeric_limits<float>::infinity();
                for (auto p : {palette::CgaPalette::p0_low,
                               palette::CgaPalette::p0_high,
                               palette::CgaPalette::p1_low,
                               palette::CgaPalette::p1_high}) {
                    auto pal4 = palette::cga_build_palette(
                        p, static_cast<std::uint8_t>(config->cga_bg));
                    std::array<color_space::OKLab, 4> pal_lab;
                    for (std::size_t i = 0; i < 4; ++i)
                        pal_lab[i] = color_space::linear_to_oklab(pal4[i]);
                    float err = 0.0f;
                    for (std::size_t y = 0; y < image->height(); ++y) {
                        for (std::size_t x = 0; x < image->width(); ++x) {
                            auto lab = color_space::linear_to_oklab((*image)[x, y]);
                            float best_d = std::numeric_limits<float>::infinity();
                            for (auto& pl : pal_lab) {
                                float dL = lab.L - pl.L;
                                float da = lab.a - pl.a;
                                float db = lab.b - pl.b;
                                float d = dL*dL + da*da + db*db;
                                if (d < best_d) best_d = d;
                            }
                            err += best_d;
                        }
                    }
                    if (err < best_err) { best_err = err; best = p; }
                }
                config->cga_palette = static_cast<int>(best);
            } else {
                best = static_cast<palette::CgaPalette>(config->cga_palette);
            }
            auto pal4 = palette::cga_build_palette(
                best, static_cast<std::uint8_t>(config->cga_bg));
            pal.colors.assign(pal4.begin(), pal4.end());
            const char* names[] = {"0-low", "0-high", "1-low", "1-high"};
            cli_status("Palette: CGA {} (bg=0x{:X}), 4 colors{}",
                         names[static_cast<int>(best)],
                         config->cga_bg & 0xF,
                         config->cga_auto_palette ? " (auto)" : "");
        } else {
            // cga_640: 2 colors = bg + white
            pal.colors = {
                color_space::srgb_hex_to_linear(
                    palette::kCgaHw[config->cga_bg & 0xF]),
                color_space::srgb_hex_to_linear(palette::kCgaHw[15]),
            };
            cli_status("Palette: CGA mono, 2 colors (bg=0x{:X}, fg=white)",
                         config->cga_bg & 0xF);
        }
    } else if (user_pal_std) {
        auto loaded = palette_io::load_palette(config->palette_file);
        if (!loaded) {
            std::println(stderr, "Palette load error: {}", loaded.error().message);
            return 1;
        }
        pal = *std::move(loaded);
        if (pal.colors.size() > max_colors)
            pal.colors.resize(max_colors);
        snap_palette(pal, chipset, config->mode);
        // Apply locks on top of user palette
        for (auto& lock : config->locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            if (idx < pal.colors.size()) {
                pal.colors[idx] = palette_locks::to_color(lock, chipset, config->mode);
                std_locked[idx] = true;
            }
        }
        cli_status("Palette: {} colors (loaded from {})",
                     pal.size(), config->palette_file);
    } else if (amiga::is_atari_hi(config->mode)) {
        // Monochrome: fixed white + black palette
        pal.colors = {Color3f{1.0f, 1.0f, 1.0f}, Color3f{0.0f, 0.0f, 0.0f}};
        cli_status("Palette: 2 colors (monochrome)");
    } else if (config->mode == amiga::Mode::ega_320 ||
               config->mode == amiga::Mode::ega_640) {
        // EGA 200-line CGA-compat: palette order must be kCgaHw exactly.
        // The ATC register layout we emit (CGA-compat IRGB with b4 = I)
        // assumes palette slot i corresponds to kCgaHw[i]. Going through
        // assemble_locked_palette with reserve_color0 prepends a second
        // black at slot 0 and shifts the whole palette up by 1, dropping
        // white off the end — found via ATCPROBE/CGA16 test image.
        pal.colors.reserve(16);
        for (auto hex : palette::kCgaHw)
            pal.colors.push_back(color_space::srgb_hex_to_linear(hex));
        cli_status("Palette: 16 colors (kCgaHw, EGA CGA-compat IRGB)");
    } else {
        auto qcount = palette_locks::quant_count(max_colors, config->locks, reserve_zero_std);
        // For discrete-gamut modes (EGA 64-color, CGA, etc.), the continuous
        // quantizer centroids collapse to the same gamut slot when snapped —
        // that's why a 16-color EGA request can end up using only 7-8 colors.
        // Pre-snap the image to the target gamut so the quantizer's histogram
        // picks from already-discrete candidates, preserving distinct slots.
        std::optional<Image> snapped;
        const Image* quant_src = &*image;
        if (amiga::is_ega(config->mode)) {
            snapped.emplace(image->width(), image->height());
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x)
                    (*snapped)[x, y] = palette::quantize_to_ega((*image)[x, y]);
            quant_src = &*snapped;
        }
        auto quantized = auto_quantize(*quant_src, qcount, chipset,
                                       config->palette_diversity,
                                       config->quantizer, config->mode);
        if (!quantized) {
            std::println(stderr, "Quantize error: {}", quantized.error().message);
            return 1;
        }
        // Snap to mode precision if the quantizer didn't already do it.
        if (amiga::is_stf(config->mode) || amiga::is_vga(config->mode) ||
            amiga::is_ega(config->mode))
            snap_palette(*quantized, chipset, config->mode);
        auto assembled = palette_locks::assemble_locked_palette(
            *quantized, config->locks, max_colors, reserve_zero_std,
            chipset, config->mode);
        pal = std::move(assembled.palette);
        std_locked = std::move(assembled.locked);
        cli_status("Palette: {} colors (auto, {})",
                     pal.size(),
                     amiga::is_stf(config->mode) ? "STF 9-bit" :
                     amiga::is_vga(config->mode) ? "VGA 18-bit" :
                     amiga::is_ega(config->mode) ? "EGA 6-bit (2bpc)" :
                     chipset == amiga::Chipset::aga ? "median-cut" : "OCS brute-force");
    }

    if (config->match_range) {
        preprocess::match_palette_range(*image, pal);
    }

    // Apply dithering
    auto pal_size = std::min(pal.size(), max_colors);

    dither::Settings dith;
    dith.method = config->dither_method;
    dith.strength = config->dither_strength;
    dith.error_clamp = config->error_clamp;

    // Dither-aware palette refinement: iteratively run the ditherer,
    // recompute each slot's centroid from the pixels actually assigned
    // to it, and update the palette. Converges to a palette that's
    // optimal for the dithered output, not just nearest-color assignment.
    // Same rationale as api.cpp:
    //   - chunky VGA (256 colors): 18-bit DAC grid is fine enough that
    //     median-cut already lands on near-optimal centroids; refine hurts
    //     PSNR ~2 dB and adds 4× dither passes.
    //   - EGA: ega_histogram_quantize picks 16 distinct slots from the
    //     64-entry gamut. Refinement's snap-to-gamut collapses them to ~11
    //     effective colors and drops PSNR 3+ dB.
    //   - CGA: hardware-fixed palettes can't be reprogrammed.
    bool skip_refine = amiga::is_chunky(config->mode) ||
                       amiga::is_cga(config->mode) ||
                       amiga::is_ega(config->mode) ||
                       amiga::is_atari_hi(config->mode);
    if (config->refine_iterations > 0 && config->palette_file.empty() &&
        !skip_refine) {
        auto refined = quantize::refine_with_dither(
            *image,
            Palette{"refined", {pal.colors.begin(),
                                pal.colors.begin() + static_cast<std::ptrdiff_t>(pal_size)}},
            dith, chipset, config->mode,
            static_cast<std::size_t>(config->refine_iterations),
            std_locked);
        if (refined) {
            pal.colors = std::move(refined->colors);
            pal_size = pal.colors.size();
        }
    }

    std::span<const Color3f> pal_span{pal.colors.data(), pal_size};

    cli_status("Dither: {} (strength: {:.2f})",
                 dither_name(dith.method), dith.strength);

    auto dither_result = dither::apply(*image, pal_span, dith);

    // Apply transparency mask: transparent pixels → index 0
    if (has_transparency) {
        for (std::size_t i = 0; i < dither_result.indices.size(); ++i) {
            if (transparency_mask[i]) {
                dither_result.indices[i] = 0;
            }
        }
    }

    // Apply pin-index swaps (post-quantization, post-dither).
    if (!config->pins.empty()) {
        auto pin_result = palette_locks::apply_pins(
            pal, dither_result.indices, std_locked, config->pins,
            image->width(), image->height());
        if (!pin_result) {
            std::println(stderr, "{}", pin_result.error().message);
            return 1;
        }
    }

    // Encode to bitplanes (Atari uses word-interleaved layout)
    // Layout: Atari uses word-interleaved (ST-specific); DOS planar modes
    // (EGA, VGA 10h/12h) want plane-sequential `standard` so each plane can
    // be blitted contiguously via the sequencer map mask; Amiga defaults
    // to line-interleaved for DMA.
    bool dos_planar = (amiga::is_ega(config->mode) || amiga::is_vga(config->mode))
                      && !amiga::is_chunky(config->mode);
    auto bp_layout = config->layout_override
        ? *config->layout_override
        : (amiga::is_atari(config->mode)
            ? bitplane::Layout::word_interleaved
            : dos_planar ? bitplane::Layout::standard
                         : bitplane::Layout::interleaved);
    auto planes = bitplane::encode(dither_result.indices,
                                   image->width(), image->height(),
                                   config->depth, bp_layout);
    if (!planes) {
        std::println(stderr, "Encode error: {}", planes.error().message);
        return 1;
    }

    std::vector<Color3f> used_palette(pal_span.begin(), pal_span.end());

    // Render preview before any DPF expansion (combined indices read from
    // the expanded planes would be non-contiguous).
    auto preview = bitplane::render(*planes, used_palette);
    if (!preview) {
        std::println(stderr, "Render error: {}", preview.error().message);
        return 1;
    }

    // Dual-playfield expansion: image lives in PF2 of a 2N-plane display,
    // PF1 zeroed, palette shifted into upper color registers.
    if (use_dpf_std) {
        auto expanded = bitplane::expand_to_dpf_pf2(planes.value());
        if (!expanded) {
            std::println(stderr, "DPF expand error: {}",
                         expanded.error().message);
            return 1;
        }
        planes = *std::move(expanded);
        auto pf2_base = std::size_t{1} << (planes->depth / 2);
        std::vector<Color3f> shifted(pf2_base, Color3f{0, 0, 0});
        shifted.insert(shifted.end(), used_palette.begin(), used_palette.end());
        used_palette = std::move(shifted);
        for (auto& idx : dither_result.indices)
            idx = static_cast<std::uint8_t>(idx + pf2_base);

    }

    float std_psnr = color_space::compute_psnr_blurred(
        image->pixels(), preview->pixels(),
        image->width(), image->height());
    cli_print_encoded(
        static_cast<int>(planes->depth),
        static_cast<int>(planes->total_bytes()),
        static_cast<int>(pal.size()),
        chipset == amiga::Chipset::aga,
        /*cap=*/0, /*scap=*/0,
        static_cast<int>(planes->height), /*max_moves=*/0,
        static_cast<int>(count_unique_colors(*preview)),
        std::nullopt,
        static_cast<double>(dither_result.total_error), std_psnr);

    // Terminal preview
    if (config->preview) show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

    // Output
    if (!config->output_path.empty()) {
        if (ends_with(config->output_path, ".pi1") ||
            ends_with(config->output_path, ".pi2") ||
            ends_with(config->output_path, ".pi3")) {
            if (!amiga::is_atari(config->mode)) {
                std::println(stderr, "Error: Degas output requires an Atari ST/STE mode");
                return 1;
            }
            auto result = degas::save(config->output_path, planes.value(),
                                      used_palette, config->mode);
            if (!result) {
                std::println(stderr, "Degas write error: {}", result.error().message);
                return 1;
            }
            cli_status("Degas:  {} (32034 bytes)", config->output_path);
        } else if (ends_with(config->output_path, ".iff") ||
            ends_with(config->output_path, ".ilbm")) {
            iff::IffOptions iff_opts;
            iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;
            iff_opts.has_transparency = has_transparency;
            iff_opts.dpf = use_dpf_std;

            auto result = iff::save_ilbm(
                config->output_path, planes.value(), used_palette,
                config->mode, iff_opts);
            if (!result) {
                std::println(stderr, "IFF write error: {}", result.error().message);
                return 1;
            }
            cli_status("IFF:    {} ({} bytes)",
                         config->output_path, planes->total_bytes());
        } else if (ends_with(config->output_path, ".h")) {
            cheader::CHeaderOptions ch_opts;
            ch_opts.symbol_name = config->symbol_name.empty()
                ? derive_symbol_name(config->output_path)
                : config->symbol_name;
            ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.dpf = use_dpf_std;

            auto result = cheader::save(
                config->output_path, planes.value(), used_palette,
                config->mode, ch_opts);
            if (!result) {
                std::println(stderr, "C header write error: {}",
                             result.error().message);
                return 1;
            }
            cli_status("Header: {}", config->output_path);
        } else if (ends_with(config->output_path, ".cpp") ||
                   ends_with(config->output_path, ".c")) {
            auto symbol = config->symbol_name.empty()
                ? derive_symbol_name(config->output_path)
                : config->symbol_name;
            // DOS modes: two viewer generators.
            //   .c   → cheader_dos_c  (16-bit real-mode C for ia16-elf-gcc,
            //          runs on 8088 / IBM PC-XT with CGA; CGA graphics
            //          modes only for now).
            //   .cpp → cheader_dos    (32-bit DJGPP C++, needs 386+; covers
            //          all DOS modes including EGA/VGA).
            // Amiga modes use the existing AmigaOS viewer.
            bool want_c16 = ends_with(config->output_path, ".c");
            if (want_c16 && amiga::is_cga(config->mode)) {
                auto raw = cheader_dos_c::pack_cga_banked(
                    dither_result.indices, target_w, target_h, config->mode);
                std::uint8_t ctrl2 =
                    static_cast<std::uint8_t>((config->cga_palette & 0x3) << 4) |
                    static_cast<std::uint8_t>(config->cga_bg & 0xF);
                cheader_dos_c::Options opts{
                    .symbol_name = symbol,
                    .cga_mode_ctrl2 = ctrl2,
                };
                auto result = cheader_dos_c::save(
                    config->output_path, config->mode,
                    target_w, target_h, raw, {}, opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Viewer: {} (DOS/ia16-elf 16-bit, -march=i8086)",
                             config->output_path);
            } else if (want_c16 && (config->mode == amiga::Mode::ega_320 ||
                                    config->mode == amiga::Mode::ega_640 ||
                                    config->mode == amiga::Mode::ega_hi)) {
                // EGA planar → plane-sequential bytes (Layout::standard).
                pad_planes_to_mode(planes.value(), config->mode, config->hires);
                std::vector<std::uint8_t> pal_bytes;
                pal_bytes.reserve(16);
                if (config->mode == amiga::Mode::ega_hi) {
                    // ega_hi (mode 10h, 350-line): full 6-bit IrgbIRGB
                    // gamut via palette::linear_to_ega → ega_to_hw.
                    for (auto& c : used_palette) {
                        auto rrggbb = palette::linear_to_ega(c);
                        pal_bytes.push_back(palette::ega_to_hw(rrggbb));
                    }
                    while (pal_bytes.size() < 16) pal_bytes.push_back(0);
                } else {
                    // ega_320/640 (200-line): CGA-compat IRGB — the 5154
                    // gates ATC bits 5/3 off at 15.75 kHz. Palette locked
                    // to kCgaHw by main.cpp so slot i = IRGB color i.
                    for (std::size_t i = 0; i < 16; ++i) {
                        pal_bytes.push_back(static_cast<std::uint8_t>(
                            (i & 0x07) | ((i & 0x08) << 1)));
                    }
                }
                cheader_dos_c::Options opts{.symbol_name = symbol};
                auto result = cheader_dos_c::save(
                    config->output_path, config->mode,
                    target_w, target_h,
                    planes.value().data, pal_bytes, opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Viewer: {} (DOS/ia16-elf 16-bit, -march=i80286)",
                             config->output_path);
            } else if (want_c16 && (config->mode == amiga::Mode::vga_10h ||
                                    config->mode == amiga::Mode::vga_12h)) {
                // VGA planar 16-color (mode 10h / 12h). 18-bit DAC, not
                // EGA ATC IrgbIRGB. Emit 16 × 3 RGB bytes.
                pad_planes_to_mode(planes.value(), config->mode, config->hires);
                std::vector<std::uint8_t> dac;
                dac.reserve(48);
                std::size_t n = std::min<std::size_t>(used_palette.size(), 16);
                for (std::size_t i = 0; i < n; ++i) {
                    auto v = palette::linear_to_vga(used_palette[i]);
                    dac.push_back(static_cast<std::uint8_t>((v >> 16) & 0x3F));
                    dac.push_back(static_cast<std::uint8_t>((v >>  8) & 0x3F));
                    dac.push_back(static_cast<std::uint8_t>( v        & 0x3F));
                }
                while (dac.size() < 48) dac.push_back(0);
                cheader_dos_c::Options opts{.symbol_name = symbol};
                auto result = cheader_dos_c::save(
                    config->output_path, config->mode,
                    target_w, target_h,
                    planes.value().data, dac, opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Viewer: {} (DOS/ia16-elf 16-bit, -march=i80286)",
                             config->output_path);
            } else if (want_c16 && config->mode == amiga::Mode::vga_13h) {
                // VGA 256-color chunky. Build 768-byte DAC from linear
                // palette (3 bytes per entry × 256, each 6-bit). Entries
                // past the used palette are zero-filled (black).
                std::vector<std::uint8_t> dac(768, 0);
                std::size_t n = std::min<std::size_t>(used_palette.size(), 256);
                for (std::size_t i = 0; i < n; ++i) {
                    auto v = palette::linear_to_vga(used_palette[i]);
                    dac[i * 3 + 0] = static_cast<std::uint8_t>((v >> 16) & 0x3F);
                    dac[i * 3 + 1] = static_cast<std::uint8_t>((v >>  8) & 0x3F);
                    dac[i * 3 + 2] = static_cast<std::uint8_t>( v        & 0x3F);
                }
                std::vector<std::uint8_t> raw(dither_result.indices.begin(),
                                              dither_result.indices.end());
                cheader_dos_c::Options opts{.symbol_name = symbol};
                auto result = cheader_dos_c::save(
                    config->output_path, config->mode,
                    target_w, target_h, raw, dac, opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Viewer: {} (DOS/ia16-elf 16-bit, -march=i80286)",
                             config->output_path);
            } else if (want_c16) {
                std::println(stderr,
                    "16-bit C output (.c) supports: CGA graphics + "
                    "text80x100, EGA 320/640/hi, VGA 13h/10h/12h.");
                return 1;
            } else if (amiga::is_vga(config->mode) || amiga::is_ega(config->mode) ||
                amiga::is_cga(config->mode)) {
                std::println(stderr,
                    "DOS viewer output: use .c extension "
                    "(ia16-elf-gcc, 16-bit real mode).");
                return 1;
            } else {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = symbol;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.dpf = use_dpf_std;

                pad_planes_to_mode(planes.value(), config->mode, config->hires);
                auto result = cheader::save_viewer(
                    config->output_path, planes.value(), used_palette,
                    config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Viewer: {}", config->output_path);
            }
        } else if (ends_with(config->output_path, ".raw")) {
            if (amiga::is_chunky(config->mode)) {
                save_raw_vga(config->output_path,
                             dither_result.indices,
                             target_w, target_h,
                             used_palette, config->mode);
            } else if (amiga::is_vga_planar(config->mode)) {
                save_raw_vga_planar(config->output_path, planes.value(),
                                    used_palette, config->mode);
            } else if (amiga::is_ega(config->mode)) {
                save_raw_ega(config->output_path, planes.value(),
                             used_palette);
            } else if (amiga::is_cga(config->mode)) {
                save_raw_cga(config->output_path, dither_result.indices,
                             target_w, target_h, config->mode);
            } else {
                save_raw(config->output_path, planes.value(),
                         used_palette, chipset);
            }
        } else if (ends_with(config->output_path, ".pal")) {
            auto result = palette_io::save_ocs_palette(
                config->output_path, used_palette);
            if (!result) {
                std::println(stderr, "Palette write error: {}",
                             result.error().message);
                return 1;
            }
            cli_status("Pal:    {} ({} colors, {} bytes)",
                         config->output_path, used_palette.size(),
                         used_palette.size() * 2);
        } else {
            auto result = save_preview(config->output_path, *preview,
                                       has_transparency, transparency_mask,
                                       config->mode, config->hires, config->interlace);
            if (!result) {
                std::println(stderr, "PNG write error: {}", result.error().message);
                return 1;
            }
            cli_status("PNG:    {}", config->output_path);
        }
    }

    // Mask export (standard bitplane mode)
    if (!config->mask_path.empty())
        save_mask(config->mask_path, transparency_mask,
                  target_w, target_h, config->mask_invert, config->interlace);

    // CMake/Ninja depfile: input PNG + optional palette file are the
    // only external inputs we read. Output is the file we just wrote.
    // Emitted last so a partial file doesn't poison the build cache if
    // an earlier write failed.
    if (!config->depfile.empty() && !config->output_path.empty()) {
        std::array<std::string_view, 2> inputs{
            config->input_path,
            config->palette_file,
        };
        write_depfile(config->depfile, config->output_path, inputs);
    }

    // JSON status object on stdout (only field that survives --quiet).
    // Build systems can capture this via OUTPUT_VARIABLE / parse with
    // jq. Keys deliberately conservative — we ship only what is
    // available across every encode path; richer per-mode stats can
    // be added incrementally.
    if (config->json) {
        auto js_escape = [](std::string_view s) {
            std::string out;
            out.reserve(s.size() + 8);
            for (char c : s) {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (c == '\n') out += "\\n";
                else out += c;
            }
            return out;
        };
        std::println("{{\"input\":\"{}\",\"output\":\"{}\",\"status\":\"ok\"}}",
                     js_escape(config->input_path),
                     js_escape(config->output_path));
    }

    return exit_code::ok;
}
