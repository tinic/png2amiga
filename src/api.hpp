#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "copper.hpp"
#include "strips.hpp"
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
// us with EHB+strips, where strips_line_moves was missing from every call
// site's bespoke math).
//
// Inputs:
//   plane_bytes    — bitplane data size (e.g. planes.total_bytes()).
//   palette_size   — number of color entries (Color3f count).
//   aga            — chipset is AGA (palette costs 2× and copper writes
//                    cost 2× per change for hi+lo nibble passes).
//   sliced_changes    — total sliced scanline changes across the frame
//                    (e.g. sum of scanline_changes[*].size()) OR 0.
//                    Note: for the .raw grid layout we use the
//                    (height × cpl) pre-allocated size, not the actual
//                    count — pass that pre-multiplied number here too.
//   strips_op_count  — total strips WAIT/MOVE ops across the frame
//                    (sum of strips_line_moves[*].size()) OR 0.
//   height         — image height (used for chip-RAM per-line copper
//                    list sizing).
//   max_moves      — worst-case copper MOVEs/line (chip RAM uses this
//                    to budget the per-line WAIT+MOVEs slot).
struct SizeBreakdown {
    int plane_bytes{};    // bitplane data
    int palette_bytes{};  // palette in .raw / .pal layout
    int copper_bytes{};   // sliced grid + strips per-line ops
    int disk_bytes{};     // .raw total = planes + palette + copper
    int chip_bytes{};     // worst-case Amiga chip-RAM
};

constexpr int kCopperInitSetupBytes = 80;
constexpr int kCopperEndMarkerBytes = 32;

inline SizeBreakdown compute_size_breakdown(
    int plane_bytes,
    int palette_size,
    bool aga,
    int sliced_grid_entries,  // height × cpl (sliced .raw grid)
    int strips_op_count,      // sum of strips_line_moves[*].size()
    int height,
    int max_moves) {
    SizeBreakdown s;
    s.plane_bytes = plane_bytes;
    s.palette_bytes = palette_size * (aga ? 4 : 2);
    int sliced_bytes = sliced_grid_entries * (aga ? 8 : 4);  // 4 B/word, AGA = hi+lo
    int strips_bytes = strips_op_count * 4;                  // 4 B per WAIT/MOVE word
    s.copper_bytes = sliced_bytes + strips_bytes;
    s.disk_bytes = s.plane_bytes + s.palette_bytes + s.copper_bytes;
    int has_copper = (s.copper_bytes > 0) ? 1 : 0;
    int pal_setup = palette_size * (aga ? 8 : 4);
    int per_line_words = 1 + max_moves;
    int cop_list_bytes = has_copper ? (height * per_line_words * 4) : 0;
    s.chip_bytes = s.plane_bytes + kCopperInitSetupBytes + pal_setup + cop_list_bytes +
                   kCopperEndMarkerBytes;
    return s;
}

// --best eligibility predicate. Returns an Error explaining the
// configuration mismatch when --best is requested but no actual
// best-search path engages for this (mode, options) combination.
// Returns success when --best is unset or fully supported. Both the
// programmatic api::run_pipeline entry and main.cpp's CLI dispatch
// call this before encoding starts so the silent fall-through is
// caught at the dispatch boundary.
struct Options;  // forward decl
Result<void> check_best_supported(const Options& options, amiga::Mode mode, bool has_transparency);

struct LockSpec {
    int index;    // palette slot, 0..max_colors-1
    int r, g, b;  // sRGB 0-255
};

// Reserve a palette slot from the quantizer entirely. Differs from
// LockSpec: locks pin a slot to a color but the quantizer may still
// route image pixels there if they happen to match. Reserves remove
// the slot from the candidate set — the encoded image never references
// it. Useful for sprite-color slots, runtime palette regions, EHB
// upper-bank carve-outs, etc.
struct ReserveSpec {
    int index;    // palette slot, 0..max_colors-1
    int r, g, b;  // sRGB 0-255 default color for the slot
};

// Pin a palette slot to whatever color the source pixel at (x, y) ends up
// at after quantization+dithering. Implemented as a post-dither index swap:
// the index pixel(x,y) holds is swapped (palette + index map) with `index`.
// Pin targets must NOT be locked. Has no effect on HAM/copper modes.
struct PinSpec {
    int index;  // target palette slot
    int x, y;   // source pixel coordinates
};

