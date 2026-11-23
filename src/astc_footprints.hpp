#pragma once

// Per-footprint entry points of the ASTC encoder. Each one lives in its own
// translation unit (astc_fp_<W>x<H>.cpp); astc.cpp dispatches on the footprint.

#include "astc.hpp"
#include "astc_rdo.hpp"

#include <cstdint>
#include <span>

namespace png2amiga::astc::detail {

using FootprintEncodeFn = EncodeResult(std::span<const std::uint8_t> rgba_srgb8,
                                       int image_w,
                                       int image_h,
                                       const Options& options,
                                       rdo::Side* side);

FootprintEncodeFn encode_image_4x4;
FootprintEncodeFn encode_image_5x4;
FootprintEncodeFn encode_image_5x5;
FootprintEncodeFn encode_image_6x5;
FootprintEncodeFn encode_image_6x6;
FootprintEncodeFn encode_image_8x5;
FootprintEncodeFn encode_image_8x6;
FootprintEncodeFn encode_image_8x8;
FootprintEncodeFn encode_image_10x5;
FootprintEncodeFn encode_image_10x6;
FootprintEncodeFn encode_image_10x8;
FootprintEncodeFn encode_image_10x10;
FootprintEncodeFn encode_image_12x10;
FootprintEncodeFn encode_image_12x12;

}  // namespace png2amiga::astc::detail
