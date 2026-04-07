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
    h2x4,            // 2x4 Bayer (perceptually square at 2:1 pixel ratio)
    clustered_dot,    // 4x4 clustered dot
    line2,            // 1x2 alternating rows
    line_checker,     // 2x2 line-biased
    line4,            // 1x4 horizontal gradient
    line8,            // 1x8 horizontal gradient

    // Error diffusion
    floyd_steinberg,
    atkinson,
    sierra_lite,
    stucki,
    jarvis,
};

// ---------------------------------------------------------------------------
// Dithering settings
// ---------------------------------------------------------------------------

struct Settings {
    Method method = Method::floyd_steinberg;
    float strength = 1.0f;      // 0.0 = no dithering, 1.0 = full
    float error_clamp = 0.12f;  // max error magnitude per OKLab channel
    bool serpentine = true;      // alternate scan direction (error diffusion)
    bool interlace = false;     // halve Y for ordered dither (each field gets checker)
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

} // namespace png2amiga::dither
