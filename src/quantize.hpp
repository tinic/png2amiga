#pragma once

#include "amiga.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstddef>
#include <span>
#include <string_view>
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
    median_cut,      // Median-cut: fast, good general quality
    ocs_bruteforce,  // OCS: histogram + k-means over all 4096 OCS colors
    pnn,             // Pairwise Nearest Neighbor (agglomerative, OKLab)
    gpu_restart,     // Lloyd k-means in OKLab + parallel restarts on Apple
                     // GPU. Wins over median_cut by mean ΔS2 ~+3 across
                     // K ∈ {8..256}; falls back to median_cut at runtime
                     // when Metal is unavailable. macOS-only build path.
};

// String name for an Algorithm — matches the CLI --quantizer values
// 1:1 so callers can print it directly in status lines.
inline std::string_view name_of(Algorithm a) noexcept {
    switch (a) {
    case Algorithm::median_cut:
        return "median-cut";
    case Algorithm::ocs_bruteforce:
        return "ocs-bruteforce";
    case Algorithm::pnn:
        return "pnn";
    case Algorithm::gpu_restart:
        return "gpu-restart";
    }
    return "unknown";
}

// One-stop resolver. Maps (mode, chipset, --quantizer string) to the
// Algorithm that will actually run. All dispatch sites — api.cpp,
// main.cpp, ham.cpp, copper.cpp, status-line printers — call this so
// the choice is made in exactly one place.
//
// Precedence:
//   1. Explicit user choice ("median-cut" / "ocs-bruteforce" / "pnn" /
//      "gpu-restart"). gpu-restart silently falls back to median-cut
//      when the runtime Metal device probe fails (non-Apple / no GPU).
//   2. CLI legacy alias "fast" = median-cut.
//   3. Auto ("" / "auto"): chipset- and mode-aware defaults
//      (see body for the table).
Algorithm resolve_algorithm(amiga::Mode mode,
                            amiga::Chipset chipset,
                            std::string_view user_choice) noexcept;

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
                         Algorithm algo = Algorithm::median_cut,
                         int palette_diversity = 0);

// ---------------------------------------------------------------------------
// Median-cut quantization (direct interface)
//
// colors: input color samples (linear RGB). Duplicates are fine.
// max_colors: target palette size.
//
// Returns palette sorted by perceptual luminance (OKLab L).
// ---------------------------------------------------------------------------

Palette median_cut(std::span<const Color3f> colors,
                   std::size_t max_colors,
                   int palette_diversity = 0);

// ---------------------------------------------------------------------------
// Pairwise Nearest Neighbor quantization (direct interface)
//
// Agglomerative clustering in OKLab space. Each starting cluster is a
// histogram bin; at each step the pair of clusters with the lowest joint
// variance merge cost (Ward's linkage) is merged until `max_colors` remain.
//
// Better than median-cut at low palette counts (≤32). Slower than
// median-cut but comparable to ocs_bruteforce.
// ---------------------------------------------------------------------------

Palette pnn_quantize(std::span<const Color3f> colors,
                     std::size_t max_colors,
                     int palette_diversity = 0,
                     bool snap_to_ocs = false);

// ---------------------------------------------------------------------------
// Palette diversity pass (ham_convert-inspired).
//
// Iteratively finds the closest pair of palette entries in OKLab and, if
// they are closer than a level-dependent threshold, merges one to the
// midpoint and reseeds the other from the cluster with highest total error.
// Runs k-means refinement after each candidate swap. Only commits the swap
// if it lowers global palette SSE and does not collapse palette slots to
// the same discrete color (when snap_to_ocs is true).
//
// diversity_level: 0 = off, 1 = conservative, 5 = aggressive.
// snap_to_ocs: if true, snap each refined entry to OCS 12-bit precision.
// ---------------------------------------------------------------------------

void diversify_palette(Palette& palette,
                       std::span<const Color3f> pixels,
                       int diversity_level,
                       bool snap_to_ocs);

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

Result<Palette> refine_with_dither(const Image& image,
                                   const Palette& initial_palette,
                                   const dither::Settings& dither_settings,
                                   amiga::Chipset chipset,
                                   amiga::Mode mode = amiga::Mode::lores,
                                   std::size_t max_iterations = 4,
                                   const std::vector<bool>& locked = {});

// ---------------------------------------------------------------------------
// EGA histogram quantizer: pick K distinct slots directly from the 64-entry
// IrgbIRGB gamut. K-means++ seeding over the histogram, then Lloyd refinement
// that re-assigns each bucket to its nearest picked slot and snaps centroids
// back to the closest still-available EGA entry. Much better slot utilization
// than continuous median-cut + post-snap, which collapses ~30% of slots.
// ---------------------------------------------------------------------------
Palette ega_histogram(const Image& image, std::size_t K);

}  // namespace png2amiga::quantize
