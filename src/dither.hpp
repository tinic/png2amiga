#pragma once

#include "color_space.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::dither {

// ---------------------------------------------------------------------------
// Dithering methods
//
// The Amiga uses square pixels in all standard modes, so we do not need
// the 2:1 pixel-ratio variants that png2c64 provides for C64 multicolor.
// ---------------------------------------------------------------------------

enum class Method : unsigned char {
    none,

    // Ordered dithering (threshold-based, no error propagation)
    bayer2x2,
    bayer4x4,
    bayer8x8,
    bayer3x3,       // dispersed-dot 3×3 (non-power-of-2)
    bayer5x5,       // dispersed-dot 5×5 (non-power-of-2)
    bayer6x6,       // dispersed-dot 6×6 (non-power-of-2)
    bayer7x7,       // dispersed-dot 7×7 (non-power-of-2)
    checker,        // 2x2 checkerboard
    clustered_dot,  // 4x4 clustered dot

    // Non-square ordered (for non-square pixel modes)
    h2x4,      // 2 wide x 4 tall (lores interlace, wide pixels)
    v4x2,      // 4 wide x 2 tall (hires, tall pixels)
    bayer4x2,  // Bayer 4 wide x 2 tall (hires)
    bayer2x4,  // Bayer 2 wide x 4 tall (interlace)

    // Horizontal lines
    line2,         // 1x2 alternating rows
    line_checker,  // 2x2 line-biased
    line4,         // 1x4 horizontal gradient
    line8,         // 1x8 horizontal gradient

    // Vertical lines
    vline2,         // 2x1 alternating columns
    vline_checker,  // 2x2 column-biased
    vline4,         // 4x1 vertical gradient
    vline8,         // 8x1 finest vertical gradient

    // Additional ordered dithering
    halftone8x8,    // 45-degree clustered dot halftone (newspaper look)
    diagonal8x8,    // 45-degree diagonal clustered dot (64 levels)
    spiral5x5,      // spiral dot growth from center
    hex8x8,         // non-rectangular hexagonal tiling
    hex5x5,         // non-rectangular slanted square tiling
    blue_noise,     // 64x64 blue noise (film-grain look, no visible pattern)
    void_cluster,   // 64x64 Ulichney void-and-cluster (per-rank optimal)
    cluster_noise,  // 64x64 cluster blue noise (wider clusters, libcaca-style)
    fractal16,      // 16×16 self-nested Bayer recursion (Niklasson/Obra Dinn)
    ign,            // Interleaved Gradient Noise (analytical, no LUT)
    ign_triangle,   // IGN with U(0,1)→triangle(-1,1) remap
    white_noise,    // pure random hash per pixel (film grain)
    r2_sequence,    // Martin Roberts R2 low-discrepancy sequence
    r2_triangle,    // R2 with U(0,1)→triangle(-1,1) remap (Wronski 2016)
    crosshatch,     // pen-and-ink crosshatching
    radial,         // concentric circles (engraving look)
    value_noise,    // coherent noise (organic clumping)

    // Error diffusion
    floyd_steinberg,
    atkinson,
    sierra_lite,
    stucki,
    jarvis,

    // Advanced error diffusion
    gilbert,            // Gilbert space-filling-curve error diffusion
    riemersma,          // Riemersma curve dither (exponential-decay error queue)
    structure_fs,       // FS with Laplacian-modulated threshold (dalpil/structure-aware)
    contrast_fs,        // FS with local-contrast modulated threshold (Mould 2009)
    zhoufang,           // Zhou-Fang adaptive-coefficient ED (intensity-dependent)
    yliluoma,           // Yliluoma/Knoll palette-aware pattern dither (8×8 Bayer)
    yliluoma2,          // Yliluoma method 2 — gamma-aware variant
    opt_checker,        // Optimal Checker — palette-aware 2-color greedy pair on a 2×2 cell
    opt_line,           // Optimal Line — 2-color pair, y&1 phase (CRT-scanline friendly)
    opt_line_checker,   // Optimal Line-Checker — 4-color plan, line_checker 2×2 phase
    opt_vline,          // Optimal VLine — 2-color pair, x&1 phase (vertical-stripe variant)
    opt_vline_checker,  // Optimal VLine-Checker — 4-color plan, vline_checker 2×2 phase
    knoll,              // Knoll pattern dither — N=16 plan on 4×4 Bayer (US 6,606,166 expired 2019)
    tri_tone,           // Yliluoma tri-tone — 2×2 cell, 3 colors (one 50%, two 25%)
    yliluoma1,          // Yliluoma Algorithm 1 — exhaustive pair × ratio search w/ luma threshold
    aseprite_old,       // Aseprite "old" 4×4 — hand-tuned, checker-biased
    libcaca_3x3,        // libcaca hand-tuned 3×3 dispersed dot
    libcaca_6x6,        // libcaca hand-tuned 6×6 dispersed dot
    pegasus_8x8,        // Pegasus/REXPaint 8×8 mosaic (center-biased)
    cranley_bayer,      // Bayer 8×8 with Cranley-Patterson random offset (Quilez)
    quasicrystal,       // Sum-of-cosines quasicrystal pattern (Sloan 2010)
    truchet,            // Truchet-tile threshold pattern

