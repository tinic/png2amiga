#include "amiga.hpp"
#include "api.hpp"
#include "c64_prg.hpp"
#include "snes_io.hpp"
#include "bitplane.hpp"
#include "cheader.hpp"
#include "cheader_dos_c.hpp"
#include "cheader_genesis.hpp"
#include "degas.hpp"
#include "color_space.hpp"
#include "console_color.hpp"
#include "cga_text.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "dither_tuning.hpp"
#include "scap.hpp"
#include "ssimulacra2.hpp"
#include "ham.hpp"
#include "iff.hpp"
#include "palette.hpp"
#include "palette_io.hpp"
#include "palette_locks.hpp"
#include "pipeline.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"
#include "version.hpp"

#include <constixel.hpp>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <print>
#include <unordered_set>
#include <string>
#include <string_view>
#include <thread>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
// POSIX guarantees this signature; some libcs (e.g. MSYS/Cygwin GCC under
// strict -std=c++26) hide ::fileno behind feature-test macros, so declare it
// explicitly. Harmless on macOS/Linux where it's already declared identically.
extern "C" int fileno(FILE*);
#else
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#endif
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
// Forward declaration; dither_name is defined far below in this TU.
const char* dither_name(dither::Method m);

// Centralised header-block helpers. Every encode path prints the same
// six-line block in the same order:
//
//   Input:   WxH
//   Target:  WxH @ D bitplanes
//   Chipset: <chipset>
//   Mode:    <mode-specific 1-line description>
//   Palette: <colors info>
//   Dither:  <name> (strength: X.XX)
//
// All headers align text at column 10 (8-char header + space).
inline void cli_print_input(std::size_t w, std::size_t h) {
    cli_status("Input:    {}x{}", w, h);
}
inline void cli_print_target(std::size_t w, std::size_t h, int depth) {
    cli_status("Target:   {}x{} @ {} bitplanes", w, h, depth);
}
inline void cli_print_chipset(amiga::Chipset cs) {
    cli_status("Chipset:  {}",
               cs == amiga::Chipset::aga ? "AGA (24-bit)" : "OCS (12-bit)");
}
inline void cli_print_mode(std::string_view mode_desc) {
    cli_status("Mode:     {}", mode_desc);
}
inline void cli_print_palette(std::string_view palette_desc) {
    cli_status("Palette:  {}", palette_desc);
}
inline void cli_print_dither(dither::Method method, float strength) {
    cli_status("Dither:   {} (strength: {:.2f})",
               dither_name(method), strength);
}

void cli_print_encoded(int depth, int plane_bytes, int palette_size,
                       bool aga, int cap_grid_entries, int scap_op_count,
                       int height, int max_moves, int num_colors,
                       std::optional<float> avg_cap,
                       double quant_error, float psnr, float s2) {
    auto sb = api::compute_size_breakdown(plane_bytes, palette_size, aga,
                                          cap_grid_entries, scap_op_count,
                                          height, max_moves);
    if (avg_cap) {
        cli_status("Encoded: {} bitplanes, disk: {}, chip: {}, {} colors, "
                   "{:.1f} avg CAP/line, error: {:.4f}, PSNR: {:.2f} dB, "
                   "S2: {:.2f}",
                   depth, fmt_size(sb.disk_bytes), fmt_size(sb.chip_bytes),
                   num_colors, *avg_cap, quant_error, psnr, s2);
    } else {
        cli_status("Encoded: {} bitplanes, disk: {}, chip: {}, {} colors, "
                   "error: {:.4f}, PSNR: {:.2f} dB, S2: {:.2f}",
                   depth, fmt_size(sb.disk_bytes), fmt_size(sb.chip_bytes),
                   num_colors, quant_error, psnr, s2);
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

// ---------------------------------------------------------------------------
// Batch mode helpers
// ---------------------------------------------------------------------------
//
// Build a horizontal atlas from N input PNGs so the encoder's quantizer +
// (when --cap) per-line copper planner see all frames simultaneously and
// produce ONE shared palette + ONE per-line plan covering every frame's
// row simultaneously. Per-frame outputs are then sliced from the atlas's
// bitplane data after the encoder runs (sharing CMAP/PCHG/copper symbols).

// Forward decl — defined below load_batch_inputs but referenced for the
// symbol-collision check. Sanitises a stem into a C identifier.
std::string sanitise_symbol(std::string_view s);

struct BatchInputFrame {
    std::string path;
    std::string stem;     // basename without extension — used for symbol names
    Image       image;
};

// Load all batch input PNGs and validate constraints:
//  * all frames same width AND height
//  * width % 16 == 0 (Amiga bitplane word alignment for clean per-frame slicing)
//  * unique stems (or hard error)
Result<std::vector<BatchInputFrame>> load_batch_inputs(
    std::span<const std::string> input_paths) {
    std::vector<BatchInputFrame> frames;
    frames.reserve(input_paths.size());
    std::unordered_set<std::string> seen_stems;
    std::unordered_set<std::string> seen_syms;
    int first_w = -1, first_h = -1;
    for (auto& p : input_paths) {
        auto img = png_io::load(p);
        if (!img) return std::unexpected{img.error()};
        std::filesystem::path fp(p);
        auto stem = fp.stem().string();
        if (stem.empty() || stem == "." || stem == "..") {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                std::format("batch: invalid empty/dot stem from path '{}' "
                            "(would yield bare '.iff' / unsafe output name)", p)}};
        }
        if (auto [_, inserted] = seen_stems.insert(stem); !inserted) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                std::format("batch: duplicate input stem '{}' "
                            "(would collide on output) from path '{}'",
                            stem, p)}};
        }
        // Symbol-collision check: distinct stems can collapse to the same
        // C identifier under sanitise_symbol (e.g. 'foo-1' and 'foo_1' both
        // → 'foo_1'). Catch here so the .h / .cpp emitters never produce
        // duplicate symbols.
        auto sym = sanitise_symbol(stem);
        if (auto [_, inserted] = seen_syms.insert(sym); !inserted) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                std::format("batch: stem '{}' (from '{}') sanitises to "
                            "C identifier '{}' which collides with a prior "
                            "frame; rename so symbols stay unique",
                            stem, p, sym)}};
        }
        int w = static_cast<int>(img->width());
        int h = static_cast<int>(img->height());
        if (first_w < 0) { first_w = w; first_h = h; }
        else if (w != first_w || h != first_h) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                std::format("batch: frame '{}' is {}x{}; expected {}x{} "
                            "(all frames must share dimensions)",
                            p, w, h, first_w, first_h)}};
        }
        if (w % 16 != 0) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                std::format("batch: frame width {} must be a multiple of 16 "
                            "(Amiga bitplane word alignment for per-frame "
                            "slicing); got '{}'", w, p)}};
        }
        frames.push_back(BatchInputFrame{p, std::move(stem), *std::move(img)});
    }
    if (frames.empty()) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            "batch: no input PNGs provided (expected positional args after --batch <dir>)"}};
    }
    return frames;
}

// Atlas gutter width (Amiga word units). Inserted to the left of every
// frame and after the last frame so error-diffusion dither errors flowing
// across frame boundaries hit edge-replicated padding instead of poisoning
// the next frame's leftmost pixels. 16 keeps atlas width word-aligned
// (frame_w is already validated as mult-of-16).
constexpr std::size_t kBatchGutter = 16;

// Concatenate frames horizontally into a single atlas image. All frames
// must share dimensions (validated by load_batch_inputs). Each frame is
// surrounded by `kBatchGutter`-wide gutters filled with that frame's
// nearest edge column (left gutter ← frame's left column, right gutter ←
// frame's right column). Error-diffusion dither errors flowing rightward
// across the frame's right edge dissipate into matching-color gutter
// pixels rather than landing on the next frame's leftmost real pixels.
// Serpentine scanning's left-flowing errors on odd rows behave the same
// way thanks to the symmetric gutter on the left.
Image build_atlas(std::span<const BatchInputFrame> frames) {
    auto frame_w = frames[0].image.width();
    auto frame_h = frames[0].image.height();
    auto atlas_w = frames.size() * (frame_w + kBatchGutter) + kBatchGutter;
    Image atlas(atlas_w, frame_h);
    for (std::size_t fi = 0; fi < frames.size(); ++fi) {
        auto frame_x0 = kBatchGutter + fi * (frame_w + kBatchGutter);
        auto& src = frames[fi].image;
        for (std::size_t y = 0; y < frame_h; ++y) {
            // Left gutter ← src column 0
            for (std::size_t g = 0; g < kBatchGutter; ++g)
                atlas[frame_x0 - kBatchGutter + g, y] = src[0, y];
            // Frame body
            for (std::size_t x = 0; x < frame_w; ++x)
                atlas[frame_x0 + x, y] = src[x, y];
            // Right gutter ← src column frame_w-1 (only the rightmost frame
            // writes the trailing gutter; intermediate gutters are written
            // by the next frame's left-gutter pass on the next iteration,
            // which overwrites these pixels with that frame's left edge —
            // intentional so the gutter between frames i and i+1 is
            // half-and-half: left half = frame i's right column, right
            // half = frame i+1's left column).
            for (std::size_t g = 0; g < kBatchGutter; ++g)
                atlas[frame_x0 + frame_w + g, y] = src[frame_w - 1, y];
        }
    }
    // Now overwrite the right half of each interior gutter with the next
    // frame's left edge. (The full-gutter write above leaves both halves
    // as frame i's right edge; here we replace the second half with frame
    // i+1's left edge.)
    auto half = kBatchGutter / 2;
    for (std::size_t fi = 0; fi + 1 < frames.size(); ++fi) {
        auto frame_x0 = kBatchGutter + fi * (frame_w + kBatchGutter);
        auto gutter_start = frame_x0 + frame_w + half;  // start of right half
        auto& next = frames[fi + 1].image;
        for (std::size_t y = 0; y < frame_h; ++y)
            for (std::size_t g = 0; g < half; ++g)
                atlas[gutter_start + g, y] = next[0, y];
    }
    return atlas;
}

// Sanitise a frame stem into a C identifier safe for symbol generation:
// spaces/hyphens/dots → underscores; leading digit → '_' prefix.
std::string sanitise_symbol(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 1);
    if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0])))
        out.push_back('_');
    for (auto c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
        else out.push_back('_');
    }
    if (out.empty()) out = "frame";
    return out;
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
// CLI-side config struct. Holds (a) the raw CLI parse target — output
// path, batch flags, preview flags, *_explicit metadata used by
// auto-tuning — and (b) a near-mirror of api::Options' encoder fields.
// api::Options (in api.hpp) is the canonical encoder schema; the bridge
// is make_api_options() below. Any new encoder knob should land in
// api::Options first, then get a CLI parse line that fills in the same
// field on this struct (or — when REFACTOR_PLAN.md target #5 finishes
// the Amiga branch migrations — directly on an embedded api::Options
// member). See REFACTOR_PLAN.md target #5.
struct Config {
    std::string input_path;
    std::string output_path;           // .png for preview, .iff for ILBM, .h for C header
    bool quiet = false;                // suppress all stdout status (errors → stderr)
    bool json = false;                 // emit JSON status object instead of human text
    std::string depfile;               // Make-format depfile path (empty = disabled)
    bool list_modes = false;           // emit mode catalog (human or JSON) and exit 0
    bool list_dithers = false;         // emit dither-method catalog and exit 0

    // Batch mode: N input PNGs encoded as a single horizontally-tiled
    // atlas so they share one palette and (if --cap) one copper plan,
    // then per-frame outputs are sliced from the atlas. Game-asset
    // pipelines for sprite sheets, animation frames, room layers etc.
    // Constraints: all frames same width/height, width % 16 == 0,
    // --scap rejected, --no-scale auto-implied.
    bool batch = false;
    std::string batch_output_dir;
    std::string batch_format = "h";        // iff, h, png, raw
    std::vector<std::string> batch_inputs; // populated from positional args

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

    // HAM op-selection metric. "oklab2" (default) optimises perceptual
    // uniformity (better SSIMULACRA2); "srgb-mse" optimises headline
    // PSNR. See api::Options.
    std::string ham_metric = "oklab2";

    // CAP best-quality planner. Multi-candidate × all-slot search + joint
    // base-palette refinement. HAM6 + copper and HAM8 + copper only —
    // indexed copper modes ignore this flag (their planner is already
    // mature). Adds ~+0.5-2 dB PSNR for ~4-5× the CAP-encoding cost.
    // Off by default — opt-in for offline / final exports.
    bool best = false;
    std::string best_metric = "ssimulacra2";  // "ssimulacra2" (default) | "msssim" | "psnr"

    // Palette diversity (ham_convert-style). 0 = off, 1-5 = progressively
    // aggressive removal of near-duplicate palette entries, re-seeded from
    // poorly-served image regions.
    int palette_diversity = 0;

    // Quantizer selection (empty = auto: OCS brute-force for OCS, median-cut
    // for AGA). "pnn" uses Pairwise Nearest Neighbor (experimental).
    std::string quantizer;

    // Dithering
    dither::Method dither_method = dither::Method::ostromoukhov;
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
    int cap_spread_radius = -1;        // -1 = use encode_copper default (4)
    float cap_spread_decay = -1.0f;    // -1 = use encode_copper default (0.85)
    bool fade_in = false;              // 16-step fade-in from black

    // CGA-specific options
    int cga_palette = 3;               // 0=P0-low, 1=P0-high, 2=P1-low, 3=P1-high (default)
    std::string c64_palette = "colodore"; // pepto/vice/colodore/deekay/godot/c64wiki/levy
    std::string c64_metric  = "mse";      // mse (OKLab²), blur (PN-sRGB), ssim (sRGB)
    bool c64_petscii_graphics_only = false;
    // Tile-based modes (c64 charset; SNES / Genesis / future Amiga 16x16
    // / PS1 64x64). 0 = mode default. >0 sets a custom dedup budget so
    // demos can ship larger glyph catalogues across charset banks.
    std::size_t tile_budget = 0;
    std::size_t tile_reserve = 0;
    int cga_bg = 0;                    // background color index (0..15 in master palette)
    bool cga_auto_palette = true;      // try all palettes, pick lowest-error

    // PAR handling: for modes with non-square hardware pixels (VGA 13h 1.2
    // tall, EGA 640x200 2x tall, etc.), --native-par preserves source aspect
    // on the target hardware display by letterboxing/pillarboxing into the
    // fixed buffer. Without the flag, modes with fixed dimensions stretch
    // source to fill the buffer (may look squished on CRT).
    bool native_par = false;
    bool lock_color0 = true;        // reserve index 0 for black (border)

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
    int  preview_scale = 0;            // 0 = auto (2× on iTerm.app, 1× elsewhere);
                                       // any value 1..8 forces that integer scale.
    bool  preview_video = false;       // batch: loop frames inline at preview_video_fps
    float preview_video_fps = 12.5f;   // default playback rate (8mm-ish)

    // Palette index manipulation
    std::vector<api::LockSpec> locks;     // --lock-index <id> <rgbhex>
    std::vector<api::PinSpec>  pins;      // --pin-index-at <id> <x> <y>
    std::vector<api::ReserveSpec> reserves;  // --reserve-range <range> <rgbhex>
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
    // Description column starts at byte 34 (32-char flag field + 2-space lead).
    // Keep flag descriptions to one line where possible; second line aligned
    // to column 34 for continuation. Run --list-modes for full mode catalog.
    std::println(stderr,
        "Modes:\n"
        "  --mode <mode>                   Graphics mode (default: lores)\n"
        "    Amiga:  lores | lores-lace | hires | hires-lace |\n"
        "            ham6[-hires][-lace] | ham8[-hires][-lace] | ehb[-lace]\n"
        "    Atari:  stf-low | stf-med | stf-hi | ste-low | ste-med | ste-hi\n"
        "    DOS:    vga-13h | vga-10h | vga-12h | ega-320 | ega-640 | ega-hi |\n"
        "            cga-320 | cga-640 | cga-composite | cga-text80x100\n"
        "    SNES:   snes-mode7-256 | snes-mode7-direct\n"
        "    Genesis: genesis-h32 | genesis-h40 | genesis-h32-sh | genesis-h40-sh\n"
        "    C64:    c64-multicolor | c64-hires | c64-fli | c64-afli |\n"
        "            c64-petscii | c64-charset-hires | c64-charset-multicolor\n"
        "  --depth <1-8>                   Bitplane depth (default: 5)\n"
        "  --chipset ocs|aga               Amiga chipset (default: auto)\n"
        "  --dual-playfield, --dpf         Encode into PF2 (depth 3 OCS / 4 AGA)\n"
        "  --width <int>                   Override output width\n"
        "  --height <int>                  Override output height\n"
        "  --no-scale                      Use source dimensions verbatim\n"
        "\n"
        "Dithering:\n"
        "  --dither <method>               Dither method (default: ostromoukhov;\n"
        "                                  --list-dithers for the full catalog)\n"
        "  --dither-strength <float>       Dither amount 0.0-2.0 (default: 1.0)\n"
        "  --error-clamp <float>           Max error per channel (default: 0.35)\n"
        "  --refine <0-32>                 Palette refinement iterations (default: 4)\n"
        "\n"
        "Palette:\n"
        "  --palette <file>                Load palette (.gpl, IFF, hex text)\n"
        "  --quantizer <name>              auto | median-cut | ocs-bruteforce | pnn\n"
        "  --palette-diversity <0-9>       Drop near-duplicate palette entries\n"
        "  --no-lock-color0                Allow palette index 0 to be image colour\n"
        "\n"
        "Image processing:\n"
        "  --brightness <float>            -1.0..1.0 (default: 0.0)\n"
        "  --contrast <float>              0.0..3.0 (default: 1.0)\n"
        "  --saturation <float>            0.0..3.0 (default: 1.0)\n"
        "  --gamma <float>                 0.1..8.0 (default: 1.0)\n"
        "  --hue-shift <float>             -180..180 degrees (default: 0)\n"
        "  --sharpen <float>               -1.0..2.0 (default: 0.0)\n"
        "  --black-point <float>           0.0..0.5 (default: 0.0)\n"
        "  --white-point <float>           0.0..0.5 (default: 0.0)\n"
        "  --match-range                   Stretch image range to palette extent\n"
        "  --crop <x,y,w,h>                Manual crop region (pixels)\n"
        "  --crop-auto                     Auto-crop to mode aspect ratio\n"
        "\n"
        "Transparency:\n"
        "  --alpha-threshold <-0.5..0.5>   Offset from 0.5 midpoint (default: 0)\n"
        "  --alpha-dither <method>         Dither alpha (default: none)\n"
        "  --alpha-dither-strength <float> Alpha dither strength (default: 1.0)\n"
        "  --mask <file>                   Export transparency mask\n"
        "  --mask-invert                   Invert mask polarity\n"
        "\n"
        "CAP — Copper-Augmented Palette (Amiga, per-line swaps):\n"
        "  --cap                           Per-scanline palette swaps\n"
        "  --cap-changes <0-16>            Swaps per line (0 = auto)\n"
        "  --best                          Multi-restart search (~20–30× slower)\n"
        "  --best-metric <m>               ssimulacra2 (default) | msssim | psnr\n"
        "\n"
        "SCAP — Super CAP (mid-line swaps, OCS lores):\n"
        "  --scap                          Mid-line swaps; pair with --dpf or ehb\n"
        "\n"
        "HAM:\n"
        "  --ham-beam <1-256>              DP search beam (default: 48)\n"
        "  --ham-triple <0-256>            Triple-pixel refinement (default: 16)\n"
        "  --ham-fast                      Greedy encoder (no DP search)\n"
        "  --ham-metric <oklab2|srgb-mse>  Op-selection metric (default: oklab2)\n"
        "\n"
        "Platform-specific:\n"
        "  --native-par                    Letterbox / pillarbox to preserve\n"
        "                                  source aspect on fixed-buffer hardware\n"
        "  --tile-budget <N>               Max unique tiles for charset / tile modes\n"
        "  --tile-reserve <N>              Reserve N tile slots from the budget\n"
        "  --cga-palette <p>               p0-low | p0-high | p1-low | p1-high\n"
        "  --cga-bg <0..15>                CGA background colour\n"
        "  --cga-text-metric <m>           blur (default) | mse\n"
        "  --c64-palette <p>               pepto | vice | colodore (default) |\n"
        "                                  deekay | godot | c64wiki | levy\n"
        "  --c64-metric <m>                mse (default) | blur\n"
        "  --c64-petscii-graphics          Restrict PETSCII to graphics glyphs\n"
        "\n"
        "Palette index pinning (lores/hires/EHB/Atari):\n"
        "  --lock-index <id> <rgbhex>      Lock palette slot to a specific colour\n"
        "                                  (quantizer may still pick this slot)\n"
        "  --reserve-range <range> <rgb>   Hard-reserve slots from the quantizer.\n"
        "                                  Range: 0,1,5-10 / -5 / 5- (open ends)\n"
        "  --pin-index-at <id> <x> <y>     Swap pixel (x,y)'s slot with <id>\n"
        "\n"
        "Output:\n"
        "  --symbol <name>                 Base symbol name (default: from filename)\n"
        "  --fade-in                       Fade in/out on viewer entry / exit\n"
        "  --layout <which>                auto | interleaved | standard |\n"
        "                                  word-interleaved\n"
        "  --non-interleaved, --planar     Alias for --layout standard\n"
        "  --interleaved                   Alias for --layout interleaved\n"
        "  --preview                       Show iTerm2 inline preview\n"
        "  --preview-scale <1-8>           Preview display scale\n"
        "  --preview-video                 Batch only: loop frames inline\n"
        "  --preview-video-fps <fps>       Playback rate (default 12.5)\n"
        "    Extensions: .png .iff .h .raw .pal .pi1 .pi2 .pi3 .prg .koa .hir\n"
        "\n"
        "Batch (multi-frame, shared palette / copper):\n"
        "  --batch <dir>                   Encode N inputs as a horizontal atlas;\n"
        "                                  emit per-frame outputs into <dir>\n"
        "  --batch-format <ext>            h (default) | iff | png | raw | cpp\n"
        "\n"
        "Build integration:\n"
        "  -q, --quiet                     Suppress stdout status (errors → stderr)\n"
        "  --json                          JSON status output (implies --quiet)\n"
        "  --depfile <path>                Write a Make-format depfile\n"
        "  --list-modes                    Print supported modes and exit\n"
        "  --list-dithers                  Print supported dither methods and exit\n"
        "\n"
        "Exit codes (sysexits.h):\n"
        "  0 ok    1 internal    64 usage    66 no input    73 cannot create\n");
}

