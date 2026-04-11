#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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
    checker,          // 2x2 checkerboard
    clustered_dot,    // 4x4 clustered dot

    // Non-square ordered (for non-square pixel modes)
    h2x4,             // 2 wide x 4 tall (lores interlace, wide pixels)
    v4x2,             // 4 wide x 2 tall (hires, tall pixels)
    bayer4x2,         // Bayer 4 wide x 2 tall (hires)
    bayer2x4,         // Bayer 2 wide x 4 tall (interlace)

    // Horizontal lines
    line2,            // 1x2 alternating rows
    line_checker,     // 2x2 line-biased
    line4,            // 1x4 horizontal gradient
    line8,            // 1x8 horizontal gradient

    // Vertical lines
    vline2,           // 2x1 alternating columns
    vline_checker,    // 2x2 column-biased
    vline4,           // 4x1 vertical gradient
    vline8,           // 8x1 finest vertical gradient

    // Additional ordered dithering
    halftone8x8,      // 45-degree clustered dot halftone (newspaper look)
    diagonal8x8,      // 45-degree diagonal clustered dot (64 levels)
    spiral5x5,        // spiral dot growth from center
    hex8x8,           // non-rectangular hexagonal tiling
    hex5x5,           // non-rectangular slanted square tiling
    blue_noise,       // 64x64 blue noise (film-grain look, no visible pattern)
    ign,              // Interleaved Gradient Noise (analytical, no LUT)
    white_noise,      // pure random hash per pixel (film grain)
    r2_sequence,      // Martin Roberts R2 low-discrepancy sequence
    crosshatch,       // pen-and-ink crosshatching
    radial,           // concentric circles (engraving look)
    value_noise,      // coherent noise (organic clumping)

    // Error diffusion
    floyd_steinberg,
    atkinson,
    sierra_lite,
    stucki,
    jarvis,

    // Advanced error diffusion
    ostromoukhov,     // variable-coefficient error diffusion
};

// ---------------------------------------------------------------------------
// Dithering settings
// ---------------------------------------------------------------------------

struct Settings {
    Method method = Method::floyd_steinberg;
    float strength = 1.0f;      // 0.0 = no dithering, 1.0 = full
    float error_clamp = 0.12f;  // max error magnitude per OKLab channel
    bool serpentine = true;      // alternate scan direction (error diffusion)
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

DitherResult apply(const Image& image,
                   std::span<const Color3f> palette,
                   const Settings& settings);

// ---------------------------------------------------------------------------
// Returns true if the method is an ordered (non-error-diffusion) method.
// ---------------------------------------------------------------------------

constexpr bool is_ordered(Method m) noexcept {
    switch (m) {
    case Method::none:
    case Method::bayer2x2:
    case Method::bayer4x4:
    case Method::bayer8x8:
    case Method::checker:
    case Method::h2x4:
    case Method::clustered_dot:
    case Method::line2:
    case Method::line_checker:
    case Method::line4:
    case Method::line8:
    case Method::vline2: case Method::vline_checker: case Method::vline4: case Method::vline8:
    case Method::v4x2: case Method::bayer4x2: case Method::bayer2x4:
    case Method::halftone8x8:
    case Method::diagonal8x8:
    case Method::spiral5x5:
    case Method::hex8x8:
    case Method::hex5x5:
    case Method::blue_noise:
    case Method::ign:
    case Method::white_noise:
    case Method::r2_sequence:
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

} // namespace png2amiga::dither
