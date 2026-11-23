// ASTC encoder — 4×4 LDR single-plane.
//
// Per-block layout (mixed):
//   - RGB blocks (alpha=255):    block_mode 0x53, CEM 8,  QUANT_8 weights,
//                                QUANT_256 endpoints, single partition
//   - RGBA blocks (any alpha):   block_mode 0x42, CEM 12, QUANT_4 weights,
//                                QUANT_256 endpoints, single partition
//   - 2-partition RGB:           block_mode 0x42, CEM 8,  QUANT_4 weights,
//                                QUANT_40  endpoints (BISE quint-pack),
//                                partition_index from spec hash.
//
// Selection: encoder tries 1p + 2p per RGB block, picks lower OKLab².
//
// The ASTC decoder picks the largest endpoint quant level that fits
// the available color-bits, so the encoder must match that choice
// exactly or the bits decode as garbage. For 2p (12 endpoint values,
// 67 color-bits available), that is QUANT_40 (3 data bits + 1 quint,
// 64 stored bits).

//
// This file holds the footprint dispatch and the decoder. The block encoders
// are in astc_impl.inc; every footprint's candidate lists and template
// instantiations are in its own translation unit, astc_fp_<W>x<H>.cpp.

#include "astc.hpp"
#include "astc_footprints.hpp"
#include "astc_rdo.hpp"

#include "astcenc.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::astc {

EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    const int W = options.block_w;
    const int H = options.block_h;
    rdo::Side rdo_side;
    rdo::Side* const sp = options.rdo > 0 ? &rdo_side : nullptr;

    struct Footprint {
        int w, h;
        detail::FootprintEncodeFn* fn;
    };
    static constexpr Footprint kFootprints[] = {
        {4, 4, &detail::encode_image_4x4},
        {5, 4, &detail::encode_image_5x4},
        {5, 5, &detail::encode_image_5x5},
        {6, 5, &detail::encode_image_6x5},
        {6, 6, &detail::encode_image_6x6},
        {8, 5, &detail::encode_image_8x5},
        {8, 6, &detail::encode_image_8x6},
        {8, 8, &detail::encode_image_8x8},
        {10, 5, &detail::encode_image_10x5},
        {10, 6, &detail::encode_image_10x6},
        {10, 8, &detail::encode_image_10x8},
        {10, 10, &detail::encode_image_10x10},
        {12, 10, &detail::encode_image_12x10},
        {12, 12, &detail::encode_image_12x12},
    };
    EncodeResult res;  // unsupported footprint: empty
    for (const Footprint& f : kFootprints)
        if (f.w == W && f.h == H) {
            res = f.fn(rgba_srgb8, image_w, image_h, options, sp);
            break;
        }
    if (sp != nullptr && !res.blocks.empty())
        rdo::optimize(res, rdo_side, rgba_srgb8, image_w, image_h, W, H, options.rdo);
    return res;
}

std::vector<std::uint8_t> decode_image(std::span<const Block> blocks,
                                       int image_w,
                                       int image_h,
                                       int block_w,
                                       int block_h) {
    std::vector<std::uint8_t> out(std::size_t(image_w) * std::size_t(image_h) * 4u);

    astcenc_config cfg{};
    auto err = astcenc_config_init(ASTCENC_PRF_LDR_SRGB,
                                   std::uint32_t(block_w),
                                   std::uint32_t(block_h), 1u,
                                   0.0f, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg);
    if (err != ASTCENC_SUCCESS) return out;
    astcenc_context* ctx = nullptr;
    if (astcenc_context_alloc(&cfg, 1u, &ctx) != ASTCENC_SUCCESS) return out;

    astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    astcenc_image img{};
    img.dim_x = std::uint32_t(image_w);
    img.dim_y = std::uint32_t(image_h);
    img.dim_z = 1u;
    img.data_type = ASTCENC_TYPE_U8;
    void* slices[1] = {out.data()};
    img.data = slices;

    astcenc_decompress_image(ctx,
                             reinterpret_cast<const std::uint8_t*>(blocks.data()),
                             blocks.size() * std::size_t(kBlockBytes),
                             &img, &sw, 0u);
    astcenc_context_free(ctx);
    return out;
}

}  // namespace png2amiga::astc
