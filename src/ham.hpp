#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace png2amiga::ham {

// ---------------------------------------------------------------------------
// Hold-And-Modify encoding (generalized for any depth 4-8)
//
// For N bitplanes: 2 control bits + (N-2) data bits
//   - Control 00: SET palette color (data = palette index)
//   - Control 01: MODIFY BLUE  (data = new blue channel value)
//   - Control 10: MODIFY RED   (data = new red channel value)
//   - Control 11: MODIFY GREEN (data = new green channel value)
//
// Base palette has 2^(N-2) entries. Modify operations use (N-2)-bit
// precision per channel, expanded to 8-bit sRGB via bit replication.
//
// Supported modes: HAM6 (OCS, 6 bitplanes) and HAM8 (AGA, 8 bitplanes)
// ---------------------------------------------------------------------------

// HAM operation codes (stored in the top 2 bits)
enum class HamOp : std::uint8_t {
    set_palette = 0b00,   // Use base palette color
    modify_blue = 0b01,   // Modify blue, keep red/green
    modify_red = 0b10,    // Modify red, keep green/blue
    modify_green = 0b11,  // Modify green, keep red/blue
};

// ---------------------------------------------------------------------------
// Bit expansion: expand an M-bit value to 8-bit via bit replication
//
// For M >= 4: standard shift-and-or works cleanly.
// General formula replicates the top bits to fill the lower bits:
//   expanded = (val << (8-M)) | (val >> (2*M - 8))   when 2*M > 8
//   expanded = (val << (8-M))                         when 2*M <= 8
//
// Examples:
//   2-bit: ab -> ababababab (val<<6 | val<<4 | val<<2 | val)
//   3-bit: abc -> abcabcab
//   4-bit: abcd -> abcdabcd
//   5-bit: abcde -> abcdeabc
//   6-bit: abcdef -> abcdefab
// ---------------------------------------------------------------------------

constexpr std::uint8_t expand_to_8bit(std::uint8_t val, std::size_t bits) noexcept {
    if (bits == 0) return 0;
    if (bits >= 8) return val;

    // Build up 8-bit result by replicating the value
    unsigned result = 0;
    auto shift = 8u;
    auto b = static_cast<unsigned>(bits);
    while (shift > 0) {
        auto s = (shift >= b) ? b : shift;
        shift -= s;
        result |= static_cast<unsigned>(val >> (b - s)) << shift;
    }
    return static_cast<std::uint8_t>(result);
}

// Reduce an 8-bit value to M-bit precision (quantize)
constexpr std::uint8_t reduce_to_bits(std::uint8_t val, std::size_t bits) noexcept {
    if (bits >= 8) return val;
    return static_cast<std::uint8_t>(val >> (8 - bits));
}

