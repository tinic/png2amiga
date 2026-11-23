// ASTC encoder: candidate lists of the 4x4 footprint. See astc_impl.inc.

#include "astc_impl.inc"
#include "astc_footprints.hpp"

namespace png2amiga::astc::detail {

EncodeResult encode_image_4x4(std::span<const std::uint8_t> rgba_srgb8,
                               int image_w,
                               int image_h,
                               const Options& options,
                               rdo::Side* sp) {
    auto run = [&]<block_compress::BlockMetric M>() -> EncodeResult {
        return encode_image_gen<M, 4, 4>(rgba_srgb8, image_w, image_h, options, sp,
                                       make_encode_fn_4x4_exhaustive<M>());
    };
    return options.metric == block_compress::BlockMetric::srgb_mse
        ? run.template operator()<block_compress::BlockMetric::srgb_mse>()
        : run.template operator()<block_compress::BlockMetric::oklab2>();
}

}  // namespace png2amiga::astc::detail
