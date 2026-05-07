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

void mutate(std::vector<Color3f>& pal, std::mt19937& rng, bool lock_color0) {
    if (pal.size() <= (lock_color0 ? 1u : 0u)) return;
    std::uniform_int_distribution<int> count_dist(1, 2);
    std::size_t lo = lock_color0 ? 1u : 0u;
    std::uniform_int_distribution<std::size_t> slot_dist(lo, pal.size() - 1);
    std::uniform_real_distribution<float> dL(-0.08f, 0.08f);
    std::uniform_real_distribution<float> dab(-0.04f, 0.04f);
    int n_mutations = count_dist(rng);
    for (int i = 0; i < n_mutations; ++i) {
        std::size_t k = slot_dist(rng);
        auto lab = color_space::linear_to_oklab(pal[k]);
        lab.L = std::clamp(lab.L + dL(rng), 0.0f, 1.0f);
        lab.a = std::clamp(lab.a + dab(rng), -0.4f, 0.4f);
        lab.b = std::clamp(lab.b + dab(rng), -0.4f, 0.4f);
        pal[k] = snap_color_ocs(color_space::oklab_to_linear(lab));
    }
}

std::vector<Color3f> crossover(const std::vector<Color3f>& a,
                               const std::vector<Color3f>& b,
                               std::mt19937& rng,
                               bool lock_color0) {
    std::vector<Color3f> child(a.size());
    if (child.empty()) return child;
    if (lock_color0) child[0] = Color3f{0, 0, 0};
    std::uniform_int_distribution<int> coin(0, 1);
    std::size_t start = lock_color0 ? 1u : 0u;
    for (std::size_t k = start; k < a.size(); ++k) {
        child[k] = (coin(rng) != 0) ? a[k] : b[k];
    }
    return child;
}

// `palette` here is the FULL effective palette (e.g. 64 entries for
// EHB after expansion); the candidate's 32 base colours are expanded
// upstream by run_population_search when ehb_expand is set.
float cpu_fitness(const Image& source,
                  std::span<const Color3f> palette,
                  const dither::Settings& dith) {
    auto dr = dither::apply(source, palette, dith);
    Image rendered(source.width(), source.height());
    auto px = rendered.pixels();
    for (std::size_t i = 0; i < px.size(); ++i) {
        px[i] = palette[dr.indices[i]];
    }
    return ssimulacra2::compute(source.pixels(), rendered.pixels(),
                                 source.width(), source.height());
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

    auto add_pal = [&](std::vector<Color3f> p,
                       std::vector<std::vector<Color3f>>& pop) {
        if (p.size() > max_colors) p.resize(max_colors);
        while (p.size() < max_colors) p.push_back(Color3f{0, 0, 0});
        snap_palette_ocs(p);
        if (lock_color0) p[0] = Color3f{0, 0, 0};
        pop.push_back(std::move(p));
    };

    std::vector<std::vector<Color3f>> population;
    population.reserve(static_cast<std::size_t>(opts.pop_size));

    // Caller-provided seed palettes (e.g. --best winner).
    for (const auto& sp : opts.seed_palettes) add_pal(sp.colors, population);

    // K-means seeds across diversity values.
    int n_kmeans = std::max(4, opts.pop_size / 8);
    for (int s = 0; s < n_kmeans; ++s) {
        int diversity = 1 + (s % 5);
        auto q = quantize::quantize(source, max_colors,
                                     quantize::Algorithm::ocs_bruteforce,
                                     diversity);
        if (q) add_pal(std::move(q->colors), population);
    }
    if (population.empty()) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                      "k-means seeding failed"}};
    }

    // Fill remainder with mutated copies of the first seed.
    auto seed0 = population[0];
    std::mt19937 rng(0xa5a5);
    while (population.size() < static_cast<std::size_t>(opts.pop_size)) {
        auto p = seed0;
        for (std::size_t k = (lock_color0 ? 1u : 0u); k < p.size(); ++k)
            mutate(p, rng, lock_color0);
        add_pal(std::move(p), population);
    }

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
                    std::span<const Color3f>(full.colors), dith);
            } else {
                scores[i] = cpu_fitness(source,
                    std::span<const Color3f>(population[i]), dith);
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
                auto child = crossover(a, b, rng, lock_color0);
                snap_palette_ocs(child);
                mutate(child, rng, lock_color0);
                new_pop.push_back(std::move(child));
            } else {
                auto child =
                    new_pop[static_cast<std::size_t>(parent_pick(rng))];
                for (int m = 0; m < 3; ++m) mutate(child, rng, lock_color0);
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
    auto& pal_vec = population[winner];
    Palette ehb_full;
    std::span<const Color3f> dither_pal = pal_vec;
    if (opts.ehb_expand) {
        ehb_full   = palette::make_ehb_palette(pal_vec);
        dither_pal = std::span<const Color3f>(ehb_full.colors);
    }
    auto dr = dither::apply(source, dither_pal, dith);
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
