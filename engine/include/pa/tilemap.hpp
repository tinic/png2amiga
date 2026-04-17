#pragma once

// TileLayer<Asset> — draws a tilemap-backed background into a Screen's back
// buffer. Asset is a png2amiga-assets-generated namespace that satisfies
// TilesetAsset (see asset_traits.hpp).
//
// Blit granularity is one 16×N tile per blitter op. Dedup'd tile indices
// come from Asset::tilemap; flip bits are honored via descending-mode blits
// on real hardware (free flip). On host, the fake blitter ignores flip bits.

#include "asset_traits.hpp"
#include "blitter.hpp"
#include "screen.hpp"
#include "types.hpp"

namespace pa {

template <TilesetAsset Asset>
class TileLayer {
public:
    static void draw(Screen& screen,
                     u16 dst_x = 0, u16 dst_y = 0) noexcept
    {
        draw_viewport(screen.bitmap(), 0, 0, Asset::map_w, Asset::map_h,
                      dst_x, dst_y);
    }

    // BitMap overload — write into an arbitrary bitmap (e.g. a scroll canvas
    // wider than the displayed viewport).
    static void draw_viewport(BitMap& dst,
                              u16 src_cell_x, u16 src_cell_y,
                              u16 cells_w, u16 cells_h,
                              u16 dst_x, u16 dst_y) noexcept
    {
        constexpr u16 tw = Asset::tile_w;
        constexpr u16 th = Asset::tile_h;
        constexpr u16 tbpr = Asset::bytes_per_row;
        constexpr u16 tplane_words = th * (tbpr / 2);
        constexpr u8  depth = Asset::depth;

        for (u16 row = 0; row < cells_h; ++row) {
            for (u16 col = 0; col < cells_w; ++col) {
                auto cell = Asset::cell_at(
                    static_cast<unsigned>(src_cell_x + col),
                    static_cast<unsigned>(src_cell_y + row));
                auto idx = cell.index();
                // TODO flip: honor h_flip/v_flip via descending-mode blit.
                const u16* src = Asset::tile_ptr(idx);
                u16 dx = static_cast<u16>(dst_x + col * tw);
                u16 dy = static_cast<u16>(dst_y + row * th);
                blitter::plane_copy(dst, src,
                                    tbpr, tplane_words,
                                    static_cast<u16>(dx / 16), dy,
                                    static_cast<u16>(tw / 16), th, depth);
            }
        }
    }

    // Screen convenience overload: draws into the scroll canvas.
    static void draw_viewport(Screen& screen,
                              u16 src_cell_x, u16 src_cell_y,
                              u16 cells_w, u16 cells_h,
                              u16 dst_x, u16 dst_y) noexcept
    {
        draw_viewport(screen.bitmap(), src_cell_x, src_cell_y,
                      cells_w, cells_h, dst_x, dst_y);
    }
};

} // namespace pa
