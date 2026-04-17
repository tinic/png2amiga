#pragma once

// AtlasView<Asset> — draws sprites from a multi-page rect-packed atlas.
// `Asset` is a png2amiga-assets-generated namespace that satisfies AtlasAsset.
//
// The engine looks up an Entry by Id (the strongly-typed enum the generator
// emits), computes the source pointer as page_ptr(entry.page) + (x/16, y)
// offset, and blits a (w_padded × h) chunk to the destination.

#include "asset_traits.hpp"
#include "blitter.hpp"
#include "screen.hpp"
#include "types.hpp"

namespace pa {

template <AtlasAsset Asset>
class AtlasView {
public:
    static void draw(Screen& screen,
                     typename Asset::Id id,
                     i16 dst_x, i16 dst_y) noexcept {
        const auto& e = Asset::get(id);

        // Source pointer: page base + (src_x_words, src_y) offset within page.
        // All four atlas entries' x values are 16-aligned by the packer, so
        // this division is exact.
        const u16* page = Asset::page_ptr(e.page);
        auto page_words_per_plane = static_cast<u32>(Asset::page_h) *
                                    static_cast<u32>(Asset::page_bpr / 2);
        (void)page_words_per_plane;  // reserved for non-interleaved layouts

        // Interleaved layout: row-level plane stride already baked into
        // page_bpr; the blitter iterates (w_words * depth) per row, so the
        // per-plane offset is just (y * page_bpr + x/16 * 2) bytes.
        auto src_off_bytes = static_cast<u32>(e.y) *
                             static_cast<u32>(Asset::page_bpr) *
                             static_cast<u32>(Asset::depth) +
                             static_cast<u32>(e.x / 16) * 2u;
        const u16* src = page + (src_off_bytes / 2u);

        u16 w_words = static_cast<u16>(e.w_padded / 16);

        if constexpr (Asset::has_mask) {
            const u16* mask_page = Asset::mask_page_ptr(e.page);
            auto mask_off_bytes = static_cast<u32>(e.y) *
                                  static_cast<u32>(Asset::page_bpr) +
                                  static_cast<u32>(e.x / 16) * 2u;
            const u16* mask = mask_page + (mask_off_bytes / 2u);
            blitter::cookie_cut(screen.back(), src, mask,
                                Asset::page_bpr, page_words_per_plane,
                                static_cast<u16>(dst_x), static_cast<u16>(dst_y),
                                e.w_padded, e.h, Asset::depth);
        } else {
            blitter::plane_copy(screen.back(), src,
                                Asset::page_bpr, page_words_per_plane,
                                static_cast<u16>(dst_x / 16),
                                static_cast<u16>(dst_y),
                                w_words, e.h, Asset::depth);
        }
    }
};

} // namespace pa
