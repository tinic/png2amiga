#pragma once

#include "types.hpp"
#include <cstddef>
#include <algorithm>

namespace png2amiga::scale {

// Separable Lanczos-3 resampler. The single best-quality default for
// the convert→retro pipeline: sharp on photo downscales without the
// aliasing of under-filtered bicubic, and acceptable on upscales too.
// No knobs — the encoder's dither + palette quantisation downstream
// hides the small amount of ringing Lanczos can introduce.
// Pixel-art resize: select existing pixels, without filtering or new colours.
inline Image nearest(const Image& src, std::size_t width, std::size_t height) {
    Image out(width, height);
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            out[x, y] = src[std::min(x * src.width() / width, src.width() - 1),
                            std::min(y * src.height() / height, src.height() - 1)];
    return out;
}

Result<Image> resample(const Image& src, std::size_t dst_width, std::size_t dst_height);

}  // namespace png2amiga::scale