    // Direct Binary Search (Allebach et al. ~1992) — the perceptual-
    // optimum halftone via greedy-descent on an HVS-blurred OKLab cost.
    // Warm-start from FS, then sweep over pixels trying every palette
    // alternative, accepting any toggle that lowers cost. Far slower
    // than ED (5-30s typical) but produces the best quality reachable
    // by any per-pixel dither — every other method approximates this.
    dbs,
};

// ---------------------------------------------------------------------------
// Dithering settings
// ---------------------------------------------------------------------------

struct Settings {
    Method method = Method::floyd_steinberg;
    float strength = 1.0f;      // 0.0 = no dithering, 1.0 = full
    float error_clamp = 0.12f;  // max error magnitude per OKLab channel
    bool serpentine = true;     // alternate scan direction (error diffusion)
};

// ---------------------------------------------------------------------------
// Dither an image to a palette, returning per-pixel palette indices.
//
// This is the primary entry point. It maps each pixel to the nearest
// palette color, using the selected dithering method to distribute
// quantization error.
//
// image:   input image (linear RGB)
// palette: palette colors (linear RGB), max 256 entries
// settings: dithering method and parameters
//
// Returns: vector of palette indices (width * height entries)
// ---------------------------------------------------------------------------

struct DitherResult {
    std::vector<std::uint8_t> indices;  // per-pixel palette index
    float total_error{};                // sum of perceptual squared error
};

DitherResult apply(const Image& image, std::span<const Color3f> palette, const Settings& settings);

// ---------------------------------------------------------------------------
// Returns true if the method is an ordered (non-error-diffusion) method.
// ---------------------------------------------------------------------------

