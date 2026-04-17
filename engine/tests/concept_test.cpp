// Concept-compliance smoke test. Confirms that the generated asset namespace
// exposes everything the engine's TilesetAsset concept requires.
//
// A namespace can't be passed as a type parameter; wrap it in a type with
// static members that forward to the namespace. This is the exact pattern
// engine users will use when consuming generated assets — the wrapper is a
// trivial boilerplate you'd put in one spot per project.

#include "pa/asset_traits.hpp"
#include "pa/types.hpp"

#include "assets/level_3_1.hpp"

struct Level3_1 {
    static constexpr unsigned tile_w        = assets::level_3_1::tile_w;
    static constexpr unsigned tile_h        = assets::level_3_1::tile_h;
    static constexpr unsigned depth         = assets::level_3_1::depth;
    static constexpr unsigned bytes_per_row = assets::level_3_1::bytes_per_row;
    static constexpr unsigned tile_bytes    = assets::level_3_1::tile_bytes;
    static constexpr unsigned num_tiles     = assets::level_3_1::num_tiles;
    static constexpr unsigned map_w         = assets::level_3_1::map_w;
    static constexpr unsigned map_h         = assets::level_3_1::map_h;
    static constexpr unsigned num_cells     = assets::level_3_1::num_cells;
    static constexpr unsigned palette_size  = assets::level_3_1::palette_size;
    static auto tile_ptr(pa::u16 idx) noexcept {
        return assets::level_3_1::tile_ptr(idx);
    }
    static auto cell_at(unsigned x, unsigned y) noexcept {
        return assets::level_3_1::cell_at(x, y);
    }
    static const pa::u16* palette_ptr() noexcept {
        return assets::level_3_1::palette.data();
    }
};

// Core contract: generator must keep emitting exactly these fields.
static_assert(assets::level_3_1::tile_w == 16);
static_assert(assets::level_3_1::tile_h == 16);
static_assert(assets::level_3_1::depth == 5);
static_assert(assets::level_3_1::num_tiles > 0);
static_assert(assets::level_3_1::palette_size > 0);

// The Level3_1 wrapper must satisfy pa::TilesetAsset. This is the key
// check — if the concept or the generator drifts, this fails at compile.
static_assert(pa::TilesetAsset<Level3_1>);
static_assert(pa::is_ocs_palette<Level3_1>);

int main() {
    // Runtime spot-check: first palette entry is 0x0000 (reserved black)
    // per png2amiga-assets convention when color 0 is reserved. The tiles
    // generator doesn't reserve color 0, so the first entry is whatever
    // the PNG's first unique color was. We just confirm the arrays are
    // accessible and non-empty.
    if (assets::level_3_1::tiles_data.size() == 0) return 1;
    if (assets::level_3_1::tilemap.size() == 0) return 1;
    if (assets::level_3_1::palette.size() == 0) return 1;

    // tile_ptr and cell_at work.
    auto* t0 = assets::level_3_1::tile_ptr(0);
    if (!t0) return 2;
    auto c = assets::level_3_1::cell_at(0, 0);
    if (c.index() >= assets::level_3_1::num_tiles) return 3;
    return 0;
}