// Canonical encoder schema. Every parameter the pipeline (preprocess →
// quantize → dither → encode) honors lives here. The CLI parses into
// main.cpp's Config and translates via make_api_options() (one-line
// bridge in main.cpp); the WASM frontend builds Options directly from
// JS. New encoder knobs should land here FIRST, then the bridge picks
// them up — see REFACTOR_PLAN.md target #5.
struct Options {
    std::string mode = "lores";  // lores, hires, ham6, ham8, ehb
    std::string chipset;         // "ocs", "aga", or "" for auto
    int depth = 5;               // bitplane depth (1-8, mode-dependent)
    bool interlace = false;      // set LACE bit in CAMG
    float gamma = 1.0f;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float hue_shift = 0.0f;
    float sharpen = 0.0f;
    float black_point = 0.0f;
    float white_point = 0.0f;
    bool match_range = false;
    int width = 0;   // override output width (0 = mode default)
    int height = 0;  // override output height (0 = mode default)

    // Dithering
    std::string dither = "floyd-steinberg";
    // (mean PSNR across 10 images × 6
    // modes). Other ED options:
    // sierra-lite, atkinson, jarvis,
    // floyd-steinberg, stucki, gilbert,
    // riemersma. Plus ordered methods
    // (bayer*, checker, line*, etc.)
    // and palette-aware (yliluoma*,
    // knoll, opt-*, tri-tone).
    // -1.0f sentinel = auto-tune via dither_tuning::defaults_for(ctx)
    // inside run_pipeline. The encoder is the single location where this
    // lookup lives. Callers (web, CLI, library) leave the field at -1.0f
    // to get the per-(mode, depth, dpf, scap, copper, chipset, method)
    // tuned optimum, or set a non-negative value to override.
    //
    // Why not 0.0f as sentinel: 0.0 is a valid value (no dither). -1.0f
    // can never be produced by the tuning table.
    float dither_strength = -1.0f;
    float error_clamp = -1.0f;

    // Palette
    std::string palette_file;                // load palette from file (empty = auto)
    std::vector<std::uint8_t> palette_data;  // inline palette data (empty = auto)
    int palette_diversity = 4;               // 0 = off, 1-5 = remove near-duplicate
                                             // colors, re-seed from worst-served pixels
    std::string quantizer;                   // "", "auto" = auto-select, or
                                             // "median-cut", "ocs-bruteforce", "pnn"

    // HAM greedy encoder (realtime profile)
    bool ham_fast = false;

    // Dither-aware palette refinement iterations (0 = off). Run after
    // initial quantization to tighten the palette against the actual
    // dithered output. Skipped for chunky / fixed-palette modes
    // (CGA / EGA / VGA / Atari hi-res) inside run_pipeline.
    int refine_iterations = 4;

    // HAM encoding
    int ham_beam = 16;    // beam width for DP search (1-256)
    int ham_triple = 16;  // triple-pixel refinement post-pass
                          // beam width (0 = off, 16 default)

    // HAM op-selection metric. "oklab2" (default) uses perceptually-
    // uniform OKLab² distance — measured ~+9 SSIMULACRA2 points and
    // ~+3.5 OKLab-dB on banded content vs sRGB-MSE. "srgb-mse" optimises
    // headline sRGB-PSNR directly (~+0.5-1 dB nominal gain) but produces
    // visibly worse output per SSIMULACRA2 — only useful when reporting
    // PSNR is the literal goal. Compile-time dispatched.
    std::string ham_metric = "oklab2";

    bool best = false;  // multi-candidate sliced planner +
                        // joint base-palette refinement.
                        // HAM6 + copper and HAM8 + copper
                        // only — indexed copper modes
                        // ignore this flag (their planner
                        // is already mature and refinement
                        // gives ≤+0.10 dB).
                        // ~4-5× cost, +0.5..4 dB PSNR

    // Optional progress callback. Called periodically with (progress 0..1,
    // stage label). Currently emitted by HAM6+sliced encoders. Callback may
    // run on encoder threads — must be thread-safe.
    std::function<void(float, std::string_view)> on_progress;

