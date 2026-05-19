// ASTC encoder/decoder — wraps ARM's astcenc reference library
// (third_party/astc-encoder). Provides functional ASTC encoding for all
// 14 LDR 2D footprints; a native OKLab²-scored encoder will replace the
// encode path in a later phase.

#include "astc.hpp"

#include "astcenc.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace png2amiga::astc {

namespace {

// Build an astcenc_image header for our row-major RGBA8 buffer. The
// image's `data` is a 2D slice array; for a 2D texture there's one slice
// pointer.
struct ImageWrap {
    astcenc_image image{};
    void* slices[1]{};

    ImageWrap(std::uint8_t* rgba, int w, int h) {
        image.dim_x = std::uint32_t(w);
        image.dim_y = std::uint32_t(h);
        image.dim_z = 1u;
        image.data_type = ASTCENC_TYPE_U8;
        slices[0] = rgba;
        image.data = slices;
    }
};

// Identity swizzle (R,G,B,A → R,G,B,A) — applied before compression and
// after decompression. We don't want any channel reordering here.
constexpr astcenc_swizzle kSwizzleIdentity{ASTCENC_SWZ_R, ASTCENC_SWZ_G,
                                           ASTCENC_SWZ_B, ASTCENC_SWZ_A};

}  // namespace

EncodeResult encode_image(std::span<const std::uint8_t> rgba_srgb8,
                          int image_w,
                          int image_h,
                          const Options& options) {
    EncodeResult res;
    res.block_cols = (image_w + options.block_w - 1) / options.block_w;
    res.block_rows = (image_h + options.block_h - 1) / options.block_h;
    res.blocks.assign(
        std::size_t(res.block_cols) * std::size_t(res.block_rows), Block{});

    astcenc_config cfg{};
    auto profile = options.srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    auto err = astcenc_config_init(profile,
                                   std::uint32_t(options.block_w),
                                   std::uint32_t(options.block_h),
                                   1u,
                                   options.quality,
                                   /*flags=*/0u,
                                   &cfg);
    if (err != ASTCENC_SUCCESS) return res;

    astcenc_context* ctx = nullptr;
    err = astcenc_context_alloc(&cfg, /*thread_count=*/1u, &ctx);
    if (err != ASTCENC_SUCCESS) return res;

    // astcenc_image takes a mutable pointer; we just hand it our buffer
    // contents (it doesn't mutate input — the signature is non-const for
    // the in-out decompress path).
    std::vector<std::uint8_t> rgba_copy(rgba_srgb8.begin(), rgba_srgb8.end());
    ImageWrap wrap(rgba_copy.data(), image_w, image_h);

    err = astcenc_compress_image(ctx, &wrap.image, &kSwizzleIdentity,
                                 reinterpret_cast<std::uint8_t*>(res.blocks.data()),
                                 res.blocks.size() * std::size_t(kBlockBytes),
                                 /*thread_index=*/0u);

    astcenc_context_free(ctx);
    if (err != ASTCENC_SUCCESS) res.blocks.clear();
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
                                   std::uint32_t(block_h),
                                   1u,
                                   /*quality=*/0.0f,
                                   ASTCENC_FLG_DECOMPRESS_ONLY,
                                   &cfg);
    if (err != ASTCENC_SUCCESS) return out;

    astcenc_context* ctx = nullptr;
    err = astcenc_context_alloc(&cfg, /*thread_count=*/1u, &ctx);
    if (err != ASTCENC_SUCCESS) return out;

    ImageWrap wrap(out.data(), image_w, image_h);
    astcenc_decompress_image(ctx,
                             reinterpret_cast<const std::uint8_t*>(blocks.data()),
                             blocks.size() * std::size_t(kBlockBytes),
                             &wrap.image,
                             &kSwizzleIdentity,
                             /*thread_index=*/0u);

    astcenc_context_free(ctx);
    return out;
}

}  // namespace png2amiga::astc
