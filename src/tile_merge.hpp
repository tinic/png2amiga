#pragma once

// Greedy tile-budget merger shared by the tile-coded modes (SNES Mode 7,
// Master System / Game Gear). Algorithm ported from png2c64's charset
// merger (charset.cpp:merge_to_256): score every pair of alive slots,
// sort ascending, then collapse the closest pairs until the budget fits.
// The slot with more cells survives each merge so popular tiles are not
// displaced by rare ones.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace png2amiga::tile_merge {

struct PairScore {
    float distance = 0.0f;
    std::uint8_t aux = 0;  // caller-defined (e.g. the flip that matched)
};

// Scores slots `a` and `b` (indices into slot_cells). Must be symmetric
// and thread-safe: pairs are scored in parallel.
using PairFn = std::function<PairScore(std::size_t a, std::size_t b)>;
// Called once per merge, before `discard`'s cells move to `keep`.
using MergeFn = std::function<void(std::size_t keep, std::size_t discard, std::uint8_t aux)>;
using ReportFn = std::function<void(float, std::string_view)>;

// slot_cells[s] lists the cells using slot s; alive[s] marks live slots.
// Merges until at most `budget` slots are alive. Discarded slots end up
// with alive[s] = false and an empty cell list. Returns the merge count.
std::size_t merge_to_budget(std::vector<std::vector<std::size_t>>& slot_cells,
                            std::vector<bool>& alive,
                            std::size_t budget,
                            const PairFn& score,
                            const MergeFn& on_merge = {},
                            const ReportFn& report = {});

}  // namespace png2amiga::tile_merge