    // Copper palette (per-scanline palette changes)
    bool copper = false;     // use per-scanline copper palettes
    int copper_changes = 0;  // override changes/line (0 = auto)
    // Experimental: emit per-line copper WAITs with vertical comparator
    // masked off (IR2=0x80FE instead of 0xFFFE). Only the first WAIT
    // anchors V; every subsequent WAIT waits on H only. Drops the
    // 0xFFDF wrap marker at line 255 (V is irrelevant so the wrap
    // doesn't matter), freeing that slot. See CHeaderOptions for the
    // emit-side details.
    bool copper_wait_h_only = false;
    // Per-line palette planner neighbor-row smoothing. -1 means use the
    // encode_copper default (radius=4, decay=0.85). Exposed for sweep
    // tooling (--slice-spread-radius / --slice-spread-decay on the CLI).
    int sliced_spread_radius = -1;
    float sliced_spread_decay = -1.0f;
    // Per-register vertical palette dithering inside the sliced encoder
    // (1-D Bayer alternation between old/new palette entries to spread
    // copper transitions across rows). Off by default — better S2/PSNR;
    // opt in via --sliced-vertical-dither when CRT row-band artefacts
    // are visible.
    bool sliced_vertical_dither = false;

    // Forward-look beam scavenge for sliced / copper modes — phase B
    // of encode_copper that takes the swap budget left unused by the
    // greedy planner and assigns dormant slots to high-residual pixels
    // (scored against a 4-row forward window). On its own it's a mixed
    // bag: clear win on diverse content (ocs_4096 +10 S2,
    // electrichues02 +5, makena +4), small regression on smooth game
    // art (shooter, fromthe). --best sweeps both states and picks the
    // S2 winner per image, so enabling it under --best is unambiguously
    // helpful; outside --best, leave at the default false unless the
    // user has measured.
    bool sliced_beam = false;

    // Transparency
    float alpha_threshold = 0.0f;        // offset from 0.5 midpoint (-0.5..0.5)
    std::string alpha_dither;            // alpha dither method (empty = hard threshold)
    float alpha_dither_strength = 1.0f;  // dither pattern intensity
    // RGB-as-transparent: every pixel whose 8-bit sRGB exactly matches
    // one of these triplets is treated as alpha=0 before quantization.
    // Useful for atlases that encode transparency via a sentinel color
    // (e.g. magenta #FF00FF in MISE Explorer's MI2 sprite extractions).
    // Multiple values supported — pass `--transparent-color` once per
    // sentinel, since some pipelines emit several rounding variants.
    std::vector<std::array<std::uint8_t, 3>> transparent_colors;

    // Slot number that alpha=0 pixels write to in the output indices
    // (.idx + paletted PNG). Default 0 — preserves legacy behavior
    // where transparent pixels collapse onto the "transparent
    // placeholder" slot 0 (= black under lock_color0). Set non-zero
    // for downstream formats whose transparency convention differs
    // from "slot 0 == transparent" (e.g. SCUMM SMAP wants slot 17
    // since slot 0 carries real black art). The palette is not
    // touched — slot 0 still exists in the palette; only the index
    // written for alpha=0 pixels is changed.
    int transparent_output_slot = 0;

    // C header output
    std::string symbol_name;  // base name for C symbols (default: "image")

    // Mask export
    bool mask_invert = false;  // invert mask polarity (default: 1=opaque, 0=transparent)

    // Cropping
    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;  // 0 = no crop
    int crop_h = 0;
    bool crop_auto = false;  // auto-crop to mode aspect ratio (center)

    // Source-pixel orientation (applied before crop/scale/encode). Matches
    // kingcon's -FlipX / -Rotate semantics so .bpl / .pal outputs drop into
    // kingcon-fed projects unchanged.
    bool flip_x = false;      // mirror over the Y axis
    bool flip_y = false;      // mirror over the X axis
    int rotate_quarters = 0;  // 0/1/2/3 = 0°/90°/180°/270° clockwise

    // Auto-trim to content bounding box (kingcon -Trim). Strips fully
    // transparent rows/columns from every edge. No-op when the source
    // has no transparency — pair with --transparent-color RRGGBB for
    // opaque sources that encode the background as a sentinel color.
    bool trim = false;

