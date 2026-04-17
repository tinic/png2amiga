// hello_scroll — edge-blit tile scroll (step 2: Y-axis mechanics, corrected).
//
// 2× viewport canvas in both dimensions (40 × 24 tiles = 640 × 384 px,
// ~120 KB/buffer). Each crossing does a SYNCHRONOUS blit of the cells that
// become visible within one frame — ≤80 plane_copys ≈ 3.6 ms per burst,
// which fits inside a PAL frame with room to spare.
//
// Edge-blit formulas (the key fix vs. step 1):
//
//   At crossing N ∈ [0, kViewTiles]:
//     LEFT-branch  (N ≥ 1): canvas cell (N-1) ← world (anchor + kViewTiles + N - 1)
//         This pre-fills the canvas cell above / left of the viewport with
//         content for AFTER the next rebase. Cell is off-top/off-left at
//         the moment of the write, so no tearing.
//     RIGHT-branch (N ≤ kViewTiles-1): canvas cell (kViewTiles + N) ←
//         world (anchor + kViewTiles + N)
//         This writes the CURRENT cycle's content (anchor + kViewTiles + N)
//         that the viewport is about to scroll into on the next frame.
//         Skipped in cycle 0 because the initial fill already matches.
//
//   Step 1 had this formula as (anchor + 2*kViewTiles + N) — next-cycle
//   content pre-filled during the current cycle — which overwrote "current
//   cycle's bottom half" with "next cycle's bottom half" and produced the
//   visible jump at each rebase.
//
// Rebase at bitmap_* ≥ kCyclePx*: anchor += kViewTiles*, bitmap_* -=
// kCyclePx*, last_crossing = -1. Crossings run BEFORE rebase so crossing
// N = kViewTiles gets its left-branch blit for cell (kViewTiles-1).

#include "pa/asset_traits.hpp"
#include "pa/blitter.hpp"
#include "pa/chipset.hpp"
#include "pa/fixed.hpp"
#include "pa/screen.hpp"

#include "assets/level_1_1.hpp"

