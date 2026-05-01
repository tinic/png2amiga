#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "copper.hpp"
#include "scap.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace png2amiga::api {

// Lock a palette slot to a specific color (sRGB 0-255), pre-quantization.
// The quantizer fills the remaining slots, and the locked color is reachable
// by the dither phase. Has no effect on HAM modes (palette is dynamic) or
// copper modes (per-scanline palettes).
// Centralised disk + chip-RAM accounting for an encoded image. ALL
// reporting sites (CLI "Encoded" line, web disk/chip readout, viewer
// header comments) must read from this — never re-derive size locally,
// or numbers go stale the next time a mode adds new payload (this bit
// us with EHB+SCAP, where scap_line_moves was missing from every call
// site's bespoke math).
//
// Inputs:
//   plane_bytes    — bitplane data size (e.g. planes.total_bytes()).
//   palette_size   — number of colour entries (Color3f count).
//   aga            — chipset is AGA (palette costs 2× and copper writes
//                    cost 2× per change for hi+lo nibble passes).
//   cap_changes    — total CAP scanline changes across the frame
//                    (e.g. sum of scanline_changes[*].size()) OR 0.
//                    Note: for the .raw grid layout we use the
//                    (height × cpl) pre-allocated size, not the actual
//                    count — pass that pre-multiplied number here too.
//   scap_op_count  — total SCAP WAIT/MOVE ops across the frame
//                    (sum of scap_line_moves[*].size()) OR 0.
//   height         — image height (used for chip-RAM per-line copper
//                    list sizing).
//   max_moves      — worst-case copper MOVEs/line (chip RAM uses this
//                    to budget the per-line WAIT+MOVEs slot).
struct SizeBreakdown {
    int plane_bytes{};      // bitplane data
    int palette_bytes{};    // palette in .raw / .pal layout
    int copper_bytes{};     // CAP grid + SCAP per-line ops
    int disk_bytes{};       // .raw total = planes + palette + copper
    int chip_bytes{};       // worst-case Amiga chip-RAM
};

constexpr int kCopperInitSetupBytes = 80;
constexpr int kCopperEndMarkerBytes = 32;

inline SizeBreakdown compute_size_breakdown(
    int plane_bytes,
    int palette_size,
    bool aga,
    int cap_grid_entries,    // height × cpl (CAP .raw grid)
    int scap_op_count,       // sum of scap_line_moves[*].size()
    int height,
    int max_moves) {
    SizeBreakdown s;
    s.plane_bytes = plane_bytes;
    s.palette_bytes = palette_size * (aga ? 4 : 2);
    int cap_bytes = cap_grid_entries * (aga ? 8 : 4);  // 4 B/word, AGA = hi+lo
    int scap_bytes = scap_op_count * 4;                // 4 B per WAIT/MOVE word
    s.copper_bytes = cap_bytes + scap_bytes;
    s.disk_bytes = s.plane_bytes + s.palette_bytes + s.copper_bytes;
    int has_copper = (s.copper_bytes > 0) ? 1 : 0;
    int pal_setup = palette_size * (aga ? 8 : 4);
    int per_line_words = 1 + max_moves;
    int cop_list_bytes = has_copper ? (height * per_line_words * 4) : 0;
    s.chip_bytes = s.plane_bytes + kCopperInitSetupBytes + pal_setup +
                   cop_list_bytes + kCopperEndMarkerBytes;
    return s;
}

struct LockSpec {
    int index;       // palette slot, 0..max_colors-1
    int r, g, b;     // sRGB 0-255
};

// Pin a palette slot to whatever color the source pixel at (x, y) ends up
// at after quantization+dithering. Implemented as a post-dither index swap:
// the index pixel(x,y) holds is swapped (palette + index map) with `index`.
// Pin targets must NOT be locked. Has no effect on HAM/copper modes.
struct PinSpec {
    int index;       // target palette slot
    int x, y;        // source pixel coordinates
};