    // Advanced
    bool lock_color0 = true;  // reserve index 0 for black (border/background)

    // Amiga dual playfield. When true, encode the image as PF2 of a
    // dual-playfield display: bitplane depth is forced to 3 (OCS, 8 colors)
    // or 4 (AGA, 16 colors), the encoded planes are placed in the even
    // hardware bitplanes (PF2), the odd planes (PF1, foreground) are
    // emitted as zero, and the palette is shifted into the upper color
    // registers (8-15 OCS / 16-31 AGA) so PF2 pixels look up there. Sets
    // the CAMG DPF flag (0x0400) and BPLCON0 DBLPF bit. Only valid for
    // standard lores/hires modes (no HAM/EHB/copper/Atari/DOS).
    bool dual_playfield = false;

    // strips calibration probe selector — DPF-only.
    //   ""   : disabled (default)
    //   "a"  : Probe A (slot HPOS sweep, OCS DPF)
    //   "b"  : Probe B (slot capacity, OCS DPF) — placeholder
    //   "c"  : Probe C (AGA bandwidth) — placeholder
    //   "d"  : Probe D (at-x vs after-x mapping) — placeholder
    // Bypasses the normal pipeline: synthesizes a calibration image plus
    // per-line WAIT/MOVE copper ops and emits a viewer .cpp/.adf you run on
    // real hardware. Slot tables are populated by hand from the observed
    // results, then the production planner uses them.
    std::string strips_probe;

    // strips encoder — DPF-only mid-line palette swaps. Requires
    // dual_playfield + chipset=ocs + mode=lores (no interlace) for
    // Phase 1. Composes with copper (each line's per-strip palette
    // chain is independent of sliced's end-of-line writes).
    bool scap = false;
    // strips slot-tuning debug bundle: forces base-palette MOVEs to
    // 0x0000 AND paints yellow PF1 rulers at every 4/8/16 px. Always
    // used together. Off in production.
    bool strips_debug = false;

    // Seamless-tile mode. Pre-replicates the (already preprocessed)
    // input image into a 3x3 layout, runs quantize / dither / encode
    // over the full 3W x 3H buffer so the error-diffused dither at
    // the right edge of the center tile converges to match its left
    // edge, then crops the center tile for export — every output
    // path gets only the center W x H. The terminal `--preview` is
    // the only consumer that re-tiles the center back to 3x3 for
    // visual seam verification.
    //
    // Allowed only on freeform indexed bitmap modes (lores / hires /
    // EHB ± interlace ± dpf). HAM, sliced palette, strip palette,
    // and fixed-size or tile-coded modes (Atari, VGA, EGA, CGA, SNES
    // Mode 7, Genesis, C64) are rejected — see the gate in
    // run_pipeline().
    bool tile = false;

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
    // cga-text blur-kernel shape: "auto" (per-mode default; tuned via
    // SSIMULACRA2 — aniso53 for 8×1 cells, wide55 for 8×2, wide77 for
    // 8×4 / 8×8) | "binomial" | "aniso53" | "aniso73" | "wide55" |
    // "wide77". Only consumed by the blur metric.
    std::string cga_text_kernel = "auto";

    // cga-composite: choose between pre-1983 IBM 5150 chroma-burst
    // phase ("old", default) and the 1983+ revised card ("new"). All
    // other analog-stage knobs (hue/sat/contrast/luma) stay at
    // MartyPC's "stock CGA monitor" preset.
    bool cga_composite_new_cga = false;


    // C64 / VIC-II palette selector. One of: pepto, vice, colodore
    // (default), deekay, godot, c64wiki, levy. Only affects c64-* modes;
    // ignored everywhere else. The VIC-II palette has no unique sRGB
    // ground-truth — different tools/scenes prefer different RGB tables.
    // Colodore is the default (matches png2c64) — it's measurement-based
    // so colors track real CRT output more closely than other tables.
    std::string c64_palette = "colodore";

    // Per-cell error metric for C64 modes (cell brute force). Both
    // run in OKLab. Mirrors png2c64.
    //   "mse" (default): per-pixel nearest-distance squared sum.
    //   "blur": Pappas-Neuhoff 3×3 binomial blur of source vs
    //          rendered cell — models eye-on-CRT averaging.
    std::string c64_metric = "blur";