namespace {

struct Level1_1 {
    static constexpr unsigned tile_w        = assets::level_1_1::tile_w;
    static constexpr unsigned tile_h        = assets::level_1_1::tile_h;
    static constexpr unsigned depth         = assets::level_1_1::depth;
    static constexpr unsigned bytes_per_row = assets::level_1_1::bytes_per_row;
    static constexpr unsigned tile_bytes    = assets::level_1_1::tile_bytes;
    static constexpr unsigned num_tiles     = assets::level_1_1::num_tiles;
    static constexpr unsigned map_w         = assets::level_1_1::map_w;
    static constexpr unsigned map_h         = assets::level_1_1::map_h;
    static constexpr unsigned num_cells     = assets::level_1_1::num_cells;
    static constexpr unsigned palette_size  = assets::level_1_1::palette_size;
    static auto tile_ptr(pa::u16 idx) noexcept {
        return assets::level_1_1::tile_ptr(idx);
    }
    static auto cell_at(unsigned x, unsigned y) noexcept {
        return assets::level_1_1::cell_at(x, y);
    }
    static const pa::u16* palette_ptr() noexcept {
        return assets::level_1_1::palette.data();
    }
};
static_assert(pa::TilesetAsset<Level1_1>);

constexpr unsigned kScreenW      = 320;
constexpr unsigned kScreenH      = 192;
constexpr unsigned kTileW        = Level1_1::tile_w;
constexpr unsigned kTileH        = Level1_1::tile_h;
constexpr unsigned kViewTilesW   = kScreenW / kTileW;        // 20
constexpr unsigned kViewTilesH   = kScreenH / kTileH;        // 12
constexpr unsigned kCanvasTilesW = 2 * kViewTilesW;          // 40
constexpr unsigned kCanvasTilesH = 2 * kViewTilesH;          // 24
constexpr unsigned kCanvasW      = kCanvasTilesW * kTileW;   // 640
constexpr unsigned kCanvasH      = kCanvasTilesH * kTileH;   // 384
constexpr unsigned kCyclePxX     = kViewTilesW * kTileW;     // 320
constexpr unsigned kCyclePxY     = kViewTilesH * kTileH;     // 192

constexpr int world_map_w = static_cast<int>(Level1_1::map_w);
constexpr int world_map_h = static_cast<int>(Level1_1::map_h);

inline pa::u16 world_tile_at(int wx, int wy) {
    if (wx < 0 || wx >= world_map_w) return 0;
    if (wy < 0 || wy >= world_map_h) return 0;
    return Level1_1::cell_at(static_cast<unsigned>(wx),
                             static_cast<unsigned>(wy)).index();
}

// Blit one tile at canvas (col, row) from world (col, row), into BOTH buffers.
// The write is always off-viewport at schedule time, so no tearing.
inline void blit_tile(pa::Screen& screen,
                      int canvas_col, int canvas_row,
                      int world_col, int world_row)
{
    constexpr pa::u16 tbpr = Level1_1::bytes_per_row;
    constexpr pa::u16 tplw = Level1_1::tile_h * (tbpr / 2);
    pa::u16 idx = world_tile_at(world_col, world_row);
    const pa::u16* src = Level1_1::tile_ptr(idx);
    const pa::u16 dst_word_x = static_cast<pa::u16>(canvas_col);
    const pa::u16 dst_y      = static_cast<pa::u16>(canvas_row * kTileH);
    pa::blitter::plane_copy(
        screen.back_buffer(), src, tbpr, tplw,
        dst_word_x, dst_y, /*w_words*/1, kTileH,
        static_cast<pa::u8>(Level1_1::depth));
    pa::blitter::plane_copy(
        screen.front_buffer(), src, tbpr, tplw,
        dst_word_x, dst_y, /*w_words*/1, kTileH,
        static_cast<pa::u8>(Level1_1::depth));
}

// Synchronous edge-blit helpers. Each crossing writes at most one canvas row
// (40 tiles) or one canvas column (kCanvasTilesH tiles), totaling ≤80
// plane_copys ≈ 3.6 ms per burst — comfortably inside a PAL frame — so we
// don't need the earlier queue+drain indirection. Sync also eliminates any
// post-rebase tearing: the row that becomes visible on the next frame is
// already fully written by the time this frame's flip() lands.
inline void blit_canvas_column(pa::Screen& screen, int canvas_col,
                               int world_col, int world_row_top) {
    for (int r = 0; r < static_cast<int>(kCanvasTilesH); ++r)
        blit_tile(screen, canvas_col, r, world_col, world_row_top + r);
}

inline void blit_canvas_row(pa::Screen& screen, int canvas_row,
                            int world_col_left, int world_row) {
    for (int c = 0; c < static_cast<int>(kCanvasTilesW); ++c)
        blit_tile(screen, c, canvas_row, world_col_left + c, world_row);
}

// Full canvas fill at startup.
inline void fill_canvas(pa::Screen& screen, int anchor_col, int anchor_row) {
    for (int r = 0; r < static_cast<int>(kCanvasTilesH); ++r)
        for (int c = 0; c < static_cast<int>(kCanvasTilesW); ++c)
            blit_tile(screen, c, r, anchor_col + c, anchor_row + r);
}

} // namespace

