#pragma once

#include "types.hpp"

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

}  // namespace png2amiga::ssimulacra2
