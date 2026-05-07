#pragma once

// Evolutionary population palette search. Replaces best_sweep at
// lores OCS d≤5 — k-means + 161 jittered restarts gets trapped in
// shallow basins; population search (crossover + mutation, ranked by
// SSIMULACRA2) escapes them and gets +1..+7 S2 over old --best.

#include "amiga.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "types.hpp"

#include <functional>
#include <string_view>
#include <vector>

namespace png2amiga::palette_search {

struct PopSearchResult {
    Palette                       palette;
    std::vector<std::uint8_t>     indices;
    Image                         rendered;
    float                         total_error{};   // dither residual
};

struct PopSearchOptions {
    int                pop_size      = 64;
    int                generations   = 40;
    int                stale_limit   = 10;
    // EHB mode: candidates are 32-color *base* palettes; the search
    // expands each to a 64-color palette via make_ehb_palette before
    // dithering. Result.palette is the 32-color base.
    bool               ehb_expand    = false;
    std::vector<Palette> seed_palettes{};  // extra seeds (e.g. --best winner)
    // Progress callback. Called once per generation with
    // (progress 0..1, "pop search gen N/G  best=…"). Same shape as
    // api::Options::on_progress so caller can forward directly.
    std::function<void(float, std::string_view)> on_progress;
};

// Run population search at lores/hires d≤4 OCS. Returns the best
// palette + its dithered output. Caller compares against the existing
// --best winner and keeps whichever scores better.
//
// Errors: invalid_dimensions on empty image; unsupported_mode if
// chipset != ocs or depth out of [1,4].
[[nodiscard]] Result<PopSearchResult> run_population_search(
    const Image&            source,
    int                     depth,
    std::size_t             max_colors,
    amiga::Mode             mode,
    amiga::Chipset          chipset,
    const dither::Settings& dith,
    bool                    lock_color0,
    const PopSearchOptions& opts = {}) noexcept;

} // namespace png2amiga::palette_search