    // PETSCII only: when true, restrict the candidate glyph set to
    // PETSCII semi-graphics, blocks, and the reverse-video graphics
    // (~130 chars) — skips letters / digits / punctuation. Useful
    // for halftone-style art where letter shapes in smooth regions
    // would look wrong.
    bool c64_petscii_graphics_only = false;

    // c64 charset modes (hires / multicolor) — and later tile-based
    // SNES / Genesis / Amiga 16x16 / PS1 64x64. When 0 the encoder
    // uses its mode default (256 for c64). Use higher budgets when
    // the runtime swaps charset banks across frames; the encoder
    // emits the full glyph catalog but writes screen_ram with the
    // platform's hardware-native index width (8-bit on c64, 16-bit
    // on Genesis / SNES).
    std::size_t tile_budget = 0;
    // Reserve N glyph slots in the final budget so dedup merges
    // down to (budget - reserve). Useful when the runtime needs N
    // free slots for animation frames / sprites / raster effects.
    std::size_t tile_reserve = 0;

    // Palette index manipulation (lores/hires/EHB/Atari only)
    std::vector<LockSpec> locks;
    std::vector<PinSpec> pins;
    std::vector<ReserveSpec> reserves;
};

struct ConvertResult {
    std::vector<std::uint8_t> data;  // output file bytes (IFF or PNG)
    int width{};
    int height{};
    int depth{};                    // bitplane depth
    int colors{};                   // number of palette colors
    float copperChanges{};          // avg actual color changes per line (0 if no copper)
    float avgPaletteUsedPerLine{};  // avg distinct palette indices touched per scanline
                                    // (sliced / strips diagnostic; 0 if mode has no
                                    // per-line palette). Pair with the per-row palette
                                    // size to see how saturated the line palette is.
    int totalColors{};              // unique colors in rendered output
    int planeBytes{};               // raw bitplane data size
    int copperBytes{};              // copper-list bytes total (sliced scanline_changes
                                    // + strips per-line moves, 4 B per word).
                                    // 0 if mode has no copper. Use this for chip-RAM
                                    // / disk-cost reporting — call sites must NOT
                                    // re-derive size from {scanlines, cpl} because new
                                    // modes (DPF/EHB+strips) carry extra data they'd miss.
    int diskBytes{};                // .raw file size: bitplanes + palette + copper data.
                                    // Computed centrally in make_result so call sites
                                    // can't drift stale as new modes are added.
    int chipBytes{};                // worst-case chip-RAM cost: bitplanes + copper list
                                    // (init MOVEs + per-line WAIT+MOVEs at max_moves_per_line)
                                    // + palette setup. Same single-source-of-truth rule
                                    // as diskBytes.
    int changesPerLine{};           // K used by encoder (post auto-stretch)
    int maxMovesPerLine{};          // worst-case copper MOVEs per scanline
    bool aga{};                     // chipset is AGA (true ⇒ palette has hi+lo halves)
    float quantError{};             // perceptual encoding error (OKLab ΔE² sum)
    float psnr{};                   // PSNR in dB (Gaussian-blurred sRGB)
    float s2{};                     // SSIMULACRA2 score (Cloudinary 2022)
    int genesisUniqueTiles{};       // Tiled modes: unique tiles in VRAM
    int genesisTotalCells{};        // Tiled modes: tilemap cells (W/8 × H/8); 0 = not tiled
    int tileDataBytes{};  // Tiled modes: unique × bytes-per-tile (32 for Genesis 4bpp, 64 for SNES
                          // Mode 7 8bpp)
    bool hasTransparency{};  // source image had alpha channel

    // Final per-palette-entry sRGB bytes (3 per entry). Used by the web
    // tool to draw a palette swatch grid. Empty when the mode has no
    // single global palette (e.g. genesis / snes have their own per-
    // line / 256-mode palette fields above).
    std::vector<std::uint8_t> paletteBytes;