// Canonical encoder schema. Every parameter the pipeline (preprocess →
// quantize → dither → encode) honours lives here. The CLI parses into
// main.cpp's Config and translates via make_api_options() (one-line
// bridge in main.cpp); the WASM frontend builds Options directly from
// JS. New encoder knobs should land here FIRST, then the bridge picks
// them up — see REFACTOR_PLAN.md target #5.
struct Options {
    std::string mode = "lores";         // lores, hires, ham6, ham8, ehb
    std::string chipset;                // "ocs", "aga", or "" for auto
    int depth = 5;                      // bitplane depth (1-8, mode-dependent)
    bool interlace = false;             // set LACE bit in CAMG
    float gamma = 1.0f;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float hue_shift = 0.0f;
    float sharpen = 0.0f;
    float black_point = 0.0f;
    float white_point = 0.0f;
    bool match_range = false;
    int width = 0;                      // override output width (0 = mode default)
    int height = 0;                     // override output height (0 = mode default)

    // Dithering
    std::string dither = "ostromoukhov";    // Top of the ED leaderboard
                                            // (mean PSNR across 10 images × 6
                                            // modes). Other ED options:
                                            // sierra-lite, atkinson, jarvis,
                                            // floyd-steinberg, stucki, gilbert,
                                            // riemersma. Plus ordered methods
                                            // (bayer*, checker, line*, etc.)
                                            // and palette-aware (yliluoma*,
                                            // knoll, opt-*, tri-tone).
    float dither_strength = 1.0f;       // 0.0 = no dither, 1.0 = full
    float error_clamp = 0.12f;          // max error per OKLab channel

    // Palette
    std::string palette_file;           // load palette from file (empty = auto)
    std::vector<std::uint8_t> palette_data; // inline palette data (empty = auto)
    int palette_diversity = 0;          // 0 = off, 1-5 = remove near-duplicate
                                        // colors, re-seed from worst-served pixels
    std::string quantizer;              // "", "auto" = auto-select, or
                                        // "median-cut", "ocs-bruteforce", "pnn"

    // HAM greedy encoder (realtime profile)
    bool ham_fast = false;

    // Dither-aware palette refinement iterations (0 = off). Run after
    // initial quantization to tighten the palette against the actual
    // dithered output. Skipped for chunky / fixed-palette modes
    // (CGA / EGA / VGA / Atari hi-res) inside run_pipeline.
    int refine_iterations = 4;

    // HAM encoding
    int ham_beam = 16;                   // beam width for DP search (1-256)
    int ham_triple = 16;                 // triple-pixel refinement post-pass
                                         // beam width (0 = off, 16 default)

    // HAM op-selection metric. "oklab2" (default) uses perceptually-
    // uniform OKLab² distance — measured ~+9 SSIMULACRA2 points and
    // ~+3.5 OKLab-dB on banded content vs sRGB-MSE. "srgb-mse" optimises
    // headline sRGB-PSNR directly (~+0.5-1 dB nominal gain) but produces
    // visibly worse output per SSIMULACRA2 — only useful when reporting
    // PSNR is the literal goal. Compile-time dispatched.
    std::string ham_metric = "oklab2";

    // best ranking metric. "msssim" (default) produces a cleaner
    // image and tracks SSIMULACRA2 better; "psnr" keeps maximum
    // fine detail at the cost of perceptual quality.
    // User flips via --best-metric.
    std::string best_metric = "msssim";
    bool best = false;               // multi-candidate CAP planner +
                                         // joint base-palette refinement.
                                         // HAM6 + copper and HAM8 + copper
                                         // only — indexed copper modes
                                         // ignore this flag (their planner
                                         // is already mature and refinement
                                         // gives ≤+0.10 dB).
                                         // ~4-5× cost, +0.5..4 dB PSNR

    // Optional progress callback. Called periodically with (progress 0..1,
    // stage label). Currently emitted by HAM6+CAP encoders. Callback may
    // run on encoder threads — must be thread-safe.
    std::function<void(float, std::string_view)> on_progress;

    // Copper palette (per-scanline palette changes)
    bool copper = false;                // use per-scanline copper palettes
    int copper_changes = 0;             // override changes/line (0 = auto)
    // Per-line palette planner neighbour-row smoothing. -1 means use the
    // encode_copper default (radius=4, decay=0.85). Exposed for sweep
    // tooling (--cap-spread-radius / --cap-spread-decay on the CLI).
    int cap_spread_radius = -1;
    float cap_spread_decay = -1.0f;

    // Transparency
    float alpha_threshold = 0.0f;       // offset from 0.5 midpoint (-0.5..0.5)
    std::string alpha_dither;           // alpha dither method (empty = hard threshold)
    float alpha_dither_strength = 1.0f; // dither pattern intensity

