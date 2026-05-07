// Population-based palette search for small palettes (d ≤ 4) — lores OCS.
//
// Background: --best does k-means + multi-restart over (jitter × strength
// × diversity) = 161 trials, ranked by SSIMULACRA2. At d ≤ 4 the palette
// basin space (4-16 colours over the 4096-colour OCS gamut) is sparse
// enough that k-means' local minima trap many candidates — observed:
// at lores d=2 makena, --best gives S2 = -54.88, basically tied with the
// no-best -54.95. The 161 jittered seeds aren't sampling enough basins.
//
// This tool runs an evolutionary palette search:
//   1. Seed a population of N candidates (k-means + perturbations).
//   2. Score each by full dither + render + SSIMULACRA2.
//   3. Each generation: keep top survivors, fill with crossover (half-
//      slots from two parents) and mutation (perturb 1-2 slots in OKLab,
//      snap back to the OCS 12-bit gamut).
//   4. Run for G generations, return the best palette + its score.
//
// Usage:
//   ./palette_search <image> <depth> [pop_size=256] [generations=40]
// Example:
//   ./palette_search examples/makena.jpg 2 256 40
//
// All trials parallelised across cores via pipeline::parallel_for; CPU
// SSIMULACRA2 (~5 ms/call after the uProf-driven optimization batch)
// makes a 256-pop × 40-gen sweep tractable in 1-2 minutes per image.

#include "api.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "pipeline.hpp"
#include "png_io.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "ssimulacra2.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <print>
#include <random>
#include <span>
#include <vector>

using namespace png2amiga;

namespace {

// Snap a Color3f (linear RGB) to the OCS 12-bit gamut (4 bits per
// channel via nibble replication).
Color3f snap_color_ocs(const Color3f& c) {
    auto srgb_l = [](float x) {
        return color_space::linear_to_srgb(x);
    };
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

// Render the dithered image: for each pixel at index idx, output =
// palette[idx]. Used for SSIMULACRA2 scoring.
Image render_dithered(const std::vector<std::uint8_t>& indices,
                      std::span<const Color3f> palette,
                      std::size_t w, std::size_t h) {
    Image out(w, h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto idx = indices[y * w + x];
            out[x, y] = palette[idx];
        }
    }
    return out;
}

// Fitness: full FS dither against the candidate palette, then S2 score
// vs source. Higher is better.
float fitness(const Image& source,
              std::span<const Color3f> candidate,
              const dither::Settings& dith) {
    auto dr = dither::apply(source, candidate, dith);
    auto rendered = render_dithered(dr.indices, candidate,
                                    source.width(), source.height());
    return ssimulacra2::compute(source.pixels(), rendered.pixels(),
                                source.width(), source.height());
}

// Nearest-neighbor fitness: per pixel, find candidate's nearest palette
// entry in OKLab, sum squared distance. NEGATIVE so higher = better.
// This is the operation the GPU compute shader implements (no FS
// dither — fully parallel). If this scoring still finds palettes that
// the encoder's FS-dither path likes, the GPU integration stays simple
// (single nearest-neighbor kernel). If not, we need wavefront FS in
// MSL — much harder.
float fitness_nn(const Image& source,
                 std::span<const Color3f> candidate) {
    std::vector<color_space::OKLab> pal_lab(candidate.size());
    for (std::size_t k = 0; k < candidate.size(); ++k) {
        pal_lab[k] = color_space::linear_to_oklab(candidate[k]);
    }
    double sum_sq = 0.0;
    auto pixels = source.pixels();
    for (auto& px : pixels) {
        auto lab = color_space::linear_to_oklab(px);
        float best_d = std::numeric_limits<float>::infinity();
        for (auto& pl : pal_lab) {
            float dL = lab.L - pl.L;
            float da = lab.a - pl.a;
            float db = lab.b - pl.b;
            float d  = dL * dL + da * da + db * db;
            if (d < best_d) best_d = d;
        }
        sum_sq += static_cast<double>(best_d);
    }
    return -static_cast<float>(sum_sq /
        static_cast<double>(source.pixels().size()));
}

// Mutate: perturb 1-2 slots in OKLab space, snap to gamut. Slot 0 is
// kept = pure black so the mutant remains valid for lock_color0.
void mutate(std::vector<Color3f>& pal, std::mt19937& rng) {
    std::uniform_int_distribution<int> count_dist(1, 2);
    std::uniform_int_distribution<std::size_t> slot_dist(1, pal.size() - 1);
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

// Crossover: half slots from each parent (random per-slot pick).
std::vector<Color3f> crossover(const std::vector<Color3f>& a,
                               const std::vector<Color3f>& b,
                               std::mt19937& rng) {
    std::vector<Color3f> child(a.size());
    if (child.empty()) return child;  // gcc -Wnull-dereference reassurance
    child[0] = Color3f{0, 0, 0};
    std::uniform_int_distribution<int> coin(0, 1);
    for (std::size_t k = 1; k < a.size(); ++k) {
        child[k] = (coin(rng) != 0) ? a[k] : b[k];
    }
    return child;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "usage: {} <image> <depth> [pop=64] "
                             "[gens=40] [out_pal=...] [fitness=fs|nn|msssim]",
                     argv[0]);
        return 2;
    }
    auto img_path = argv[1];

