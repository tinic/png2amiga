// hello_tiles — tiny end-to-end example that opens a screen and paints a
// Turrican-2 level-3-1 tilemap onto it. Bare-metal conventions; builds
// against the png2amiga engine + the vscode-amiga-debug toolchain.
//
// This is a first-slice skeleton: Screen::open/flip and the blitter ops
// are still stubbed on the Amiga side, so the binary links but doesn't
// produce visible output yet. The value is proving:
//   - engine headers compile end-to-end with the m68k toolchain
//   - generated asset headers compile (via the same compiler, -nostdlib)
//   - the `TilesetAsset` concept matches the generator's output
//   - CMake wires the ELF → HUNK (elf2hunk) pipeline correctly

#include "pa/chipset.hpp"
#include "pa/screen.hpp"
#include "pa/tilemap.hpp"
#include "pa/asset_traits.hpp"

#include "assets/level_3_1.hpp"

namespace {
// Wrap the asset namespace in a struct so it satisfies the TilesetAsset
// concept. A single namespace isn't a type; this boilerplate is the only
// thing a project ever has to write once per asset.
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
static_assert(pa::TilesetAsset<Level3_1>,
              "generator/concept drift: Level3_1 no longer satisfies TilesetAsset");
} // namespace

int main() {
    pa::Screen screen;
    pa::ScreenOptions opts;
    opts.width  = 320;
    opts.height = 192;    // 12 rows × 16px fits level 3-1 (map_h = 6) twice
    opts.depth  = static_cast<pa::u8>(Level3_1::depth);
    if (!screen.open(opts)) return 1;

    screen.set_palette_ocs({
        assets::level_3_1::palette.data(),
        assets::level_3_1::palette.size()
    });

    // Paint the 40x6-cell tileset once; it fits in the first 6 rows of the
    // 12-row screen. Second half stays black for visual contrast.
    pa::TileLayer<Level3_1>::draw_viewport(screen,
        /*src_cell_x*/ 0, /*src_cell_y*/ 0,
        /*cells_w*/ 20, /*cells_h*/ 6,   // left half of the tilemap
        /*dst_x*/ 0, /*dst_y*/ 0);

    // Hold until the user releases… err, clicks.
    while (!pa::Screen::mouse_left()) {
        screen.flip();
    }

    screen.close();
    return 0;
}
