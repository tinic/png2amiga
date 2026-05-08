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
    int                pop_size      = 128;
    int                generations   = 64;
    int                stale_limit   = 32;
    // EHB mode: candidates are 32-color *base* palettes; the search
    // expands each to a 64-color palette via make_ehb_palette before
    // dithering. Result.palette is the 32-color base.
    bool               ehb_expand    = false;
    // Initial palette seeds (e.g. --best winner, encode_plain_auto's
    // assembled-with-reserves output). When non-empty, candidate 0 is
    // seeded from the first entry verbatim; subsequent candidates are
    // mutations of it. The internal k-means seeds are skipped — the
    // caller has already produced a high-quality starting palette.
    std::vector<Palette> seed_palettes{};
    // Slots whose color is held fixed across all mutations. Sized to
    // max_colors. true = locked. Used for both --lock-index AND
    // --reserve-range: mutations skip these slots and OCS-snap
    // respects them so locked / reserved colours stay put.
    // NOTE: `locked_mask` is purely a mutate/crossover gate; it does
    // NOT exclude slots from the dither candidate set. To exclude
    // slots from being routable by the dither (e.g. reserves and the
    // transparency slot 0), use `dither_exclude_mask` below.
    std::vector<bool>  locked_mask;
    // Slots the dither must not route image pixels to. Strictly a
    // subset of `locked_mask` in the locks-vs-reserves split:
    //   --lock-index    in locked_mask only (dither CAN route here)
    //   --reserve-range in BOTH masks       (dither CANNOT route here)
    //   transparency    slot 0 in BOTH      (transparent → idx 0 only)
    // Empty means "no exclusions" (every slot is dither-routable),
    // which matches the pre-split behaviour for the common no-locks
    // case.
    std::vector<bool>  dither_exclude_mask;
    // Per-pixel transparency mask (w*h). When non-empty, transparent
    // pixels:
    //  - are skipped during dither (their indices forced to 0
    //    post-dither);
    //  - are NOT scored by SSIMULACRA2 (forced to black in both
    //    source and rendered).
    // When set, the caller MUST also lock slot 0 in `locked_mask` so
    // mutations don't move transparent-black off slot 0.
    std::vector<bool>  tmask;
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