    int depth = std::atoi(argv[2]);
    int pop_size = (argc > 3) ? std::atoi(argv[3]) : 64;
    int generations = (argc > 4) ? std::atoi(argv[4]) : 40;
    std::string out_pal = (argc > 5) ? argv[5] : "/tmp/palette_search_best.txt";
    std::string fit_arg = (argc > 6) ? argv[6] : "fs";
    bool use_nn     = (fit_arg == "nn");
    bool use_msssim = (fit_arg == "msssim");

    if (depth < 1 || depth > 6) {
        std::println(stderr, "depth must be 1-6 (lores OCS)");
        return 2;
    }
    std::size_t max_colors = std::size_t{1} << depth;

    // Load + scale source to lores 320×height-from-aspect.
    auto img = png_io::load(img_path);
    if (!img) {
        std::println(stderr, "load error: {}", img.error().message);
        return 1;
    }
    std::size_t target_w = 320;
    std::size_t target_h = (img->height() * target_w) / img->width();
    Image source(target_w, target_h);
    if (img->width() != target_w || img->height() != target_h) {
        auto scaled = scale::resample(*img, target_w, target_h);
        if (!scaled) {
            std::println(stderr, "scale error: {}", scaled.error().message);
            return 1;
        }
        source = *std::move(scaled);
    } else {
        source = std::move(*img);
    }
    std::println("Source: {}x{} (lores OCS, depth={}, max_colors={}, "
                 "pop={}, gens={})",
                 source.width(), source.height(), depth, max_colors,
                 pop_size, generations);

    // Dither settings: pull the encoder's auto-tuned (strength,
    // error_clamp) for this (mode, depth, chipset, method) bucket.
    // Without this our fitness ranks palettes against a different
    // dither distribution than the encoder will actually use, so the
    // population-search winner could LOSE to k-means+best when the
    // encoder runs.
    api::Options tune_opts;
    tune_opts.mode = "lores";
    tune_opts.depth = depth;
    tune_opts.dither = "floyd-steinberg";
    tune_opts.chipset = "ocs";
    auto tune = api::dither_defaults_for(tune_opts);
    dither::Settings dith;
    dith.method = dither::Method::floyd_steinberg;
    dith.strength = tune.strength;
    dith.error_clamp = tune.error_clamp;
    std::println("Encoder-matched dither: strength={:.3f}, error_clamp={:.3f}",
                 dith.strength, dith.error_clamp);

    // Seed population.
    std::vector<std::vector<Color3f>> population;
    population.reserve(static_cast<std::size_t>(pop_size));
    auto add_pal = [&](std::vector<Color3f> p) {
        if (p.size() > max_colors) p.resize(max_colors);
        while (p.size() < max_colors) p.push_back(Color3f{0, 0, 0});
        snap_palette_ocs(p);
        p[0] = Color3f{0, 0, 0};
        population.push_back(std::move(p));
    };

    auto algo = quantize::Algorithm::ocs_bruteforce;
    int n_kmeans = std::max(4, pop_size / 8);
    std::println("Seeding population...");
    for (int s = 0; s < n_kmeans; ++s) {
        int diversity = 1 + (s % 5);
        auto q = quantize::quantize(source, max_colors, algo, diversity);
        if (q) add_pal(std::move(q->colors));
    }
    if (population.empty()) {
        std::println(stderr, "k-means seed failed");
        return 1;
    }
    auto seed0 = population[0];
    std::mt19937 rng(42);
    while (population.size() < static_cast<std::size_t>(pop_size)) {
        auto p = seed0;
        for (std::size_t k = 1; k < p.size(); ++k) mutate(p, rng);
        add_pal(std::move(p));
    }

    std::println("Fitness mode: {}",
                 use_nn      ? "nearest-neighbor (CPU)"
                 : use_msssim ? "CPU FS + MS-SSIM"
                              : "CPU FS + SSIMULACRA2");

