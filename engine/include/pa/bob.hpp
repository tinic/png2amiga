#pragma once

// BobSprite<Asset> — draws a pre-quantized animation-strip asset onto a
// Screen's back buffer. `Asset` is a png2amiga-assets-generated namespace
// that satisfies StripAsset.
//
// Honors the strip's `preshift` count: draw() picks the right pre-shifted
// copy based on the destination X's sub-word bits, and blits the 16-aligned
// chunk. Masked BOBs use cookie_cut; opaque ones use plane_copy.

#include "asset_traits.hpp"
#include "blitter.hpp"
#include "screen.hpp"
#include "types.hpp"

namespace pa {

template <StripAsset Asset>
class BobSprite {
public:
    // Draw a single frame at pixel position (x, y). If the strip has mask
    // data, uses cookie_cut; otherwise plane_copy. The blit reads
    // storage_w pixels from the chosen pre-shifted copy; engine users only
    // observe the frame_w visible pixels.
    static void draw(Screen& screen, u16 frame, i16 x, i16 y) noexcept {
        if (frame >= Asset::num_frames) return;
        if (x + static_cast<i16>(Asset::frame_w) <= 0 || y + static_cast<i16>(Asset::frame_h) <= 0) return;
        if (x >= static_cast<i16>(screen.back().width) || y >= static_cast<i16>(screen.back().height)) return;

        unsigned k = Asset::shift_for_x(static_cast<unsigned>(x));
        const u16* src  = Asset::frame_ptr(frame, k);
        constexpr u16 plane_words = Asset::frame_h * (Asset::bytes_per_row / 2u);
        u16 word_x = static_cast<u16>(x / 16);

        if constexpr (Asset::has_mask) {
            const u16* mask = Asset::mask_ptr(frame, k);
            blitter::cookie_cut(screen.back(), src, mask,
                                Asset::bytes_per_row, plane_words,
                                static_cast<u16>(x), static_cast<u16>(y),
                                Asset::storage_w, Asset::frame_h,
                                Asset::depth);
        } else {
            blitter::plane_copy(screen.back(), src,
                                Asset::bytes_per_row, plane_words,
                                word_x, static_cast<u16>(y),
                                static_cast<u16>(Asset::storage_w / 16),
                                Asset::frame_h, Asset::depth);
        }
    }
};

} // namespace pa