    // Per-scanline palette swatch grid for sliced / strips / copper-HAM
    // modes. Packed `height × scanlinePaletteSize × 3 RGB bytes` — row y
    // starts at offset `y * scanlinePaletteSize * 3`. Empty when the mode
    // doesn't have per-line palette evolution. Used by the web tool to
    // draw a vertical strip beside the preview showing how the palette
    // shifts down the frame.
    std::vector<std::uint8_t> scanlinePaletteBytes;
    int scanlinePaletteSize{};

    // Per-pixel palette index map for non-HAM, non-sliced/strips modes.
    // Layout: width × height bytes, idx = pixel's slot in paletteBytes.
    // Empty when the encoder doesn't emit a single 1:1 index grid
    // (HAM ops / per-line sliced palette / per-tile palette).
    std::vector<std::uint8_t> indices;

    // c64 charset modes — copy of the encoder's raw_frame so the web
    // frontend can render a charset diagnostic (the actual generated
    // glyphs, colored by their first-occurrence cell). Layout:
    //   bytes 0..unique_glyphs*8       — charset_data (8 bytes/glyph)
    //   then  cols*rows                — screen_ram (glyph index)
    //   then  cols*rows                — color_ram (per-cell color)
    // Empty for non-charset runs.
    std::vector<std::uint8_t> c64CharsetData;
    int c64Mc1{};      // multicolor: shared mc1 (0..15)
    int c64Mc2{};      // multicolor: shared mc2 (0..15)
    int c64BgColor{};  // shared bg (0..15)

    // Genesis tile diagnostic — same idea as c64CharsetData but with
    // 4bpp 8×8 tiles and a 4-line palette structure. Empty for
    // non-Genesis runs.
    std::vector<std::uint8_t> genesisTileBytes;     // unique × 32
    std::vector<std::uint8_t> genesisTilemapBytes;  // cells × 2 (u16 LE)
    std::vector<std::uint8_t> genesisPaletteBytes;  // 4 × 16 × 2 (BGR333 LE)

    // SNES Mode 7 tile diagnostic. snesPaletteBytes is empty for the
    // Direct Color variant (BBGGGRRR pixel bytes expand inline).
    std::vector<std::uint8_t> snesTileBytes;     // unique × 64
    std::vector<std::uint8_t> snesTilemapBytes;  // 128×128 (16384)
    std::vector<std::uint8_t> snesPaletteBytes;  // 256 × 3 RGB (256-mode only)

    std::string error;  // error message (empty on success)
};