    // Score initial population.
    std::vector<float> scores(population.size(), 0.0f);
    auto score_all = [&]() {
        if (use_msssim) {
            pipeline::parallel_for(population.size(), [&](std::size_t i) {
                auto cand = std::span<const Color3f>(population[i]);
                auto dr = dither::apply(source, cand, dith);
                Image rendered(source.width(), source.height());
                auto px = rendered.pixels();
                for (std::size_t p = 0; p < px.size(); ++p)
                    px[p] = cand[dr.indices[p]];
                scores[i] = pipeline::compute_msssim(source.pixels(),
                                                      rendered.pixels(),
                                                      source.width(),
                                                      source.height());
            });
        } else {
            pipeline::parallel_for(population.size(), [&](std::size_t i) {
                scores[i] = use_nn
                    ? fitness_nn(source, std::span<const Color3f>(population[i]))
                    : fitness(source, std::span<const Color3f>(population[i]),
                              dith);
            });
        }
    };
    auto t0 = std::chrono::steady_clock::now();
    score_all();
    auto t1 = std::chrono::steady_clock::now();
    auto gen0_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    auto best_it = std::max_element(scores.begin(), scores.end());
    std::size_t best_idx = static_cast<std::size_t>(best_it - scores.begin());
    std::println("Gen  0: best S2 = {:.4f} ({:.1f}s, {:.2f} ms/trial)",
                 scores[best_idx], gen0_ms / 1000.0, gen0_ms / pop_size);

    // Evolution loop.
    auto evo_t0 = std::chrono::steady_clock::now();
    float prev_best = scores[best_idx];
    int stale_gens = 0;
    for (int gen = 1; gen <= generations; ++gen) {
        std::vector<std::size_t> idx(population.size());
        std::iota(idx.begin(), idx.end(), std::size_t{0});
        std::sort(idx.begin(), idx.end(),
                  [&](std::size_t a, std::size_t b) {
                      return scores[a] > scores[b];
                  });
        int n_keep = std::max(2, pop_size / 4);
        std::vector<std::vector<Color3f>> new_pop;
        new_pop.reserve(static_cast<std::size_t>(pop_size));
        for (int i = 0; i < n_keep; ++i)
            new_pop.push_back(population[idx[static_cast<std::size_t>(i)]]);
        std::uniform_int_distribution<int> parent_pick(0, n_keep - 1);
        while (new_pop.size() < static_cast<std::size_t>(pop_size)) {
            int role = static_cast<int>(new_pop.size()) % 4;
            if (role < 2) {
                auto& a = new_pop[static_cast<std::size_t>(parent_pick(rng))];
                auto& b = new_pop[static_cast<std::size_t>(parent_pick(rng))];
                auto child = crossover(a, b, rng);
                mutate(child, rng);
                new_pop.push_back(std::move(child));
            } else {
                auto child =
                    new_pop[static_cast<std::size_t>(parent_pick(rng))];
                for (int m = 0; m < 3; ++m) mutate(child, rng);
                new_pop.push_back(std::move(child));
            }
        }
        population = std::move(new_pop);
        scores.assign(population.size(), 0.0f);
        score_all();
        auto cur_best_it = std::max_element(scores.begin(), scores.end());
        std::size_t cur_best =
            static_cast<std::size_t>(cur_best_it - scores.begin());
        std::println("Gen {:3}: best S2 = {:.4f}", gen, scores[cur_best]);
        if (scores[cur_best] - prev_best < 0.001f) {
            stale_gens++;
            if (stale_gens >= 10) {
                std::println("(no improvement in 10 gens — stopping early)");
                break;
            }
        } else {
            stale_gens = 0;
            prev_best = scores[cur_best];
        }
    }
    auto evo_t1 = std::chrono::steady_clock::now();
    auto total_s = std::chrono::duration<double>(evo_t1 - evo_t0).count();

    auto final_it = std::max_element(scores.begin(), scores.end());
    std::size_t final_best =
        static_cast<std::size_t>(final_it - scores.begin());

    std::println("\n=== Final best palette ===");
    std::println("S2 = {:.4f}, total search time = {:.1f} s",
                 scores[final_best], total_s);
    std::println("Palette ({} entries):", population[final_best].size());
    std::ofstream pal_f(out_pal);
    for (std::size_t k = 0; k < population[final_best].size(); ++k) {
        Color3f srgb_lin = population[final_best][k];
        float r = std::clamp(color_space::linear_to_srgb(srgb_lin.r), 0.0f, 1.0f);
        float g = std::clamp(color_space::linear_to_srgb(srgb_lin.g), 0.0f, 1.0f);
        float b = std::clamp(color_space::linear_to_srgb(srgb_lin.b), 0.0f, 1.0f);
        auto ri = static_cast<int>(std::round(r * 255.0f));
        auto gi = static_cast<int>(std::round(g * 255.0f));
        auto bi = static_cast<int>(std::round(b * 255.0f));
        std::println("  {:3}: #{:02x}{:02x}{:02x}", k, ri, gi, bi);
        std::println(pal_f, "{:02x}{:02x}{:02x}", ri, gi, bi);
    }
    std::println("\nPalette written to: {}", out_pal);
    std::println("Verify via: png2amiga --mode lores --depth {} --palette {} "
                 "<image> /tmp/out.png", depth, out_pal);
    return 0;
}
