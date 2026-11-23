// ASTC encoder: candidate lists of the 5x5 footprint. See astc_impl.inc.

#include "astc_impl.inc"
#include "astc_footprints.hpp"

namespace png2amiga::astc::detail {

EncodeResult encode_image_5x5(std::span<const std::uint8_t> rgba_srgb8,
                               int image_w,
                               int image_h,
                               const Options& options,
                               rdo::Side* sp) {
    auto run = [&]<block_compress::BlockMetric M>() -> EncodeResult {
        return encode_image_gen<M, 5, 5>(rgba_srgb8, image_w, image_h, options, sp,
            combine_encode_fns<5, 5>(
            make_encode_fn_2p_pack<5, 5, M,
                DecimCfg2p{2,3,0x32E,16,64},
                DecimCfg2p{2,3,0x33F,32,48},
                DecimCfg2p{2,4,0x15F,8,64},
                DecimCfg2p{2,4,0x34E,16,40},
                DecimCfg2p{2,4,0x35F,32,24},
                DecimCfg2p{2,5,0x17F,8,48},
                DecimCfg2p{2,5,0x36E,16,24},
                DecimCfg2p{3,2,0x38E,16,64},
                DecimCfg2p{3,2,0x39F,32,48},
                DecimCfg2p{3,3,0x1BF,8,64},
                DecimCfg2p{3,3,0x3AE,16,32},
                DecimCfg2p{3,4,0x1CE,4,64},
                DecimCfg2p{3,4,0x1DF,8,32},
                DecimCfg2p{3,5,0x1EE,4,48},
                DecimCfg2p{4,2,0x013,8,64},
                DecimCfg2p{4,2,0x202,16,40},
                DecimCfg2p{4,2,0x213,32,24},
                DecimCfg2p{4,3,0x022,4,64},
                DecimCfg2p{4,3,0x033,8,32},
                DecimCfg2p{4,4,0x042,4,40},
                DecimCfg2p{4,5,0x062,4,24},
                DecimCfg2p{5,2,0x093,8,48},
                DecimCfg2p{5,2,0x282,16,24},
                DecimCfg2p{5,3,0x0A2,4,48},
                DecimCfg2p{5,4,0x0C2,4,24},
                DecimCfg2p{5,5,0x0E1,2,64}>(),
            make_encode_fn_decim_pack<5, 5, M,
                DecimCfg{2,3,0x33F,32}, DecimCfg{2,4,0x35F,32},
                DecimCfg{2,5,0x37F,32}, DecimCfg{3,2,0x39F,32},
                DecimCfg{3,3,0x3BF,32}, DecimCfg{3,4,0x3DF,32},
                DecimCfg{3,5,0x3EE,16}, DecimCfg{4,2,0x213,32},
                DecimCfg{4,3,0x233,32}, DecimCfg{4,4,0x251,12},
                DecimCfg{4,5,0x073,8}, DecimCfg{5,2,0x293,32},
                DecimCfg{5,3,0x2A2,16}, DecimCfg{5,4,0x0D3,8},
                DecimCfg{5,5,0x0F2,5},
                // Tighter-endpoint candidates (enumerated):
                DecimCfg{3,5,0x3FE,20, 8, 192},
                DecimCfg{3,5,0x3EF,24, 8, 128},
                DecimCfg{3,5,0x3FF,32, 8, 64},
                DecimCfg{4,4,0x242,16, 8, 192},
                DecimCfg{4,4,0x252,20, 8, 96},
                DecimCfg{4,4,0x243,24, 8, 64},
                DecimCfg{4,4,0x253,32, 8, 32},
                DecimCfg{4,5,0x261,10, 8, 160},
                DecimCfg{4,5,0x271,12, 8, 80},
                DecimCfg{4,5,0x262,16, 8, 32},
                DecimCfg{5,3,0x2B2,20, 8, 192},
                DecimCfg{5,3,0x2A3,24, 8, 128},
                DecimCfg{5,3,0x2B3,32, 8, 64},
                DecimCfg{5,4,0x2C1,10, 8, 160},
                DecimCfg{5,4,0x2D1,12, 8, 80},
                DecimCfg{5,4,0x2C2,16, 8, 32},
                DecimCfg{5,5,0x0E3,6, 8, 192},
                DecimCfg{5,5,0x0F3,8, 8, 64}>()));
    };
    return options.metric == block_compress::BlockMetric::srgb_mse
        ? run.template operator()<block_compress::BlockMetric::srgb_mse>()
        : run.template operator()<block_compress::BlockMetric::oklab2>();
}

}  // namespace png2amiga::astc::detail