// ---------------------------------------------------------------------------
// HAM single-pixel encoding primitives — exposed so external encoders
// (e.g. scap.cpp's HAM6+strips planner) can score HAM ops against a
// per-strip palette without re-implementing the picker.
//
// HamPrecomp: caller-owned cache. Build once per palette change.
// encode_ham_pixel: pick the lowest-error SET / MODIFY-{R,G,B} op for
//   `target` given previous pixel's output `prev`. Returns the encoded
//   value (control<<data_bits | data), the actual output sRGB color,
//   and OKLab² error.
// ---------------------------------------------------------------------------
// Padded to 4 bytes. The natural 3-byte layout caused a store-to-load
// forwarding stall in expand_ham_t's SET loop: `out[oi] = BeamState{
// base_srgb[i], …}` compiled to a 2-byte + 1-byte store into a stack
// temp followed by a 4-byte read back — store-buffer can't forward
// mismatched-width writes. AMDuProf attributed ~9.6s (of 14s total)
// to that single 4-byte mov. With explicit pad, the read of
// base_srgb[i] and the write to out[oi].color are both single 4-byte
// movs, so no round-trip via stack and no SLF stall.
// Trivially-default-constructible by design (no in-class member
// initializers, defaulted default ctor). Without this, BeamState
// inherits non-triviality and `std::vector<BeamState>::resize(N)`
// (called per pixel in expand_ham) value-initialises every slot —
// wasted work since every slot is overwritten before use. AMDuProf
// attributed ~4 % of HAM6+strips CPU to that zero-fill. The 3/4-arg
// ctor lets existing `SRGBColor{r,g,b}` brace-init sites stay
// untouched (4-arg ctor with defaulted pad). `SRGBColor{}` keeps
// its zero-init behavior because the defaulted default ctor is
// not user-provided, so value-init zero-initialises first.
struct SRGBColor {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t _pad;
    constexpr SRGBColor() noexcept = default;
    constexpr SRGBColor(std::uint8_t r_,
                        std::uint8_t g_,
                        std::uint8_t b_,
                        std::uint8_t pad_ = 0) noexcept
        : r{r_}, g{g_}, b{b_}, _pad{pad_} {}
    bool operator==(const SRGBColor& other) const noexcept {
        return r == other.r && g == other.g && b == other.b;
    }
};

struct HamPixelResult {
    std::uint8_t value;  // encoded HAM op (control<<data_bits | data)
    SRGBColor result_color;
    float error;  // OKLab² distance to target
};

struct HamPrecomp {
    std::vector<color_space::OKLab> palette_lab;
    std::vector<std::uint8_t> expand_lut;
    std::size_t data_bits;
    std::size_t num_data_values;

    HamPrecomp(std::span<const Color3f> palette, std::size_t db);
};

SRGBColor linear_to_srgb8(Color3f c);

HamPixelResult encode_ham_pixel(SRGBColor prev,
                                Color3f target,
                                const HamPrecomp& pre,
                                std::span<const SRGBColor> base_srgb);

// Row-level DP beam search HAM encoder with per-strip palettes. Same
// algorithm encode_ham_copper uses internally for its per-row palette
// case — exposed so strips HAM6 can drive a beam search with mid-line
// palette swaps. The HAM rolling state crosses strip boundaries
// unchanged; only the palette consulted at each pixel swaps.
//
// pres[s] / base_srgbs[s] is the active palette for pixels x where
// strip_for_x[x] == s. Returns the per-pixel HAM value sequence and
// total cumulative OKLab² error.
struct ScanlineResult {
    std::vector<std::uint8_t> values;
    float error{};
};
enum class HamMetric;  // forward decl; full definition below.
ScanlineResult encode_scanline_dp_per_strip(std::span<const Color3f> target_row,
                                            SRGBColor start_color,
                                            std::span<const HamPrecomp> pres,
                                            std::span<const std::span<const SRGBColor>> base_srgbs,
                                            std::span<const std::uint16_t> strip_for_x,
                                            std::size_t beam_width,
                                            HamMetric metric);

// Per-strip palette-aware ordered-dither HAM encoder. Drives a yliluoma-
// family picker (opt-checker / opt-line / opt-vline / opt-line-checker /
// opt-vline-checker / yliluoma{,1,2} / knoll / tri-tone) over the per-
// pixel HAM-reachable set (SET ∪ MODIFY-R/G/B variants of prev), using
// `pres[strip_for_x[x]]` + `base_srgbs[strip_for_x[x]]` at each column.
// Returns the per-pixel HAM codes; HAM prev state crosses strip
// boundaries unchanged.
ScanlineResult encode_scanline_ham_pal_aware_per_strip(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    dither::Method method,
    float strength,
    std::size_t y);

