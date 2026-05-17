#pragma once

#include "aligned_vector.hpp"  // AlignedFloatVec — exposed for PrecomputedSource
#include "types.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace png2amiga::ssimulacra2 {

// Compute SSIMULACRA2 (Cloudinary 2022/2023, Jon Sneyers) between two
// linear-RGB images of equal size. Score range is approximately
// (-inf, 100]: 30 = low, 50 = fair, 70 = high, 90 = visually lossless.
//
// Faithful port of cloudinary/ssimulacra2 against our Color3f primitives:
// - linear sRGB → XYB color transform (libjxl-compatible opsin matrix)
// - MakePositiveXYB rescaling
// - 6-scale multi-scale SSIM + edge-diff (ringing / blurring) maps
// - Each map evaluated under 1-norm and 4-norm
// - 108 calibrated weights → cubic warp → 100-anchored score
//
// One difference from the reference: blur uses a separable finite
// Gaussian kernel (σ=1.5, half-width=5) rather than libjxl's recursive
// FastGaussian. Numeric drift is small (<0.05 on typical images);
// validated for ranking-metric use.
float compute(std::span<const Color3f> orig,
              std::span<const Color3f> distorted,
              std::size_t width,
              std::size_t height);

// Per-source-image precompute. Holds the source-only outputs of every
// stage that doesn't depend on the distorted image: per-scale linear
// downsamples → XYB planes → mu1 (gaussian-blurred channel) and s11
// (gaussian-blurred square). pop_search scores ~8K candidates against
// the same source; building this once outside the loop cuts a measured
// ~16% of total CPU on --best EHB (Zen 1 / MSVC, AMD uProf 2026-05-17),
// matching the same anti-pattern as the dither precompute hoist.
struct PrecomputedSource {
    static constexpr int kMaxScales = 6;

    struct Scale {
        std::size_t w = 0, h = 0;
        AlignedFloatVec X, Y, B;
        std::array<AlignedFloatVec, 3> mu1;  // per channel (X, Y, B)
        std::array<AlignedFloatVec, 3> s11;  // per channel
    };

    std::array<Scale, kMaxScales> scales{};
    int n_active = 0;            // number of populated scales
    std::size_t width = 0;       // source w/h at scale 0
    std::size_t height = 0;

    // Idempotent — no-op when (width, height) match and content was
    // built. Use a fresh instance per source image; the per-scale
    // buffers grow but do not shrink across builds.
    void prepare(std::span<const Color3f> src, std::size_t w, std::size_t h);
};

// Overload that uses a prebuilt source-side cache. Skips source
// downsample / XYB / mu1 / s11 — only the distorted side and cross
// terms (mu2, s22, s12, ssim, edgediff) are computed.
float compute(const PrecomputedSource& src_pre, std::span<const Color3f> distorted);

}  // namespace png2amiga::ssimulacra2