Result<dither::Method> parse_dither_method(std::string_view s) {
    if (s == "none") return dither::Method::none;
    if (s == "bayer2x2") return dither::Method::bayer2x2;
    if (s == "bayer4x4") return dither::Method::bayer4x4;
    if (s == "bayer8x8") return dither::Method::bayer8x8;
    if (s == "bayer3x3") return dither::Method::bayer3x3;
    if (s == "bayer5x5") return dither::Method::bayer5x5;
    if (s == "bayer6x6") return dither::Method::bayer6x6;
    if (s == "bayer7x7") return dither::Method::bayer7x7;
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
    if (s == "void-cluster") return dither::Method::void_cluster;
    if (s == "cluster-noise") return dither::Method::cluster_noise;
    if (s == "fractal16") return dither::Method::fractal16;
    if (s == "floyd-steinberg") return dither::Method::floyd_steinberg;
    if (s == "atkinson") return dither::Method::atkinson;
    if (s == "sierra-lite") return dither::Method::sierra_lite;
    if (s == "stucki") return dither::Method::stucki;
    if (s == "jarvis") return dither::Method::jarvis;
    if (s == "ostromoukhov") return dither::Method::ostromoukhov;
    if (s == "dbs") return dither::Method::dbs;
    if (s == "gilbert") return dither::Method::gilbert;
    if (s == "riemersma") return dither::Method::riemersma;
    if (s == "structure-fs") return dither::Method::structure_fs;
    if (s == "contrast-fs") return dither::Method::contrast_fs;
    if (s == "zhoufang") return dither::Method::zhoufang;
    if (s == "yliluoma") return dither::Method::yliluoma;
    if (s == "yliluoma2") return dither::Method::yliluoma2;
    if (s == "opt-checker") return dither::Method::opt_checker;
    if (s == "knoll") return dither::Method::knoll;
    if (s == "tri-tone") return dither::Method::tri_tone;
    if (s == "yliluoma1") return dither::Method::yliluoma1;
    if (s == "opt-line") return dither::Method::opt_line;
    if (s == "opt-line-checker") return dither::Method::opt_line_checker;
    if (s == "aseprite-old") return dither::Method::aseprite_old;
    if (s == "libcaca3") return dither::Method::libcaca_3x3;
    if (s == "libcaca6") return dither::Method::libcaca_6x6;
    if (s == "pegasus") return dither::Method::pegasus_8x8;
    if (s == "cranley-bayer") return dither::Method::cranley_bayer;
    if (s == "quasicrystal") return dither::Method::quasicrystal;
    if (s == "truchet") return dither::Method::truchet;
    if (s == "ign") return dither::Method::ign;
    if (s == "ign-tri") return dither::Method::ign_triangle;
    if (s == "white-noise") return dither::Method::white_noise;
    if (s == "r2") return dither::Method::r2_sequence;
    if (s == "r2-tri") return dither::Method::r2_triangle;
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

        if (arg == "--best-metric") {
            if (i + 1 >= argc)
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--best-metric requires msssim, ssimulacra2, or psnr"}};
            std::string v = argv[++i];
            if (v != "msssim" && v != "psnr" && v != "ssimulacra2")
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--best-metric must be msssim, ssimulacra2, or psnr"}};
            config.best_metric = std::move(v);
            continue;
        }

        if (arg == "--preview-scale") {
            if (i + 1 >= argc)
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--preview-scale requires an integer argument"}};
            auto v = std::atoi(argv[++i]);
            if (v < 1 || v > 8)
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--preview-scale must be in 1..8"}};
            config.preview_scale = v;
            continue;
        }

        if (arg == "--preview-video") {
            config.preview_video = true;
            continue;
        }
        if (arg == "--preview-video-fps") {
            if (i + 1 >= argc)
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--preview-video-fps requires a numeric argument"}};
            config.preview_video_fps =
                std::strtof(argv[++i], nullptr);
            if (!(config.preview_video_fps > 0.0f))
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--preview-video-fps must be > 0"}};
            continue;
        }


        if (arg == "--no-lock-color0") {
            config.lock_color0 = false;
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

        if (arg == "--best") {
            // Best-quality multi-restart sweep. The encoder route is
            // selected by --cap / --scap; --best toggles the parallel
            // jitter pass on top of whichever encoder runs.
            config.best = true;
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

        if (arg == "--list-dithers") {
            config.list_dithers = true;
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

        if (arg == "--batch") {
            if (i + 1 >= argc) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--batch requires a directory argument"}};
            }
            config.batch = true;
            config.batch_output_dir = std::string(argv[++i]);
            // Force --no-scale: atlas dimensions must not be auto-resampled.
            config.no_scale = true;
            continue;
        }

        if (arg == "--batch-format") {
            if (i + 1 >= argc) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--batch-format requires an extension argument (h, iff, png, raw)"}};
            }
            config.batch_format = std::string(argv[++i]);
            // Strip leading '.' if present.
            if (!config.batch_format.empty() && config.batch_format[0] == '.')
                config.batch_format.erase(0, 1);
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

        if (arg == "--c64-petscii-graphics") {
            config.c64_petscii_graphics_only = true;
            continue;
        }
        if (arg == "--tile-budget" && i + 1 < argc) {
            auto v = std::string(argv[++i]);
            try {
                long n = std::stol(v);
                if (n < 1) {
                    return std::unexpected{Error{ErrorCode::unsupported_mode,
                        "--tile-budget must be ≥ 1"}};
                }
                config.tile_budget = static_cast<std::size_t>(n);
            } catch (...) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    std::format("--tile-budget: invalid integer '{}'", v)}};
            }
            continue;
        }
        if (arg == "--tile-reserve" && i + 1 < argc) {
            auto v = std::string(argv[++i]);
            try {
                long n = std::stol(v);
                if (n < 0) {
                    return std::unexpected{Error{ErrorCode::unsupported_mode,
                        "--tile-reserve must be ≥ 0"}};
                }
                config.tile_reserve = static_cast<std::size_t>(n);
            } catch (...) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    std::format("--tile-reserve: invalid integer '{}'", v)}};
            }
            continue;
        }
        if (arg == "--c64-metric" && i + 1 < argc) {
            auto v = std::string_view(argv[++i]);
            if (v != "blur" && v != "mse") {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    std::format("Unknown C64 metric: {} (expected blur/mse)", v)}};
            }
            config.c64_metric = std::string(v);
            continue;
        }

        if (arg == "--c64-palette" && i + 1 < argc) {
            auto v = std::string_view(argv[++i]);
            if (v != "pepto" && v != "vice" && v != "colodore" &&
                v != "deekay" && v != "godot" && v != "c64wiki" &&
                v != "wiki" && v != "levy") {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    std::format("Unknown C64 palette: {} (expected pepto/"
                                "vice/colodore/deekay/godot/c64wiki/levy)", v)}};
            }
            config.c64_palette = std::string(v == "wiki" ? "c64wiki" : v);
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

        // --reserve-range <ranges> <rgbhex>: hard-reserve palette slots.
        // Range form: "0,1,5-10" / "-5" (= 0..5) / "5-" (= 5..palette-end,
        // resolved per mode in the pipeline). Quantizer cannot place
        // image colours into reserved slots; the slots are filled with
        // the user-supplied default colour.
        if (arg == "--reserve-range" && i + 2 < argc) {
            auto ranges = std::string_view(argv[++i]);
            auto hex = std::string(argv[++i]);
            if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
            if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
                hex.erase(0, 2);
            if (hex.size() == 3) {
                std::string e;
                for (char c : hex) { e.push_back(c); e.push_back(c); }
                hex = e;
            }
            if (hex.size() != 6) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--reserve-range color must be a 3- or 6-digit hex value "
                    "(e.g. f0c or ff00cc)"}};
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
                    "--reserve-range color must be a hex value (e.g. ff00cc)"}};
            }
            // Token forms:
            //   "5"     → single index 5
            //   "5-10"  → 5..10 inclusive
            //   "-5"    → 0..5
            //   "5-"    → 5..255 (pipeline drops indices ≥ palette size)
            auto parse_int = [](std::string_view s) -> int {
                int v = 0; bool any = false;
                for (char c : s) {
                    if (c < '0' || c > '9') return -1;
                    v = v * 10 + (c - '0'); any = true;
                }
                return any ? v : -1;
            };
            std::size_t pos = 0;
            while (pos < ranges.size()) {
                auto next = ranges.find(',', pos);
                auto tok = (next == std::string_view::npos)
                    ? ranges.substr(pos)
                    : ranges.substr(pos, next - pos);
                pos = (next == std::string_view::npos) ? ranges.size() : next + 1;
                if (tok.empty()) continue;
                int lo = 0, hi = 0;
                auto dash = tok.find('-');
                if (dash == std::string_view::npos) {
                    lo = hi = parse_int(tok);
                } else if (dash == 0) {
                    lo = 0; hi = parse_int(tok.substr(1));
                } else if (dash == tok.size() - 1) {
                    lo = parse_int(tok.substr(0, dash)); hi = 255;
                } else {
                    lo = parse_int(tok.substr(0, dash));
                    hi = parse_int(tok.substr(dash + 1));
                }
                if (lo < 0 || hi < 0 || hi < lo || lo > 255 || hi > 255) {
                    return std::unexpected{Error{ErrorCode::unsupported_mode,
                        std::format("--reserve-range: bad range '{}' "
                                    "(use 0..255, e.g. 0,1,5-10)", tok)}};
                }
                for (int k = lo; k <= hi; ++k) {
                    config.reserves.push_back({k, r, g, b});
                }
            }
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
                else if (v == "snes-mode7-256" || v == "snes-256")
                    config.mode = amiga::Mode::snes_mode7_256;
                else if (v == "snes-mode7-direct" || v == "snes-direct" ||
                         v == "snes-mode7-rgb443")
                    config.mode = amiga::Mode::snes_mode7_direct;
                else if (v == "genesis-h32" || v == "md-h32")
                    config.mode = amiga::Mode::genesis_h32;
                else if (v == "genesis-h40" || v == "md-h40" ||
                         v == "genesis" || v == "megadrive")
                    config.mode = amiga::Mode::genesis_h40;
                else if (v == "genesis-h32-sh" || v == "md-h32-sh")
                    config.mode = amiga::Mode::genesis_h32_sh;
                else if (v == "genesis-h40-sh" || v == "md-h40-sh" ||
                         v == "genesis-sh" || v == "megadrive-sh")
                    config.mode = amiga::Mode::genesis_h40_sh;
                else if (v == "c64-multicolor" || v == "c64-mc" ||
                         v == "c64" || v == "vic2-multicolor")
                    config.mode = amiga::Mode::c64_multicolor;
                else if (v == "c64-hires" || v == "c64-hi" ||
                         v == "vic2-hires")
                    config.mode = amiga::Mode::c64_hires;
                else if (v == "c64-fli" || v == "fli")
                    config.mode = amiga::Mode::c64_fli;
                else if (v == "c64-afli" || v == "afli")
                    config.mode = amiga::Mode::c64_afli;
                else if (v == "c64-petscii" || v == "petscii")
                    config.mode = amiga::Mode::c64_petscii;
                else if (v == "c64-charset-hires" || v == "c64-charset-hi" ||
                         v == "charset-hires")
                    config.mode = amiga::Mode::c64_charset_hires;
                else if (v == "c64-charset-multicolor" ||
                         v == "c64-charset-mc" || v == "charset-multicolor")
                    config.mode = amiga::Mode::c64_charset_multicolor;
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
            else if (arg == "--ham-metric") {
                config.ham_metric = std::string(val);
                if (config.ham_metric != "srgb-mse" &&
                    config.ham_metric != "oklab2") {
                    return std::unexpected{Error{
                        ErrorCode::unsupported_mode,
                        std::format("Invalid --ham-metric '{}': use "
                                    "'srgb-mse' (default) or 'oklab2'",
                                    config.ham_metric)}};
                }
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
            else if (arg == "--cap-spread-radius") {
                config.cap_spread_radius = std::atoi(std::string(val).c_str());
            }
            else if (arg == "--cap-spread-decay") {
                config.cap_spread_decay = std::stof(std::string(val));
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
            if (config.batch) {
                // In batch mode every positional arg is an input PNG;
                // the output dir + format come from --batch / --batch-format.
                config.batch_inputs.emplace_back(arg);
            } else {
                if (positional == 0) config.input_path = std::string(arg);
                else if (positional == 1) config.output_path = std::string(arg);
            }
            ++positional;
        }
    }

    if (config.batch) {
        // Reject incompatible flags up front so the user sees the issue
        // before we do any work.
        if (config.scap) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "batch mode is incompatible with --scap (per-frame mid-line "
                "swap chains aren't shareable across frames)"}};
        }
        if (!config.batch_output_dir.empty()) {
            // Auto-create the output dir.
            std::error_code ec;
            std::filesystem::create_directories(config.batch_output_dir, ec);
            if (ec) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    std::format("batch: cannot create output dir '{}': {}",
                                config.batch_output_dir, ec.message())}};
            }
        }
        // Validate format.
        if (config.batch_format != "h" && config.batch_format != "iff" &&
            config.batch_format != "png" && config.batch_format != "raw" &&
            config.batch_format != "cpp") {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                std::format("batch: unknown --batch-format '{}' "
                            "(expected: h, iff, png, raw, cpp)",
                            config.batch_format)}};
        }
        if (config.batch_format == "cpp" && config.interlace) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "batch: --batch-format cpp does not support --interlace "
                "(multi-frame BPLxPT patching needs both fields' lists "
                "kept in sync; not implemented)"}};
        }
        // input_path is unused in batch mode — set to a dummy non-empty
        // value so the no-input gate below passes.
        if (config.input_path.empty()) config.input_path = "<batch>";
    }

    if (config.input_path.empty() && config.scap_probe.empty() &&
        !config.list_modes && !config.list_dithers) {
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
using pipeline::derive_symbol_name;

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

// Forward declaration: scale_preview() is defined later in this file
// (alongside the other display-scaling helpers), but inline_image_escape
// below needs it to pre-scale by g_preview_scale.
Image scale_preview(const Image& src, std::size_t sx, std::size_t sy);

// Display-scale multiplier applied to inline preview images. Resolved
// once in main() from --preview-scale (CLI override 1..8) or auto
// (2× on macOS iTerm/Kitty/Ghostty, 1× everywhere else — Linux HiDPI
// is rare enough not to be worth a default). Until main() sets it the
// fallback is 1 — safe for non-interactive paths.
unsigned g_preview_scale = 1;

// Which inline-image protocol does this terminal speak?
//   - iTerm: OSC 1337 ;File=inline=1; ... <BEL>. iTerm.app only —
//     Kitty literally dumps the base64 to screen, contrary to a
//     plausible "compat" assumption.
//   - Kitty: APC _G a=T,f=100, ... <ST>. Kitty's native protocol.
//     Ghostty implements Kitty graphics (alongside its own UI), no
//     OSC 1337 fallback. Kitty likewise does NOT speak OSC 1337.
//   - sixel: DEC sixel (\033Pq ... \033\\). Windows Terminal >= 1.22
//     speaks sixel and sets WT_SESSION; older WT versions just won't
//     render it. Detected by env var, no DA1 probe yet.
//   - none:  Anywhere else (Terminal.app, ssh w/o env passthrough,
//     non-image terminals). Skip the inline emit; preview becomes
//     a no-op.
enum class InlineImageProtocol { none, iterm, kitty, sixel };

InlineImageProtocol detect_inline_image_protocol() {
    auto eq = [](const char* a, const char* b) {
        return a && std::strcmp(a, b) == 0;
    };
    auto contains = [](const char* hay, const char* needle) {
        return hay && std::strstr(hay, needle) != nullptr;
    };
    auto set = [](const char* name) {
        auto v = std::getenv(name);
        return v && *v;
    };

    auto term = std::getenv("TERM");
    auto term_program = std::getenv("TERM_PROGRAM");

    // Kitty / Ghostty — both speak the Kitty graphics protocol.
    if (contains(term, "kitty"))         return InlineImageProtocol::kitty;
    if (contains(term, "ghostty"))       return InlineImageProtocol::kitty;
    if (set("KITTY_WINDOW_ID"))          return InlineImageProtocol::kitty;
    if (eq(term_program, "ghostty"))     return InlineImageProtocol::kitty;
    if (set("GHOSTTY_RESOURCES_DIR"))    return InlineImageProtocol::kitty;

    // iTerm.app — OSC 1337 only.
    if (eq(term_program, "iTerm.app"))   return InlineImageProtocol::iterm;

    // Windows Terminal — sixel since 1.22. WT_SESSION is set on every
    // pane; we don't probe DA1 because the env var is sufficient and
    // the probe needs raw-mode stdin handling on Windows.
    if (set("WT_SESSION"))               return InlineImageProtocol::sixel;

    return InlineImageProtocol::none;
}

unsigned resolve_preview_scale(int user_override) {
    if (user_override >= 1 && user_override <= 8)
        return static_cast<unsigned>(user_override);
#ifdef __APPLE__
    // HiDPI is the default on Apple laptops since 2012; doubling on a
    // recognised inline-image terminal makes preview readable. Other
    // platforms default to 1× — Linux + Windows HiDPI exists but isn't
    // common enough to assume; users on those setups can pass
    // --preview-scale 2 explicitly.
    if (detect_inline_image_protocol() != InlineImageProtocol::none) return 2;
#endif
    return 1;
}

// Build the inline-image escape sequence for the current terminal.
// Pre-scales the image by g_preview_scale so both protocols can use
// their native-pixel render path uniformly: iTerm renders OSC `Npx`
// at device pixels, Kitty renders the PNG at its encoded pixel size,
// so a pre-scaled image looks the right visual size on either path
// without per-protocol display-size arithmetic. Returns empty string
// when the terminal speaks neither protocol — caller suppresses the
// preview emit.
std::string inline_image_escape(const Image& image) {
    auto proto = detect_inline_image_protocol();
    if (proto == InlineImageProtocol::none) return {};

    Image scaled = (g_preview_scale > 1)
        ? scale_preview(image, g_preview_scale, g_preview_scale)
        : image;

    if (proto == InlineImageProtocol::sixel) {
        // Convert linear-RGB Image to sRGB RGBA bytes. Then derive a
        // custom palette from the unique colors present in the rendered
        // preview — this is the encoded image's actual palette (post-
        // dither), so the sixel maps each pixel to an exact match. If
        // there are >256 unique colors (HAM modes), fall back to
        // constixel's built-in 256-color quantizer.
        // Sixel emits in 6-pixel-tall bands; pad height up to the next
        // multiple of 6 by replicating the last row so the trailing
        // band has defined content (avoids a terminal-bg-color stripe).
        const auto w = scaled.width();
        const auto orig_h = scaled.height();
        const auto h = ((orig_h + 5) / 6) * 6;
        std::vector<std::uint8_t> rgba(w * h * 4);
        std::unordered_set<std::uint32_t> unique_colors;
        unique_colors.reserve(512);
        bool overflow = false;
        for (std::size_t y = 0; y < orig_h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto srgb = color_space::linear_to_srgb(scaled[x, y]).clamped();
                auto base = (y * w + x) * 4;
                auto r = static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f));
                auto g = static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f));
                auto b = static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f));
                rgba[base + 0] = r;
                rgba[base + 1] = g;
                rgba[base + 2] = b;
                rgba[base + 3] = 255;
                if (!overflow) {
                    unique_colors.insert(
                        (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | std::uint32_t(b));
                    if (unique_colors.size() > 256) overflow = true;
                }
            }
        }
        if (h > orig_h && orig_h > 0) {
            const auto* src = &rgba[(orig_h - 1) * w * 4];
            for (std::size_t y = orig_h; y < h; ++y) {
                std::memcpy(&rgba[y * w * 4], src, w * 4);
            }
        }
        std::string out;
        out.reserve(w * h);
        auto sink = [&](char c) { out.push_back(c); };
        std::vector<std::array<std::uint8_t, 3>> pal;
        if (!overflow) {
            pal.reserve(unique_colors.size());
            for (auto c : unique_colors) {
                pal.push_back({static_cast<std::uint8_t>((c >> 16) & 0xFF),
                               static_cast<std::uint8_t>((c >> 8) & 0xFF),
                               static_cast<std::uint8_t>(c & 0xFF)});
            }
        } else {
            // >256 unique colors (HAM6/8): use png2amiga's median-cut to
            // build a 256-color palette adapted to the rendered preview.
            // Better than constixel's generic 256-color built-in.
            // 255 not 256: constixel's runtime_palette reserves index 0
            // as a sentinel (sixel #0 is transparent under P2=1).
            auto pal_result = quantize::quantize(scaled, 255, quantize::Algorithm::median_cut);
            if (pal_result) {
                pal.reserve(pal_result->size());
                for (const auto& col : pal_result->colors) {
                    auto srgb = color_space::linear_to_srgb(col).clamped();
                    pal.push_back({static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f)),
                                   static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f)),
                                   static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f))});
                }
            }
        }
        if (!pal.empty()) {
            constixel::dynamic_image<constixel::format_8bit_dyn> dimg(
                w, h, std::span<const std::array<std::uint8_t, 3>>(pal));
            dimg.blit_RGBA(0, 0, static_cast<std::int32_t>(w), static_cast<std::int32_t>(h),
                           rgba.data(), static_cast<std::int32_t>(w), static_cast<std::int32_t>(h),
                           static_cast<std::int32_t>(w * 4));
            dimg.sixel(sink);
        } else {
            constixel::dynamic_image<constixel::format_8bit_dyn> dimg(w, h);
            dimg.blit_RGBA(0, 0, static_cast<std::int32_t>(w), static_cast<std::int32_t>(h),
                           rgba.data(), static_cast<std::int32_t>(w), static_cast<std::int32_t>(h),
                           static_cast<std::int32_t>(w * 4));
            dimg.sixel(sink);
        }
        return out;
    }

    auto png = png_io::encode(scaled);
    if (!png) return {};
    auto encoded = base64_encode(*png);

    if (proto == InlineImageProtocol::iterm) {
        return std::format(
            "\033]1337;File=inline=1;size={};width={}px;height={}px:{}\a",
            png->size(), scaled.width(), scaled.height(), encoded);
    }

    // Kitty graphics protocol — same chunking pattern as constixel.hpp's
    // png_to_kitty(), with q=2 added to suppress OK / failure responses
    // from the terminal (otherwise they'd land on stdin).
    //   First chunk:      \033_Ga=T,f=100,q=2,m=<0|1>;<base64><\033\\>
    //   Subsequent:       \033_Gm=<0|1>;<base64><\033\\>
    //   m=0 → final chunk; m=1 → more to come. Per spec all chunks but
    //   the last must be ≤4096 bytes of base64.
    std::string out;
    constexpr std::size_t kMaxChunk = 4096;
    bool first = true;
    std::size_t pos = 0;
    while (pos < encoded.size()) {
        auto bytes = std::min(encoded.size() - pos, kMaxChunk);
        bool last = (pos + bytes == encoded.size());
        out.append(first ? "\033_Ga=T,f=100,q=2,"  : "\033_G");
        first = false;
        out.append(last  ? "m=0;" : "m=1;");
        out.append(encoded, pos, bytes);
        out.append("\033\\");
        pos += bytes;
    }
    return out;
}