// Triple-pixel refinement post-pass for per-strip encoded scanlines.
// Same algorithm as the single-palette refine_triple inside ham.cpp,
// but each pixel's HAM op is scored against pres[strip_for_x[i]] /
// base_srgbs[strip_for_x[i]]. Triples that straddle a strip boundary
// pick ops valid for each pixel's own palette. Mutates `values` in
// place when a strictly-better window is found.
void refine_scanline_triple_per_strip(std::vector<std::uint8_t>& values,
                                      std::span<const Color3f> target_row,
                                      SRGBColor start_color,
                                      std::span<const HamPrecomp> pres,
                                      std::span<const std::span<const SRGBColor>> base_srgbs,
                                      std::span<const std::uint16_t> strip_for_x,
                                      std::size_t beam_k,
                                      HamMetric metric);

// ---------------------------------------------------------------------------
// HAM op-selection metric. OKLab² is perceptually-uniform and matches
// SSIMULACRA2 (the modern subjective-quality metric) much better than
// sRGB-MSE — measured ~+9 SSIMULACRA2 points on dithered HAM6+sliced, and
// ~+3.5 OKLab-dB on banded/HDR content. sRGB-MSE wins headline PSNR
// (~+0.5-1 dB) by optimizing the *reported metric* directly, but PSNR
// is a poor predictor of subjective quality on HAM output (where dither
// noise and discrete banding dominate the perception).
//
// Default is oklab2; --ham-metric srgb-mse selects the headline-PSNR
// scorer for the rare "I need to win published-PSNR" use case.
// ---------------------------------------------------------------------------
enum class HamMetric { srgb_mse, oklab2 };

// ---------------------------------------------------------------------------
// HAM encoding options
// ---------------------------------------------------------------------------

struct HamOptions {
    std::size_t beam_width = 48;  // beam search width for DP

    // Op-selection metric. See HamMetric for tradeoff.
    HamMetric metric = HamMetric::oklab2;

    // Dithering (error diffusion applied during HAM encoding)
    dither::Method dither_method = dither::Method::none;  // none = no dithering (default)
    float dither_strength = 1.0f;
    float error_clamp = 0.12f;

    // Palette diversity (0 = off, 1-5 = remove near-duplicate base colors,
    // re-seed from worst-served pixels). Experimental.
    int palette_diversity = 0;

    // Base palette quantizer (empty = median-cut, "pnn" = PNN agglomerative).
    std::string quantizer;

    // External base palette. When non-empty, encoders use this directly
    // (trimmed/padded to 2^(depth-2) entries) instead of running their
    // own quantizer. Plumbed from `--palette <file>`. For HAM6+sliced, the
    // first scanline still uses the full base palette and subsequent
    // lines diverge per the planner — but the *initial* base palette
    // is fixed to this. AGA users who pass --palette get a 24-bit base;
    // OCS users get whatever was loaded (typically already OCS-snapped
    // by palette_io).
    std::vector<Color3f> external_palette;

    // Triple-pixel refinement. After the main DP produces a scanline,
    // slide a 3-pixel window and re-optimize with a wider beam to catch
    // fringe lag that 1-pixel DP misses. beam_k = 0 disables.
    // 16 is the sweet spot (plateau past that); enabled by default
    // since the optimized inner loop makes the extra ~1ms/frame
    // cheap for the ~0.5-1 dB blurred-PSNR gain.
    std::size_t triple_beam = 16;

    // Greedy encoder — bypasses the DP beam search entirely. Each pixel
    // picks the locally best HAM op (nearest SET + best single MODIFY per
    // channel). ~20-40× faster than DP, ~1 dB PSNR loss. For realtime /
    // preview use cases where latency matters more than peak quality.
    bool greedy = false;

    // Number of initial rows that must display with the base palette only
    // (no copper swaps). For interlace, rows 0 and 1 are each field's first
    // displayed line with no prior scanline to schedule a pre-display WAIT on.
    std::size_t skip_initial_swap_rows = 0;

    // Better but ~2-4× slower sliced swap planner: per-iteration considers
    // top-K worst-error pixels + their OKLab centroid as candidates and
    // tries the top-N least-used slots, picking the (slot, color) pair
    // that drops row error the most. Off by default — the cheaper single-
    // candidate greedy is good enough for typical interactive use.
    bool best = false;

