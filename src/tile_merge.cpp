#include "tile_merge.hpp"
#include "pipeline.hpp"

#include <algorithm>

namespace png2amiga::tile_merge {

std::size_t merge_to_budget(std::vector<std::vector<std::size_t>>& slot_cells,
                            std::vector<bool>& alive,
                            std::size_t budget,
                            const PairFn& score,
                            const MergeFn& on_merge,
                            const ReportFn& report) {
    std::vector<std::size_t> live;
    live.reserve(slot_cells.size());
    for (std::size_t i = 0; i < slot_cells.size(); ++i)
        if (alive[i]) live.push_back(i);
    if (live.size() <= budget) return 0;
    const auto merges_needed = live.size() - budget;

    struct Pair {
        std::size_t a, b;
        float distance;
        std::uint8_t aux;
    };
    const std::size_t Na = live.size();
    std::vector<Pair> pairs(Na * (Na - 1) / 2);
    if (report) report(0.0f, "merging tiles");
    // O(n²) pair scoring dominates; chunk by outer row with a pre-indexed
    // output slot (triangular load, parallel_for work-steals).
    pipeline::parallel_for(Na, [&](std::size_t i) {
        const std::size_t base = i * (2 * Na - i - 1) / 2;
        for (std::size_t j = i + 1; j < Na; ++j) {
            auto s = score(live[i], live[j]);
            pairs[base + (j - i - 1)] = {live[i], live[j], s.distance, s.aux};
        }
    });
    if (report) {
        report(0.95f, "sorting pairs");
        report(0.96f, "sorting pairs");
    }
    std::ranges::sort(pairs, {}, &Pair::distance);
    if (report) report(0.98f, "merging tiles");

    std::size_t merges_done = 0;
    for (auto& p : pairs) {
        if (merges_done >= merges_needed) break;
        if (!alive[p.a] || !alive[p.b]) continue;
        auto keep = p.a, discard = p.b;
        if (slot_cells[keep].size() < slot_cells[discard].size()) std::swap(keep, discard);
        if (on_merge) on_merge(keep, discard, p.aux);
        auto& kc = slot_cells[keep];
        kc.insert(kc.end(), slot_cells[discard].begin(), slot_cells[discard].end());
        slot_cells[discard].clear();
        alive[discard] = false;
        ++merges_done;
    }
    return merges_done;
}

}  // namespace png2amiga::tile_merge