    // C header output
    std::string symbol_name;            // base name for C symbols (default: "image")

    // Mask export
    bool mask_invert = false;           // invert mask polarity (default: 1=opaque, 0=transparent)

    // Cropping
    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;                     // 0 = no crop
    int crop_h = 0;
    bool crop_auto = false;             // auto-crop to mode aspect ratio (center)

    // Advanced
    bool reserve_color0 = true;         // reserve index 0 for black (border/background)

    // Amiga dual playfield. When true, encode the image as PF2 of a
    // dual-playfield display: bitplane depth is forced to 3 (OCS, 8 colors)
    // or 4 (AGA, 16 colors), the encoded planes are placed in the even
    // hardware bitplanes (PF2), the odd planes (PF1, foreground) are
    // emitted as zero, and the palette is shifted into the upper color
    // registers (8-15 OCS / 16-31 AGA) so PF2 pixels look up there. Sets
    // the CAMG DPF flag (0x0400) and BPLCON0 DBLPF bit. Only valid for
    // standard lores/hires modes (no HAM/EHB/copper/Atari/DOS).
    bool dual_playfield = false;

    // SCAP calibration probe selector — DPF-only.
    //   ""   : disabled (default)
    //   "a"  : Probe A (slot HPOS sweep, OCS DPF)
    //   "b"  : Probe B (slot capacity, OCS DPF) — placeholder
    //   "c"  : Probe C (AGA bandwidth) — placeholder
    //   "d"  : Probe D (at-x vs after-x mapping) — placeholder
    // Bypasses the normal pipeline: synthesizes a calibration image plus
    // per-line WAIT/MOVE copper ops and emits a viewer .cpp/.adf you run on
    // real hardware. Slot tables are populated by hand from the observed
    // results, then the production planner uses them.
    std::string scap_probe;

    // SCAP encoder — DPF-only mid-line palette swaps. Requires
    // dual_playfield + chipset=ocs + mode=lores (no interlace) for
    // Phase 1. Composes with copper (each line's per-strip palette
    // chain is independent of CAP's end-of-line writes).
    bool scap = false;
    // SCAP slot-tuning debug bundle: forces base-palette MOVEs to
    // 0x0000 AND paints yellow PF1 rulers at every 4/8/16 px. Always
    // used together. Off in production.
    bool scap_debug = false;

    // IBM PC / DOS modes only. If true, preserve source aspect ratio by
    // letterboxing or pillarboxing the image inside the fixed hardware
    // buffer (padded with black). If false (default), stretch the image
    // to fill the full buffer.
    bool native_par = false;

    // CGA text mode only: per-cell error metric for the glyph + (fg, bg)
    // brute-force search. "blur" (Pappas-Neuhoff perceptual halftoning,
    // default) or "mse" (per-pixel OKLab; pairs with --dither). The
    // blur metric expects a continuous source — pipeline ignores any
    // pre-dither stage when blur is selected.
    std::string cga_text_metric = "blur";

    // C64 / VIC-II palette selector. One of: pepto, vice, colodore
    // (default), deekay, godot, c64wiki, levy. Only affects c64-* modes;
    // ignored everywhere else. The VIC-II palette has no unique sRGB
    // ground-truth — different tools/scenes prefer different RGB tables.
    // Colodore is the default (matches png2c64) — it's measurement-based
    // so colours track real CRT output more closely than other tables.
    std::string c64_palette = "colodore";

    // Per-cell error metric for C64 modes. Mirrors png2c64's metric
    // option — useful for tweaking and A/B comparison.
    //   "mse" (default): per-pixel OKLab² nearest distance. Matches
    //          png2c64's default exactly so quality is directly
    //          comparable.
    //   "blur": Pappas-Neuhoff perceptual blur in sRGB (gamma-encoded
    //          space matches what the CRT emits and the eye averages
    //          through display blur).
    //   "ssim": Structural Similarity Index in sRGB.
    std::string c64_metric = "mse";

    // PETSCII only: when true, restrict the candidate glyph set to
    // PETSCII semi-graphics, blocks, and the reverse-video graphics
    // (~130 chars) — skips letters / digits / punctuation. Useful
    // for halftone-style art where letter shapes in smooth regions
    // would look wrong.
    bool c64_petscii_graphics_only = false;

