#pragma once

#include "amiga.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <span>
#include <vector>

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

// ---------------------------------------------------------------------------
// Dither-aware palette refinement.
//
// Standard k-means assigns each pixel to its nearest palette color. But
// with dithering, the assignment depends on accumulated error from
// neighbors — the ditherer may push a pixel to a non-nearest entry. The
// "optimal" palette for dithered output is different from the one that
// minimizes per-pixel nearest-color error.
//
// This pass iterates:
//   1. Run the selected dithering method with the current palette
//   2. For each palette slot, compute the centroid (in OKLab) of all
//      pixels actually assigned to it by the ditherer
//   3. Update the palette, snap to chipset precision, repeat
//
// Locked slots (e.g., color 0 = black) are never moved.
// ---------------------------------------------------------------------------

Result<Palette> refine_with_dither(
    const Image& image,
    const Palette& initial_palette,
    const dither::Settings& dither_settings,
    amiga::Chipset chipset,
    std::size_t max_iterations = 4,
    const std::vector<bool>& locked = {});

} // namespace png2amiga::quantize