void iterm2_display(const Image& image) {
    auto seq = inline_image_escape(image);
    if (!seq.empty()) cli_status("{}", seq);
}

const char* dither_name(dither::Method m) {
    switch (m) {
    case dither::Method::none: return "none";
    case dither::Method::bayer2x2: return "bayer2x2";
    case dither::Method::bayer4x4: return "bayer4x4";
    case dither::Method::bayer8x8: return "bayer8x8";
    case dither::Method::bayer3x3: return "bayer3x3";
    case dither::Method::bayer5x5: return "bayer5x5";
    case dither::Method::bayer6x6: return "bayer6x6";
    case dither::Method::bayer7x7: return "bayer7x7";
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
    case dither::Method::void_cluster: return "void-cluster";
    case dither::Method::cluster_noise: return "cluster-noise";
    case dither::Method::fractal16: return "fractal16";
    case dither::Method::floyd_steinberg: return "floyd-steinberg";
    case dither::Method::atkinson: return "atkinson";
    case dither::Method::sierra_lite: return "sierra-lite";
    case dither::Method::stucki: return "stucki";
    case dither::Method::jarvis: return "jarvis";
    case dither::Method::ostromoukhov: return "ostromoukhov";
    case dither::Method::dbs: return "dbs";
    case dither::Method::gilbert: return "gilbert";
    case dither::Method::riemersma: return "riemersma";
    case dither::Method::structure_fs: return "structure-fs";
    case dither::Method::contrast_fs: return "contrast-fs";
    case dither::Method::zhoufang: return "zhoufang";
    case dither::Method::yliluoma: return "yliluoma";
    case dither::Method::yliluoma2: return "yliluoma2";
    case dither::Method::opt_checker: return "opt-checker";
    case dither::Method::knoll: return "knoll";
    case dither::Method::tri_tone: return "tri-tone";
    case dither::Method::yliluoma1: return "yliluoma1";
    case dither::Method::opt_line: return "opt-line";
    case dither::Method::opt_line_checker: return "opt-line-checker";
    case dither::Method::aseprite_old: return "aseprite-old";
    case dither::Method::libcaca_3x3: return "libcaca3";
    case dither::Method::libcaca_6x6: return "libcaca6";
    case dither::Method::pegasus_8x8: return "pegasus";
    case dither::Method::cranley_bayer: return "cranley-bayer";
    case dither::Method::quasicrystal: return "quasicrystal";
    case dither::Method::truchet: return "truchet";
    case dither::Method::ign: return "ign";
    case dither::Method::ign_triangle: return "ign-tri";
    case dither::Method::white_noise: return "white-noise";
    case dither::Method::r2_sequence: return "r2";
    case dither::Method::r2_triangle: return "r2-tri";
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
    return pipeline::resolve_chipset(cfg.chipset, cfg.mode);
}

// Mode → api::Options.mode string. api::parse_mode is the inverse used
// by run_pipeline / encode_state. Modes not listed fall back to "lores"
// — callers that care (SNES/Genesis) override explicitly post-build.
std::string_view mode_to_options_string(amiga::Mode m) {
    switch (m) {
    case amiga::Mode::lores:            return "lores";
    case amiga::Mode::lores_interlace:  return "lores-lace";
    case amiga::Mode::hires:            return "hires";
    case amiga::Mode::hires_interlace:  return "hires-lace";
    case amiga::Mode::ham6:             return "ham6";
    case amiga::Mode::ham8:             return "ham8";
    case amiga::Mode::ehb:              return "ehb";
    case amiga::Mode::stf_low:          return "stf-low";
    case amiga::Mode::stf_med:          return "stf-med";
    case amiga::Mode::stf_hi:           return "stf-hi";
    case amiga::Mode::ste_low:          return "ste-low";
    case amiga::Mode::ste_med:          return "ste-med";
    case amiga::Mode::ste_hi:           return "ste-hi";
    default: return "lores";
    }
}

// dither::Method → api::Options.dither string. api::parse_dither is the
// inverse. Falls back to "floyd-steinberg" for unmapped methods.
// Mirror of api::parse_dither — must stay in sync. Any method exposed
// to --dither MUST round-trip through this so best / SCAP / SNES /
// Genesis paths that route through api::encode_state see the user's
// actual choice (silent fallback to floyd-steinberg here was a real
// bug that hid opt-checker / yliluoma / structure-fs / knoll / etc.).
std::string_view dither_to_options_string(dither::Method m) {
    switch (m) {
    case dither::Method::none:            return "none";
    case dither::Method::bayer2x2:        return "bayer2x2";
    case dither::Method::bayer4x4:        return "bayer4x4";
    case dither::Method::bayer8x8:        return "bayer8x8";
    case dither::Method::bayer3x3:        return "bayer3x3";
    case dither::Method::bayer5x5:        return "bayer5x5";
    case dither::Method::bayer6x6:        return "bayer6x6";
    case dither::Method::bayer7x7:        return "bayer7x7";
    case dither::Method::checker:         return "checker";
    case dither::Method::h2x4:            return "h2x4";
    case dither::Method::clustered_dot:   return "clustered-dot";
    case dither::Method::line2:           return "line2";
    case dither::Method::line_checker:    return "line-checker";
    case dither::Method::line4:           return "line4";
    case dither::Method::line8:           return "line8";
    case dither::Method::v4x2:            return "v4x2";
    case dither::Method::bayer4x2:        return "bayer4x2";
    case dither::Method::bayer2x4:        return "bayer2x4";
    case dither::Method::vline2:          return "vline2";
    case dither::Method::vline_checker:   return "vline-checker";
    case dither::Method::vline4:          return "vline4";
    case dither::Method::vline8:          return "vline8";
    case dither::Method::halftone8x8:     return "halftone8x8";
    case dither::Method::diagonal8x8:     return "diagonal8x8";
    case dither::Method::spiral5x5:       return "spiral5x5";
    case dither::Method::hex8x8:          return "hex8x8";
    case dither::Method::hex5x5:          return "hex5x5";
    case dither::Method::blue_noise:      return "blue-noise";
    case dither::Method::void_cluster:    return "void-cluster";
    case dither::Method::cluster_noise:   return "cluster-noise";
    case dither::Method::fractal16:       return "fractal16";
    case dither::Method::floyd_steinberg: return "floyd-steinberg";
    case dither::Method::atkinson:        return "atkinson";
    case dither::Method::sierra_lite:     return "sierra-lite";
    case dither::Method::stucki:          return "stucki";
    case dither::Method::jarvis:          return "jarvis";
    case dither::Method::ostromoukhov:    return "ostromoukhov";
    case dither::Method::dbs:             return "dbs";
    case dither::Method::gilbert:         return "gilbert";
    case dither::Method::riemersma:       return "riemersma";
    case dither::Method::structure_fs:    return "structure-fs";
    case dither::Method::contrast_fs:     return "contrast-fs";
    case dither::Method::zhoufang:        return "zhoufang";
    case dither::Method::yliluoma:        return "yliluoma";
    case dither::Method::yliluoma2:       return "yliluoma2";
    case dither::Method::opt_checker:     return "opt-checker";
    case dither::Method::knoll:           return "knoll";
    case dither::Method::tri_tone:        return "tri-tone";
    case dither::Method::yliluoma1:       return "yliluoma1";
    case dither::Method::opt_line:        return "opt-line";
    case dither::Method::opt_line_checker:return "opt-line-checker";
    case dither::Method::aseprite_old:    return "aseprite-old";
    case dither::Method::libcaca_3x3:     return "libcaca3";
    case dither::Method::libcaca_6x6:     return "libcaca6";
    case dither::Method::pegasus_8x8:     return "pegasus";
    case dither::Method::cranley_bayer:   return "cranley-bayer";
    case dither::Method::quasicrystal:    return "quasicrystal";
    case dither::Method::truchet:         return "truchet";
    case dither::Method::ign:             return "ign";
    case dither::Method::ign_triangle:    return "ign-tri";
    case dither::Method::white_noise:     return "white-noise";
    case dither::Method::r2_sequence:     return "r2";
    case dither::Method::r2_triangle:     return "r2-tri";
    case dither::Method::crosshatch:      return "crosshatch";
    case dither::Method::radial:          return "radial";
    case dither::Method::value_noise:     return "value-noise";
    }
    return "floyd-steinberg";
}

// Build an api::Options from a CLI Config. Single source of truth for
// the field-by-field translation that previously lived inline at every
// api::encode_state call site (batch atlas, SNES Mode 7, Genesis tile
// pipeline). Caller may still override any field after the call —
// notably opts.mode for SNES/Genesis where the string isn't covered by
// mode_to_options_string, and opts.width/height when the source has
// already been scaled to a non-default size.
// Zero out the preprocess fields on an api::Options so a downstream
// api::encode_state / run_pipeline call won't apply preprocess again.
//
// CLI paths that re-encode an already-preprocessed Image to PNG bytes
// (SNES, Genesis, SCAP, batch atlas) previously passed make_api_options()
// straight through — opts carried Config's gamma / brightness /
// contrast / saturation / hue_shift / sharpen / black_point /
// white_point / match_range, and run_pipeline preprocessed those bytes
// AGAIN. For default Config values the second pass is a no-op; with
// explicit user preprocess flags the result was applied twice (e.g.
// --gamma 1.5 → ~1.5² ≈ 2.25 effective). Call this helper right after
// make_api_options() at any site that has already run preprocess locally.
void neutralize_preprocess(api::Options& opts) {
    opts.gamma = 1.0f;
    opts.brightness = 0.0f;
    opts.contrast = 1.0f;
    opts.saturation = 1.0f;
    opts.hue_shift = 0.0f;
    opts.sharpen = 0.0f;
    opts.black_point = 0.0f;
    opts.white_point = 0.0f;
    opts.match_range = false;
}

api::Options make_api_options(const Config& cfg) {
    api::Options opts;
    opts.mode = std::string{mode_to_options_string(cfg.mode)};
    if (cfg.chipset)
        opts.chipset = (*cfg.chipset == amiga::Chipset::aga) ? "aga" : "ocs";
    opts.depth = static_cast<int>(cfg.depth);
    opts.interlace = cfg.interlace;
    opts.gamma = cfg.preprocess.gamma;
    opts.brightness = cfg.preprocess.brightness;
    opts.contrast = cfg.preprocess.contrast;
    opts.saturation = cfg.preprocess.saturation;
    opts.hue_shift = cfg.preprocess.hue_shift;
    opts.sharpen = cfg.preprocess.sharpen;
    opts.black_point = cfg.preprocess.black_point;
    opts.white_point = cfg.preprocess.white_point;
    opts.match_range = cfg.match_range;
    opts.dither = std::string{dither_to_options_string(cfg.dither_method)};
    // Per-mode dither auto-tuning. Inline Amiga branches (HAM, EHB,
    // plain CAP, plain Amiga fallback) all call dither_tuning::defaults_for
    // and override dither_strength/error_clamp when the user didn't
    // pass them explicitly. Migrating any of those branches to
    // api::encode_state without applying the same tuning here would
    // silently regress CLI-side defaults. Apply at the bridge so any
    // downstream encode_state caller gets the same tuned values the
    // inline branches use.
    auto tune_ctx = dither_tuning::Context{
        .mode    = cfg.mode,
        .depth   = static_cast<int>(cfg.depth),
        .dpf     = cfg.dual_playfield,
        .scap    = cfg.scap,
        .copper  = cfg.copper,
        .chipset = pipeline::resolve_chipset(cfg.chipset, cfg.mode),
        .method  = cfg.dither_method,
    };
    auto tune = dither_tuning::defaults_for(tune_ctx);
    opts.dither_strength = cfg.dither_strength_explicit
        ? cfg.dither_strength : tune.strength;
    opts.error_clamp = cfg.error_clamp_explicit
        ? cfg.error_clamp : tune.error_clamp;
    opts.palette_diversity = cfg.palette_diversity;
    opts.quantizer = cfg.quantizer;
    opts.ham_fast = cfg.ham_fast;
    opts.ham_metric = cfg.ham_metric;
    opts.ham_beam = static_cast<int>(cfg.ham_beam);
    opts.ham_triple = static_cast<int>(cfg.ham_triple);
    opts.refine_iterations = cfg.refine_iterations;
    opts.best = cfg.best;
    opts.best_metric = cfg.best_metric;
    opts.copper = cfg.copper;
    opts.copper_changes = static_cast<int>(cfg.copper_changes);
    opts.cap_spread_radius = cfg.cap_spread_radius;
    opts.cap_spread_decay = cfg.cap_spread_decay;
    opts.lock_color0 = cfg.lock_color0;
    opts.dual_playfield = cfg.dual_playfield;
    opts.scap = cfg.scap;
    opts.scap_debug = cfg.scap_debug;
    opts.scap_probe = cfg.scap_probe;
    opts.alpha_threshold = cfg.alpha_threshold;
    opts.alpha_dither = std::string{dither_to_options_string(cfg.alpha_dither)};
    opts.alpha_dither_strength = cfg.alpha_dither_strength;
    opts.symbol_name = cfg.symbol_name;
    opts.mask_invert = cfg.mask_invert;
    opts.crop_x = cfg.crop_x;
    opts.crop_y = cfg.crop_y;
    opts.crop_w = cfg.crop_w;
    opts.crop_h = cfg.crop_h;
    opts.crop_auto = cfg.crop_auto;
    opts.native_par = cfg.native_par;
    opts.cga_text_metric = cfg.cga_text_metric;
    opts.c64_palette = cfg.c64_palette;
    opts.c64_metric = cfg.c64_metric;
    opts.c64_petscii_graphics_only = cfg.c64_petscii_graphics_only;
    opts.tile_budget = cfg.tile_budget;
    opts.tile_reserve = cfg.tile_reserve;
    opts.locks = cfg.locks;
    opts.pins = cfg.pins;
    opts.reserves = cfg.reserves;
    opts.palette_file = cfg.palette_file;
    if (cfg.width)  opts.width  = static_cast<int>(*cfg.width);
    if (cfg.height) opts.height = static_cast<int>(*cfg.height);
    return opts;
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

// Single canonical preview-scaling stage. Encodes the per-mode aspect
// rules every consumer needs:
//   - DOS modes (CGA / EGA / VGA): nearest-neighbour to the PAR-aware
//     display dimensions so the PNG / iTerm2 inline image shows the
//     scene at roughly 4:3 like a CRT would. CGA text modes skip the
//     2× base scale since 640x200 cell rendering is already comfortable.
//   - Amiga / Atari modes: integer 2× horizontal for lores, 2× vertical
//     for non-interlaced; 1×1 for hires-lace. Matches hardware PAR.
// Used by save_preview() (PNG output), show_terminal_preview() (iTerm2),
// and the batch / video preview loops.
Image scale_for_display(const Image& preview, amiga::Mode mode,
                        bool hires, bool interlace) {
    if (amiga::is_vga(mode) || amiga::is_ega(mode) ||
        amiga::is_cga(mode) || amiga::is_c64(mode)) {
        auto params = amiga::get_mode_params(mode);
        std::size_t base_scale = amiga::is_cga_text(mode) ? 1u : 2u;
        auto [pw, ph] = preview_dims_for_par(preview.width(), preview.height(),
                                             static_cast<double>(params.par),
                                             base_scale);
        return scale_preview_nn(preview, pw, ph);
    }
    std::size_t sx = hires ? 1 : 2;
    std::size_t sy = interlace ? 1 : 2;
    if (hires && interlace) { sx = 1; sy = 1; }
    return scale_preview(preview, sx, sy);
}

// Companion mask-scaler for the same display rules. Source mask is
// (preview_w × preview_h); returned mask matches the dimensions
// scale_for_display() would produce for the same (mode, hires, interlace).
std::vector<bool> scale_mask_for_display(const std::vector<bool>& mask,
                                         std::size_t src_w, std::size_t src_h,
                                         amiga::Mode mode,
                                         bool hires, bool interlace) {
    if (amiga::is_vga(mode) || amiga::is_ega(mode) ||
        amiga::is_cga(mode) || amiga::is_c64(mode)) {
        auto params = amiga::get_mode_params(mode);
        std::size_t base_scale = amiga::is_cga_text(mode) ? 1u : 2u;
        auto [pw, ph] = preview_dims_for_par(src_w, src_h,
                                             static_cast<double>(params.par),
                                             base_scale);
        std::vector<bool> scaled(pw * ph, false);
        for (std::size_t y = 0; y < ph; ++y) {
            auto sy = std::min(src_h - 1, (y * src_h) / ph);
            for (std::size_t x = 0; x < pw; ++x) {
                auto sx = std::min(src_w - 1, (x * src_w) / pw);
                scaled[y * pw + x] = mask[sy * src_w + sx];
            }
        }
        return scaled;
    }
    std::size_t sx = hires ? 1 : 2;
    std::size_t sy = interlace ? 1 : 2;
    if (hires && interlace) { sx = 1; sy = 1; }
    return scale_mask(mask, src_w, src_h, sx, sy);
}

Result<void> save_preview(std::string_view path, const Image& preview,
                          bool has_trans, const std::vector<bool>& mask,
                          amiga::Mode mode, bool hires = false,
                          bool interlace = false) {
    auto scaled = scale_for_display(preview, mode, hires, interlace);
    if (has_trans) {
        auto scaled_mask = scale_mask_for_display(
            mask, preview.width(), preview.height(), mode, hires, interlace);
        return png_io::save(path, scaled, scaled_mask);
    }
    return png_io::save(path, scaled);
}

// Show preview in terminal (iTerm2 inline image protocol)
void show_terminal_preview(const Image& preview, amiga::Mode mode,
                           bool hires = false, bool interlace = false) {
    iterm2_display(scale_for_display(preview, mode, hires, interlace));
}

// CLI progress reporter. Throttles to ~20Hz redraw, writes "\rEncoding NN.N%
// [stage]\033[K" to stderr when it's a TTY (no-op otherwise so piped runs
// don't get carriage returns in their stderr capture). Thread-safe — encoder
// workers may invoke it concurrently. Final tick prints a newline so the
// next CLI status line ("Encoded: ...") starts cleanly.
std::function<void(float, std::string_view)> make_cli_progress_reporter() {
    if (!isatty(fileno(stderr))) return {};
    struct State {
        std::mutex mu;
        std::chrono::steady_clock::time_point last_emit{};
        std::string last_stage;
    };
    auto state = std::make_shared<State>();
    return [state](float p, std::string_view stage) {
        std::lock_guard lk(state->mu);
        auto now = std::chrono::steady_clock::now();
        // "done" is the terminal stage — end-cap the line with a
        // newline so the next "Mode:" / "Encoded:" prints cleanly.
        // Encoders also fire (p=1.0, stage="encoding") when their
        // own pass completes; that's NOT terminal (a pass-2 or
        // refinement pass may follow), so don't newline there.
        bool final_tick = (stage == "done");
        // Stage change: end the previous bar with a newline before
        // starting the new one — without this, an inner stage that
        // doesn't end with "done" (e.g. "extra-ehb-opt" before the
        // dither pass) leaves its bar attached to whatever status
        // line prints next, which clobbers it on the same row.
        bool stage_changed = !state->last_stage.empty() &&
                              state->last_stage != stage;
        // Newline on stage change EXCEPT when:
        //   - the new stage is "done" (final_tick already adds \n)
        //   - the previous stage was "done" (its final_tick already
        //     end-capped with \n; another would make a blank line
        //     between the two Encoding... bars)
        bool prev_was_done = state->last_stage == "done";
        if (stage_changed && !final_tick && !prev_was_done) {
            std::fputc('\n', stderr);
            state->last_emit = std::chrono::steady_clock::time_point{};
        }
        // 16 ms throttle ≈ 60 Hz — gives the user actual visible
        // motion on fast parallel passes (previous 50 ms swallowed
        // most updates from a 25-50 ms HAM6 beam search and made the
        // bar look frozen). Final tick always emits.
        if (!final_tick && !stage_changed &&
            now - state->last_emit < std::chrono::milliseconds(16)) {
            return;
        }
        state->last_emit = now;
        state->last_stage.assign(stage);
        float pct = std::clamp(p, 0.0f, 1.0f) * 100.0f;
        std::fprintf(stderr, "\rEncoding... %5.1f%% [%.*s]\033[K",
                     static_cast<double>(pct),
                     static_cast<int>(stage.size()), stage.data());
        if (final_tick) std::fputc('\n', stderr);
        std::fflush(stderr);
    };
}

// ---------------------------------------------------------------------------
// Batch mode entry point — encode N PNGs as a single horizontal atlas so
// they share one palette and (if --cap) one per-line copper plan, then
// emit per-frame outputs into config.batch_output_dir.
//
// Strategy: build atlas Image, serialise as PNG bytes, run api::encode_state
// once on the atlas to get all encoder intermediates (planes, palette,
// scanline_changes, scap_line_moves, ...) in one shot. Then decode the
// atlas's bitplane data to per-pixel indices, slice column ranges per
// frame, re-encode each slice to its own BitplaneData, and run the
// existing per-format writers (iff::write_ilbm, cheader::save, png_io,
// raw) against the slice + the SHARED palette/copper from the atlas.
//
// The shared-CAP property holds because horizontal stacking puts every
// frame's row Y at the SAME atlas row Y, so the per-line CAP plan
// computed for the atlas applies verbatim to each frame.
//
// SCAP is rejected up front (per-frame mid-line swap chains aren't
// shareable across frames). HAM/EHB/DPF are all allowed.
// ---------------------------------------------------------------------------
int run_batch(const Config& cfg) {
    // 1. Load + validate inputs.
    auto frames_r = load_batch_inputs(cfg.batch_inputs);
    if (!frames_r) {
        std::println(stderr, "Error: {}", frames_r.error().message);
        return exit_code::usage;
    }
    auto& frames = *frames_r;
    std::size_t frame_w = frames[0].image.width();
    std::size_t frame_h = frames[0].image.height();
    std::size_t n_frames = frames.size();

    cli_status("Batch: {} frames, {}x{} each",
               n_frames, frame_w, frame_h);

    // 2. Build atlas + encode atlas → PNG bytes (api::encode_state takes
    //    PNG bytes).
    auto atlas = build_atlas(frames);
    auto atlas_png = png_io::encode(atlas);
    if (!atlas_png) {
        std::println(stderr, "batch: atlas PNG encode failed: {}",
                     atlas_png.error().message);
        return exit_code::internal;
    }

    // 3. Run encoder on atlas. Map Config → api::Options via the shared
    //    builder; override width/height to the atlas dimensions and
    //    force scap off (already rejected at parse time).
    auto opts = make_api_options(cfg);
    opts.scap = false;
    opts.width = static_cast<int>(atlas.width());
    opts.height = static_cast<int>(atlas.height());

    auto enc = api::encode_state(atlas_png->data(), atlas_png->size(), opts);
    if (!enc.ok()) {
        std::println(stderr, "batch: atlas encode failed: {}", enc.error_msg);
        return exit_code::internal;
    }
    auto& st = enc.state;

    // 4. Decode atlas planes → per-pixel indices (uint8 each). For HAM
    //    these encode HAM control+data; for indexed modes they're
    //    palette indices. bitplane::decode round-trips with bitplane::
    //    encode either way.
    auto atlas_indices_r = bitplane::decode(st.planes);
    if (!atlas_indices_r) {
        std::println(stderr, "batch: atlas decode failed: {}",
                     atlas_indices_r.error().message);
        return exit_code::internal;
    }
    auto& atlas_indices = *atlas_indices_r;

    // 5. Slice atlas indices per frame and re-encode each slice into
    //    its own BitplaneData.
    std::vector<bitplane::BitplaneData> frame_planes;
    frame_planes.reserve(n_frames);
    std::size_t atlas_w = st.planes.width;
    for (std::size_t fi = 0; fi < n_frames; ++fi) {
        // Skip leading gutter + (frame_w + gutter)·fi to land on this frame's
        // first real column. Gutters are dither-error sponges, not output.
        auto x0 = kBatchGutter + fi * (frame_w + kBatchGutter);
        std::vector<std::uint8_t> idx(frame_w * frame_h);
        for (std::size_t y = 0; y < frame_h; ++y) {
            auto src = atlas_indices.data() + y * atlas_w + x0;
            std::copy(src, src + frame_w, idx.data() + y * frame_w);
        }
        auto enc_planes = bitplane::encode(
            idx, frame_w, frame_h, st.planes.depth, st.planes.layout);
        if (!enc_planes) {
            std::println(stderr, "batch: frame {} encode failed: {}",
                         fi, enc_planes.error().message);
            return exit_code::internal;
        }
        frame_planes.push_back(*std::move(enc_planes));
    }

    // 6. Per-format emission. Each format writes per-frame files +
    //    optional shared companion files into cfg.batch_output_dir.
    namespace fs = std::filesystem;
    auto out_dir = fs::path(cfg.batch_output_dir);

    auto& fmt = cfg.batch_format;

    if (fmt == "iff") {
        for (std::size_t fi = 0; fi < n_frames; ++fi) {
            iff::IffOptions iff_opts;
            iff_opts.hires = st.hires;
            iff_opts.interlace = st.interlace;
            iff_opts.dpf = st.dpf;
            iff_opts.has_transparency = st.has_transparency;
            if (!st.scanline_palettes.empty())
                iff_opts.scanline_palettes = &st.scanline_palettes;
            auto out_path = (out_dir / (frames[fi].stem + ".iff")).string();
            auto r = iff::save_ilbm(out_path, frame_planes[fi],
                                    st.palette, st.mode, iff_opts);
            if (!r) {
                std::println(stderr, "batch: IFF write '{}' failed: {}",
                             out_path, r.error().message);
                return exit_code::cant_create;
            }
        }
        // Companion .pal — base palette, OCS 12-bit packed.
        auto pal_path = (out_dir / "palette.pal").string();
        auto pr = palette_io::save_ocs_palette(pal_path, st.palette);
        if (!pr) std::println(stderr, "batch: warning, .pal write failed: {}",
                              pr.error().message);
    }
    else if (fmt == "png") {
        for (std::size_t fi = 0; fi < n_frames; ++fi) {
            // Render frame from atlas (the encoder already produced the
            // full atlas preview in st.rendered; we slice past the gutter
            // pad inserted to absorb cross-frame dither error).
            Image frame_img(frame_w, frame_h);
            auto x0 = kBatchGutter + fi * (frame_w + kBatchGutter);
            for (std::size_t y = 0; y < frame_h; ++y)
                for (std::size_t x = 0; x < frame_w; ++x)
                    frame_img[x, y] = st.rendered[x0 + x, y];
            auto out_path = (out_dir / (frames[fi].stem + ".png")).string();
            auto r = png_io::save(out_path, frame_img);
            if (!r) {
                std::println(stderr, "batch: PNG write '{}' failed: {}",
                             out_path, r.error().message);
                return exit_code::cant_create;
            }
        }
        // .pal companion — useful for tooling that wants the palette
        // even though it's already baked into each PNG's pixels.
        auto pal_path = (out_dir / "palette.pal").string();
        if (auto pr = palette_io::save_ocs_palette(pal_path, st.palette); !pr)
            std::println(stderr, "batch: warning, .pal write failed: {}",
                         pr.error().message);
    }
    else if (fmt == "raw") {
        for (std::size_t fi = 0; fi < n_frames; ++fi) {
            auto out_path = (out_dir / (frames[fi].stem + ".raw")).string();
            std::ofstream f(out_path, std::ios::binary);
            if (!f) {
                std::println(stderr, "batch: cannot create '{}'", out_path);
                return exit_code::cant_create;
            }
            f.write(reinterpret_cast<const char*>(frame_planes[fi].data.data()),
                    static_cast<std::streamsize>(frame_planes[fi].data.size()));
        }
        // Shared .pal.
        auto pal_path = (out_dir / "palette.pal").string();
        if (auto pr = palette_io::save_ocs_palette(pal_path, st.palette); !pr)
            std::println(stderr, "batch: warning, .pal write failed: {}",
                         pr.error().message);
    }
    else if (fmt == "h") {
        // Single combined .h with N frame plane arrays, one shared
        // palette, optional shared copper changes. Symbol prefix is
        // derived from the output dir's basename. Format is hand-rolled
        // here (mirrors cheader::generate's UWORD-array shape) so the
        // shared palette/copper aren't duplicated per frame.
        auto dir_name = out_dir.filename().string();
        if (dir_name.empty()) dir_name = "sprites";
        auto prefix = sanitise_symbol(dir_name);
        auto h_path = (out_dir / (prefix + ".h")).string();
        std::ofstream f(h_path);
        if (!f) {
            std::println(stderr, "batch: cannot create '{}'", h_path);
            return exit_code::cant_create;
        }
        auto upper_prefix = prefix;
        for (auto& c : upper_prefix) c = static_cast<char>(std::toupper(c));
        auto& plane_ref = frame_planes[0];
        auto words_per_row = plane_ref.bytes_per_row / 2;

        f << "/* Generated by png2amiga --batch — do not edit */\n";
        f << "/* " << n_frames << " frames, " << frame_w << "x" << frame_h
          << ", " << static_cast<int>(plane_ref.depth)
          << " bitplanes, shared palette"
          << (st.scanline_changes.empty() ? "" : " + per-line copper") << " */\n\n";
        f << "#ifndef " << upper_prefix << "_H\n";
        f << "#define " << upper_prefix << "_H\n\n";
        f << "#ifndef UWORD\n#define UWORD unsigned short\n#endif\n";
        f << "#ifndef ULONG\n#define ULONG unsigned long\n#endif\n\n";
        f << "#define " << upper_prefix << "_FRAME_COUNT  " << n_frames << "\n";
        f << "#define " << upper_prefix << "_FRAME_WIDTH  " << frame_w << "\n";
        f << "#define " << upper_prefix << "_FRAME_HEIGHT " << frame_h << "\n";
        f << "#define " << upper_prefix << "_DEPTH        "
          << static_cast<int>(plane_ref.depth) << "\n";
        f << "#define " << upper_prefix << "_BPR          "
          << static_cast<int>(plane_ref.bytes_per_row) << "\n\n";

        // Detect plane-i is all-zero across every frame in the batch.
        // Such planes (typical for DPF where one playfield is unused on
        // every frame) get omitted from the .h: emitting them would be
        // pure padding the runtime can supply via calloc(bpr*height,1).
        std::vector<bool> plane_zero(plane_ref.depth, true);
        for (std::size_t p = 0; p < plane_ref.depth; ++p) {
            for (std::size_t fi = 0; fi < n_frames && plane_zero[p]; ++fi) {
                const auto& bp = frame_planes[fi];
                for (std::size_t y = 0; y < bp.height && plane_zero[p]; ++y) {
                    auto off = bp.plane_row_offset(p, y);
                    for (std::size_t b = 0; b < bp.bytes_per_row; ++b) {
                        if (bp.data[off + b]) { plane_zero[p] = false; break; }
                    }
                }
            }
        }
        bool any_zero_plane = std::ranges::any_of(plane_zero,
                                                  [](bool z) { return z; });
        if (any_zero_plane) {
            f << "/* Plane mask (all-zero across every frame): ";
            for (std::size_t p = 0; p < plane_ref.depth; ++p)
                f << (plane_zero[p] ? '0' : '1');
            f << " — NULL slots in _frames[][] = caller must supply "
                 "bpr*height zero'd chip RAM at runtime. */\n\n";
        }

        // Per-frame plane arrays. Each frame emits one UWORD array per
        // *non-zero* bitplane (sym_planeN). Zero planes across the whole
        // batch are skipped; the plane table below uses 0 (NULL) for them.
        for (std::size_t fi = 0; fi < n_frames; ++fi) {
            const auto& planes = frame_planes[fi];
            auto sym = prefix + "_" + sanitise_symbol(frames[fi].stem);
            for (std::size_t p = 0; p < planes.depth; ++p) {
                if (plane_zero[p]) continue;
                f << "static const UWORD " << sym << "_plane" << p << "[] = {\n";
                auto total_words = words_per_row * planes.height;
                std::size_t word_count = 0;
                for (std::size_t y = 0; y < planes.height; ++y) {
                    f << "    ";
                    auto offset = planes.plane_row_offset(p, y);
                    for (std::size_t w = 0; w < words_per_row; ++w) {
                        auto byte_off = offset + w * 2;
                        auto hi = static_cast<std::uint16_t>(planes.data[byte_off]);
                        auto lo = static_cast<std::uint16_t>(planes.data[byte_off + 1]);
                        auto word = static_cast<std::uint16_t>((hi << 8) | lo);
                        ++word_count;
                        f << std::format("0x{:04X}", word);
                        if (word_count < total_words) f << ",";
                    }
                    f << "\n";
                }
                f << "};\n";
            }
            f << "\n";
        }

        // Plane-pointer table: const UWORD* prefix_frames[N][DEPTH].
        // NULL entries = caller supplies a zero'd buffer at runtime.
        f << "static const UWORD* const " << prefix << "_frames["
          << n_frames << "]["
          << static_cast<int>(plane_ref.depth) << "] = {\n";
        for (std::size_t fi = 0; fi < n_frames; ++fi) {
            auto sym = prefix + "_" + sanitise_symbol(frames[fi].stem);
            f << "    {";
            for (std::size_t p = 0; p < plane_ref.depth; ++p) {
                if (plane_zero[p]) f << " 0";
                else f << " " << sym << "_plane" << p;
                if (p + 1 < plane_ref.depth) f << ",";
            }
            f << " }";
            if (fi + 1 < n_frames) f << ",";
            f << "\n";
        }
        f << "};\n\n";

        // Shared palette (OCS 12-bit 0x0RGB).
        auto pal_count = st.palette.size();
        if (st.mode == amiga::Mode::ehb && pal_count > 32) pal_count = 32;
        f << "#define " << upper_prefix << "_COLORS  " << pal_count << "\n\n";
        f << "static const UWORD " << prefix << "_palette[] = {\n";
        for (std::size_t i = 0; i < pal_count; ++i) {
            auto rgb12 = palette::linear_to_ocs(st.palette[i]);
            f << std::format("    0x{:04X}", rgb12);
            if (i + 1 < pal_count) f << ",";
            auto srgb = color_space::linear_to_srgb(st.palette[i]).clamped();
            int r8 = static_cast<int>(std::lround(srgb.r * 255.0f));
            int g8 = static_cast<int>(std::lround(srgb.g * 255.0f));
            int b8 = static_cast<int>(std::lround(srgb.b * 255.0f));
            f << std::format("  /* #{:02X}{:02X}{:02X} */\n", r8, g8, b8);
        }
        f << "};\n\n";

        // Shared CAP copper plan (per-scanline palette swaps), if any.
        if (!st.scanline_changes.empty()) {
            auto& changes = st.scanline_changes;
            auto cpl = st.changes_per_line;
            auto height = changes.size();
            f << "#define " << upper_prefix << "_COPPER_CHANGES  " << cpl
              << "  /* max register changes per scanline */\n\n";
            f << "struct " << prefix << "_copper_entry {\n"
              << "    UWORD reg;\n"
              << "    UWORD color;\n"
              << "};\n\n";
            f << "static const struct " << prefix << "_copper_entry "
              << prefix << "_copper[" << height << "][" << cpl << "] = {\n";
            for (std::size_t y = 0; y < height; ++y) {
                f << "    { ";
                auto& line = changes[y];
                for (std::size_t s = 0; s < cpl; ++s) {
                    if (s < line.size()) {
                        auto rgb12 = palette::linear_to_ocs(line[s].color);
                        f << std::format("{{{},0x{:04X}}}", line[s].reg, rgb12);
                    } else {
                        f << "{0xFFFF,0x0000}";
                    }
                    if (s + 1 < cpl) f << ", ";
                }
                f << " }";
                if (y + 1 < height) f << ",";
                f << "\n";
            }
            f << "};\n\n";
        }

        f << "#endif /* " << upper_prefix << "_H */\n";
        cli_status("Header: {}", h_path);
    }
    else if (fmt == "cpp") {
        // Single AmigaOS viewer with all frames; left-click cycles, right-click exits.
        auto dir_name = out_dir.filename().string();
        if (dir_name.empty()) dir_name = "sprites";
        auto prefix = sanitise_symbol(dir_name);
        auto cpp_path = (out_dir / (prefix + ".cpp")).string();

        auto ch = pipeline::make_ch_opts({
            .symbol_override = prefix,
            .hires = st.hires,
            .interlace = st.interlace,
            .aga = st.aga,
            .dpf = st.dpf,
            .total_unique_colors =
                static_cast<std::size_t>(count_unique_colors(st.rendered)),
        });
        std::span<const bitplane::BitplaneData> extras{
            frame_planes.data() + 1, frame_planes.size() - 1};
        ch.extra_frame_planes = extras;
        if (!st.scanline_changes.empty()) {
            ch.copper_changes = &st.scanline_changes;
            ch.copper_changes_per_line = st.changes_per_line;
            ch.copper_scanline_palettes = &st.scanline_palettes;
        }
        auto r = cheader::save_viewer(cpp_path, frame_planes[0],
                                      st.palette, st.mode, ch);
        if (!r) {
            std::println(stderr, "batch: viewer write '{}' failed: {}",
                         cpp_path, r.error().message);
            return exit_code::cant_create;
        }
        cli_status("Viewer: {} ({} frames, click to cycle, right-click to exit)",
                   cpp_path, n_frames);
    }
    else {
        std::println(stderr, "batch: unknown --batch-format '{}'", fmt);
        return exit_code::usage;
    }

    // Inline preview: dump each generated frame via iTerm2 escape codes.
    // The atlas's rendered Image has every frame baked in; slice + show.
    if (cfg.preview && !is_quiet()) {
        for (std::size_t fi = 0; fi < n_frames; ++fi) {
            Image frame_img(frame_w, frame_h);
            auto x0 = kBatchGutter + fi * (frame_w + kBatchGutter);
            for (std::size_t y = 0; y < frame_h; ++y)
                for (std::size_t x = 0; x < frame_w; ++x)
                    frame_img[x, y] = st.rendered[x0 + x, y];
            cli_status("Preview frame {}/{} ({}):",
                       fi + 1, n_frames, frames[fi].stem);
            show_terminal_preview(frame_img, st.mode, st.hires, st.interlace);
        }
    }

    // Inline video preview: loop the rendered frames (sliced from
    // st.rendered, the internal atlas buffer — not from the output
    // files) at preview_video_fps using iTerm2 inline images. Exits on
    // any keypress. Uses raw stdin mode + select() for non-blocking
    // peek; falls through silently on non-tty stdin.
    if (cfg.preview_video && !is_quiet()) {
        // Pre-build frame PNG payloads + their inline-image escape
        // sequences once so the loop is just a write+sleep cycle. Goes
        // through the same scale_for_display + inline_image_escape
        // path as the static --preview, so the looped video matches the
        // single-shot terminal preview pixel-for-pixel.
        std::vector<std::string> frame_payloads;
        frame_payloads.reserve(n_frames);
        for (std::size_t fi = 0; fi < n_frames; ++fi) {
            Image frame_img(frame_w, frame_h);
            auto x0 = kBatchGutter + fi * (frame_w + kBatchGutter);
            for (std::size_t y = 0; y < frame_h; ++y)
                for (std::size_t x = 0; x < frame_w; ++x)
                    frame_img[x, y] = st.rendered[x0 + x, y];
            auto scaled = scale_for_display(frame_img, st.mode,
                                            st.hires, st.interlace);
            auto seq = inline_image_escape(scaled);
            if (seq.empty()) continue;
            frame_payloads.push_back("\033[H" + seq);
        }

        bool tty_raw = false;
#ifndef _WIN32
        bool tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
        struct termios saved{};
        if (tty && tcgetattr(STDIN_FILENO, &saved) == 0) {
            struct termios raw = saved;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
                tty_raw = true;
        }
#endif
        // Hide cursor + clear once for clean playback.
        std::fputs("\033[?25l\033[2J", stdout);
        std::fflush(stdout);

        auto frame_dt = std::chrono::duration<double>(
            1.0 / static_cast<double>(cfg.preview_video_fps));
        auto sleep_dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
            frame_dt);

        cli_status("Looping {} frames at {:.2f} fps — press any key to stop.",
                   n_frames, cfg.preview_video_fps);

        bool exit_requested = false;
        while (!exit_requested && !frame_payloads.empty()) {
            for (auto& payload : frame_payloads) {
                std::fwrite(payload.data(), 1, payload.size(), stdout);
                std::fflush(stdout);
                std::this_thread::sleep_for(sleep_dt);
#ifndef _WIN32
                if (tty_raw) {
                    char c;
                    if (read(STDIN_FILENO, &c, 1) == 1) {
                        exit_requested = true;
                        break;
                    }
                }
#endif
            }
            if (!tty_raw) break;  // single pass when no tty (CI capture).
        }

        // Restore terminal + cursor.
#ifndef _WIN32
        if (tty_raw) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
#endif
        std::fputs("\033[?25h\n", stdout);
        std::fflush(stdout);
    }

    // Summary. Disk accounting splits per-frame (bitplanes only, the
    // bytes that get duplicated per frame) from shared (palette + CAP
    // copper, one copy serves all frames). The total bundle = N ×
    // per-frame planes + shared. compute_size_breakdown does the math
    // for one full image; we ask it for both halves so per-frame
    // doesn't pretend to include the shared overhead.
    int cap_entries = st.scanline_changes.empty() ? 0
        : static_cast<int>(static_cast<int>(frame_h) *
                           static_cast<int>(st.changes_per_line));
    // Skip all-zero planes in disk accounting. DPF uses two playfields'
    // worth of bitplanes (e.g. depth=3 + dpf → 6 total), but if one
    // playfield is unused (e.g. PF1 transparent for video frames) those
    // planes serialise as pure zeros and can be memset(0) at runtime
    // instead of being stored. Reporting them as "real" disk bytes
    // overstates the cost.
    auto count_nonzero_planes = [](const bitplane::BitplaneData& bp) {
        int nonzero = 0;
        auto bpr = bp.bytes_per_row;
        for (std::size_t p = 0; p < bp.depth; ++p) {
            bool any = false;
            for (std::size_t y = 0; y < bp.height && !any; ++y) {
                auto off = bp.plane_row_offset(p, y);
                for (std::size_t b = 0; b < bpr; ++b)
                    if (bp.data[off + b]) { any = true; break; }
            }
            if (any) ++nonzero;
        }
        return nonzero;
    };
    int nonzero_planes = count_nonzero_planes(frame_planes[0]);
    int per_plane_bytes = static_cast<int>(frame_planes[0].height *
                                            frame_planes[0].bytes_per_row);
    int plane_b = nonzero_planes * per_plane_bytes;
    int pal_b = static_cast<int>(st.palette.size()) * (st.aga ? 4 : 2);
    int cap_b = cap_entries * (st.aga ? 8 : 4);
    int shared_b = pal_b + cap_b;
    int total_b = plane_b * static_cast<int>(n_frames) + shared_b;
    auto total_colors = count_unique_colors(st.rendered);

    Image frame_img(frame_w, frame_h);
    float psnr_sum = 0.0f;
    int psnr_count = 0;
    for (std::size_t fi = 0; fi < n_frames; ++fi) {
        auto x0 = kBatchGutter + fi * (frame_w + kBatchGutter);
        for (std::size_t y = 0; y < frame_h; ++y)
            for (std::size_t x = 0; x < frame_w; ++x)
                frame_img[x, y] = st.rendered[x0 + x, y];
        float fp = color_space::compute_psnr_blurred(
            frames[fi].image.pixels(), frame_img.pixels(),
            frame_w, frame_h);
        int fc = count_unique_colors(frame_img);
        cli_status("  frame {}/{} ({}): {} colors, PSNR {:.2f} dB",
                   fi + 1, n_frames, frames[fi].stem, fc, fp);
        if (std::isfinite(fp)) { psnr_sum += fp; ++psnr_count; }
    }
    float avg_psnr = psnr_count ? psnr_sum / static_cast<float>(psnr_count)
                                : std::numeric_limits<float>::infinity();

    cli_status("Batch: encoded {} frames into '{}', "
               "{} colors, {:.1f} avg CAP/line, "
               "planes {}/frame ({}/{} non-zero) × {} + shared {} = {} total, "
               "PSNR avg {:.2f} dB",
               n_frames, cfg.batch_output_dir,
               total_colors,
               st.copper_changes,
               fmt_size(plane_b),
               nonzero_planes, static_cast<int>(frame_planes[0].depth),
               n_frames,
               fmt_size(shared_b),
               fmt_size(total_b),
               avg_psnr);
    return exit_code::ok;
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
    g_preview_scale = resolve_preview_scale(config->preview_scale);

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
                "snes-mode7-256", "snes-mode7-direct",
                "genesis-h32", "genesis-h40",
                "genesis-h32-sh", "genesis-h40-sh",
                "c64-multicolor", "c64-hires", "c64-fli", "c64-afli",
                "c64-petscii",
                "c64-charset-hires", "c64-charset-multicolor",
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
            cli_status("  SNES:     snes-mode7-256, snes-mode7-direct");
            cli_status("  Genesis:  genesis-h32, genesis-h40 (+ -sh variants)");
            cli_status("  C64:      c64-multicolor, c64-hires, c64-fli, c64-afli, c64-petscii,");
            cli_status("            c64-charset-hires, c64-charset-multicolor");
        }
        return exit_code::ok;
    }

    if (config->list_dithers) {
        cli_status("png2amiga {} — dither methods:", png2amiga::version);
        cli_status("  ED:        ostromoukhov (default), sierra-lite, atkinson, jarvis,");
        cli_status("             floyd-steinberg, stucki, gilbert, riemersma");
        cli_status("  Palette-aware: opt-checker, opt-line, opt-line-checker, tri-tone,");
        cli_status("             knoll, yliluoma1, yliluoma, yliluoma2");
        cli_status("  DBS:       dbs (perceptual optimum, slow)");
        cli_status("  Structure: structure-fs, contrast-fs, zhoufang");
        cli_status("  Bayer:     bayer2x2, bayer4x4, bayer8x8, bayer4x2, bayer2x4,");
        cli_status("             bayer3x3, bayer5x5, bayer6x6, bayer7x7,");
        cli_status("             aseprite-old, libcaca3, libcaca6, cranley-bayer, fractal16");
        cli_status("  Halftone:  checker, clustered-dot, halftone8x8, diagonal8x8,");
        cli_status("             spiral5x5, pegasus, h2x4, v4x2");
        cli_status("  Lines:     line2, line4, line8, line-checker,");
        cli_status("             vline2, vline4, vline8, vline-checker, crosshatch");
        cli_status("  Pattern:   hex8x8, hex5x5, radial, quasicrystal, truchet");
        cli_status("  Noise:     blue-noise, void-cluster, cluster-noise,");
        cli_status("             ign, ign-tri, r2, r2-tri, white-noise, value-noise");
        cli_status("  none");
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

        auto fallback_sym = std::string("scap_") + res.probe_label;
        auto ch_opts = pipeline::make_ch_opts({
            .symbol_override = config->symbol_name.empty()
                ? std::string_view{fallback_sym}
                : std::string_view{config->symbol_name},
            .dpf = true,                // OCS DPF for Phase 1
        });
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

        // Reject dither methods that don't apply to the chosen mode —
        // but only when the user passed --dither explicitly. The
        // default (ostromoukhov) hits the same incompatibility on
        // HAM, so silent auto-fallback there matches the web's
        // HAM_INCOMPATIBLE_DITHERS watcher and keeps existing scripts
        // / CMake invocations working without --dither. Explicit
        // picks fail loud — scripted callers chose them for a reason.
        if (config->dither_explicit &&
            dither::needs_discrete_palette(config->dither_method)) {
            auto dname = dither_name(config->dither_method);
            if (amiga::is_ham(config->mode)) {
                std::println(stderr,
                    "Error: dither '{}' needs a discrete palette and "
                    "silently degenerates in HAM modes (no per-pixel "
                    "palette index — encoder picks SET/MODIFY ops). "
                    "Use atkinson, floyd-steinberg, sierra-lite, jarvis, "
                    "stucki, or an ordered method.",
                    dname);
                return 1;
            }
            if (amiga::is_snes_direct(config->mode) &&
                dither::is_yliluoma(config->dither_method)) {
                std::println(stderr,
                    "Error: dither '{}' is palette-aware but Mode 7 "
                    "Direct has no palette table. Use a non-yliluoma "
                    "method (atkinson, floyd-steinberg, ordered).",
                    dname);
                return 1;
            }
        }
        // Implicit default + incompatible mode: auto-fallback. Atkinson
        // wins HAM6 7/10 and ties HAM8 4/10 in the dither sweep, so
        // it's the right default for HAM. SNES Mode 7 Direct uses
        // ostromoukhov instead — yliluoma-family is the only thing
        // that really breaks there and the default isn't yliluoma.
        if (!config->dither_explicit &&
            dither::needs_discrete_palette(config->dither_method) &&
            amiga::is_ham(config->mode)) {
            config->dither_method = dither::Method::atkinson;
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

        // Depth defaults for standard Amiga modes when --depth wasn't given:
        //   AGA lores / lores-lace / hires / hires-lace → 8 (full 256-colour
        //                                                    gamut).
        //   OCS hires / hires-lace → 4 (hardware cap; the global default of
        //                              5 would fail the depth-vs-max gate
        //                              below).
        //   OCS lores / lores-lace → 5 (the existing global default).
        if (!config->depth_explicit &&
            (config->mode == amiga::Mode::lores ||
             config->mode == amiga::Mode::lores_interlace ||
             config->mode == amiga::Mode::hires ||
             config->mode == amiga::Mode::hires_interlace)) {
            if (cs == amiga::Chipset::aga) {
                config->depth = 8;
            } else if (config->mode == amiga::Mode::hires ||
                       config->mode == amiga::Mode::hires_interlace) {
                config->depth = 4;
            }
            // OCS lores keeps the global default 5.
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

    // --reserve-range gating for paths that bypass api::run_pipeline
    // (DPF, CGA fixed-palette, batch). Modes routed through api::encode_state
    // are gated inside api.cpp; this block catches the rest so the user
    // gets a clear error instead of a silent no-op.
    if (!config->reserves.empty()) {
        auto reject = [](std::string_view why) {
            std::println(stderr, "Error: --reserve-range: {}", why);
            return 1;
        };
        if (config->dual_playfield)
            return reject("not supported with --dpf (two split palettes; "
                          "specify which playfield is needed)");
        if (amiga::is_cga(config->mode) || amiga::is_cga_text(config->mode))
            return reject("not supported in CGA modes (palette is "
                          "hardware-fixed)");
    }

    // Batch mode runs an entirely different path: build atlas, encode
    // once, slice per frame, write N outputs. Skip the normal single-
    // input pipeline below.
    if (config->batch) {
        return run_batch(*config);
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
                                   amiga::is_ega(config->mode) ||
                                   amiga::is_snes(config->mode) ||
                                   amiga::is_genesis(config->mode) ||
                                   amiga::is_c64(config->mode);
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

    cli_print_input(image->width(), image->height());

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
    // DPF override: --dpf turns OCS lores depth-3 PF2 + zeroed PF1 into a
    // 6-plane DPF frame. Detect early so the Target line reports the
    // actual encoded plane count (6 for OCS DPF, 8 for AGA DPF) rather
    // than the user's --depth or mode-default for the standard path.
    bool use_dpf_early = config->dual_playfield &&
                          !amiga::is_ham(config->mode) &&
                          config->mode != amiga::Mode::ehb &&
                          !amiga::is_atari(config->mode) &&
                          !amiga::is_vga(config->mode) &&
                          !amiga::is_ega(config->mode) &&
                          !amiga::is_cga(config->mode);
    int print_depth = static_cast<int>(actual_depth);
    if (use_dpf_early) {
        auto early_chipset = effective_chipset(*config);
        print_depth = (early_chipset == amiga::Chipset::aga) ? 8 : 6;
    }
    cli_print_target(static_cast<std::size_t>(target_w),
                     static_cast<std::size_t>(target_h),
                     print_depth);

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
            auto scaled = scale::resample(*image, target_w, target_h);
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
    cli_print_chipset(chipset);

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

    // --- SNES Mode 7 (chunky 8bpp + BGR555 palette / RGB443 direct) ---
    // Routes through api::encode_state's SNES branch — same path the
    // WASM/web frontend uses — and writes raw bytes, palette, header,
    // or PNG preview based on the output extension.
    if (amiga::is_c64(config->mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        auto src_png = png_io::encode(*image);
        if (!src_png) {
            std::println(stderr, "C64: source re-encode failed: {}",
                         src_png.error().message);
            return exit_code::internal;
        }
        auto aopts = make_api_options(*config);
        neutralize_preprocess(aopts);
        aopts.mode = [&] -> const char* {
            switch (config->mode) {
            case amiga::Mode::c64_hires:           return "c64-hires";
            case amiga::Mode::c64_fli:             return "c64-fli";
            case amiga::Mode::c64_afli:            return "c64-afli";
            case amiga::Mode::c64_petscii:         return "c64-petscii";
            case amiga::Mode::c64_charset_hires:   return "c64-charset-hires";
            case amiga::Mode::c64_charset_multicolor:
                                                   return "c64-charset-multicolor";
            default:                               return "c64-multicolor";
            }
        }();
        aopts.width = static_cast<int>(image->width());
        aopts.height = static_cast<int>(image->height());
        aopts.on_progress = make_cli_progress_reporter();
        auto enc = api::encode_state(src_png->data(), src_png->size(), aopts);
        if (!enc.ok()) {
            std::println(stderr, "C64 encode error: {}", enc.error_msg);
            return exit_code::internal;
        }
        auto& st = enc.state;
        switch (config->mode) {
        case amiga::Mode::c64_hires:
            cli_status("Mode:   C64 hires (320×200, 2 colours/cell)"); break;
        case amiga::Mode::c64_fli:
            cli_status("Mode:   C64 FLI (160×200, multicolor + per-row screen)"); break;
        case amiga::Mode::c64_afli:
            cli_status("Mode:   C64 AFLI (320×200, hires + per-row screen)"); break;
        case amiga::Mode::c64_petscii:
            cli_status("Mode:   C64 PETSCII (40×25 text, PN-sRGB blur metric)"); break;
        case amiga::Mode::c64_charset_hires:
            cli_status("Mode:   C64 charset-hires (40×25 8×8 cells, ≤256 dedup glyphs)"); break;
        case amiga::Mode::c64_charset_multicolor:
            cli_status("Mode:   C64 charset-multicolor (40×25 4×8 cells, shared mc + per-cell fg)"); break;
        default:
            cli_status("Mode:   C64 multicolor (160×200, 4 colours/cell)"); break;
        }
        cli_status("Encoded: {} bytes, PSNR: {:.2f} dB, S2: {:.2f}",
                     st.raw_frame.size(), st.psnr, st.ssimulacra2_score);
        if (ends_with(config->output_path, ".bin") ||
            ends_with(config->output_path, ".raw")) {
            std::ofstream of(config->output_path, std::ios::binary);
            of.write(reinterpret_cast<const char*>(st.raw_frame.data()),
                     static_cast<std::streamsize>(st.raw_frame.size()));
            cli_status("Raw:    {} ({} bytes)", config->output_path,
                       st.raw_frame.size());
        } else if (ends_with(config->output_path, ".h")
                   && amiga::is_c64_charset(config->mode)) {
            // Charset .h: reconstruct an EncodeResult from the api state
            // (raw_frame holds the bytes; cols/rows/unique_glyphs/mc1/mc2
            // come back through EncodeState). No re-encoding.
            c64::EncodeResult ch_enc;
            ch_enc.cols = st.c64_cols;
            ch_enc.rows = st.c64_rows;
            ch_enc.unique_glyphs = st.c64_unique_glyphs;
            ch_enc.bg_color = st.c64_bg_color;
            ch_enc.mc1 = st.c64_mc1;
            ch_enc.mc2 = st.c64_mc2;
            std::size_t charset_bytes = st.c64_unique_glyphs * 8;
            std::size_t cells = st.c64_cols * st.c64_rows;
            if (st.raw_frame.size() < charset_bytes + 2 * cells) {
                std::println(stderr,
                    "C64 charset .h: raw_frame too small ({} < {})",
                    st.raw_frame.size(), charset_bytes + 2 * cells);
                return exit_code::internal;
            }
            ch_enc.bitmap.assign(
                st.raw_frame.begin(),
                st.raw_frame.begin()
                    + static_cast<std::ptrdiff_t>(charset_bytes));
            ch_enc.screen_ram.assign(
                st.raw_frame.begin()
                    + static_cast<std::ptrdiff_t>(charset_bytes),
                st.raw_frame.begin()
                    + static_cast<std::ptrdiff_t>(charset_bytes + cells));
            ch_enc.color_ram.assign(
                st.raw_frame.begin()
                    + static_cast<std::ptrdiff_t>(charset_bytes + cells),
                st.raw_frame.begin()
                    + static_cast<std::ptrdiff_t>(charset_bytes + 2 * cells));

            auto sym = config->symbol_name.empty()
                ? std::string("img") : config->symbol_name;
            auto hdr = c64::charset_header(
                ch_enc, sym,
                config->mode == amiga::Mode::c64_charset_multicolor,
                c64::parse_palette(config->c64_palette));
            if (!hdr) {
                std::println(stderr, "Header generate: {}",
                             hdr.error().message);
                return exit_code::internal;
            }
            std::ofstream of(config->output_path);
            of << *hdr;
            cli_status("Header: {} ({}x{} cells, {} glyphs)",
                       config->output_path, ch_enc.cols, ch_enc.rows,
                       ch_enc.unique_glyphs);
        } else if (ends_with(config->output_path, ".prg") ||
                   ends_with(config->output_path, ".koa") ||
                   ends_with(config->output_path, ".hir")) {
            // C64 runnable .prg (Koala/hires/FLI/AFLI/PETSCII displayer)
            // or .koa / .hir raw image-only formats.
            auto enc_result = c64::prg::unpack_pipeline_raw(
                config->mode, st.raw_frame, st.c64_bg_color);
            if (!enc_result) {
                std::println(stderr, "C64 PRG unpack: {}",
                             enc_result.error().message);
                return exit_code::internal;
            }
            Result<c64::prg::PrgData> prg = [&] {
                if (ends_with(config->output_path, ".koa"))
                    return c64::prg::koala_raw(*enc_result);
                if (ends_with(config->output_path, ".hir"))
                    return c64::prg::hires_raw(*enc_result);
                return c64::prg::from_mode(config->mode, *enc_result);
            }();
            if (!prg) {
                std::println(stderr, "C64 PRG encode: {}",
                             prg.error().message);
                return exit_code::internal;
            }
            auto wr = c64::prg::write(config->output_path, *prg);
            if (!wr) {
                std::println(stderr, "C64 PRG write: {}",
                             wr.error().message);
                return exit_code::internal;
            }
            cli_status("PRG:    {} ({} bytes)", config->output_path,
                       prg->bytes.size());
        } else {
            auto r = save_preview(config->output_path, st.rendered,
                                  has_transparency, transparency_mask,
                                  config->mode, /*hires=*/false,
                                  /*interlace=*/false);
            if (!r) {
                std::println(stderr, "PNG write error: {}", r.error().message);
                return exit_code::internal;
            }
            cli_status("PNG:    {}", config->output_path);
        }
        if (!config->depfile.empty() && !config->output_path.empty()) {
            std::array<std::string_view, 2> inputs{
                config->input_path, config->palette_file,
            };
            write_depfile(config->depfile, config->output_path, inputs);
        }
        return exit_code::ok;
    }

    if (amiga::is_snes(config->mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        // Re-encode the (preprocessed, scaled, cropped) image as PNG so
        // we can hand it to api::encode_state as bytes.
        auto src_png = png_io::encode(*image);
        if (!src_png) {
            std::println(stderr, "SNES: source re-encode failed: {}",
                         src_png.error().message);
            return exit_code::internal;
        }

        auto aopts = make_api_options(*config);
        // *image already went through preprocess::apply above; src_png
        // re-encodes that processed Image. Stop run_pipeline from
        // preprocessing again on top.
        neutralize_preprocess(aopts);
        aopts.mode = (config->mode == amiga::Mode::snes_mode7_256)
            ? "snes-mode7-256" : "snes-mode7-direct";
        aopts.width = static_cast<int>(image->width());
        aopts.height = static_cast<int>(image->height());
        aopts.on_progress = make_cli_progress_reporter();

        auto enc = api::encode_state(src_png->data(), src_png->size(), aopts);
        if (!enc.ok()) {
            std::println(stderr, "SNES encode error: {}", enc.error_msg);
            return exit_code::internal;
        }
        auto& st = enc.state;
        cli_status("Dither:   {} (strength: {:.2f})",                     dither_name(config->dither_method),
                     config->dither_strength);
        cli_status("Mode:   SNES Mode 7 ({}), {}x{}, 256 colours",
                     (config->mode == amiga::Mode::snes_mode7_256
                          ? "256-palette BGR555"
                          : "Direct Color BBGGGRRR"),
                     aopts.width, aopts.height);
        cli_status("Quantised: {} bytes (32 KB Mode 7 frame after pack), PSNR: {:.2f} dB, S2: {:.2f}",
                     st.raw_frame.size(), st.psnr, st.ssimulacra2_score);

        if (ends_with(config->output_path, ".bin") ||
            ends_with(config->output_path, ".raw")) {
            // api::encode_state has already packed the Mode 7 frame
            // (tilemap + tile data) into st.raw_frame. Just write it
            // out + (for 256 mode) emit the .pal companion.
            cli_status("Tiles:  {} unique tiles in 32×28 cells",
                         st.genesis_unique_tiles);
            std::ofstream of(config->output_path, std::ios::binary);
            of.write(reinterpret_cast<const char*>(st.raw_frame.data()),
                     static_cast<std::streamsize>(st.raw_frame.size()));
            if (!of) {
                std::println(stderr, "SNES write error: {}",
                             config->output_path);
                return exit_code::internal;
            }
            cli_status("Raw:    {} ({} bytes = 16384 tilemap + {} tiles)",
                         config->output_path, st.raw_frame.size(),
                         st.raw_frame.size() - 16384);

            if (config->mode == amiga::Mode::snes_mode7_256) {
                // Companion palette file: same path with .pal extension.
                std::filesystem::path pal_path{config->output_path};
                pal_path.replace_extension(".pal");
                std::vector<std::uint8_t> pal_bytes(256 * 2, 0);
                for (std::size_t i = 0; i < st.palette.size() && i < 256; ++i) {
                    auto w16 = console_color::to_bgr555_word(st.palette[i]);
                    pal_bytes[i * 2 + 0] = static_cast<std::uint8_t>(w16 & 0xFF);
                    pal_bytes[i * 2 + 1] = static_cast<std::uint8_t>((w16 >> 8) & 0xFF);
                }
                std::ofstream pf(pal_path, std::ios::binary);
                pf.write(reinterpret_cast<const char*>(pal_bytes.data()),
                         static_cast<std::streamsize>(pal_bytes.size()));
                cli_status("Pal:    {} ({} bytes)",
                             pal_path.string(), pal_bytes.size());
            }
        } else if (ends_with(config->output_path, ".h")) {
            // Re-route through api::convert_cheader so the inline SNES
            // header writer there (api.cpp::snes_header) does the work
            // — keeps a single source of truth for the .h layout.
            auto cr = api::convert_cheader(
                src_png->data(), src_png->size(), aopts);
            if (!cr.error.empty()) {
                std::println(stderr, "SNES header: {}", cr.error);
                return exit_code::internal;
            }
            std::ofstream of(config->output_path);
            of.write(reinterpret_cast<const char*>(cr.data.data()),
                     static_cast<std::streamsize>(cr.data.size()));
            cli_status("Header: {} ({} bytes)",
                       config->output_path, cr.data.size());
        } else {
            // PNG preview using st.rendered.
            auto r = save_preview(config->output_path, st.rendered,
                                  has_transparency, transparency_mask,
                                  config->mode, /*hires=*/false,
                                  /*interlace=*/false);
            if (!r) {
                std::println(stderr, "PNG write error: {}", r.error().message);
                return exit_code::internal;
            }
            cli_status("PNG:    {}", config->output_path);
        }

        if (!config->depfile.empty() && !config->output_path.empty()) {
            std::array<std::string_view, 2> inputs{
                config->input_path, config->palette_file,
            };
            write_depfile(config->depfile, config->output_path, inputs);
        }
        return exit_code::ok;
    }

    // --- Sega Genesis / Mega Drive (tile-bitmap title art) ---
    if (amiga::is_genesis(config->mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        auto src_png = png_io::encode(*image);
        if (!src_png) {
            std::println(stderr, "Genesis: source re-encode failed: {}",
                         src_png.error().message);
            return exit_code::internal;
        }

        // Genesis-specific dither default: opt-checker. The 2×2 phase
        // window aligns with 8-pixel tile boundaries so tile-dedup
        // survives (~40% on photos), unlike error diffusion which
        // serpentine-states across tiles → 0% dedup → blown VRAM
        // budget. PSNR is ~3 dB below ostromoukhov but VRAM usage
        // halves, which is the right Genesis tradeoff. User can
        // override with --dither.
        auto genesis_dither = config->dither_explicit
            ? config->dither_method : dither::Method::opt_checker;

        auto aopts = make_api_options(*config);
        // *image already preprocessed; the PNG-encoded src bytes feed
        // run_pipeline which would otherwise re-apply preprocess.
        neutralize_preprocess(aopts);
        switch (config->mode) {
        case amiga::Mode::genesis_h32:    aopts.mode = "genesis-h32";    break;
        case amiga::Mode::genesis_h40:    aopts.mode = "genesis-h40";    break;
        case amiga::Mode::genesis_h32_sh: aopts.mode = "genesis-h32-sh"; break;
        case amiga::Mode::genesis_h40_sh: aopts.mode = "genesis-h40-sh"; break;
        default: aopts.mode = "genesis-h40"; break;
        }
        aopts.dither = std::string{dither_to_options_string(genesis_dither)};
        aopts.width = static_cast<int>(image->width());
        aopts.height = static_cast<int>(image->height());

        auto enc = api::encode_state(src_png->data(), src_png->size(), aopts);
        if (!enc.ok()) {
            std::println(stderr, "Genesis encode error: {}", enc.error_msg);
            return exit_code::internal;
        }
        auto& st = enc.state;
        cli_status("Dither:   {} (strength: {:.2f})",                     dither_name(genesis_dither),
                     config->dither_strength);
        const char* mode_label = "";
        switch (config->mode) {
        case amiga::Mode::genesis_h32:    mode_label = "H32 256-wide"; break;
        case amiga::Mode::genesis_h40:    mode_label = "H40 320-wide"; break;
        case amiga::Mode::genesis_h32_sh: mode_label = "H32 + Shadow"; break;
        case amiga::Mode::genesis_h40_sh: mode_label = "H40 + Shadow"; break;
        default: mode_label = "Genesis"; break;
        }
        cli_status("Mode:   Sega Genesis ({}), {}x{}, 4 palettes × 16 BGR333",
                     mode_label, aopts.width, aopts.height);
        // Tile-dedup stats. The "after dedup" tile count maps directly to
        // VRAM bytes (×32). Real Genesis VRAM is 64 KB total, with the
        // upper bound for plane-A title art around ~1280 tiles before
        // sprites and plane-B start contending.
        if (st.genesis_total_cells > 0) {
            auto unique = st.genesis_unique_tiles;
            auto total = st.genesis_total_cells;
            float dedup_pct = total > 0
                ? 100.0f * (1.0f - static_cast<float>(unique) /
                                    static_cast<float>(total))
                : 0.0f;
            cli_status("Tiles:  {} unique / {} cells ({:.1f}% dedup, {} VRAM bytes)",
                         unique, total, dedup_pct, unique * 32);
            if (unique > 1280) {
                cli_status("Warning: {} unique tiles exceeds the typical "
                            "~1280 plane-A budget (~40 KB).", unique);
            }
        }
        cli_status("Encoded: {} bytes total (tiles + tilemap + CRAM), "
                     "PSNR: {:.2f} dB, S2: {:.2f}",
                     st.raw_frame.size(), st.psnr, st.ssimulacra2_score);

        if (ends_with(config->output_path, ".bin") ||
            ends_with(config->output_path, ".raw")) {
            std::ofstream of(config->output_path, std::ios::binary);
            of.write(reinterpret_cast<const char*>(st.raw_frame.data()),
                     static_cast<std::streamsize>(st.raw_frame.size()));
            if (!of) {
                std::println(stderr, "Genesis write error: {}",
                             config->output_path);
                return exit_code::internal;
            }
            cli_status("Raw:    {} ({} bytes)",
                         config->output_path, st.raw_frame.size());
        } else if (ends_with(config->output_path, ".h")) {
            cheader_genesis::GenesisHeaderOptions hopts;
            hopts.tile_bytes = st.genesis_tile_bytes;
            hopts.tilemap = st.genesis_tilemap_cells;
            hopts.palette = st.genesis_palette_words;
            hopts.width_pixels = static_cast<std::size_t>(aopts.width);
            hopts.height_pixels = static_cast<std::size_t>(aopts.height);
            hopts.symbol = config->symbol_name.empty()
                ? std::string{"genesis_image"} : config->symbol_name;
            auto hdr = cheader_genesis::generate(hopts);
            if (!hdr) {
                std::println(stderr, "Genesis header error: {}",
                             hdr.error().message);
                return exit_code::internal;
            }
            std::ofstream of(config->output_path, std::ios::binary);
            of.write(reinterpret_cast<const char*>(hdr->data()),
                     static_cast<std::streamsize>(hdr->size()));
            if (!of) {
                std::println(stderr, "Genesis header write error: {}",
                             config->output_path);
                return exit_code::internal;
            }
            cli_status("Header: {} ({} bytes)",
                         config->output_path, hdr->size());
        } else {
            // PNG preview.
            auto r = save_preview(config->output_path, st.rendered,
                                  has_transparency, transparency_mask,
                                  config->mode, /*hires=*/false,
                                  /*interlace=*/false);
            if (!r) {
                std::println(stderr, "PNG write error: {}", r.error().message);
                return exit_code::internal;
            }
            cli_status("PNG:    {}", config->output_path);
        }

        if (!config->depfile.empty() && !config->output_path.empty()) {
            std::array<std::string_view, 2> inputs{
                config->input_path, config->palette_file,
            };
            write_depfile(config->depfile, config->output_path, inputs);
        }
        return exit_code::ok;
    }

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
        // The validation block above has already either errored on an
        // explicit HAM-incompatible pick or auto-fallbacked the
        // implicit default to atkinson, so config->dither_method is
        // guaranteed safe for HAM here. Just trust it.
        auto ham_dither = config->dither_method;
        auto ham_data_bits = static_cast<std::size_t>(
            ham_params.bitplane_depth - 2);
        std::size_t ham_base_colors = std::size_t{1} << ham_data_bits;
        if (config->scap && config->mode == amiga::Mode::ham6) {
            cli_print_mode(std::format("HAM6 + SCAP (beam: {})",
                                        config->ham_beam));
        } else {
            cli_print_mode(std::format("HAM{} (beam: {})",
                                        ham_params.bitplane_depth,
                                        config->ham_beam));
        }
        cli_print_palette(std::format(
            "{} base colors, {}-bit MODIFY", ham_base_colors, ham_data_bits));
        cli_print_dither(ham_dither, config->dither_strength);

        // Force transparent pixels to black before HAM encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        // Re-encode the (preprocessed, scaled, cropped) image as PNG so
        // we can route through api::encode_state — same shape as
        // SNES/Genesis/SCAP. Auto-tuning lives in make_api_options now;
        // dither method already includes the HAM-specific override
        // applied above (ham_dither). neutralize_preprocess prevents
        // run_pipeline's own preprocess from double-applying.
        auto src_png = png_io::encode(*image);
        if (!src_png) {
            std::println(stderr, "HAM: source re-encode failed: {}",
                         src_png.error().message);
            return exit_code::internal;
        }
        auto aopts = make_api_options(*config);
        neutralize_preprocess(aopts);
        aopts.dither = std::string{dither_to_options_string(ham_dither)};
        aopts.width = static_cast<int>(image->width());
        aopts.height = static_cast<int>(image->height());
        aopts.on_progress = make_cli_progress_reporter();

        auto enc = api::encode_state(src_png->data(), src_png->size(), aopts);
        if (!enc.ok()) {
            std::println(stderr, "HAM encode error: {}", enc.error_msg);
            return 1;
        }
        auto& st = enc.state;
        int ham_unique = count_unique_colors(st.rendered);
        if (config->copper && !st.scanline_changes.empty()) {
            std::size_t total_ch = 0;
            for (auto& ch : st.scanline_changes) total_ch += ch.size();
            float avg_ch = st.scanline_changes.size() > 0
                ? static_cast<float>(total_ch) / static_cast<float>(st.scanline_changes.size())
                : 0.0f;
            int ham_cap_entries = static_cast<int>(
                st.scanline_changes.size() * st.changes_per_line);
            cli_print_encoded(
                static_cast<int>(st.planes.depth),
                static_cast<int>(st.planes.total_bytes()),
                static_cast<int>(st.palette.size()),
                chipset == amiga::Chipset::aga,
                ham_cap_entries, /*scap=*/0,
                static_cast<int>(st.planes.height),
                static_cast<int>(st.changes_per_line),
                ham_unique,
                std::optional<float>{avg_ch},
                static_cast<double>(st.quant_error), st.psnr, st.ssimulacra2_score);
        } else {
            cli_print_encoded(
                static_cast<int>(st.planes.depth),
                static_cast<int>(st.planes.total_bytes()),
                static_cast<int>(st.palette.size()),
                chipset == amiga::Chipset::aga,
                /*cap=*/0, /*scap=*/0,
                static_cast<int>(st.planes.height),
                /*max_moves=*/0,
                ham_unique, std::nullopt,
                static_cast<double>(st.quant_error), st.psnr, st.ssimulacra2_score);
        }

        if (config->preview)
            show_terminal_preview(st.rendered, config->mode,
                                  config->hires, config->interlace);

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;
                iff_opts.has_transparency = has_transparency;
                if (!st.scanline_palettes.empty()) {
                    iff_opts.scanline_palettes = &st.scanline_palettes;
                }

                auto result = iff::save_ilbm(
                    config->output_path, st.planes,
                    st.palette, config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}", result.error().message);
                    return 1;
                }
                cli_status("IFF:    {}", config->output_path);
            } else if (ends_with(config->output_path, ".h") ||
                       ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                bool is_h = ends_with(config->output_path, ".h");
                auto ch_opts = pipeline::make_ch_opts({
                    .output_path = config->output_path,
                    .symbol_override = config->symbol_name,
                    .hires = config->hires,
                    .interlace = config->interlace,
                    .aga = (chipset == amiga::Chipset::aga),
                    .fade_in = config->fade_in,
                    .total_unique_colors =
                        static_cast<std::size_t>(count_unique_colors(st.rendered)),
                });
                if (!st.scanline_changes.empty()) {
                    ch_opts.copper_changes = &st.scanline_changes;
                    ch_opts.copper_changes_per_line = st.changes_per_line;
                    if (!st.scanline_palettes.empty())
                        ch_opts.copper_scanline_palettes = &st.scanline_palettes;
                }
                // HAM6+SCAP: emit the SCAP mid-line MOVE table the same
                // way EHB+SCAP and DPF+SCAP do. HAM6 shares the EHB slot
                // table (6-plane DMA, 19 mid-line slots — see scap.cpp
                // 2176). Without this the .h / .cpp drops the per-line
                // copper list and the rendered output decodes as plain
                // HAM6 against the base 16-colour palette.
                if (config->scap && !st.scap_line_moves.empty()) {
                    ch_opts.scap_line_moves = &st.scap_line_moves;
                    bool scap_ham6 = config->mode == amiga::Mode::ham6;
                    auto& table = scap_ham6 ? scap::kScap6bplHam6
                                            : scap::kScap6bplOcs;
                    ch_opts.scap_label = scap_ham6 ? "scap_ham6_ocs"
                                                   : "scap_dpf_ocs";
                    ch_opts.scap_anchor_hpos = table.line_gate_hpos;
                    ch_opts.scap_total_planes = table.total_planes;
                    ch_opts.fade_in = false;
                }
                if (!is_h) pad_planes_to_mode(st.planes, config->mode,
                                              config->hires);
                auto result = is_h
                    ? cheader::save(config->output_path, st.planes,
                                    st.palette, config->mode, ch_opts)
                    : cheader::save_viewer(config->output_path, st.planes,
                                           st.palette, config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "{} write error: {}",
                                 is_h ? "C header" : "Viewer",
                                 result.error().message);
                    return 1;
                }
                cli_status("{}: {}", is_h ? "Header" : "Viewer",
                           config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                save_raw(config->output_path, st.planes,
                         st.palette, chipset,
                         config->copper && !st.scanline_changes.empty()
                             ? &st.scanline_changes : nullptr,
                         st.changes_per_line);
            } else if (ends_with(config->output_path, ".pal")) {
                auto result = palette_io::save_ocs_palette(
                    config->output_path, st.palette);
                if (!result) {
                    std::println(stderr, "Palette write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Pal:    {} ({} colors, {} bytes)",
                             config->output_path,
                             st.palette.size(),
                             st.palette.size() * 2);
            } else {
                auto result = save_preview(config->output_path, st.rendered,
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

            // --reserve-range support for EHB+CAP: validate against the
            // 32-base palette (half-brite copies are derived). Build the
            // locked + dither_excluded vectors that flow into encode_copper
            // and the per-row dither below.
            auto reserves_in_ehb_cap = palette_locks::validate_reserves(
                config->reserves, config->locks, /*max_colors=*/32,
                config->lock_color0);
            if (!reserves_in_ehb_cap) {
                std::println(stderr, "{}", reserves_in_ehb_cap.error().message);
                return 1;
            }
            std::vector<std::pair<std::size_t, Color3f>> ehb_cap_locked;
            std::vector<std::size_t> ehb_cap_excluded;
            for (auto& r : config->reserves) {
                auto i = static_cast<std::size_t>(r.index);
                if (r.index >= 0 && i < 32) {
                    ehb_cap_locked.emplace_back(i, palette_locks::to_color(
                        api::LockSpec{r.index, r.r, r.g, r.b},
                        chipset, config->mode));
                    ehb_cap_excluded.push_back(i);
                }
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
                .method  = config->dither_method,
            });
            dith.strength = config->dither_strength_explicit
                            ? config->dither_strength : ehb_cap_tune.strength;
            dith.error_clamp = config->error_clamp_explicit
                              ? config->error_clamp : ehb_cap_tune.error_clamp;

            // Dither: line is now emitted AFTER Mode/Palette below for
            // consistent header-block ordering.

            // Copper encoder optimizes 32 base colors per scanline (depth=5)
            // Interlace: skip swaps on rows 0 and 1 (each field's first
            // displayed line must show base palette only).
            std::size_t skip_initial = config->interlace ? 2 : 0;
            auto w = image->width();
            auto h = image->height();

            // Single-pass EHB+CAP encode. Body factored so best
            // below can replay it under a parallel jitter sweep without
            // duplicating the (copper → re-dither → render) flow.
            struct EhbCapTrial {
                copper::CopperResult copper_result;
                std::vector<std::uint8_t> indices;
                Image rendered;
                float total_error;
            };
            auto encode_once = [&](const Image& img,
                                   const dither::Settings& d,
                                   int diversity,
                                   bool report_progress) -> Result<EhbCapTrial> {
                auto cr = copper::encode_copper(
                    img, 5, d, chipset,
                    static_cast<std::size_t>(config->copper_changes),
                    nullptr, true, ehb_cap_locked, diversity,
                    skip_initial, config->interlace, /*is_ehb=*/true,
                    report_progress
                        ? std::function<void(float, std::string_view)>(
                              make_cli_progress_reporter())
                        : std::function<void(float, std::string_view)>{},
                    // Sentinel → encode_copper picks depth-aware default.
                    config->cap_spread_radius >= 0
                        ? static_cast<std::size_t>(config->cap_spread_radius)
                        : std::numeric_limits<std::size_t>::max(),
                    config->cap_spread_decay >= 0.0f
                        ? config->cap_spread_decay : -1.0f,
                    ehb_cap_excluded);
                if (!cr) return std::unexpected{cr.error()};

                // For reserved base slots, also exclude their half-brite
                // copies (idx + 32) from the per-row 64-color dither
                // candidate set. cand_to_full[y] maps filtered index back
                // to the actual 0..63 EHB slot.
                std::vector<bool> ehb_blocked(64, false);
                for (auto i : ehb_cap_excluded) {
                    if (i < 32) {
                        ehb_blocked[i] = true;
                        ehb_blocked[i + 32] = true;
                    }
                }
                std::vector<std::uint8_t> indices(w * h);
                std::vector<std::vector<color_space::OKLab>> pal_lab_per_row(h);
                std::vector<std::vector<std::uint8_t>> cand_to_full_per_row;
                if (!ehb_cap_excluded.empty()) cand_to_full_per_row.resize(h);
                for (std::size_t y = 0; y < h; ++y) {
                    auto& base32 = cr->scanline_palettes[y];
                    Palette bp;
                    bp.colors.assign(base32.begin(), base32.end());
                    auto ehb64 = palette::make_ehb_palette(bp.colors);
                    if (ehb_cap_excluded.empty()) {
                        pal_lab_per_row[y].resize(ehb64.colors.size());
                        for (std::size_t i = 0; i < ehb64.colors.size(); ++i)
                            pal_lab_per_row[y][i] =
                                color_space::linear_to_oklab(ehb64.colors[i]);
                    } else {
                        pal_lab_per_row[y].reserve(64);
                        cand_to_full_per_row[y].reserve(64);
                        for (std::size_t i = 0; i < ehb64.colors.size(); ++i) {
                            if (ehb_blocked[i]) continue;
                            pal_lab_per_row[y].push_back(
                                color_space::linear_to_oklab(ehb64.colors[i]));
                            cand_to_full_per_row[y].push_back(
                                static_cast<std::uint8_t>(i));
                        }
                    }
                }

                float total_err = dither::diffuse_raw_buffer(
                    img, d,
                    [&](const color_space::OKLab& target,
                        std::size_t x, std::size_t y) -> dither::PickResult {
                        auto& pal_lab = pal_lab_per_row[y];
                        std::size_t k = 0;
                        color_space::OKLab chosen{};
                        float thr = dither::pick_palette_index_with_ostro(
                            d.method, target, pal_lab, x, y,
                            d.strength, /*k_min=*/0, k, chosen);
                        indices[y * w + x] = ehb_cap_excluded.empty()
                            ? static_cast<std::uint8_t>(k)
                            : cand_to_full_per_row[y][k];
                        return {chosen, thr};
                    });

                if (d.method == dither::Method::dbs) {
                    if (ehb_cap_excluded.empty()) {
                        dither::apply_dbs_post_pass(
                            img, indices,
                            [&](std::size_t /*x*/, std::size_t y)
                                -> std::span<const color_space::OKLab> {
                                return pal_lab_per_row[y];
                            });
                    } else {
                        std::vector<std::vector<std::uint8_t>>
                            full_to_cand_per_row(h);
                        for (std::size_t y = 0; y < h; ++y) {
                            full_to_cand_per_row[y].assign(64, 255);
                            auto& cand = cand_to_full_per_row[y];
                            for (std::size_t k = 0; k < cand.size(); ++k)
                                full_to_cand_per_row[y][cand[k]] =
                                    static_cast<std::uint8_t>(k);
                        }
                        std::vector<std::uint8_t> cand_indices(indices.size());
                        for (std::size_t i = 0; i < indices.size(); ++i)
                            cand_indices[i] =
                                full_to_cand_per_row[i / w][indices[i]];
                        dither::apply_dbs_post_pass(
                            img, cand_indices,
                            [&](std::size_t /*x*/, std::size_t y)
                                -> std::span<const color_space::OKLab> {
                                return pal_lab_per_row[y];
                            });
                        for (std::size_t i = 0; i < cand_indices.size(); ++i)
                            indices[i] =
                                cand_to_full_per_row[i / w][cand_indices[i]];
                    }
                }

                if (has_transparency) {
                    for (std::size_t i = 0; i < transparency_mask.size() && i < indices.size(); ++i)
                        if (transparency_mask[i]) indices[i] = 0;
                }

                Image rendered(w, h);
                for (std::size_t y = 0; y < h; ++y) {
                    auto& base32 = cr->scanline_palettes[y];
                    Palette bp;
                    bp.colors.assign(base32.begin(), base32.end());
                    auto ehb64 = palette::make_ehb_palette(bp.colors);
                    for (std::size_t x = 0; x < w; ++x) {
                        auto idx = indices[y * w + x];
                        if (idx < ehb64.colors.size())
                            rendered[x, y] = ehb64.colors[idx];
                    }
                }

                return EhbCapTrial{
                    *std::move(cr),
                    std::move(indices),
                    std::move(rendered),
                    total_err,
                };
            };

            std::optional<EhbCapTrial> winner;
            if (config->best) {
                // Same sweep shape as plain CAP and SCAP EHB: 8 jitter
                // seeds × 5 strengths × 4 diversities + 1 baseline.
                // 32-colour base palette (depth=5 copper); EHB is
                // OCS-bound so amplitude=1.0 (no AGA shimmer concern).
                auto cap_metric = pipeline::parse_best_metric(config->best_metric);
                auto progress_fn = make_cli_progress_reporter();
                winner = pipeline::best_sweep<EhbCapTrial>(
                    *image, dith, config->palette_diversity,
                    /*jitter_count=*/8,
                    [&](const Image& jittered_in,
                        const dither::Settings& d, int div)
                            -> Result<EhbCapTrial> {
                        return encode_once(jittered_in, d, div,
                                           /*report_progress=*/false);
                    },
                    [](const EhbCapTrial& t) -> const Image& {
                        return t.rendered;
                    },
                    progress_fn,
                    /*jitter_amp=*/1.0f,
                    cap_metric);
            }
            if (!winner.has_value()) {
                auto r = encode_once(*image, dith,
                                     config->palette_diversity,
                                     /*report_progress=*/true);
                if (!r) {
                    std::println(stderr, "Copper encode error: {}",
                                 r.error().message);
                    return 1;
                }
                winner = std::move(*r);
            }

            auto& copper_result_obj = winner->copper_result;
            cli_print_mode(std::format(
                "EHB + CAP ({} changes/line, max {} MOVEs/line)",
                copper_result_obj.changes_per_line,
                copper_result_obj.max_moves_per_line));
            cli_print_palette(std::format(
                "{} base + {} half-brite = {} colors",
                copper_result_obj.base_palette.size(),
                copper_result_obj.base_palette.size(),
                copper_result_obj.base_palette.size() * 2));
            cli_print_dither(dith.method, dith.strength);

            auto& all_indices = winner->indices;
            float total_error = winner->total_error;
            (void)all_indices;  // kept for downstream compatibility

            auto planes = bitplane::encode(winner->indices, w, h, ehb_depth);
            if (!planes) {
                std::println(stderr, "Encode error: {}", planes.error().message);
                return 1;
            }

            Image rendered = std::move(winner->rendered);
            // Pointer-rebind so the rest of the existing block (which
            // reads copper_result->...) keeps compiling without
            // wholesale renaming below.
            auto* copper_result_ptr = &copper_result_obj;
            auto& copper_result = copper_result_ptr;

            float ehb_psnr = color_space::compute_psnr_blurred(
                image->pixels(), rendered.pixels(), w, h);
            float ehb_s2 = ssimulacra2::compute(
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
                static_cast<double>(total_error), ehb_psnr, ehb_s2);

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
                    auto ch_opts = pipeline::make_ch_opts({
                        .output_path = config->output_path,
                        .symbol_override = config->symbol_name,
                        .hires = config->hires,
                        .interlace = config->interlace,
                        .fade_in = config->fade_in,
                        .total_unique_colors =
                            static_cast<std::size_t>(count_unique_colors(rendered)),
                    });
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
                    auto ch_opts = pipeline::make_ch_opts({
                        .output_path = config->output_path,
                        .symbol_override = config->symbol_name,
                        .hires = config->hires,
                        .interlace = config->interlace,
                        .total_unique_colors =
                            static_cast<std::size_t>(count_unique_colors(rendered)),
                    });
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
        cli_print_mode("EHB (Extra Half-Brite)");

        // Force transparency to black before encoding (api::run_pipeline
        // does the same internally, but also do it on *image so the
        // mask export below references a consistent source).
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        // Re-encode the (preprocessed, scaled, cropped) image as PNG so
        // we can route through api::encode_state. EHB's full palette
        // logic (64-color base+half-brite, locks, pins, user palette,
        // make_ehb_palette) lives in api::run_pipeline's EHB branch and
        // runs the same code path the web frontend uses.
        auto src_png = png_io::encode(*image);
        if (!src_png) {
            std::println(stderr, "EHB: source re-encode failed: {}",
                         src_png.error().message);
            return exit_code::internal;
        }
        auto aopts = make_api_options(*config);
        neutralize_preprocess(aopts);
        aopts.width = static_cast<int>(image->width());
        aopts.height = static_cast<int>(image->height());
        aopts.on_progress = make_cli_progress_reporter();

        auto enc = api::encode_state(src_png->data(), src_png->size(), aopts);
        if (!enc.ok()) {
            std::println(stderr, "EHB encode error: {}", enc.error_msg);
            return 1;
        }
        auto& st = enc.state;
        // st.palette holds the full 64-entry EHB palette here (the
        // EHB pipeline emits {32 base} ++ {32 half-brite} into
        // result.palette). The SCAP-EHB path (cli_print_palette
        // around line 5347) stores only the 32 base; that
        // inconsistency is the reason this branch can't reuse the
        // same `palette.size() / size()*2` formula. EHB is always
        // 32 + 32 = 64 by definition; print the invariant.
        cli_print_palette("32 base + 32 half-brite = 64 colors");
        cli_print_dither(config->dither_method, aopts.dither_strength);
        cli_print_encoded(
            static_cast<int>(st.planes.depth),
            static_cast<int>(st.planes.total_bytes()),
            static_cast<int>(st.palette.size()),
            /*aga=*/false,
            /*cap=*/0, /*scap=*/0,
            static_cast<int>(st.planes.height), /*max_moves=*/0,
            static_cast<int>(count_unique_colors(st.rendered)),
            std::nullopt,
            static_cast<double>(st.quant_error), st.psnr, st.ssimulacra2_score);

        if (config->preview)
            show_terminal_preview(st.rendered, config->mode,
                                  config->hires, config->interlace);

        // Alias so the existing output-dispatch code below — which
        // expects a `full_palette` Color3f vector — compiles unchanged.
        auto& full_palette = st.palette;

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;

                // IFF writer will trim palette to 32 base colors for EHB
                auto result = iff::save_ilbm(
                    config->output_path, st.planes, full_palette,
                    config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("IFF:    {} ({} bytes)",
                             config->output_path, st.planes.total_bytes());
            } else if (ends_with(config->output_path, ".h")) {
                auto ch_opts = pipeline::make_ch_opts({
                    .output_path = config->output_path,
                    .symbol_override = config->symbol_name,
                    .hires = config->hires,
                    .interlace = config->interlace,
                    .aga = (chipset == amiga::Chipset::aga),
                    .fade_in = config->fade_in,
                    .total_unique_colors =
                        static_cast<std::size_t>(count_unique_colors(st.rendered)),
                });

                auto result = cheader::save(
                    config->output_path, st.planes, full_palette,
                    config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "C header write error: {}",
                                 result.error().message);
                    return 1;
                }
                cli_status("Header: {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                auto ch_opts2 = pipeline::make_ch_opts({
                    .output_path = config->output_path,
                    .symbol_override = config->symbol_name,
                    .hires = config->hires,
                    .interlace = config->interlace,
                    .aga = (chipset == amiga::Chipset::aga),
                    .fade_in = config->fade_in,
                    .total_unique_colors =
                        static_cast<std::size_t>(count_unique_colors(st.rendered)),
                });

                pad_planes_to_mode(st.planes, config->mode, config->hires);
                auto result2 = cheader::save_viewer(
                    config->output_path, st.planes, full_palette,
                    config->mode, ch_opts2);
                if (!result2) {
                    std::println(stderr, "Viewer write error: {}",
                                 result2.error().message);
                    return 1;
                }
                cli_status("Viewer: {}", config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                save_raw(config->output_path, st.planes,
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
                auto result = save_preview(config->output_path, st.rendered,
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
    // Mirror api::run_pipeline's gates: SCAP layers on top of CAP and
    // owns the per-line stream itself, EHB has its own copper branch
    // (line 4745), HAM has its own copper branch (line 4523). Skip the
    // plain CAP path for those so `--cap --scap` doesn't fall into
    // depth-5 CAP and produce a 32-colour result with EHB-mode confusion.
    if (config->copper && !config->scap &&
        !amiga::is_ham(config->mode) &&
        config->mode != amiga::Mode::ehb) {
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
            .method  = config->dither_method,
        });
        dith.strength = config->dither_strength_explicit
                        ? config->dither_strength : cap_tune.strength;
        dith.error_clamp = config->error_clamp_explicit
                          ? config->error_clamp : cap_tune.error_clamp;

        // Dither: line is now emitted AFTER Mode/Palette below.

        // Build locked slot list from --lock-index specs.
        // --reserve-range entries also feed `locked` (so the copper never
        // re-programs them) AND `cap_excluded` (so the dither pass treats
        // them as forbidden across all scanlines).
        std::vector<std::pair<std::size_t, Color3f>> copper_locks;
        for (auto& lock : config->locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            copper_locks.emplace_back(idx,
                palette_locks::to_color(lock, chipset, config->mode));
        }
        auto cap_max_colors = std::size_t{1} << config->depth;
        auto reserves_in_cap = palette_locks::validate_reserves(
            config->reserves, config->locks, cap_max_colors,
            config->lock_color0);
        if (!reserves_in_cap) {
            std::println(stderr, "{}", reserves_in_cap.error().message);
            return 1;
        }
        std::vector<std::size_t> cap_excluded;
        for (auto& r : config->reserves) {
            auto i = static_cast<std::size_t>(r.index);
            if (r.index >= 0 && i < cap_max_colors) {
                copper_locks.emplace_back(i, palette_locks::to_color(
                    api::LockSpec{r.index, r.r, r.g, r.b},
                    chipset, config->mode));
                cap_excluded.push_back(i);
            }
        }

        std::size_t skip_initial_lace = config->interlace ? 2 : 0;

        // Single-pass encoder factored so best below can replay it
        // under a parallel jitter sweep without duplicating the args.
        auto encode_once = [&](const Image& img,
                               const dither::Settings& d, int diversity,
                               std::function<void(float, std::string_view)>
                                   prog) {
            return copper::encode_copper(
                img, config->depth, d, chipset,
                static_cast<std::size_t>(config->copper_changes), nullptr,
                config->lock_color0, copper_locks, diversity,
                skip_initial_lace, config->interlace, /*is_ehb=*/false,
                std::move(prog),
                config->cap_spread_radius >= 0
                    ? static_cast<std::size_t>(config->cap_spread_radius)
                    : std::numeric_limits<std::size_t>::max(),
                config->cap_spread_decay >= 0.0f
                    ? config->cap_spread_decay : -1.0f,
                cap_excluded);
        };

        Result<copper::CopperResult> copper_result;
        if (config->best) {
            // Plain CAP: 8 jitter seeds (16-colour palette has shallower
            // basins than DPF's 8-colour PF2; 8 seeds × 5×4 = 161 trials,
            // ~30–60 s on 8 cores). Same parallel-sweep machinery as
            // SCAP DPF/EHB via pipeline::best_sweep.
            struct CapTrial {
                copper::CopperResult result;
                Image rendered;
            };
            // AGA's continuous-RGB median-cut means a full ±1/255 jitter
            // amplitude can drift the picked palette far enough to
            // introduce per-line swap shimmer that PSNR doesn't see.
            // OCS's discrete 12-bit gamut already snaps small nudges.
            float jitter_amp = (chipset == amiga::Chipset::aga)
                ? 0.4f : 1.0f;
            auto cap_metric = pipeline::parse_best_metric(config->best_metric);
            auto best = pipeline::best_sweep<CapTrial>(
                *image, dith, config->palette_diversity,
                /*jitter_count=*/8,
                [&](const Image& jittered_in,
                    const dither::Settings& d, int div) -> Result<CapTrial> {
                    auto enc = encode_once(jittered_in, d, div, {});
                    if (!enc) return std::unexpected{enc.error()};
                    auto preview = pipeline::render_preview(
                        enc->planes, enc->base_palette,
                        /*is_ham=*/false, config->interlace, chipset,
                        &enc->scanline_palettes, enc->changes_per_line);
                    if (!preview) return std::unexpected{preview.error()};
                    return CapTrial{*std::move(enc), *std::move(preview)};
                },
                [](const CapTrial& t) -> const Image& { return t.rendered; },
                make_cli_progress_reporter(),
                jitter_amp,
                cap_metric);
            if (best.has_value()) {
                copper_result = std::move(best->result);
            } else {
                copper_result = encode_once(*image, dith,
                                            config->palette_diversity,
                                            make_cli_progress_reporter());
            }
        } else {
            copper_result = encode_once(*image, dith,
                                        config->palette_diversity,
                                        make_cli_progress_reporter());
        }
        if (!copper_result) {
            std::println(stderr, "Copper encode error: {}",
                         copper_result.error().message);
            return 1;
        }
        // Print actual cpl after orchestration (auto mode may have stretched
        // or fallen back).
        cli_print_mode(std::format(
            "{}CAP ({} changes/line, max {} MOVEs/line)",
            config->dual_playfield ? "DPF + " : "",
            copper_result->changes_per_line,
            copper_result->max_moves_per_line));
        cli_print_palette(std::format(
            "{} colors", copper_result->base_palette.size()));
        cli_print_dither(dith.method, dith.strength);

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
        // Use the capped renderer so the preview reflects the top-K-
        // clipped diff cascade the cheader emitter applies to lace
        // copper lists. Without the cap, the preview shows the
        // planner's idealised per-row palette and silently diverges
        // from what the chip actually displays — most visibly on OCS
        // depth ≤ 4 where the vertical palette dither inflates the
        // per-row-pair diff count past the K budget.
        auto preview = pipeline::render_preview(
            copper_result->planes, copper_result->base_palette,
            /*is_ham=*/false, config->interlace, chipset,
            &copper_result->scanline_palettes,
            copper_result->changes_per_line);
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
        float cop_s2 = ssimulacra2::compute(
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
            static_cast<double>(copper_result->total_error), cop_psnr, cop_s2);

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
                auto ch_opts = pipeline::make_ch_opts({
                    .output_path = config->output_path,
                    .symbol_override = config->symbol_name,
                    .hires = config->hires,
                    .interlace = config->interlace,
                    .aga = (chipset == amiga::Chipset::aga),
                    .fade_in = config->fade_in,
                    .dpf = use_dpf_std,
                    .total_unique_colors =
                        static_cast<std::size_t>(count_unique_colors(*preview)),
                });
                ch_opts.copper_changes = &copper_result->scanline_changes;
                ch_opts.copper_changes_per_line = copper_result->changes_per_line;
                // Pass scanline_palettes so cheader's lace_rebuild can
                // recompute per-field same-row diffs in interlace; without
                // it the emitted .h carries the encoder's progressive
                // per-row list (which is wrong for lace, where field 1
                // walks 0,2,4,... and field 2 walks 1,3,5,...).
                ch_opts.copper_scanline_palettes = &copper_result->scanline_palettes;

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
                auto ch_opts2 = pipeline::make_ch_opts({
                    .output_path = config->output_path,
                    .symbol_override = config->symbol_name,
                    .hires = config->hires,
                    .interlace = config->interlace,
                    .aga = (chipset == amiga::Chipset::aga),
                    .fade_in = config->fade_in,
                    .dpf = use_dpf_std,
                    .total_unique_colors =
                        static_cast<std::size_t>(count_unique_colors(*preview)),
                });
                ch_opts2.copper_changes = &copper_result->scanline_changes;
                ch_opts2.copper_changes_per_line = copper_result->changes_per_line;
                // Pass scanline_palettes so cheader's lace_rebuild can
                // recompute per-field same-row diffs (see .h branch above).
                ch_opts2.copper_scanline_palettes = &copper_result->scanline_palettes;

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
        // HAM6+SCAP is handled by the HAM branch above, so the gate here
        // only needs to cover the EHB/DPF combos. Keep the error wording
        // explicit about all three legal forms.
        if ((!scap_dpf && !scap_ehb) || chipset != amiga::Chipset::ocs ||
            config->interlace) {
            std::println(stderr,
                "Error: --scap requires OCS (no interlace) with either "
                "--dpf lores depth=3, --mode ehb, or --mode ham6.");
            return 1;
        }
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        // Re-encode the (preprocessed, scaled, cropped) image as PNG so
        // we can hand it to api::encode_state — same shape as the
        // SNES/Genesis paths. encode_state runs the canonical SCAP
        // pipeline (scap::encode_scap_*_ocs internally) and populates
        // st.scap_* + the per-line move-budget breakdown the printer
        // below consumes.
        auto src_png = png_io::encode(*image);
        if (!src_png) {
            std::println(stderr, "SCAP: source re-encode failed: {}",
                         src_png.error().message);
            return exit_code::internal;
        }
        auto sopts = make_api_options(*config);
        // *image already preprocessed; stop run_pipeline preprocessing
        // these bytes a second time.
        neutralize_preprocess(sopts);
        sopts.scap = true;
        sopts.scap_debug = config->scap_debug;
        sopts.width = static_cast<int>(image->width());
        sopts.height = static_cast<int>(image->height());
        sopts.on_progress = make_cli_progress_reporter();
        // SCAP layers on top of CAP since b0be343; force the flag so
        // dither_tuning's per-mode defaults inside run_pipeline match
        // what the inline path computed.
        sopts.copper = true;

        auto enc = api::encode_state(src_png->data(), src_png->size(), sopts);
        if (!enc.ok()) {
            std::println(stderr, "SCAP encode error: {}", enc.error_msg);
            return 1;
        }
        auto& st = enc.state;

        const char* scap_label = scap_ehb
            ? "OCS EHB 6bpp"
            : "OCS DPF";
        cli_print_mode(std::format(
            "SCAP ({}, {} slots, {:.1f} useful swaps/line)",
            scap_label, st.scap_slot_count, st.copper_changes));
        if (scap_ehb) {
            // EHB SCAP: st.palette holds 32 base; halfbrites are derived.
            cli_print_palette(std::format(
                "{} base + {} half-brite = {} colors",
                st.palette.size(), st.palette.size(), st.palette.size() * 2));
        } else {
            // DPF SCAP: st.palette is the 16-entry COLOR00..15 register
            // layout (output_palette in scap.cpp); only 8 of those slots
            // are actually used (PF2 has 3 bitplanes → 8 colours).
            // Report the visible-colour count, not the register count.
            std::size_t pf2_colors = std::size_t{1} << 3;  // OCS DPF lores
            cli_print_palette(std::format(
                "{} colors (PF2 3bpl)", pf2_colors));
        }
        cli_print_dither(config->dither_method, config->dither_strength);
        cli_status("Copper:   hblank avg {:.1f} (max {}), visible avg "
                     "{:.1f} (max {}), total avg {:.1f} (max {}/line)",
                     st.scap_avg_hblank_moves_per_line,
                     st.scap_max_hblank_moves_per_line,
                     st.scap_avg_visible_moves_per_line,
                     st.scap_max_visible_moves_per_line,
                     st.scap_avg_total_moves_per_line,
                     st.max_moves_per_line);
        int scap_ops = 0;
        for (auto& moves : st.scap_line_moves) scap_ops += static_cast<int>(moves.size());
        cli_print_encoded(
            static_cast<int>(st.planes.depth),
            static_cast<int>(st.planes.total_bytes()),
            static_cast<int>(st.palette.size()),
            /*aga=*/false,
            /*cap_grid_entries=*/0,
            scap_ops,
            static_cast<int>(st.rendered.height()),
            static_cast<int>(st.max_moves_per_line),
            static_cast<int>(count_unique_colors(st.rendered)),
            std::optional<float>{st.copper_changes},
            static_cast<double>(st.quant_error), st.psnr, st.ssimulacra2_score);

        if (config->preview)
            show_terminal_preview(st.rendered, config->mode,
                                  config->hires, config->interlace);

        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".png")) {
                auto r = save_preview(config->output_path,
                                      st.rendered,
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
                auto ch_opts = pipeline::make_ch_opts({
                    .output_path = config->output_path,
                    .symbol_override = config->symbol_name,
                    .dpf = scap_dpf,        // false for EHB SCAP
                    .total_unique_colors =
                        static_cast<std::size_t>(count_unique_colors(st.rendered)),
                });
                ch_opts.scap_line_moves = &st.scap_line_moves;
                ch_opts.scap_label = scap_ehb ? "scap_ehb_ocs"
                                              : "scap_dpf_ocs";
                // Use the static slot tables (same shape encode_state
                // ran against); matches api.cpp::convert_viewer.
                auto& table = scap_ehb ? scap::kScap6bplEhb
                                       : scap::kScap6bplOcs;
                ch_opts.scap_anchor_hpos = table.line_gate_hpos;
                ch_opts.scap_total_planes = table.total_planes;
                pad_planes_to_mode(st.planes, config->mode, config->hires);
                bool is_h = ends_with(config->output_path, ".h");
                auto r = is_h
                    ? cheader::save(
                        config->output_path, st.planes,
                        st.palette, config->mode, ch_opts)
                    : cheader::save_viewer(
                        config->output_path, st.planes,
                        st.palette, config->mode, ch_opts);
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

    // Mode line for the standard bitplane path: short hardware-flavour
    // label so every Amiga / DOS / Atari path prints a Mode: header,
    // matching CAP / SCAP / HAM / EHB.
    cli_print_mode(std::format(
        "{}{} ({}bpp)",
        config->dual_playfield ? "DPF + " : "",
        mode_to_options_string(config->mode),
        config->dual_playfield
            ? ((chipset == amiga::Chipset::aga) ? 8 : 6)
            : static_cast<int>(amiga::get_mode_params(config->mode).bitplane_depth)));

    // Force transparent pixels to black before quantization/encoding
    if (has_transparency) {
        for (std::size_t i = 0; i < transparency_mask.size(); ++i)
            if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
    }

    // Build palette
    auto max_colors = std::size_t{1} << config->depth;
    auto is_atari_std = amiga::is_atari(config->mode);
    bool user_pal_std = !config->palette_file.empty();
    auto lock_zero_std = !user_pal_std && config->lock_color0 &&
                             (has_transparency || !is_atari_std);

    // Validate locks/pins
    if (auto v = palette_locks::validate_locks(config->locks, max_colors); !v) {
        std::println(stderr, "{}", v.error().message);
        return 1;
    }
    if (auto v = palette_locks::validate_pins(config->pins, config->locks, max_colors,
                                              image->width(), image->height(),
                                              lock_zero_std); !v) {
        std::println(stderr, "{}", v.error().message);
        return 1;
    }
    auto reserves_in_pal_std = palette_locks::validate_reserves(
        config->reserves, config->locks, max_colors, lock_zero_std);
    if (!reserves_in_pal_std) {
        std::println(stderr, "{}", reserves_in_pal_std.error().message);
        return 1;
    }
    std::size_t reserve_count_std = *reserves_in_pal_std;
    std::vector<bool> reserved_mask_std(max_colors, false);
    for (auto& r : config->reserves) {
        auto i = static_cast<std::size_t>(r.index);
        if (r.index >= 0 && i < max_colors) reserved_mask_std[i] = true;
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
            cli_status("Palette:  CGA composite, 16 colors (NTSC artifact, old CGA)");
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
            cli_status("Palette:  CGA {} (bg=0x{:X}), 4 colors{}",
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
            cli_status("Palette:  CGA mono, 2 colors (bg=0x{:X}, fg=white)",
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
        cli_status("Palette:  {} colors (loaded from {})",
                     pal.size(), config->palette_file);
    } else if (amiga::is_atari_hi(config->mode)) {
        // Monochrome: fixed white + black palette
        pal.colors = {Color3f{1.0f, 1.0f, 1.0f}, Color3f{0.0f, 0.0f, 0.0f}};
        cli_status("Palette:  2 colors (monochrome)");
    } else if (config->mode == amiga::Mode::ega_320 ||
               config->mode == amiga::Mode::ega_640) {
        // EGA 200-line CGA-compat: palette order must be kCgaHw exactly.
        // The ATC register layout we emit (CGA-compat IRGB with b4 = I)
        // assumes palette slot i corresponds to kCgaHw[i]. Going through
        // assemble_locked_palette with lock_color0 prepends a second
        // black at slot 0 and shifts the whole palette up by 1, dropping
        // white off the end — found via ATCPROBE/CGA16 test image.
        pal.colors.reserve(16);
        for (auto hex : palette::kCgaHw)
            pal.colors.push_back(color_space::srgb_hex_to_linear(hex));
        cli_status("Palette:  16 colors (kCgaHw, EGA CGA-compat IRGB)");
    } else {
        auto qcount = palette_locks::quant_count(max_colors, config->locks, lock_zero_std);
        if (qcount > reserve_count_std) qcount -= reserve_count_std;
        else                            qcount = 1;
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
            *quantized, config->locks, max_colors, lock_zero_std,
            chipset, config->mode);
        pal = std::move(assembled.palette);
        std_locked = std::move(assembled.locked);
        // Overlay --reserve-range: snap user RGB to chipset, drop into
        // its slot, mark locked so refine treats as fixed.
        for (auto& r : config->reserves) {
            auto i = static_cast<std::size_t>(r.index);
            if (r.index >= 0 && i < max_colors) {
                pal.colors[i] = palette_locks::to_color(
                    api::LockSpec{r.index, r.r, r.g, r.b}, chipset, config->mode);
                std_locked[i] = true;
            }
        }
        cli_status("Palette:  {} colors (auto, {})",
                     pal.size(),
                     amiga::is_stf(config->mode) ? "STF 9-bit" :
                     amiga::is_vga(config->mode) ? "VGA 18-bit" :
                     amiga::is_ega(config->mode) ? "EGA 6-bit (2bpc)" :
                     chipset == amiga::Chipset::aga ? "median-cut" : "OCS brute-force");
    }

    if (config->match_range) {
        preprocess::match_palette_range(*image, pal);
    }

    // Apply dithering. Pull mode-aware defaults from dither_tuning so
    // hires / lores-lace / ehb / etc. get the per-mode strength tuned
    // against SSIMULACRA2 rather than the unsafe-at-low-color
    // strength=1.0 fallback (which used to runaway-FS at hires d=4).
    auto pal_size = std::min(pal.size(), max_colors);

    dither::Settings dith;
    dith.method = config->dither_method;
    auto plain_tune = dither_tuning::defaults_for(dither_tuning::Context{
        .mode    = config->mode,
        .depth   = static_cast<int>(config->depth),
        .dpf     = config->dual_playfield,
        .scap    = false,
        .copper  = false,
        .chipset = chipset,
        .method  = config->dither_method,
    });
    dith.strength = config->dither_strength_explicit
        ? config->dither_strength : plain_tune.strength;
    dith.error_clamp = config->error_clamp_explicit
        ? config->error_clamp : plain_tune.error_clamp;

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
        !skip_refine && reserve_count_std == 0) {
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

    cli_print_dither(dith.method, dith.strength);

    dither::DitherResult dither_result;
    // Build candidate palette: skip reserved slots (and slot 0 if
    // transparency, the legacy "color 0 = transparent" rule).
    if (reserve_count_std > 0 || has_transparency) {
        std::vector<Color3f> cand_pal;
        std::vector<std::uint8_t> cand_to_full;
        cand_pal.reserve(pal_size);
        cand_to_full.reserve(pal_size);
        for (std::size_t i = 0; i < pal_size; ++i) {
            if (has_transparency && i == 0) continue;
            if (reserved_mask_std[i]) continue;
            cand_pal.push_back(pal.colors[i]);
            cand_to_full.push_back(static_cast<std::uint8_t>(i));
        }
        std::span<const Color3f> dither_span{cand_pal.data(), cand_pal.size()};
        dither_result = dither::apply(*image, dither_span, dith);
        for (auto& idx : dither_result.indices) idx = cand_to_full[idx];
        if (has_transparency) {
            for (std::size_t i = 0; i < dither_result.indices.size(); ++i) {
                if (transparency_mask[i]) dither_result.indices[i] = 0;
            }
        }
    } else {
        dither_result = dither::apply(*image, pal_span, dith);
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
    auto preview = pipeline::render_preview(
        *planes, used_palette,
        /*is_ham=*/false, config->interlace, chipset);
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
    float std_s2 = ssimulacra2::compute(
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
        static_cast<double>(dither_result.total_error), std_psnr, std_s2);

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
            auto ch_opts = pipeline::make_ch_opts({
                .output_path = config->output_path,
                .symbol_override = config->symbol_name,
                .hires = config->hires,
                .interlace = config->interlace,
                .aga = (chipset == amiga::Chipset::aga),
                .fade_in = config->fade_in,
                .dpf = use_dpf_std,
                .total_unique_colors =
                    static_cast<std::size_t>(count_unique_colors(*preview)),
            });

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
                auto ch_opts = pipeline::make_ch_opts({
                    .symbol_override = symbol,
                    .hires = config->hires,
                    .interlace = config->interlace,
                    .aga = (chipset == amiga::Chipset::aga),
                    .fade_in = config->fade_in,
                    .dpf = use_dpf_std,
                    .total_unique_colors =
                        static_cast<std::size_t>(count_unique_colors(*preview)),
                });

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