constexpr bool is_ordered(Method m) noexcept {
    switch (m) {
    case Method::none:
    case Method::bayer2x2:
    case Method::bayer4x4:
    case Method::bayer8x8:
    case Method::bayer3x3:
    case Method::bayer5x5:
    case Method::bayer6x6:
    case Method::bayer7x7:
    case Method::checker:
    case Method::h2x4:
    case Method::clustered_dot:
    case Method::line2:
    case Method::line_checker:
    case Method::line4:
    case Method::line8:
    case Method::vline2:
    case Method::vline_checker:
    case Method::vline4:
    case Method::vline8:
    case Method::v4x2:
    case Method::bayer4x2:
    case Method::bayer2x4:
    case Method::halftone8x8:
    case Method::diagonal8x8:
    case Method::spiral5x5:
    case Method::hex8x8:
    case Method::hex5x5:
    case Method::blue_noise:
    case Method::void_cluster:
    case Method::cluster_noise:
    case Method::fractal16:
    case Method::aseprite_old:
    case Method::libcaca_3x3:
    case Method::libcaca_6x6:
    case Method::pegasus_8x8:
    case Method::cranley_bayer:
    case Method::quasicrystal:
    case Method::truchet:
    case Method::ign:
    case Method::ign_triangle:
    case Method::white_noise:
    case Method::r2_sequence:
    case Method::r2_triangle:
    case Method::crosshatch:
    case Method::radial:
    case Method::value_noise:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Get the raw ordered dither threshold at pixel (x, y).
// Returns value in [-0.5, 0.5]. Returns 0 for non-ordered methods.
// ---------------------------------------------------------------------------

float ordered_threshold(Method method, std::size_t x, std::size_t y);

// ---------------------------------------------------------------------------
// Error-diffusion kernel entry: distribute `weight` of the quantization
// error to the pixel offset (dx, dy) from the current one. Used by callers
// that need to run their own diffusion loop (e.g., copper mode with
// per-scanline palette swaps).
// ---------------------------------------------------------------------------

struct DiffusionEntry {
    int dx;
    int dy;
    float weight;
};

// Returns the kernel for the given error-diffusion method, or an empty
// span for ordered / none. Floyd-Steinberg, Atkinson, Sierra Lite,
// Stucki, Jarvis are supported.
std::span<const DiffusionEntry> error_diffusion_kernel(Method method);

// True if `method` is a structure-aware FS variant that needs per-pixel
// L-channel bias precomputed from the whole image.
bool needs_structure_bias(Method method);

// Precompute per-pixel L-channel bias for structure-aware FS variants.
// Empty if `method` is not a structure-aware variant. The returned vector
// is row-major, width×height, applied as `pixel.L += bias[idx]` before
// nearest-palette selection in the caller's diffusion loop.
std::vector<float> compute_structure_bias(const Image& image, Method method);

// True if `method` is the Riemersma curve dither — caller should add a
// 16-element exponential-decay error queue on top of the FS kernel.
bool needs_riemersma_queue(Method method);

// True if `method` is a Yliluoma palette-aware pattern dither — needs
// per-pixel mixing-plan construction with absolute (x, y) for the Bayer
// indexing. Use `pick_yliluoma_index` instead of routing through
// `apply()` on a 1-row sub-image (which would fix y=0 and stripe).
bool is_yliluoma(Method method);

// Methods that need a discrete palette to operate on: yliluoma family
// (palette-aware ordered) (variable scaling against the nearest palette
// pair), and DBS (sweeps palette indices). HAM modes have no STATIC
// palette but ham.cpp builds a per-pixel reachable set (SET ∪ MODIFY-
// R/G/B variants of prev) and feeds it to the same pickers, so opt-*
// and yliluoma are supported on HAM6/HAM7/HAM8 (including --sliced).
// SNES Mode 7 Direct has no palette table at all — still degenerate.
bool needs_discrete_palette(Method method);

// Per-pixel Yliluoma quantizer. Builds the 64-step mixing plan against
// `palette_lab`, sorts by luma, and picks via Bayer8 at absolute (x,y).
// `mode2` selects Yliluoma method 2 (luma-weighted distance).
std::uint8_t pick_yliluoma_index(const png2amiga::color_space::OKLab& target,
                                 std::span<const png2amiga::color_space::OKLab> palette_lab,
                                 std::size_t x,
                                 std::size_t y,
                                 bool mode2,
                                 float strength);

// Per-pixel Yliluoma 2×2-checker variant — brute-force optimal palette
// PAIR whose average matches target, picked by (x+y)&1 phase. CRT-
// friendly; phosphor + scanline blur averages adjacent pixels.
// `strength` scales how often the phase wins vs collapsing to nearest
// (1.0 = full checker, 0.0 = no visible dither).
std::uint8_t pick_opt_checker_index(const png2amiga::color_space::OKLab& target,
                                    std::span<const png2amiga::color_space::OKLab> palette_lab,
                                    std::size_t x,
                                    std::size_t y,
                                    float strength);

// Same 2-color optimal-pair pick as opt_checker, but the phase
// function is y & 1 — alternating rows. Each scanline alternates
// between the pair's darker and brighter member. Aligns with CRT
// scanline structure; no left/right asymmetry.
std::uint8_t pick_opt_line_index(const png2amiga::color_space::OKLab& target,
                                 std::span<const png2amiga::color_space::OKLab> palette_lab,
                                 std::size_t x,
                                 std::size_t y,
                                 float strength);

// 4-color greedy plan on a 2×2 cell, indexed by the line_checker
// threshold matrix — row-major dominant with a subtle column shift
// inside each row. Produces a 4-tone line-tinted pattern, not a
// 2-tone line.
std::uint8_t pick_opt_line_checker_index(const png2amiga::color_space::OKLab& target,
                                         std::span<const png2amiga::color_space::OKLab> palette_lab,
                                         std::size_t x,
                                         std::size_t y,
                                         float strength);

// Same as pick_opt_line_index but phase = (x & 1) instead of (y & 1).
// Produces a vertical-stripe pattern (column-dominant) — visually
// preferred on tall pixels (hires modes) and orthogonal to the
// CRT-scanline-friendly horizontal version.
std::uint8_t pick_opt_vline_index(const png2amiga::color_space::OKLab& target,
                                  std::span<const png2amiga::color_space::OKLab> palette_lab,
                                  std::size_t x,
                                  std::size_t y,
                                  float strength);

// Same as pick_opt_line_checker_index but uses vline_checker_mat so
// the dominance axis is columns (not rows) with a subtle row shift.
std::uint8_t pick_opt_vline_checker_index(
    const png2amiga::color_space::OKLab& target,
    std::span<const png2amiga::color_space::OKLab> palette_lab,
    std::size_t x,
    std::size_t y,
    float strength);

// Per-pixel Knoll pattern dither (Photoshop's "Pattern", US patent
// 6,606,166 expired 2019). N=16 greedy mixing plan on a 4×4 Bayer cell.
std::uint8_t pick_knoll_index(const png2amiga::color_space::OKLab& target,
                              std::span<const png2amiga::color_space::OKLab> palette_lab,
                              std::size_t x,
                              std::size_t y,
                              float strength);

// Tri-tone — Yliluoma's named 2×2 / 3-color preset. One palette entry
// at 50% (Bayer cells 0 and 1) plus two more at 25% each (cells 2 and
// 3). Implemented as a 4-step greedy plan with palette repeats allowed,
// indexed by Bayer 2×2.
std::uint8_t pick_tri_tone_index(const png2amiga::color_space::OKLab& target,
                                 std::span<const png2amiga::color_space::OKLab> palette_lab,
                                 std::size_t x,
                                 std::size_t y,
                                 float strength);

// ---------------------------------------------------------------------------
// Direct Binary Search post-pass refinement for tiled-palette modes
// (sliced per-line, strips per-strip). The base sliced/strips encoder has
// already filled `indices` with an initial assignment; DBS sweeps
// each pixel and tries every candidate in that pixel's effective
// palette (looked up via `palette_for_pixel(x, y)`). Accepts any
// toggle that lowers the HVS-blurred OKLab cost.
//
// This complements the global-palette `Method::dbs` path in apply()
// by delegating palette resolution to the caller — so sliced/strips can
// supply per-row or per-strip palette spans without the DBS code
// needing to know about sliced/strips layout.
// ---------------------------------------------------------------------------

using PalettePerPixel =
    std::function<std::span<const png2amiga::color_space::OKLab>(std::size_t x, std::size_t y)>;

void apply_dbs_post_pass(const Image& image,
                         std::vector<std::uint8_t>& indices,  // in/out, size w*h
                         const PalettePerPixel& palette_for_pixel,
                         int max_sweeps = 8);

// ---------------------------------------------------------------------------
// diffuse_raw_buffer — single source of truth for per-pixel error-diffusion
// scaffolding across all "raw buffer" mode pipelines (HAM, EHB+sliced, copper,
// strips, SNES Mode 7, EHB main path). Owns:
//
//   • input → OKLab conversion
//   • serpentine scan
//   • structure-aware bias map (Laplacian / contrast / Zhou-Fang)
//   • Riemersma 16-element exponential-decay queue
//   • Ostromoukhov 0.6 + 0.8·threshold variable kernel scaling
//   • Ordered dither L/a/b threshold offsets (0.15 / 0.03 / 0.03)
//   • Error-clamping per OKLab channel
//
// The picker is the only mode-specific piece. It is invoked once per
// pixel with the *adjusted* target Lab (bias + error feedback + ordered
// offsets already applied) and returns the chosen output Lab plus, for
// Ostromoukhov, the near/far threshold ratio. Side effects (writing
// indices, raw bytes, HAM ops) are the picker's responsibility.
//
// Returns the accumulated squared OKLab error (sum of |source−chosen|²),
// useful for PSNR-style metrics. Caller pays for the structure-bias and
// Riemersma queue precompute.
// ---------------------------------------------------------------------------

struct PickResult {
    color_space::OKLab chosen_lab;
    // Used only for Method::floyd_steinberg: sqrt(best)/(sqrt(best)+sqrt(2nd))
    // ∈ [0, 0.5]. Pickers without a meaningful "second nearest" (raw grid
    // snap, HAM ops) leave this at 0.5 → unit kernel scale.
    float ostro_threshold = 0.5f;
};

using PixelPicker =
    std::function<PickResult(const color_space::OKLab& target, std::size_t x, std::size_t y)>;

float diffuse_raw_buffer(const Image& image, const Settings& settings, const PixelPicker& pick);

// True if `method` actually performs error diffusion (has an FS-style
// kernel and isn't an ordered or palette-aware family). Centralised
// because the same 4-clause expression
//   method != none && !is_ordered(m) && !is_yliluoma(m) && !kernel.empty()
// previously appeared verbatim in copper.cpp / scap.cpp / main.cpp /
// api.cpp / ham.cpp — drifting independently when methods were added.
bool uses_error_diffusion(Method method);

// Pick a palette index for a target Lab using the right strategy:
//   • yliluoma family → pick_yliluoma_family_index
//   • everything else → nearest neighbor with second-nearest tracked
//     for ostromoukhov's variable scaling
// `k_min` skips reserved entries (e.g. transparent color 0 in DPF/EHB).
// Out-param `chosen_lab` is the picked palette color. Returns the
// ostromoukhov threshold (sb / (sb+ss) for nearest pair, 0.5 for
// yliluoma — i.e. unit kernel scale).
//
// Replaces the duplicate 25-line if(yli)/else(nearest+second) blocks
// in api.cpp / copper.cpp / scap.cpp×2 / main.cpp.
float pick_palette_index_with_ostro(Method method,
                                    const png2amiga::color_space::OKLab& target,
                                    std::span<const png2amiga::color_space::OKLab> palette_lab,
                                    std::size_t x,
                                    std::size_t y,
                                    float strength,
                                    std::size_t k_min,
                                    std::size_t& chosen_index,
                                    png2amiga::color_space::OKLab& chosen_lab);

// Dispatch helper for the yliluoma family — picks the right
// per-pixel quantizer (opt_checker/opt_line/opt_line_checker/knoll/
// tri_tone/yliluoma1/yliluoma/yliluoma2) based on `method`.
//
// Caller must check `is_yliluoma(method)` first; passing a non-yliluoma
// method falls back to plain yliluoma alg-2 greedy. Replaces the
// 8-case switch that was duplicated in copper.cpp / scap.cpp /
// ham.cpp / main.cpp / api.cpp.
std::uint8_t pick_yliluoma_family_index(Method method,
                                        const png2amiga::color_space::OKLab& target,
                                        std::span<const png2amiga::color_space::OKLab> palette_lab,
                                        std::size_t x,
                                        std::size_t y,
                                        float strength);

// Yliluoma Algorithm 1 — exhaustive search over (Color1, Color2, ratio)
// triples. For each unique palette pair (i ≤ j) and each ratio r in
// {1, 2, …, N-1} (N=16 cell area on a 4×4 Bayer), score the linear mix
// against target; pick the best triple. A luminance-difference cutoff
// trims the pair list ("Improvement to Algorithm 1") so the search
// stays tractable on 32+ color palettes.
std::uint8_t pick_yliluoma1_index(const png2amiga::color_space::OKLab& target,
                                  std::span<const png2amiga::color_space::OKLab> palette_lab,
                                  std::size_t x,
                                  std::size_t y,
                                  float strength);

// ----------------------------------------------------------------------
// Canonical string ↔ Method registry.
//
// One source of truth for the dither name table. Previously this lived
// in three places — main.cpp's parse_dither_method, main.cpp's two
// method_to_name functions, and api.cpp's parse_dither — and adding a
// new method silently broke at any of them that wasn't updated. The
// vline / vline-checker miss landed because api.cpp's parse_dither is
// an `if`-chain, not a switch, so the compiler couldn't flag the
// missing entries.
//
// `method_name` returns the canonical CLI / option string for `m`
// (matches the historical names exactly). `parse_method_or_null` is
// the lookup half — empty optional on unknown name. Both walk the same
// constexpr-defined table; the only error policy difference between
// main.cpp (returns Result) and api.cpp (falls back to FS) is the
// wrapper's choice.
//
// To add a new method: add a single row to `kMethodNames` in dither.cpp
// and everything else picks it up automatically.
std::string_view method_name(Method m) noexcept;
std::optional<Method> parse_method_or_null(std::string_view s) noexcept;

}  // namespace png2amiga::dither