// Convert raw image data (PNG/JPEG bytes) to Amiga format.
// Returns PNG bytes of the converted (preview) image.
ConvertResult convert(const std::uint8_t* input_data,
                      std::size_t input_size,
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

// Convert raw image data and return KTX2 bytes containing an ETC2 RGB8
// compressed texture. Used by --mode etc2 in the CLI and by the WASM
// frontend. Bypasses the Amiga bitplane / palette / dither pipeline —
// ETC2's per-block encoding is independent of all those concepts.
// Honoured options:
//   options.width / options.height (preserves source aspect when one given)
//   options.dither — selects the block-grid ED kernel (Floyd-Steinberg
//                    default; Atkinson / Stucki / Jarvis / Sierra-Lite
//                    all valid via dither::Method)
//   options.best  — pre-image jitter sweep with N=8 trials ranked by
//                    SSIMULACRA2 (mirrors project_cap_best_design.md)
// Plus ETC2-specific knobs accessible via Options (etc2 sub-namespace TBD —
// for now api passes through the encoder's defaults; CLI threads its
// own --etc2-* flags directly via main.cpp's run_etc2()).
ConvertResult convert_ktx2(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options);

// c64 only: convert and return a runnable C64 .prg with embedded
// 6502 displayer (Koala for c64-multicolor, Art-Studio for c64-hires,
// FLI/AFLI/PETSCII as appropriate). Charset modes are not yet
// supported pending mc1/mc2 surfacing in the c64 EncodeResult.
ConvertResult convert_prg(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// c64 only: raw image-only formats — Koala Paint .koa
// (multicolor) or Art Studio .hir (hires). No displayer prepended.
ConvertResult convert_koa(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);
ConvertResult convert_hir(const std::uint8_t* input_data,
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
// Fields are populated for the relevant mode; sliced fields stay empty for
// non-sliced modes, strips_line_moves stays empty for non-strips modes, etc.
// ---------------------------------------------------------------------------
struct EncodeState {
    Image rendered;                     // preview (palette-applied RGB)
    bitplane::BitplaneData planes;      // raw bitplane data
    std::vector<Color3f> palette;       // base palette (linear RGB)
    std::vector<std::uint8_t> indices;  // per-pixel palette indices
                                        // (non-sliced modes only; empty
                                        // for HAM and sliced since their
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
    std::vector<std::vector<Color3f>> scanline_palettes;  // sliced/strips per-line palettes
    std::vector<std::vector<copper::CopperChange>> scanline_changes;  // sliced per-line MOVEs
    std::vector<std::vector<strips::ScapMove>> strips_line_moves;     // strips per-line ops
    std::size_t copper_num_colors{};
    std::size_t changes_per_line{};
    std::size_t max_moves_per_line{};
    float copper_changes{};

    // strips-only stats — see PipelineResult for descriptions.
    float strips_avg_total_moves_per_line{};
    float strips_avg_hblank_moves_per_line{};
    std::size_t strips_max_hblank_moves_per_line{};
    float strips_avg_visible_moves_per_line{};
    std::size_t strips_max_visible_moves_per_line{};
    std::size_t strips_slot_count{};
    float quant_error{};
    float psnr{};
    float ssimulacra2_score{};

    // Raw byte stream for chunky modes (VGA 13h, SNES Mode 7). Empty for
    // bitplane modes (use `planes` instead). For SNES 256 it's the 8bpp
    // palette-index array; for SNES Direct it's the 8bpp packed
    // bbgggrrr-format Direct Color pixel byte array.
    std::vector<std::uint8_t> raw_frame;

    // c64 modes: shared VIC-II background color. Needed for PRG /
    // .koa / .hir export. 0 for non-c64 runs.
    std::uint8_t c64_bg_color = 0;

    // c64 charset modes (hires + multicolor) — tile-mode metadata
    // surfaced from c64::EncodeResult so downstream .h export doesn't
    // need to re-run the encoder. 0 for non-charset runs.
    std::size_t c64_cols = 0;
    std::size_t c64_rows = 0;
    std::size_t c64_unique_glyphs = 0;
    std::uint8_t c64_mc1 = 0;
    std::uint8_t c64_mc2 = 0;

    // Tile-dedup stats — Genesis (4bpp 8×8 = 32 B/tile) and SNES Mode 7
    // (8bpp 8×8 = 64 B/tile). 0 = not a tiled run.
    std::size_t genesis_unique_tiles = 0;
    std::size_t genesis_total_cells = 0;
    std::size_t tile_data_bytes = 0;
    // Genesis split byte streams (used by the SGDK header generator).
    std::vector<std::uint8_t> genesis_tile_bytes;
    std::vector<std::uint16_t> genesis_tilemap_cells;
    std::vector<std::uint16_t> genesis_palette_words;

    // Algorithm name from the Palette generated for `palette` (and
    // for HAM/EHB/copper variants, the base palette before per-line
    // evolution). "median-cut" / "pnn" / "ocs-optimal" / "gpu-restart"
    // / "ega" / "user" / "" if not applicable. Surfaces in the
    // status line as "(quantizer)".
    std::string base_palette_quantizer;
};

// Run the encoder pipeline and return its full intermediate state. Same
// input as convert() but instead of serialising to bytes, hands back
// every object the caller might want to slice/recompose (planes,
// palette, scanline_changes, strips_line_moves, etc.).
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

// Float-pixel sibling of encode_state: takes a pre-decoded Image at the
// mode's target dimensions (already preprocessed, scaled, cropped, etc.)
// and runs the same downstream encode path. Lets CLI sites avoid the
// 8-bit PNG round-trip png_io::encode + encode_state(bytes,...) imposes
// — the round-trip quantizes float pixels to 8-bit sRGB and back, costing
// precision the encoder didn't have to lose.
//
// width/height options are ignored; the image's actual dimensions are
// used as the target. Crop / scale / preprocess options are ignored too;
// the caller has already applied them.
EncodeStateOrError encode_state_image(const Image& image, const Options& options);

}  // namespace png2amiga::api
