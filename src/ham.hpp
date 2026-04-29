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
    set_palette = 0b00,     // Use base palette color
    modify_blue = 0b01,     // Modify blue, keep red/green
    modify_red  = 0b10,     // Modify red, keep green/blue
    modify_green = 0b11,    // Modify green, keep red/blue
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
// (e.g. scap.cpp's HAM6+SCAP planner) can score HAM ops against a
// per-strip palette without re-implementing the picker.
//
// HamPrecomp: caller-owned cache. Build once per palette change.
// encode_ham_pixel: pick the lowest-error SET / MODIFY-{R,G,B} op for
//   `target` given previous pixel's output `prev`. Returns the encoded
//   value (control<<data_bits | data), the actual output sRGB colour,
//   and OKLab² error.
// ---------------------------------------------------------------------------
struct SRGBColor {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    bool operator==(const SRGBColor&) const = default;
};

struct HamPixelResult {
    std::uint8_t value;       // encoded HAM op (control<<data_bits | data)
    SRGBColor    result_color;
    float        error;       // OKLab² distance to target
};

struct HamPrecomp {
    std::vector<color_space::OKLab> palette_lab;
    std::vector<std::uint8_t>       expand_lut;
    std::size_t                     data_bits;
    std::size_t                     num_data_values;

    HamPrecomp(std::span<const Color3f> palette, std::size_t db);
};

SRGBColor linear_to_srgb8(Color3f c);

HamPixelResult encode_ham_pixel(SRGBColor prev,
                                 Color3f target,
                                 const HamPrecomp& pre,
                                 std::span<const SRGBColor> base_srgb);

// Row-level DP beam search HAM encoder with per-strip palettes. Same
// algorithm encode_ham_copper uses internally for its per-row palette
// case — exposed so SCAP HAM6 can drive a beam search with mid-line
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
ScanlineResult encode_scanline_dp_per_strip(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_width,
    HamMetric metric);

// Triple-pixel refinement post-pass for per-strip encoded scanlines.
// Same algorithm as the single-palette refine_triple inside ham.cpp,
// but each pixel's HAM op is scored against pres[strip_for_x[i]] /
// base_srgbs[strip_for_x[i]]. Triples that straddle a strip boundary
// pick ops valid for each pixel's own palette. Mutates `values` in
// place when a strictly-better window is found.
void refine_scanline_triple_per_strip(
    std::vector<std::uint8_t>& values,
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_k,
    HamMetric metric);

// ---------------------------------------------------------------------------
// HAM op-selection metric. sRGB-MSE matches our reported PSNR metric and
// HAM hardware semantics (per-channel sRGB DAC values), and on average
// produces +0.5..+1 dB gains over OKLab² in apples-to-apples shootouts.
// OKLab² is perceptually-uniform: better on banded/HDR content (chuck31
// is +3.5 OKLab-dB worse under sRGB-MSE) at the cost of headline PSNR.
// Default is srgb_mse; --ham-metric oklab2 selects the perceptual scorer.
// ---------------------------------------------------------------------------
enum class HamMetric { srgb_mse, oklab2 };

// ---------------------------------------------------------------------------
// HAM encoding options
// ---------------------------------------------------------------------------

struct HamOptions {
    std::size_t beam_width = 48;    // beam search width for DP

    // Op-selection metric. See HamMetric for tradeoff.
    HamMetric metric = HamMetric::srgb_mse;

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
    // own quantizer. Plumbed from `--palette <file>`. For HAM6+CAP, the
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

    // Better but ~2-4× slower CAP swap planner: per-iteration considers
    // top-K worst-error pixels + their OKLab centroid as candidates and
    // tries the top-N least-used slots, picking the (slot, color) pair
    // that drops row error the most. Off by default — the cheaper single-
    // candidate greedy is good enough for typical interactive use.
    bool cap_best = false;

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
    bitplane::BitplaneData planes;          // encoded bitplane data
    std::vector<Color3f> base_palette;      // the base palette chosen
    float total_error{};

    // Copper HAM: per-scanline data (empty if not copper)
    std::vector<std::vector<Color3f>> scanline_palettes;         // effective palette per line
    std::vector<std::vector<copper::CopperChange>> copper_changes;  // register changes per line
    std::size_t changes_per_line{};
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
Result<Image> render_ham_copper(
    const bitplane::BitplaneData& planes,
    const std::vector<std::vector<Color3f>>& scanline_palettes,
    std::size_t data_bits);

// Convenience wrappers for backwards compatibility and explicit mode selection
Result<HamResult> encode_ham6(const Image& image,
                              amiga::Chipset chipset = amiga::Chipset::ocs,
                              const HamOptions& opts = {});

Result<HamResult> encode_ham8(const Image& image,
                              const HamOptions& opts = {});

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

} // namespace png2amiga::ham
