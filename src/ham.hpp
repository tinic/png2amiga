#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
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
// HAM encoding options
// ---------------------------------------------------------------------------

struct HamOptions {
    std::size_t beam_width = 16;    // beam search width for DP

    // Dithering (error diffusion applied during HAM encoding)
    dither::Method dither_method = dither::Method::none;  // none = no dithering (default)
    float dither_strength = 1.0f;
    float error_clamp = 0.12f;

    // Palette diversity (0 = off, 1-5 = remove near-duplicate base colors,
    // re-seed from worst-served pixels). Experimental.
    int palette_diversity = 0;

    // Base palette quantizer (empty = median-cut, "pnn" = PNN agglomerative).
    std::string quantizer;

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