    // Palette index manipulation (lores/hires/EHB/Atari only)
    std::vector<LockSpec> locks;
    std::vector<PinSpec>  pins;
};

struct ConvertResult {
    std::vector<std::uint8_t> data;     // output file bytes (IFF or PNG)
    int width{};
    int height{};
    int depth{};                        // bitplane depth
    int colors{};                       // number of palette colors
    float copperChanges{};              // avg actual color changes per line (0 if no copper)
    int totalColors{};                  // unique colors in rendered output
    int planeBytes{};                   // raw bitplane data size
    int copperBytes{};                  // copper-list bytes total (CAP scanline_changes
                                        // + SCAP per-line moves, 4 B per word).
                                        // 0 if mode has no copper. Use this for chip-RAM
                                        // / disk-cost reporting — call sites must NOT
                                        // re-derive size from {scanlines, cpl} because new
                                        // modes (DPF/EHB+SCAP) carry extra data they'd miss.
    int diskBytes{};                    // .raw file size: bitplanes + palette + copper data.
                                        // Computed centrally in make_result so call sites
                                        // can't drift stale as new modes are added.
    int chipBytes{};                    // worst-case chip-RAM cost: bitplanes + copper list
                                        // (init MOVEs + per-line WAIT+MOVEs at max_moves_per_line)
                                        // + palette setup. Same single-source-of-truth rule
                                        // as diskBytes.
    int changesPerLine{};               // K used by encoder (post auto-stretch)
    int maxMovesPerLine{};              // worst-case copper MOVEs per scanline
    bool aga{};                         // chipset is AGA (true ⇒ palette has hi+lo halves)
    float quantError{};                 // perceptual encoding error (OKLab ΔE² sum)
    float psnr{};                       // PSNR in dB (Gaussian-blurred sRGB)
    float s2{};                         // SSIMULACRA2 score (Cloudinary 2022)
    int genesisUniqueTiles{};           // Tiled modes: unique tiles in VRAM
    int genesisTotalCells{};             // Tiled modes: tilemap cells (W/8 × H/8); 0 = not tiled
    int tileDataBytes{};                 // Tiled modes: unique × bytes-per-tile (32 for Genesis 4bpp, 64 for SNES Mode 7 8bpp)
    bool hasTransparency{};             // source image had alpha channel
    std::string error;                  // error message (empty on success)
};

// Convert raw image data (PNG/JPEG bytes) to Amiga format.
// Returns PNG bytes of the converted (preview) image.
ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options);

