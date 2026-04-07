#pragma once

#include "types.hpp"

#include <cstddef>
#include <span>

namespace png2amiga::quantize {

// ---------------------------------------------------------------------------
// Palette quantization algorithms
//
// Given an image, find the best N colors to represent it.
// Unlike png2c64 (which quantizes per cell due to VIC-II constraints),
// the Amiga uses a single global palette for standard bitplane modes,
// so this is a straightforward whole-image quantization.
// ---------------------------------------------------------------------------

enum class Algorithm : unsigned char {
    median_cut,     // Median-cut: fast, good general quality
    ocs_bruteforce, // OCS: histogram + k-means over all 4096 OCS colors
};

// ---------------------------------------------------------------------------
// Generate an optimal N-color palette from an image.
//
// image:     input image (linear RGB)
// max_colors: maximum number of colors in the palette (2-256)
// algo:      quantization algorithm
//
// Returns a Palette with at most max_colors entries.
// ---------------------------------------------------------------------------

Result<Palette> quantize(const Image& image,
                         std::size_t max_colors,
                         Algorithm algo = Algorithm::median_cut);

// ---------------------------------------------------------------------------
// Median-cut quantization (direct interface)
//
// colors: input color samples (linear RGB). Duplicates are fine.
// max_colors: target palette size.
//
// Returns palette sorted by perceptual luminance (OKLab L).
// ---------------------------------------------------------------------------

Palette median_cut(std::span<const Color3f> colors,
                   std::size_t max_colors);

} // namespace png2amiga::quantize