int main() {
    pa::Screen screen;
    pa::ScreenOptions opts;
    opts.width         = kScreenW;
    opts.height        = kScreenH;
    opts.depth         = Level1_1::depth;
    opts.bitmap_width  = kCanvasW;
    opts.bitmap_height = kCanvasH;
    if (!screen.open(opts)) return 1;

    screen.set_palette_ocs({
        assets::level_1_1::palette.data(),
        assets::level_1_1::palette.size()
    });

    constexpr int kStartAnchorCol = 0;
    constexpr int kStartAnchorRow = 0;
    int anchor_col = kStartAnchorCol;
    int anchor_row = kStartAnchorRow;

    fill_canvas(screen, anchor_col, anchor_row);
    pa::blitter::wait_idle();

    pa::fx view_x_fx = pa::fx::from_int(anchor_col * static_cast<int>(kTileW));
    pa::fx view_y_fx = pa::fx::from_int(anchor_row * static_cast<int>(kTileH));
    // Step 2: Y-only scroll. Combined X+Y lands in step 3 (the corrected
    // formula below works for X+Y too — cross-axis coupling is resolved
    // because sync blits use the current anchors at write time).
    const pa::fx vel_x = pa::fx::from_raw(0);
    const pa::fx vel_y = pa::fx::from_raw(256);  // 1.0 px/frame
    int last_crossing_x = -1;
    int last_crossing_y = -1;

    while (!pa::Screen::mouse_left()) {
        view_x_fx += vel_x;
        view_y_fx += vel_y;

        // Clamp at map edges so edge-blits never enqueue rows/cols past
        // map_h / map_w (which would be OOB tile-0 fill and show on screen).
        // Max view = (map - viewTiles) * tile, at which point the last row
        // of valid world tiles sits at the bottom of the viewport.
        const pa::fx max_view_x = pa::fx::from_int(
            (world_map_w - static_cast<int>(kViewTilesW))
            * static_cast<int>(kTileW));
        const pa::fx max_view_y = pa::fx::from_int(
            (world_map_h - static_cast<int>(kViewTilesH))
            * static_cast<int>(kTileH));
        if (view_x_fx >= max_view_x) view_x_fx = max_view_x;
        if (view_y_fx >= max_view_y) view_y_fx = max_view_y;

        int view_x_int = view_x_fx.to_int();
        int view_y_int = view_y_fx.to_int();
        int bitmap_x = view_x_int - anchor_col * static_cast<int>(kTileW);
        int bitmap_y = view_y_int - anchor_row * static_cast<int>(kTileH);

        // ---- X crossings ---- (sync blits, corrected formula)
        int highest_x = bitmap_x / static_cast<int>(kTileW);
        if (highest_x > static_cast<int>(kViewTilesW))
            highest_x = static_cast<int>(kViewTilesW);
        while (last_crossing_x < highest_x) {
            int N = last_crossing_x + 1;
            // Right-branch: write CURRENT-cycle content to canvas col
            // (kViewTilesW + N). In the first cycle (anchor still at start),
            // this is a no-op (initial fill already matches), so skip.
            if (N <= static_cast<int>(kViewTilesW) - 1
                && anchor_col > kStartAnchorCol) {
                int canvas_col = static_cast<int>(kViewTilesW) + N;
                int world_col  = anchor_col + static_cast<int>(kViewTilesW) + N;
                if (world_col < world_map_w)
                    blit_canvas_column(screen, canvas_col, world_col, anchor_row);
            }
            // Left-branch: pre-fill canvas col (N-1) with POST-rebase content.
            if (N >= 1) {
                int canvas_col = N - 1;
                int world_col  = anchor_col + static_cast<int>(kViewTilesW) + (N - 1);
                if (world_col < world_map_w)
                    blit_canvas_column(screen, canvas_col, world_col, anchor_row);
            }
            last_crossing_x = N;
        }
        while (bitmap_x >= static_cast<int>(kCyclePxX)) {
            anchor_col += static_cast<int>(kViewTilesW);
            bitmap_x   -= static_cast<int>(kCyclePxX);
            last_crossing_x = -1;
        }

        // ---- Y crossings ---- (mirror of X with rows)
        int highest_y = bitmap_y / static_cast<int>(kTileH);
        if (highest_y > static_cast<int>(kViewTilesH))
            highest_y = static_cast<int>(kViewTilesH);
        while (last_crossing_y < highest_y) {
            int M = last_crossing_y + 1;
            if (M <= static_cast<int>(kViewTilesH) - 1
                && anchor_row > kStartAnchorRow) {
                int canvas_row = static_cast<int>(kViewTilesH) + M;
                int world_row  = anchor_row + static_cast<int>(kViewTilesH) + M;
                if (world_row < world_map_h)
                    blit_canvas_row(screen, canvas_row, anchor_col, world_row);
            }
            if (M >= 1) {
                int canvas_row = M - 1;
                int world_row  = anchor_row + static_cast<int>(kViewTilesH) + (M - 1);
                if (world_row < world_map_h)
                    blit_canvas_row(screen, canvas_row, anchor_col, world_row);
            }
            last_crossing_y = M;
        }
        while (bitmap_y >= static_cast<int>(kCyclePxY)) {
            anchor_row += static_cast<int>(kViewTilesH);
            bitmap_y   -= static_cast<int>(kCyclePxY);
            last_crossing_y = -1;
        }

        pa::blitter::wait_idle();

        screen.set_view(static_cast<pa::u32>(bitmap_x),
                        static_cast<pa::u32>(bitmap_y));
        screen.flip();
    }

    screen.close();
    return 0;
}
