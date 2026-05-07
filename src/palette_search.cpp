// Population palette search — replaces best_sweep at lores OCS d≤5.
// Evolutionary 64-pop × 40-gen with crossover + mutation, ranked by
// SSIMULACRA2. Gets a +1..+7 S2 bump over the previous k-means + 161
// jittered restarts at d≤5 (search basins are sparse in OCS gamut).

#include "palette_search.hpp"

#include "color_space.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "pipeline.hpp"
#include "quantize.hpp"
#include "ssimulacra2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <span>

namespace png2amiga::palette_search {

namespace {

Color3f snap_color_ocs(const Color3f& c) {
    auto srgb_l = [](float x) { return color_space::linear_to_srgb(x); };
    float r = std::clamp(srgb_l(c.r), 0.0f, 1.0f);
    float g = std::clamp(srgb_l(c.g), 0.0f, 1.0f);
    float b = std::clamp(srgb_l(c.b), 0.0f, 1.0f);
    auto r4 = std::clamp(static_cast<int>(std::round(r * 15.0f)), 0, 15);
    auto g4 = std::clamp(static_cast<int>(std::round(g * 15.0f)), 0, 15);
    auto b4 = std::clamp(static_cast<int>(std::round(b * 15.0f)), 0, 15);
    auto r8 = (r4 << 4) | r4;
    auto g8 = (g4 << 4) | g4;
    auto b8 = (b4 << 4) | b4;
    return color_space::srgb_to_linear(Color3f{
        static_cast<float>(r8) / 255.0f,
        static_cast<float>(g8) / 255.0f,
        static_cast<float>(b8) / 255.0f});
}

void snap_palette_ocs(std::vector<Color3f>& pal) {
    for (auto& c : pal) c = snap_color_ocs(c);
}

// Mutate non-locked slots in `pal`. `locked` (if non-empty) flags
// slots that must not change; lock_color0 implicitly locks slot 0
// when locked is empty. Slots flagged as locked stay at their input
// colour after this call.
void mutate(std::vector<Color3f>& pal, std::mt19937& rng,
            bool lock_color0,
            const std::vector<bool>& locked = {}) {
    if (pal.empty()) return;
    auto is_locked = [&](std::size_t k) {
        if (!locked.empty() && k < locked.size() && locked[k]) return true;
        if (lock_color0 && k == 0) return true;
        return false;
    };
    // Build the list of mutable slots once — significantly faster than
    // rejection-sampling when many slots are locked (e.g. d=4 with 4
    // reserves leaves only 12 mutable slots).
    std::vector<std::size_t> free_slots;
    free_slots.reserve(pal.size());
    for (std::size_t k = 0; k < pal.size(); ++k)
        if (!is_locked(k)) free_slots.push_back(k);
    if (free_slots.empty()) return;
    std::uniform_int_distribution<int> count_dist(1, 2);
    std::uniform_int_distribution<std::size_t> slot_dist(0, free_slots.size() - 1);
    std::uniform_real_distribution<float> dL(-0.08f, 0.08f);
    std::uniform_real_distribution<float> dab(-0.04f, 0.04f);
    int n_mutations = count_dist(rng);
    for (int i = 0; i < n_mutations; ++i) {
        std::size_t k = free_slots[slot_dist(rng)];
        auto lab = color_space::linear_to_oklab(pal[k]);
        lab.L = std::clamp(lab.L + dL(rng), 0.0f, 1.0f);
        lab.a = std::clamp(lab.a + dab(rng), -0.4f, 0.4f);
        lab.b = std::clamp(lab.b + dab(rng), -0.4f, 0.4f);
        pal[k] = snap_color_ocs(color_space::oklab_to_linear(lab));
    }
}

// Crossover that preserves locked slots from parent A. lock_color0
// implicitly forces slot 0 to black (matches encode_plain_auto's
// behaviour when transparent or non-Atari).
std::vector<Color3f> crossover(const std::vector<Color3f>& a,
                               const std::vector<Color3f>& b,
                               std::mt19937& rng,
                               bool lock_color0,
                               const std::vector<bool>& locked = {}) {
    std::vector<Color3f> child(a.size());
    if (child.empty()) return child;
    if (lock_color0) child[0] = Color3f{0, 0, 0};
    std::uniform_int_distribution<int> coin(0, 1);
    for (std::size_t k = 0; k < a.size(); ++k) {
        bool is_locked = (!locked.empty() && k < locked.size() && locked[k])
                         || (lock_color0 && k == 0);
        if (is_locked) {
            // Use parent A's value (caller seeds A's locked slots
            // with the user-supplied reserve colour; B may have
            // different values from earlier mutations on a different
            // path, but we standardise on A so the output is stable).
            child[k] = a[k];
        } else {
            child[k] = (coin(rng) != 0) ? a[k] : b[k];
        }
    }
    return child;
}

// Fitness with optional reserves + transparency support. When
// `locked_mask` flags slots that the dither pass must not pick (the
// reserve colours encode_plain_auto excludes from the candidate
// pool), we build a sub-palette of free slots, dither against it,
// then remap indices. When `tmask` is non-empty, transparent pixels
// get index 0 post-dither and contribute neither to the dither
// candidate set nor to the SSIMULACRA2 score (rendered + source
// both forced to black at those pixels — same shape as the
// upstream encode_plain_auto).
float cpu_fitness(const Image& source,
                  std::span<const Color3f> palette,
                  const dither::Settings& dith,
                  const std::vector<bool>& locked_mask = {},
                  const std::vector<bool>& tmask = {}) {
    const std::size_t W = source.width();
    const std::size_t H = source.height();
    const bool has_reserves =
        std::any_of(locked_mask.begin(), locked_mask.end(),
                    [](bool b) { return b; });
    const bool has_trans = !tmask.empty();

    // Pop search calls this 2560×/run; W*H ≈ 64K pixels here means
    // 768 KB rendered + smaller cand_pal/src_masked allocations per
    // call → ~22% ucrtbase on AMD uProf. Reuse buffers via thread_local
    // scratch (each parallel_for worker has its own TLS).
    thread_local Image rendered_tls(0, 0);
    thread_local Image src_masked_tls(0, 0);
    thread_local std::vector<Color3f>      cand_pal_tls;
    thread_local std::vector<std::uint8_t> cand_to_full_tls;
    if (rendered_tls.width() != W || rendered_tls.height() != H) {
        rendered_tls = Image(W, H);
    }

    dither::DitherResult dr;
    if (has_reserves || has_trans) {
        // Build candidate sub-palette excluding locked + (when
        // transparent) slot 0.
        cand_pal_tls.clear();
        cand_to_full_tls.clear();
        cand_pal_tls.reserve(palette.size());
        cand_to_full_tls.reserve(palette.size());
        for (std::size_t i = 0; i < palette.size(); ++i) {
            if (has_trans && i == 0) continue;
            if (i < locked_mask.size() && locked_mask[i]) continue;
            cand_pal_tls.push_back(palette[i]);
            cand_to_full_tls.push_back(static_cast<std::uint8_t>(i));
        }
        if (cand_pal_tls.empty()) cand_pal_tls.push_back(Color3f{0, 0, 0});
        dr = dither::apply(source,
            std::span<const Color3f>(cand_pal_tls.data(), cand_pal_tls.size()),
            dith);
        for (auto& idx : dr.indices) idx = cand_to_full_tls[idx];
        if (has_trans) {
            for (std::size_t i = 0;
                 i < tmask.size() && i < dr.indices.size(); ++i)
                if (tmask[i]) dr.indices[i] = 0;
        }
    } else {
        dr = dither::apply(source, palette, dith);
    }

    auto px = rendered_tls.pixels();
    for (std::size_t i = 0; i < px.size(); ++i) {
        px[i] = palette[dr.indices[i]];
    }
    if (has_trans) {
        // Score against the source with transparent pixels masked to
        // black on both sides, so SSIMULACRA2 doesn't penalise the
        // (ignored) transparent regions.
        if (src_masked_tls.width() != W || src_masked_tls.height() != H) {
            src_masked_tls = Image(W, H);
        }
        auto src_px = src_masked_tls.pixels();
        auto orig_px = source.pixels();
        for (std::size_t i = 0; i < src_px.size(); ++i) {
            if (i < tmask.size() && tmask[i]) {
                src_px[i] = Color3f{0, 0, 0};
                px[i] = Color3f{0, 0, 0};
            } else {
                src_px[i] = orig_px[i];
            }
        }
        return ssimulacra2::compute(src_masked_tls.pixels(),
                                     rendered_tls.pixels(), W, H);
    }
    return ssimulacra2::compute(source.pixels(), rendered_tls.pixels(),
                                 W, H);
}

} // namespace

Result<PopSearchResult> run_population_search(
    const Image&            source,
    int                     depth,
    std::size_t             max_colors,
    amiga::Mode             /*mode*/,
    amiga::Chipset          chipset,
    const dither::Settings& dith,
    bool                    lock_color0,
    const PopSearchOptions& opts) noexcept
{
    if (source.width() == 0 || source.height() == 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                      "empty image"}};
    }
    if (chipset != amiga::Chipset::ocs) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
                                      "pop search is OCS-only for now"}};
    }
    if (depth < 1 || depth > 5) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
                                      "pop search applies to d ∈ [1,5]"}};
    }

    // Capture lock + transparency context. When the caller supplies
    // a fully-assembled seed palette (encode_plain_auto's output —
    // already has reserves at their fixed slots), we use it verbatim
    // and only mutate the non-locked slots. When no seed is supplied,
    // we fall back to the internal k-means seeding which doesn't
    // know about reserves; gate that case on the locked_mask being
    // empty.
    const auto& locked_mask = opts.locked_mask;
    const bool  has_locked  =
        std::any_of(locked_mask.begin(), locked_mask.end(),
                    [](bool b) { return b; });

    auto enforce_locks = [&](std::vector<Color3f>& p,
                              const std::vector<Color3f>* lock_src) {
        // Restore locked-slot colours from the seed (or an explicit
        // template). Called after any mutation that might have
        // touched a locked slot via the rng.
        for (std::size_t k = 0; k < p.size() && k < locked_mask.size(); ++k) {
            if (locked_mask[k] && lock_src && k < lock_src->size())
                p[k] = (*lock_src)[k];
        }
    };

    auto add_pal = [&](std::vector<Color3f> p,
                       std::vector<std::vector<Color3f>>& pop) {
        if (p.size() > max_colors) p.resize(max_colors);
        while (p.size() < max_colors) p.push_back(Color3f{0, 0, 0});
        snap_palette_ocs(p);
        if (lock_color0) p[0] = Color3f{0, 0, 0};
        // The locked-slot enforcement happens via enforce_locks at
        // the call site, since we need a `lock_src` reference here.
        pop.push_back(std::move(p));
    };

    std::vector<std::vector<Color3f>> population;
    population.reserve(static_cast<std::size_t>(opts.pop_size));

    // Caller-provided seed palettes (e.g. --best winner). When the
    // caller threaded reserves through, the first seed already has
    // them slotted in.
    for (const auto& sp : opts.seed_palettes) add_pal(sp.colors, population);

    // Internal k-means seeding. SKIPPED when the caller has reserves
    // — the k-means path doesn't know about reserve slots and would
    // produce candidates that violate them. Caller-supplied seed
    // (the only entry above) becomes the basin we mutate around.
    if (!has_locked) {
        int n_kmeans = std::max(4, opts.pop_size / 8);
        for (int s = 0; s < n_kmeans; ++s) {
            int diversity = 1 + (s % 5);
            auto q = quantize::quantize(source, max_colors,
                                         quantize::Algorithm::ocs_bruteforce,
                                         diversity);
            if (q) add_pal(std::move(q->colors), population);
        }
    }
    if (population.empty()) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                      "no seeds available — caller must "
                                      "supply seed_palettes when reserves "
                                      "are present"}};
    }

    // Fill remainder with mutated copies of the first seed. Each
    // mutation respects locked_mask (skips reserved slots) and
    // reapplies the seed's locked colours via enforce_locks in case
    // of any drift.
    auto seed0 = population[0];
    std::mt19937 rng(0xa5a5);
    while (population.size() < static_cast<std::size_t>(opts.pop_size)) {
        auto p = seed0;
        for (std::size_t k = 0; k < p.size(); ++k)
            mutate(p, rng, lock_color0, locked_mask);
        enforce_locks(p, &seed0);
        add_pal(std::move(p), population);
    }
    // The first candidate is the unmutated seed itself — pop[0] is
    // population[0] verbatim from above. Make sure its locked slots
    // are also enforced (they should be, since seed0 came from the
    // caller; this is just a safety belt against the snap step).
    enforce_locks(population[0], &seed0);

    std::vector<float> scores(population.size(), 0.0f);
    // Score-cache mask: when a candidate carries over unchanged from
    // the previous generation, its score is already known and stays
    // in `scores[i]` — we just skip the re-evaluation. Reduces per-gen
    // S2 calls by `n_keep` (e.g. 16/64 = 25% on the default pop=64).
    std::vector<bool> needs_score(population.size(), true);
    auto score_all = [&]() {
        pipeline::parallel_for(population.size(), [&](std::size_t i) {
            if (!needs_score[i]) return;
            if (opts.ehb_expand) {
                // Expand 32 base → 64-entry EHB palette in-place
                // (cheap: half_brite is a per-channel ×0.5 in sRGB).
                auto full = palette::make_ehb_palette(population[i]);
                scores[i] = cpu_fitness(source,
                    std::span<const Color3f>(full.colors), dith,
                    opts.locked_mask, opts.tmask);
            } else {
                scores[i] = cpu_fitness(source,
                    std::span<const Color3f>(population[i]), dith,
                    opts.locked_mask, opts.tmask);
            }
        });
    };

    auto report = [&](int gen, float best) {
        if (!opts.on_progress) return;
        float frac = static_cast<float>(gen) /
                     static_cast<float>(opts.generations);
        char label[64];
        std::snprintf(label, sizeof(label),
                      "pop search gen %d/%d  best=%.2f",
                      gen, opts.generations,
                      static_cast<double>(best));
        opts.on_progress(frac, label);
    };

    score_all();

    float prev_best = *std::max_element(scores.begin(), scores.end());
    int   stale_gens = 0;
    report(0, prev_best);

    for (int gen = 1; gen <= opts.generations; ++gen) {
        std::vector<std::size_t> idx(population.size());
        std::iota(idx.begin(), idx.end(), std::size_t{0});
        std::sort(idx.begin(), idx.end(),
                  [&](std::size_t a, std::size_t b) {
                      return scores[a] > scores[b];
                  });
        int n_keep = std::max(2, opts.pop_size / 4);
        std::vector<std::vector<Color3f>> new_pop;
        std::vector<float>                new_scores;
        new_pop.reserve(static_cast<std::size_t>(opts.pop_size));
        new_scores.reserve(static_cast<std::size_t>(opts.pop_size));
        // Top n_keep survive verbatim — carry their scores forward.
        for (int i = 0; i < n_keep; ++i) {
            std::size_t k = idx[static_cast<std::size_t>(i)];
            new_pop.push_back(population[k]);
            new_scores.push_back(scores[k]);
        }

        std::uniform_int_distribution<int> parent_pick(0, n_keep - 1);
        while (new_pop.size() < static_cast<std::size_t>(opts.pop_size)) {
            int role = static_cast<int>(new_pop.size()) % 4;
            if (role < 2) {
                auto& a = new_pop[static_cast<std::size_t>(parent_pick(rng))];
                auto& b = new_pop[static_cast<std::size_t>(parent_pick(rng))];
                auto child = crossover(a, b, rng, lock_color0, locked_mask);
                snap_palette_ocs(child);
                mutate(child, rng, lock_color0, locked_mask);
                enforce_locks(child, &seed0);
                new_pop.push_back(std::move(child));
            } else {
                auto child =
                    new_pop[static_cast<std::size_t>(parent_pick(rng))];
                for (int m = 0; m < 3; ++m)
                    mutate(child, rng, lock_color0, locked_mask);
                enforce_locks(child, &seed0);
                new_pop.push_back(std::move(child));
            }
            new_scores.push_back(0.0f);  // placeholder, will be filled
        }
        population = std::move(new_pop);
        scores     = std::move(new_scores);
        needs_score.assign(population.size(), true);
        for (int i = 0; i < n_keep; ++i) needs_score[static_cast<std::size_t>(i)] = false;
        score_all();

        float cur_best = *std::max_element(scores.begin(), scores.end());
        report(gen, cur_best);
        if (cur_best - prev_best < 0.001f) {
            if (++stale_gens >= opts.stale_limit) break;
        } else {
            stale_gens = 0;
            prev_best  = cur_best;
        }
    }

    auto winner_it = std::max_element(scores.begin(), scores.end());
    std::size_t winner = static_cast<std::size_t>(
        winner_it - scores.begin());

    // Final dither via the canonical CPU path so the result is
    // exactly what the encoder will produce. For EHB the indices
    // address into the 64-entry expanded palette; the returned
    // PopSearchResult.palette holds the BASE 32 (caller will re-
    // expand for storage / IFF CMAP / etc.).
    //
    // When reserves or transparency are present, build the dither
    // candidate sub-palette (excluding locked + transparent-zero
    // slots), dither against it, and remap indices to the full
    // palette — same pattern as encode_plain_auto.
    auto& pal_vec = population[winner];
    Palette ehb_full;
    std::span<const Color3f> dither_pal = pal_vec;
    if (opts.ehb_expand) {
        ehb_full   = palette::make_ehb_palette(pal_vec);
        dither_pal = std::span<const Color3f>(ehb_full.colors);
    }
    const bool has_reserves =
        std::any_of(opts.locked_mask.begin(), opts.locked_mask.end(),
                    [](bool b) { return b; });
    const bool has_trans = !opts.tmask.empty();

    dither::DitherResult dr;
    if (has_reserves || has_trans) {
        std::vector<Color3f>      cand_pal;
        std::vector<std::uint8_t> cand_to_full;
        cand_pal.reserve(dither_pal.size());
        cand_to_full.reserve(dither_pal.size());
        for (std::size_t i = 0; i < dither_pal.size(); ++i) {
            if (has_trans && i == 0) continue;
            if (i < opts.locked_mask.size() && opts.locked_mask[i]) continue;
            cand_pal.push_back(dither_pal[i]);
            cand_to_full.push_back(static_cast<std::uint8_t>(i));
        }
        if (cand_pal.empty()) cand_pal.push_back(Color3f{0, 0, 0});
        dr = dither::apply(source,
            std::span<const Color3f>(cand_pal.data(), cand_pal.size()), dith);
        for (auto& idx : dr.indices) idx = cand_to_full[idx];
        if (has_trans) {
            for (std::size_t i = 0;
                 i < opts.tmask.size() && i < dr.indices.size(); ++i)
                if (opts.tmask[i]) dr.indices[i] = 0;
        }
    } else {
        dr = dither::apply(source, dither_pal, dith);
    }
    Image rendered(source.width(), source.height());
    {
        auto px = rendered.pixels();
        for (std::size_t i = 0; i < px.size(); ++i) {
            px[i] = dither_pal[dr.indices[i]];
        }
    }

    PopSearchResult out;
    out.palette.name   = opts.ehb_expand ? "ehb-pop-base" : "pop";
    out.palette.colors = std::move(pal_vec);
    out.indices        = std::move(dr.indices);
    out.rendered       = std::move(rendered);
    out.total_error    = dr.total_error;
    return out;
}

} // namespace png2amiga::palette_search
