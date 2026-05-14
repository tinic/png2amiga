#pragma once

#include "types.hpp"
#include <cstddef>

namespace png2amiga::scale {

// Separable Lanczos-3 resampler. The single best-quality default for
// the convert→retro pipeline: sharp on photo downscales without the
// aliasing of under-filtered bicubic, and acceptable on upscales too.
// No knobs — the encoder's dither + palette quantisation downstream
// hides the small amount of ringing Lanczos can introduce.
Result<Image> resample(const Image& src, std::size_t dst_width, std::size_t dst_height);

}  // namespace png2amiga::scale