    // Optional progress reporter. Called periodically with (progress 0..1,
    // stage label). Encoder makes no guarantees about call frequency or
    // monotonicity across stages; values are smooth within a stage. Cheap
    // when null (no allocation). Callback runs on encoder threads — must
    // be thread-safe.
    std::function<void(float, std::string_view)> on_progress;
};

// ---------------------------------------------------------------------------
// HAM encoding result
// ---------------------------------------------------------------------------

struct HamResult {
    bitplane::BitplaneData planes;      // encoded bitplane data
    std::vector<Color3f> base_palette;  // the base palette chosen
    float total_error{};

    // Copper HAM: per-scanline data (empty if not copper)
    std::vector<std::vector<Color3f>> scanline_palettes;            // effective palette per line
    std::vector<std::vector<copper::CopperChange>> copper_changes;  // register changes per line
    std::size_t changes_per_line{};

    // Algorithm name from the Palette generated for base_palette
    // ("pnn" / "median-cut" / "gpu-restart"). Surfaces in the
    // CLI's "Palette: …" line; never duplicated from dispatch logic.
    std::string base_palette_quantizer;
};

// ---------------------------------------------------------------------------
// Encode an image using HAM with the specified mode
//
// Supports HAM6 (OCS) and HAM8 (AGA). Selects an optimal base palette
// of 2^(depth-2) colors, then encodes each scanline.
//
// Uses DP beam search: considers all reachable color states at each pixel
// position and prunes to the top beam_width candidates by cumulative error.
// ---------------------------------------------------------------------------

Result<HamResult> encode_ham(const Image& image,
                             amiga::Mode mode,
                             amiga::Chipset chipset = amiga::Chipset::ocs,
                             const HamOptions& opts = {});

// ---------------------------------------------------------------------------
// Encode with per-scanline copper base palettes
//
// Same as encode_ham but generates an optimal base palette for each scanline
// independently (copper changes palette registers per scanline).
// Returns per-scanline palettes in HamResult::scanline_palettes.
// HamResult::base_palette is set to the first scanline's palette (for CMAP).
// ---------------------------------------------------------------------------

Result<HamResult> encode_ham_copper(const Image& image,
                                    amiga::Mode mode,
                                    amiga::Chipset chipset = amiga::Chipset::ocs,
                                    const HamOptions& opts = {},
                                    bool is_hires = false,
                                    std::size_t override_changes = 0);

// Render copper HAM with per-scanline palettes
Result<Image> render_ham_copper(const bitplane::BitplaneData& planes,
                                const std::vector<std::vector<Color3f>>& scanline_palettes,
                                std::size_t data_bits);

// Convenience wrappers for backwards compatibility and explicit mode selection
Result<HamResult> encode_ham6(const Image& image,
                              amiga::Chipset chipset = amiga::Chipset::ocs,
                              const HamOptions& opts = {});

Result<HamResult> encode_ham8(const Image& image, const HamOptions& opts = {});

// ---------------------------------------------------------------------------
// Render HAM bitplane data back to an Image
//
// Unlike bitplane::render() which does simple palette[index] lookup, this
// function properly simulates HAM hardware decoding:
//   - Extracts control bits (top 2 bits) and data bits per pixel
//   - Control 00: set to base_palette[data]
//   - Control 01: modify blue channel
//   - Control 10: modify red channel
//   - Control 11: modify green channel
//   - Each scanline starts from palette[0]
//
// data_bits: planes.depth - 2 (computed automatically from depth)
// Works for any HAM depth (2-6 data bits).
// ---------------------------------------------------------------------------

Result<Image> render_ham(const bitplane::BitplaneData& planes,
                         std::span<const Color3f> base_palette,
                         std::size_t data_bits);

}  // namespace png2amiga::ham