// Convert raw image data and return IFF ILBM bytes.
ConvertResult convert_iff(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// Convert raw image data and return C header (.h) as UTF-8 text bytes.
ConvertResult convert_cheader(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options);

// Convert raw image data and return Degas .PI1/.PI2 file bytes (Atari ST/STE).
ConvertResult convert_degas(const std::uint8_t* input_data,
                            std::size_t input_size,
                            const Options& options);

// Convert raw image data and return standalone viewer .cpp source (UTF-8 text).
ConvertResult convert_viewer(const std::uint8_t* input_data,
                             std::size_t input_size,
                             const Options& options);

// Convert raw image data and return RGBA pixel data (for live preview).
ConvertResult convert_rgba(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options);

// Convert raw image data and return raw bitplane bytes (interleaved, no header).
ConvertResult convert_raw(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// Convert raw image data and return OCS 12-bit palette bytes
// (2 bytes per color, big-endian 0x0RGB).
ConvertResult convert_palette(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options);

// Convert raw image data and return the 1-bit transparency mask.
// Output format depends on caller:
//   - convert_mask: returns PNG bytes (1-bit B/W)
//   - convert_mask_raw: returns raw 1-bitplane data (word-aligned)
//   - convert_mask_iff: returns IFF ILBM (1 bitplane, B/W palette)
// White (1) = opaque, black (0) = transparent (or inverted with mask_invert).
// Returns empty data if source has no transparency.
ConvertResult convert_mask(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options);

ConvertResult convert_mask_raw(const std::uint8_t* input_data,
                               std::size_t input_size,
                               const Options& options);

ConvertResult convert_mask_iff(const std::uint8_t* input_data,
                               std::size_t input_size,
                               const Options& options);

// Per-mode dither tuning defaults (Floyd-Steinberg-tuned). Returns the
// (strength, error_clamp) the encoder would use if the user hadn't
// passed explicit values. Used by the web UI to refresh the displayed
// error clamp when the mode changes.
struct DitherDefaults {
    float strength;
    float error_clamp;
};
DitherDefaults dither_defaults_for(const Options& options);

// ---------------------------------------------------------------------------
// EncodeState — full encoder intermediates exposed for batch / atlas tooling
// where N frames must share one palette + one copper plan and per-frame
// outputs are sliced from a single atlas encode. The CLI batch handler
// (--batch) is the primary consumer; non-batch users should call convert/
// convertIFF/convertRGBA/convertHeader instead.
//
// Fields are populated for the relevant mode; CAP fields stay empty for
// non-CAP modes, scap_line_moves stays empty for non-SCAP modes, etc.
// ---------------------------------------------------------------------------
struct EncodeState {
    Image rendered;                                       // preview (palette-applied RGB)
    bitplane::BitplaneData planes;                        // raw bitplane data
    std::vector<Color3f> palette;                         // base palette (linear RGB)
    std::vector<std::uint8_t> indices;                    // per-pixel palette indices
                                                          // (non-CAP modes only; empty
                                                          // for HAM and CAP since their
                                                          // palette varies per pixel/row)
    amiga::Mode mode{};
    bool aga = false;
    bool hires = false;
    bool interlace = false;
    bool dpf = false;
    bool copper = false;
    bool scap = false;
    bool has_transparency = false;
    std::vector<bool> transparency_mask;
    std::vector<std::vector<Color3f>> scanline_palettes;  // CAP/SCAP per-line palettes
    std::vector<std::vector<copper::CopperChange>> scanline_changes;  // CAP per-line MOVEs
    std::vector<std::vector<scap::ScapMove>> scap_line_moves;         // SCAP per-line ops
    std::size_t copper_num_colors{};
    std::size_t changes_per_line{};
    std::size_t max_moves_per_line{};
    float copper_changes{};

    // SCAP-only stats — see PipelineResult for descriptions.
    float scap_avg_total_moves_per_line{};
    float scap_avg_hblank_moves_per_line{};
    std::size_t scap_max_hblank_moves_per_line{};
    float scap_avg_visible_moves_per_line{};
    std::size_t scap_max_visible_moves_per_line{};
    std::size_t scap_slot_count{};
    float quant_error{};
    float psnr{};
    float ssimulacra2_score{};

    // Raw byte stream for chunky modes (VGA 13h, SNES Mode 7). Empty for
    // bitplane modes (use `planes` instead). For SNES 256 it's the 8bpp
    // palette-index array; for SNES Direct it's the 8bpp packed
    // bbgggrrr-format Direct Color pixel byte array.
    std::vector<std::uint8_t> raw_frame;

    // Tile-dedup stats — Genesis (4bpp 8×8 = 32 B/tile) and SNES Mode 7
    // (8bpp 8×8 = 64 B/tile). 0 = not a tiled run.
    std::size_t genesis_unique_tiles = 0;
    std::size_t genesis_total_cells = 0;
    std::size_t tile_data_bytes = 0;
    // Genesis split byte streams (used by the SGDK header generator).
    std::vector<std::uint8_t>  genesis_tile_bytes;
    std::vector<std::uint16_t> genesis_tilemap_cells;
    std::vector<std::uint16_t> genesis_palette_words;
};

// Run the encoder pipeline and return its full intermediate state. Same
// input as convert() but instead of serialising to bytes, hands back
// every object the caller might want to slice/recompose (planes,
// palette, scanline_changes, scap_line_moves, etc.).
//
// On failure returns a non-empty error string in EncodeState::error_msg
// (TODO: rework as Result<EncodeState> when we want to stop folding all
// errors to one channel). For now, callers should check rendered.width().
//
// The web/native pipelines should keep using convert*() — encode_state
// is intentionally thicker than ConvertResult and exposes implementation
// details that aren't part of the long-term stable API.
struct EncodeStateOrError {
    EncodeState state;
    std::string error_msg;  // empty on success
    bool ok() const { return error_msg.empty(); }
};
EncodeStateOrError encode_state(const std::uint8_t* input_data,
                                std::size_t input_size,
                                const Options& options);

} // namespace png2amiga::api
