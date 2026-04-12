#include "quantize.hpp"
#include "color_space.hpp"
#include "palette.hpp"

namespace {
using OKLab = png2amiga::color_space::OKLab;

inline float oklab_dist_sq(OKLab a, OKLab b) noexcept {
    float dL = (a.L - b.L) * png2amiga::color_space::WEIGHT_L;
    float da = (a.a - b.a) * png2amiga::color_space::WEIGHT_A;
    float db = (a.b - b.b) * png2amiga::color_space::WEIGHT_B;
    return dL * dL + da * da + db * db;
}
} // namespace

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <span>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#include <vector>

namespace png2amiga::quantize {

namespace {

// ===========================================================================
// Median-cut algorithm
//
// 1. Start with all pixels in one "box" in linear RGB space.
// 2. Find the box with the largest range along any channel.
// 3. Split that box at the median of its widest channel.
// 4. Repeat until we have max_colors boxes.
// 5. The centroid of each box is one palette color.
//
// We work in linear RGB because the palette values are stored in linear
// RGB and the color space conversion happens at the boundary. The split
// decisions could alternatively use OKLab for more perceptual uniformity,
// but linear RGB gives good results and is much faster (no cbrt per pixel).
// ===========================================================================

// (ColorBox / box_centroid removed — Wu's quantization replaces median-cut)

// ===========================================================================
// OCS brute-force quantizer
//
// Since OCS only has 4096 possible colors (12-bit, 4 bits/channel),
// we can find the optimal N-color palette by:
//
// 1. Build a histogram: map every pixel to its nearest OCS color.
// 2. Weighted median-cut on the histogram to get initial N colors.
// 3. K-means refinement: for each cluster, brute-force search all 4096
//    OCS colors to find the one minimizing total perceptual error.
//    Each cluster is refined independently → parallelizable.
//
// This produces genuinely optimal OCS palettes, avoiding the lossy
// "median-cut in continuous space, then snap to 12-bit" approach.
// ===========================================================================

// Precomputed OCS color tables (computed once, shared across calls)
struct OcsLut {
    std::array<Color3f, 4096> linear;
    std::array<color_space::OKLab, 4096> oklab;

    OcsLut() {
        for (std::uint16_t i = 0; i < 4096; ++i) {
            linear[i] = palette::ocs_to_linear(i);
            oklab[i] = color_space::linear_to_oklab(linear[i]);
        }
    }
};

static const OcsLut& ocs_lut() {
    static const OcsLut lut;
    return lut;
}

// Weighted color entry for histogram-based quantization
struct WeightedOcs {
    std::uint16_t ocs_index;    // 0-4095
    std::uint32_t weight;       // pixel count
};

Palette ocs_bruteforce_quantize(std::span<const Color3f> pixels,
                                std::size_t max_colors,
                                int palette_diversity = 0);

// Forward declaration — the diversity pass is defined after median_cut.
// (Also exposed publicly as quantize::diversify_palette.)
void apply_palette_diversity(Palette& palette,
                             std::span<const Color3f> pixels,
                             int diversity_level,
                             bool snap_to_ocs);

Palette ocs_bruteforce_quantize(std::span<const Color3f> pixels,
                                std::size_t max_colors,
                                int palette_diversity) {
    if (max_colors == 0) max_colors = 1;
    if (pixels.empty()) {
        return Palette{"ocs-optimal", {Color3f{0.0f, 0.0f, 0.0f}}};
    }

    auto& lut = ocs_lut();

    // Step 1: Build histogram of OCS colors
    std::array<std::uint32_t, 4096> histogram{};
    for (auto& pixel : pixels) {
        auto ocs = palette::linear_to_ocs(pixel);
        histogram[ocs]++;
    }

    // Collect non-zero entries
    std::vector<WeightedOcs> entries;
    entries.reserve(4096);
    for (std::uint16_t i = 0; i < 4096; ++i) {
        if (histogram[i] > 0) {
            entries.push_back({i, histogram[i]});
        }
    }

    // If we have fewer unique colors than max_colors, just use them all
    if (entries.size() <= max_colors) {
        Palette result;
        result.name = "ocs-optimal";
        for (auto& e : entries) {
            result.colors.push_back(lut.linear[e.ocs_index]);
        }
        // Sort by luminance
        std::sort(result.colors.begin(), result.colors.end(),
                  [](const Color3f& a, const Color3f& b) {
                      return color_space::linear_to_oklab(a).L <
                             color_space::linear_to_oklab(b).L;
                  });
        return result;
    }

    // Step 2: Greedy sequential palette construction.
    // Add one color at a time, each time picking the OCS color that
    // minimizes the total weighted error across all histogram entries.
    // Same approach as abc (AmigAtari Bitmap Converter).
    std::vector<std::uint16_t> palette_ocs(max_colors);

    // Per-entry cache: current minimum distance to any existing palette color
    std::vector<float> best_dist(entries.size(),
                                 std::numeric_limits<float>::max());

    for (std::size_t k = 0; k < max_colors; ++k) {
        // Try all 4096 OCS colors, pick the one that reduces total error most
        float best_total = std::numeric_limits<float>::max();
        std::uint16_t best_ocs = 0;

        for (std::uint16_t candidate = 0; candidate < 4096; ++candidate) {
            auto cand_lab = lut.oklab[candidate];
            float total = 0.0f;

            for (std::size_t i = 0; i < entries.size(); ++i) {
                float d = oklab_dist_sq(lut.oklab[entries[i].ocs_index], cand_lab);
                float effective = std::min(d, best_dist[i]);
                total += effective * static_cast<float>(entries[i].weight);
            }

            if (total < best_total) {
                best_total = total;
                best_ocs = candidate;
            }
        }

        palette_ocs[k] = best_ocs;

        // Update per-entry best distances with the newly added color
        auto new_lab = lut.oklab[best_ocs];
        for (std::size_t i = 0; i < entries.size(); ++i) {
            float d = oklab_dist_sq(lut.oklab[entries[i].ocs_index], new_lab);
            best_dist[i] = std::min(best_dist[i], d);
        }
    }

    // (k-means refinement removed — greedy sequential is sufficient)

    // Build result palette
    Palette result;
    result.name = "ocs-optimal";
    result.colors.reserve(max_colors);
    for (auto ocs : palette_ocs) {
        result.colors.push_back(lut.linear[ocs]);
    }

    // Optional palette diversity pass (remove near-duplicates, re-seed from
    // worst-served pixels). Snap back to OCS precision after each move.
    if (palette_diversity > 0) {
        apply_palette_diversity(result, pixels, palette_diversity,
                                /*snap_to_ocs=*/true);
    }

    // Sort by perceptual luminance
    std::sort(result.colors.begin(), result.colors.end(),
              [](const Color3f& a, const Color3f& b) {
                  return color_space::linear_to_oklab(a).L <
                         color_space::linear_to_oklab(b).L;
              });

    return result;
}

// ===========================================================================
// Wu's optimal color quantization in OKLab space
//
// Based on Xiaolin Wu, "Efficient Statistical Computations for Optimal
// Color Quantization" (Graphics Gems II, 1991).
//
// Uses a 3D histogram in OKLab space with prefix-sum moment tables
// for O(1) box variance computation. Splits always minimize the total
// weighted variance of the two resulting boxes.
// ===========================================================================

constexpr std::size_t WU_BINS = 65;  // 0..64 per axis (higher resolution)
constexpr std::size_t WU_SIZE = WU_BINS * WU_BINS * WU_BINS;

struct WuMoments {
    // Cumulative sums (3D prefix sums indexed [L][a][b])
    std::vector<int64_t> count;       // pixel count
    std::vector<double> sum_L;        // sum of OKLab L
    std::vector<double> sum_a;        // sum of OKLab a
    std::vector<double> sum_b;        // sum of OKLab b
    std::vector<double> sum_sq;       // sum of L² + a² + b²

    WuMoments()
        : count(WU_SIZE, 0), sum_L(WU_SIZE, 0), sum_a(WU_SIZE, 0),
          sum_b(WU_SIZE, 0), sum_sq(WU_SIZE, 0) {}

    static std::size_t idx(std::size_t l, std::size_t a, std::size_t b) {
        return l * WU_BINS * WU_BINS + a * WU_BINS + b;
    }
};

struct WuBox {
    std::size_t l0{}, l1{};  // L range [l0, l1]
    std::size_t a0{}, a1{};  // a range
    std::size_t b0{}, b1{};  // b range
    float variance{};
    int64_t pixel_count{};
};

// Volume of a box from the prefix sum table
template <typename T>
T wu_vol(const std::vector<T>& table,
         std::size_t l0, std::size_t l1,
         std::size_t a0, std::size_t a1,
         std::size_t b0, std::size_t b1) {
    auto I = WuMoments::idx;
    return table[I(l1, a1, b1)]
         - table[I(l1, a1, b0)] - table[I(l1, a0, b1)] - table[I(l0, a1, b1)]
         + table[I(l1, a0, b0)] + table[I(l0, a1, b0)] + table[I(l0, a0, b1)]
         - table[I(l0, a0, b0)];
}

float wu_box_variance(const WuMoments& m, const WuBox& box) {
    auto cnt = wu_vol(m.count, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    if (cnt <= 0) return 0.0f;
    auto sL = wu_vol(m.sum_L, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    auto sa = wu_vol(m.sum_a, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    auto sb = wu_vol(m.sum_b, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    auto sq = wu_vol(m.sum_sq, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    auto n = static_cast<double>(cnt);
    return static_cast<float>(sq - (sL * sL + sa * sa + sb * sb) / n);
}

color_space::OKLab wu_box_centroid(const WuMoments& m, const WuBox& box) {
    auto cnt = wu_vol(m.count, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    if (cnt <= 0) return {};
    auto n = static_cast<double>(cnt);
    auto sL = wu_vol(m.sum_L, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    auto sa = wu_vol(m.sum_a, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    auto sb = wu_vol(m.sum_b, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    return {static_cast<float>(sL / n), static_cast<float>(sa / n),
            static_cast<float>(sb / n)};
}

// Find the optimal cut on one axis that minimizes total variance
// Returns {cut_position, combined_variance}. cut_position 0 = no valid cut.
std::pair<std::size_t, float> wu_best_cut(
    const WuMoments& m, const WuBox& box, int axis) {

    // The whole-box stats
    auto whole_cnt = wu_vol(m.count, box.l0, box.l1, box.a0, box.a1, box.b0, box.b1);
    if (whole_cnt <= 1) return {0, std::numeric_limits<float>::max()};

    float best_var = std::numeric_limits<float>::max();
    std::size_t best_pos = 0;

    auto lo = (axis == 0) ? box.l0 : (axis == 1) ? box.a0 : box.b0;
    auto hi = (axis == 0) ? box.l1 : (axis == 1) ? box.a1 : box.b1;

    for (auto cut = lo + 1; cut <= hi; ++cut) {
        // Box A: [lo, cut], Box B: (cut, hi]
        WuBox a = box, b = box;
        if (axis == 0) { a.l1 = cut; b.l0 = cut; }
        else if (axis == 1) { a.a1 = cut; b.a0 = cut; }
        else { a.b1 = cut; b.b0 = cut; }

        auto cnt_a = wu_vol(m.count, a.l0, a.l1, a.a0, a.a1, a.b0, a.b1);
        auto cnt_b = wu_vol(m.count, b.l0, b.l1, b.a0, b.a1, b.b0, b.b1);
        if (cnt_a <= 0 || cnt_b <= 0) continue;

        float var = wu_box_variance(m, a) + wu_box_variance(m, b);
        if (var < best_var) {
            best_var = var;
            best_pos = cut;
        }
    }

    return {best_pos, best_var};
}

[[maybe_unused]]
std::vector<color_space::OKLab> wu_quantize_oklab(
    std::span<const Color3f> pixels, std::size_t max_colors) {

    if (max_colors == 0) max_colors = 1;

    // OKLab ranges for binning
    // L: [0, 1], a: [-0.4, 0.4], b: [-0.4, 0.4] (approximate)
    constexpr float L_min = 0.0f, L_max = 1.0f;
    constexpr float a_min = -0.5f, a_max = 0.5f;
    constexpr float b_min = -0.5f, b_max = 0.5f;
    constexpr float L_scale = 64.0f / (L_max - L_min);
    constexpr float a_scale = 64.0f / (a_max - a_min);
    constexpr float b_scale = 64.0f / (b_max - b_min);

    auto bin = [](float v, float vmin, float scale) -> std::size_t {
        auto b = static_cast<std::size_t>(
            std::clamp((v - vmin) * scale, 0.0f, 63.0f)) + 1;  // 1..64
        return b;
    };

    // Build histogram
    WuMoments hist;
    for (auto& pixel : pixels) {
        auto lab = color_space::linear_to_oklab(pixel);
        auto il = bin(lab.L, L_min, L_scale);
        auto ia = bin(lab.a, a_min, a_scale);
        auto ib = bin(lab.b, b_min, b_scale);
        auto i = WuMoments::idx(il, ia, ib);
        hist.count[i]++;
        hist.sum_L[i] += static_cast<double>(lab.L);
        hist.sum_a[i] += static_cast<double>(lab.a);
        hist.sum_b[i] += static_cast<double>(lab.b);
        hist.sum_sq[i] += static_cast<double>(
            lab.L * lab.L + lab.a * lab.a + lab.b * lab.b);
    }

    // Compute 3D prefix sums
    auto prefix_sum_3d = []<typename T>(std::vector<T>& t) {
        for (std::size_t l = 1; l < WU_BINS; ++l)
            for (std::size_t a = 0; a < WU_BINS; ++a)
                for (std::size_t b = 0; b < WU_BINS; ++b)
                    t[WuMoments::idx(l, a, b)] += t[WuMoments::idx(l - 1, a, b)];
        for (std::size_t l = 0; l < WU_BINS; ++l)
            for (std::size_t a = 1; a < WU_BINS; ++a)
                for (std::size_t b = 0; b < WU_BINS; ++b)
                    t[WuMoments::idx(l, a, b)] += t[WuMoments::idx(l, a - 1, b)];
        for (std::size_t l = 0; l < WU_BINS; ++l)
            for (std::size_t a = 0; a < WU_BINS; ++a)
                for (std::size_t b = 1; b < WU_BINS; ++b)
                    t[WuMoments::idx(l, a, b)] += t[WuMoments::idx(l, a, b - 1)];
    };

    prefix_sum_3d(hist.count);
    prefix_sum_3d(hist.sum_L);
    prefix_sum_3d(hist.sum_a);
    prefix_sum_3d(hist.sum_b);
    prefix_sum_3d(hist.sum_sq);

    // Initialize with one box spanning the full range
    std::vector<WuBox> boxes;
    boxes.reserve(max_colors);
    boxes.push_back({0, 64, 0, 64, 0, 64, 0.0f, 0});
    boxes[0].variance = wu_box_variance(hist, boxes[0]);
    boxes[0].pixel_count = wu_vol(hist.count, 0, 64, 0, 64, 0, 64);

    // Iteratively split the box with highest variance
    while (boxes.size() < max_colors) {
        // Find box with highest variance that can be split
        std::size_t best_box = 0;
        float best_var = -1.0f;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].pixel_count > 1 && boxes[i].variance > best_var) {
                best_var = boxes[i].variance;
                best_box = i;
            }
        }
        if (best_var <= 0.0f) break;

        auto& parent = boxes[best_box];

        // Try all 3 axes, pick the best split
        auto [cut_l, var_l] = wu_best_cut(hist, parent, 0);
        auto [cut_a, var_a] = wu_best_cut(hist, parent, 1);
        auto [cut_b, var_b] = wu_best_cut(hist, parent, 2);

        int best_axis = 0;
        float best_split_var = var_l;
        std::size_t best_cut = cut_l;
        if (var_a < best_split_var && cut_a > 0) {
            best_axis = 1; best_split_var = var_a; best_cut = cut_a;
        }
        if (var_b < best_split_var && cut_b > 0) {
            best_axis = 2; best_split_var = var_b; best_cut = cut_b;
        }
        if (best_cut == 0) break;  // no valid split found

        WuBox child_a = parent, child_b = parent;
        if (best_axis == 0) { child_a.l1 = best_cut; child_b.l0 = best_cut; }
        else if (best_axis == 1) { child_a.a1 = best_cut; child_b.a0 = best_cut; }
        else { child_a.b1 = best_cut; child_b.b0 = best_cut; }

        child_a.variance = wu_box_variance(hist, child_a);
        child_b.variance = wu_box_variance(hist, child_b);
        child_a.pixel_count = wu_vol(hist.count, child_a.l0, child_a.l1,
                                     child_a.a0, child_a.a1, child_a.b0, child_a.b1);
        child_b.pixel_count = wu_vol(hist.count, child_b.l0, child_b.l1,
                                     child_b.a0, child_b.a1, child_b.b0, child_b.b1);

        boxes[best_box] = child_a;
        boxes.push_back(child_b);
    }

    // Extract centroids
    std::vector<color_space::OKLab> result;
    result.reserve(boxes.size());
    for (auto& box : boxes) {
        result.push_back(wu_box_centroid(hist, box));
    }
    return result;
}

} // namespace

// ===========================================================================
// Median-cut implementation
// ===========================================================================

Palette median_cut(std::span<const Color3f> colors,
                   std::size_t max_colors,
                   int palette_diversity) {
    if (max_colors == 0) max_colors = 1;
    if (colors.empty()) {
        return Palette{"quantized", {Color3f{0.0f, 0.0f, 0.0f}}};
    }

    // Subsample large images for k-means refinement
    std::vector<Color3f> work(colors.begin(), colors.end());
    constexpr std::size_t max_samples = 262144;
    if (work.size() > max_samples) {
        auto stride = work.size() / max_samples;
        std::vector<Color3f> sampled;
        sampled.reserve(max_samples);
        for (std::size_t i = 0; i < work.size(); i += stride) {
            sampled.push_back(work[i]);
        }
        work = std::move(sampled);
    }

    // ---------------------------------------------------------------
    // Median-cut → k-means refinement in OKLab perceptual space
    //
    // Median-cut gives stable initial centroids. K-means in OKLab
    // (perceptually uniform) refines them with empty cluster handling.
    // ---------------------------------------------------------------

    // Run median-cut in linear RGB for initial centroids
    // (reuse the subsampled 'work' array)
    auto mc_result = [&]() {
        std::vector<Color3f> mc_work = work;  // median-cut sorts in-place
        struct Box {
            std::size_t start, count;
            float vol{};
            int axis{};
            void compute(std::span<const Color3f> c) {
                if (count == 0) { vol = 0; return; }
                float minr=1e9f,maxr=-1e9f,ming=1e9f,maxg=-1e9f,minb=1e9f,maxb=-1e9f;
                for (std::size_t i = start; i < start+count; ++i) {
                    minr=std::min(minr,c[i].r); maxr=std::max(maxr,c[i].r);
                    ming=std::min(ming,c[i].g); maxg=std::max(maxg,c[i].g);
                    minb=std::min(minb,c[i].b); maxb=std::max(maxb,c[i].b);
                }
                float rr=maxr-minr, rg=maxg-ming, rb=maxb-minb;
                vol = std::max({rr,rg,rb});
                axis = (rr>=rg && rr>=rb) ? 0 : (rg>=rb) ? 1 : 2;
            }
        };
        std::vector<Box> boxes;
        boxes.reserve(max_colors);
        boxes.push_back({0, mc_work.size()});
        boxes[0].compute(mc_work);
        while (boxes.size() < max_colors) {
            std::size_t bi = 0; float bv = -1;
            for (std::size_t i = 0; i < boxes.size(); ++i)
                if (boxes[i].count >= 2 && boxes[i].vol > bv) { bv = boxes[i].vol; bi = i; }
            if (bv <= 0) break;
            auto& b = boxes[bi];
            auto bb = mc_work.begin()+static_cast<std::ptrdiff_t>(b.start);
            auto be = bb+static_cast<std::ptrdiff_t>(b.count);
            switch (b.axis) {
            case 0: std::sort(bb,be,[](auto&p,auto&q){return p.r<q.r;}); break;
            case 1: std::sort(bb,be,[](auto&p,auto&q){return p.g<q.g;}); break;
            case 2: std::sort(bb,be,[](auto&p,auto&q){return p.b<q.b;}); break;
            }
            auto m = b.count/2;
            Box a{b.start,m}, c{b.start+m,b.count-m};
            a.compute(mc_work); c.compute(mc_work);
            boxes[bi] = a; boxes.push_back(c);
        }
        std::vector<Color3f> centroids;
        for (auto& b : boxes) {
            double sr=0,sg=0,sb=0;
            for (std::size_t i=b.start; i<b.start+b.count; ++i) {
                sr+=static_cast<double>(mc_work[i].r);
                sg+=static_cast<double>(mc_work[i].g);
                sb+=static_cast<double>(mc_work[i].b);
            }
            auto n=static_cast<double>(b.count);
            centroids.push_back(Color3f{
                static_cast<float>(sr/n),
                static_cast<float>(sg/n),
                static_cast<float>(sb/n)}.clamped());
        }
        return centroids;
    }();

    auto n_colors = mc_result.size();

    Palette result;
    result.name = "quantized";
    result.colors = mc_result;

    constexpr int kmeans_max_iter = 40;

    // Precompute all samples in OKLab
    std::vector<color_space::OKLab> samples_lab(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) {
        samples_lab[i] = color_space::linear_to_oklab(work[i]);
    }

    // Initialize centroids in OKLab from median-cut
    std::vector<color_space::OKLab> centroids(n_colors);
    for (std::size_t i = 0; i < n_colors; ++i) {
        centroids[i] = color_space::linear_to_oklab(result.colors[i]);
    }

    std::vector<std::size_t> assignments(work.size());
    std::vector<float> pixel_errors(work.size());

    for (int iter = 0; iter < kmeans_max_iter; ++iter) {
        // Assign each sample to nearest centroid
        for (std::size_t i = 0; i < work.size(); ++i) {
            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            for (std::size_t k = 0; k < n_colors; ++k) {
                float d = oklab_dist_sq(samples_lab[i], centroids[k]);
                if (d < best_d) { best_d = d; best_k = k; }
            }
            assignments[i] = best_k;
            pixel_errors[i] = best_d;
        }

        // Accumulate per-cluster stats
        struct Acc { double L{}, a{}, b{}; std::size_t n{}; double total_err{}; };
        std::vector<Acc> acc(n_colors);
        for (std::size_t i = 0; i < work.size(); ++i) {
            auto k = assignments[i];
            acc[k].L += static_cast<double>(samples_lab[i].L);
            acc[k].a += static_cast<double>(samples_lab[i].a);
            acc[k].b += static_cast<double>(samples_lab[i].b);
            acc[k].n++;
            acc[k].total_err += static_cast<double>(pixel_errors[i]);
        }

        // Recompute centroids; handle empty clusters
        bool changed = false;
        for (std::size_t k = 0; k < n_colors; ++k) {
            if (acc[k].n == 0) {
                // Re-seed from the farthest pixel of the worst cluster
                std::size_t worst_cluster = 0;
                double worst_err = -1.0;
                for (std::size_t j = 0; j < n_colors; ++j) {
                    if (acc[j].total_err > worst_err) {
                        worst_err = acc[j].total_err;
                        worst_cluster = j;
                    }
                }
                // Find the farthest pixel in the worst cluster
                float farthest_d = -1.0f;
                std::size_t farthest_idx = 0;
                for (std::size_t i = 0; i < work.size(); ++i) {
                    if (assignments[i] == worst_cluster &&
                        pixel_errors[i] > farthest_d) {
                        farthest_d = pixel_errors[i];
                        farthest_idx = i;
                    }
                }
                centroids[k] = samples_lab[farthest_idx];
                changed = true;
                continue;
            }

            auto dn = static_cast<double>(acc[k].n);
            color_space::OKLab nlab{
                static_cast<float>(acc[k].L / dn),
                static_cast<float>(acc[k].a / dn),
                static_cast<float>(acc[k].b / dn),
            };
            if (oklab_dist_sq(nlab, centroids[k]) > 1e-12f) {
                changed = true;
                centroids[k] = nlab;
            }
        }
        if (!changed) break;
    }

    // Write refined centroids back
    for (std::size_t i = 0; i < n_colors; ++i) {
        result.colors[i] = color_space::oklab_to_linear(centroids[i]).clamped();
    }

    // Optional palette diversity pass: remove near-duplicate entries and
    // re-seed them from image regions that are poorly served by the current
    // palette. Operates on the full (non-subsampled) pixel array.
    if (palette_diversity > 0) {
        apply_palette_diversity(result, colors, palette_diversity,
                                /*snap_to_ocs=*/false);
    }

    // Sort palette by perceptual luminance (OKLab L) for consistent ordering
    std::sort(result.colors.begin(), result.colors.end(),
              [](const Color3f& a, const Color3f& b) {
                  return color_space::linear_to_oklab(a).L <
                         color_space::linear_to_oklab(b).L;
              });

    return result;
}

// ===========================================================================
// Pairwise Nearest Neighbor quantization (Equitz 1989, Kurz/Thyssen)
//
// Agglomerative clustering with Ward-linkage merge cost in OKLab:
//
//     cost(i, j) = (w_i * w_j / (w_i + w_j)) * ||c_i - c_j||²
//
// Starting from a histogram of the input image, we repeatedly merge the
// pair with lowest merge cost until `max_colors` clusters remain.
// Published results have PNN beating median-cut at low palette counts
// (≤32) at the cost of higher runtime — but since our typical N is
// 16–64 and histogram bins top out at ~4096 (OCS) or 32768 (coarse AGA
// grid), the O(B²) init is fast enough.
//
// Nearest-neighbor pointers plus lazy heap validation give O(B²) init
// and O((B-N)·B) merges in the worst case.
// ===========================================================================

Palette pnn_quantize(std::span<const Color3f> colors,
                     std::size_t max_colors,
                     int palette_diversity,
                     bool snap_to_ocs) {

    if (max_colors == 0) max_colors = 1;
    if (colors.empty()) {
        return Palette{"pnn", {Color3f{0.0f, 0.0f, 0.0f}}};
    }

    // Each cluster stores a weighted centroid in OKLab AND a "representative"
    // that the cost function uses. For continuous PNN these are identical.
    // For OCS PNN the representative is the OCS color nearest the weighted
    // centroid, so every merge cost and the final palette respect the
    // discrete 12-bit gamut.
    struct Cluster {
        // Weighted sums (in OKLab); centroid = sum / weight
        double sum_L{}, sum_a{}, sum_b{};
        float weight{};
        OKLab rep{};          // representative: either centroid or OCS-snapped
        std::size_t nn{};
        float nn_cost{};
        bool alive{};
    };
    std::vector<Cluster> clusters;

    auto refresh_rep = [&](Cluster& c) {
        auto w = static_cast<double>(c.weight);
        OKLab centroid{
            static_cast<float>(c.sum_L / w),
            static_cast<float>(c.sum_a / w),
            static_cast<float>(c.sum_b / w),
        };
        if (snap_to_ocs) {
            auto rgb = color_space::oklab_to_linear(centroid).clamped();
            rgb = palette::quantize_to_ocs(rgb);
            c.rep = color_space::linear_to_oklab(rgb);
        } else {
            c.rep = centroid;
        }
    };

    if (snap_to_ocs) {
        // Initialize one cluster per distinct OCS color that the image uses.
        // Max 4096 clusters, cheap O(B²).
        auto& lut = ocs_lut();
        std::array<std::uint32_t, 4096> hist{};
        std::array<double, 4096> sum_L{}, sum_a{}, sum_b{};
        for (auto& pixel : colors) {
            auto ocs = palette::linear_to_ocs(pixel);
            auto lab = color_space::linear_to_oklab(pixel);
            ++hist[ocs];
            sum_L[ocs] += static_cast<double>(lab.L);
            sum_a[ocs] += static_cast<double>(lab.a);
            sum_b[ocs] += static_cast<double>(lab.b);
        }
        clusters.reserve(4096);
        for (std::uint16_t i = 0; i < 4096; ++i) {
            if (hist[i] == 0) continue;
            Cluster c{};
            c.sum_L  = sum_L[i];
            c.sum_a  = sum_a[i];
            c.sum_b  = sum_b[i];
            c.weight = static_cast<float>(hist[i]);
            c.rep    = lut.oklab[i];  // OCS color exactly
            c.alive  = true;
            c.nn_cost = std::numeric_limits<float>::max();
            clusters.push_back(c);
        }
    } else {
        // Continuous-space histogram: 24³ = 13824 bins in OKLab.
        constexpr std::size_t BINS = 24;
        constexpr float L_min = 0.0f, L_max = 1.0f;
        constexpr float a_min = -0.5f, a_max = 0.5f;
        constexpr float b_min = -0.5f, b_max = 0.5f;
        constexpr float L_scale = static_cast<float>(BINS) / (L_max - L_min);
        constexpr float a_scale = static_cast<float>(BINS) / (a_max - a_min);
        constexpr float b_scale = static_cast<float>(BINS) / (b_max - b_min);

        struct Bin {
            double L{}, a{}, b{};
            std::uint32_t weight{};
        };
        std::vector<Bin> hist(BINS * BINS * BINS);
        for (auto& pixel : colors) {
            auto lab = color_space::linear_to_oklab(pixel);
            auto iL = static_cast<std::size_t>(
                std::clamp((lab.L - L_min) * L_scale, 0.0f,
                           static_cast<float>(BINS - 1)));
            auto ia = static_cast<std::size_t>(
                std::clamp((lab.a - a_min) * a_scale, 0.0f,
                           static_cast<float>(BINS - 1)));
            auto ib = static_cast<std::size_t>(
                std::clamp((lab.b - b_min) * b_scale, 0.0f,
                           static_cast<float>(BINS - 1)));
            auto& bin = hist[(iL * BINS + ia) * BINS + ib];
            bin.L += static_cast<double>(lab.L);
            bin.a += static_cast<double>(lab.a);
            bin.b += static_cast<double>(lab.b);
            ++bin.weight;
        }
        clusters.reserve(1024);
        for (auto& bin : hist) {
            if (bin.weight == 0) continue;
            Cluster c{};
            c.sum_L  = bin.L;
            c.sum_a  = bin.a;
            c.sum_b  = bin.b;
            c.weight = static_cast<float>(bin.weight);
            c.rep    = OKLab{
                static_cast<float>(bin.L / bin.weight),
                static_cast<float>(bin.a / bin.weight),
                static_cast<float>(bin.b / bin.weight),
            };
            c.alive  = true;
            c.nn_cost = std::numeric_limits<float>::max();
            clusters.push_back(c);
        }
    }

    auto merge_cost = [](const Cluster& a, const Cluster& b) {
        float d = oklab_dist_sq(a.rep, b.rep);
        float w = (a.weight * b.weight) / (a.weight + b.weight);
        return w * d;
    };

    // If we already have few enough clusters, return them directly
    if (clusters.size() <= max_colors) {
        Palette result;
        result.name = "pnn";
        for (auto& c : clusters) {
            result.colors.push_back(
                color_space::oklab_to_linear(c.rep).clamped());
        }
        std::sort(result.colors.begin(), result.colors.end(),
                  [](const Color3f& a, const Color3f& b) {
                      return color_space::linear_to_oklab(a).L <
                             color_space::linear_to_oklab(b).L;
                  });
        if (palette_diversity > 0)
            apply_palette_diversity(result, colors, palette_diversity, false);
        return result;
    }

    // Initialize each cluster's nearest neighbor (O(B²) one-time).
    auto update_nn = [&](std::size_t i) {
        float best = std::numeric_limits<float>::max();
        std::size_t best_j = i;
        for (std::size_t j = 0; j < clusters.size(); ++j) {
            if (j == i || !clusters[j].alive) continue;
            float c = merge_cost(clusters[i], clusters[j]);
            if (c < best) { best = c; best_j = j; }
        }
        clusters[i].nn = best_j;
        clusters[i].nn_cost = best;
    };

    for (std::size_t i = 0; i < clusters.size(); ++i) update_nn(i);

    std::size_t alive_count = clusters.size();
    while (alive_count > max_colors) {
        // Find alive cluster with lowest merge cost
        std::size_t best_i = 0;
        float best_c = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            if (!clusters[i].alive) continue;
            if (clusters[i].nn_cost < best_c) {
                best_c = clusters[i].nn_cost;
                best_i = i;
            }
        }
        auto j = clusters[best_i].nn;
        if (!clusters[j].alive || j == best_i) {
            // Stale pointer; refresh and retry
            update_nn(best_i);
            continue;
        }

        // Merge j into best_i. Accumulate weighted sums (not weighted
        // reps — reps are snapped discrete values and don't compose).
        auto& a = clusters[best_i];
        auto& b = clusters[j];
        a.sum_L  += b.sum_L;
        a.sum_a  += b.sum_a;
        a.sum_b  += b.sum_b;
        a.weight += b.weight;
        refresh_rep(a);
        b.alive = false;
        --alive_count;

        // Any cluster whose NN was best_i or j needs its NN recomputed.
        // best_i itself also needs a new NN.
        update_nn(best_i);
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            if (!clusters[i].alive || i == best_i) continue;
            if (clusters[i].nn == best_i || clusters[i].nn == j) {
                update_nn(i);
            } else {
                // Check if best_i (with its updated centroid) is now closer
                // than its current nn.
                float c = merge_cost(clusters[i], clusters[best_i]);
                if (c < clusters[i].nn_cost) {
                    clusters[i].nn = best_i;
                    clusters[i].nn_cost = c;
                }
            }
        }
    }

    // Collect surviving clusters
    Palette result;
    result.name = "pnn";
    for (auto& c : clusters) {
        if (!c.alive) continue;
        result.colors.push_back(
            color_space::oklab_to_linear(c.rep).clamped());
    }

    // Sort by perceptual luminance for consistent ordering
    std::sort(result.colors.begin(), result.colors.end(),
              [](const Color3f& a, const Color3f& b) {
                  return color_space::linear_to_oklab(a).L <
                         color_space::linear_to_oklab(b).L;
              });

    if (palette_diversity > 0)
        apply_palette_diversity(result, colors, palette_diversity, false);

    return result;
}

// ===========================================================================
// Palette diversity pass — inspired by ham_convert's diversity option.
//
// Iteratively replaces the two closest palette entries (in OKLab) with the
// image pixel that is currently worst-served by the palette (the pixel with
// the largest min-distance to any entry). After each swap, run a few
// k-means iterations restricted to a small neighborhood around the swapped
// entry so the rest of the palette stays stable.
//
// Level 0: skip. Level N runs up to N swap rounds. The pass is conservative:
// a swap is only committed if the candidate pixel is more distant from the
// existing palette than the closest-pair distance — otherwise the swap
// would create a new near-duplicate.
// ===========================================================================

namespace {

std::pair<std::size_t, std::size_t> find_closest_pair(
    std::span<const color_space::OKLab> pal_lab, float& out_dist) {

    float best = std::numeric_limits<float>::max();
    std::size_t ia = 0, ib = 1;
    for (std::size_t i = 0; i < pal_lab.size(); ++i) {
        for (std::size_t j = i + 1; j < pal_lab.size(); ++j) {
            float d = oklab_dist_sq(pal_lab[i], pal_lab[j]);
            if (d < best) { best = d; ia = i; ib = j; }
        }
    }
    out_dist = best;
    return {ia, ib};
}

// Compute total weighted SSE (sum over samples of squared distance to nearest
// palette entry). Used as the objective for greedy swap acceptance.
float palette_total_sse(
    std::span<const color_space::OKLab> samples_lab,
    std::span<const color_space::OKLab> pal_lab) {

    float total = 0.0f;
    for (auto& s : samples_lab) {
        float best = std::numeric_limits<float>::max();
        for (auto& pc : pal_lab) {
            float d = oklab_dist_sq(s, pc);
            if (d < best) best = d;
        }
        total += best;
    }
    return total;
}

// Run a few k-means iterations in OKLab, keeping entries mutable. Returns
// the refined palette (copy).
std::vector<color_space::OKLab> kmeans_refine(
    std::span<const color_space::OKLab> samples_lab,
    std::vector<color_space::OKLab> centroids,
    int iterations) {

    auto n = centroids.size();
    std::vector<std::size_t> assignments(samples_lab.size());
    for (int iter = 0; iter < iterations; ++iter) {
        for (std::size_t i = 0; i < samples_lab.size(); ++i) {
            float best = std::numeric_limits<float>::max();
            std::size_t bk = 0;
            for (std::size_t k = 0; k < n; ++k) {
                float d = oklab_dist_sq(samples_lab[i], centroids[k]);
                if (d < best) { best = d; bk = k; }
            }
            assignments[i] = bk;
        }
        struct Acc { double L{}, a{}, b{}; std::size_t n{}; };
        std::vector<Acc> acc(n);
        for (std::size_t i = 0; i < samples_lab.size(); ++i) {
            auto& A = acc[assignments[i]];
            A.L += static_cast<double>(samples_lab[i].L);
            A.a += static_cast<double>(samples_lab[i].a);
            A.b += static_cast<double>(samples_lab[i].b);
            ++A.n;
        }
        bool changed = false;
        for (std::size_t k = 0; k < n; ++k) {
            if (acc[k].n == 0) continue;
            auto dn = static_cast<double>(acc[k].n);
            color_space::OKLab nlab{
                static_cast<float>(acc[k].L / dn),
                static_cast<float>(acc[k].a / dn),
                static_cast<float>(acc[k].b / dn),
            };
            if (oklab_dist_sq(nlab, centroids[k]) > 1e-12f) {
                changed = true;
                centroids[k] = nlab;
            }
        }
        if (!changed) break;
    }
    return centroids;
}

void apply_palette_diversity(Palette& palette,
                             std::span<const Color3f> pixels,
                             int diversity_level,
                             bool snap_to_ocs) {

    if (diversity_level <= 0 || palette.colors.size() < 3 || pixels.empty())
        return;

    // Subsample large images for responsiveness
    std::vector<Color3f> work(pixels.begin(), pixels.end());
    constexpr std::size_t max_samples = 131072;
    if (work.size() > max_samples) {
        auto stride = work.size() / max_samples;
        std::vector<Color3f> sampled;
        sampled.reserve(max_samples);
        for (std::size_t i = 0; i < work.size(); i += stride)
            sampled.push_back(work[i]);
        work = std::move(sampled);
    }

    std::vector<color_space::OKLab> samples_lab(work.size());
    for (std::size_t i = 0; i < work.size(); ++i)
        samples_lab[i] = color_space::linear_to_oklab(work[i]);

    // Working palette in OKLab
    std::vector<color_space::OKLab> pal_lab(palette.colors.size());
    for (std::size_t i = 0; i < palette.colors.size(); ++i)
        pal_lab[i] = color_space::linear_to_oklab(palette.colors[i]);

    // Current best SSE
    float best_sse = palette_total_sse(samples_lab, pal_lab);

    // Cluster-assignment error accumulation: find the cluster with highest
    // total SSE — that's where we should reseed a centroid.
    auto find_worst_cluster_centroid = [&](std::vector<color_space::OKLab>& out_centroid) -> bool {
        struct Acc { double L{}, a{}, b{}, err{}; std::size_t n{}; };
        std::vector<Acc> acc(pal_lab.size());
        for (auto& s : samples_lab) {
            float best = std::numeric_limits<float>::max();
            std::size_t bk = 0;
            for (std::size_t k = 0; k < pal_lab.size(); ++k) {
                float d = oklab_dist_sq(s, pal_lab[k]);
                if (d < best) { best = d; bk = k; }
            }
            acc[bk].L += static_cast<double>(s.L);
            acc[bk].a += static_cast<double>(s.a);
            acc[bk].b += static_cast<double>(s.b);
            acc[bk].err += static_cast<double>(best);
            ++acc[bk].n;
        }
        // Pick cluster with largest total error, then split it: new centroid
        // at the pixel farthest from the cluster's current centroid.
        std::size_t worst_k = 0; double worst_err = -1.0;
        for (std::size_t k = 0; k < acc.size(); ++k) {
            if (acc[k].err > worst_err) { worst_err = acc[k].err; worst_k = k; }
        }
        if (acc[worst_k].n < 2) return false;
        // Find farthest sample assigned to worst_k
        float fd = -1.0f; std::size_t fi = 0;
        for (std::size_t i = 0; i < samples_lab.size(); ++i) {
            // Recompute assignment (cheap enough for a single pass)
            float bd = std::numeric_limits<float>::max();
            std::size_t bk = 0;
            for (std::size_t k = 0; k < pal_lab.size(); ++k) {
                float d = oklab_dist_sq(samples_lab[i], pal_lab[k]);
                if (d < bd) { bd = d; bk = k; }
            }
            if (bk == worst_k && bd > fd) { fd = bd; fi = i; }
        }
        out_centroid.clear();
        out_centroid.push_back(samples_lab[fi]);
        return true;
    };

    // Diversity threshold: for each level, allow closer pairs to be merged.
    // Level 1 = very conservative (only merge near-identical pairs), level 5
    // = aggressive (merge pairs up to 2x the average nearest-neighbor dist).
    float avg_nn = 0.0f;
    {
        for (std::size_t i = 0; i < pal_lab.size(); ++i) {
            float best = std::numeric_limits<float>::max();
            for (std::size_t j = 0; j < pal_lab.size(); ++j) {
                if (i == j) continue;
                float d = oklab_dist_sq(pal_lab[i], pal_lab[j]);
                if (d < best) best = d;
            }
            avg_nn += std::sqrt(best);
        }
        avg_nn /= static_cast<float>(pal_lab.size());
    }
    // Threshold: allow closer pairs to be merged at higher diversity levels.
    //   level 1  →  0.35 * avg_nn  (very conservative: only near-duplicates)
    //   level 5  →  0.95 * avg_nn  (merge pairs up to the average)
    //   level 9  →  1.55 * avg_nn  (merge pairs 50% wider than average)
    float merge_threshold_dist =
        avg_nn * (0.2f + 0.15f * static_cast<float>(diversity_level));
    float merge_threshold_sq = merge_threshold_dist * merge_threshold_dist;

    // Swap budget scales with level so high settings actually get to try
    // more merges (low levels cap out quickly once the threshold is met).
    //   level 1  →  ~N swaps
    //   level 5  →  ~3N
    //   level 9  →  ~5N
    std::size_t max_swaps =
        (palette.colors.size() *
         (1 + static_cast<std::size_t>(std::max(0, diversity_level - 1)) / 2)) * 2;
    std::size_t committed = 0;

    for (std::size_t attempt = 0; attempt < max_swaps; ++attempt) {
        float pair_dist_sq = 0.0f;
        auto [ia, ib] = find_closest_pair(pal_lab, pair_dist_sq);
        if (pair_dist_sq > merge_threshold_sq) break;  // nothing close enough

        // Candidate palette: merge ia and ib into their midpoint, reseed ib
        // from the worst-served cluster centroid.
        auto candidate = pal_lab;
        candidate[ia] = color_space::OKLab{
            (pal_lab[ia].L + pal_lab[ib].L) * 0.5f,
            (pal_lab[ia].a + pal_lab[ib].a) * 0.5f,
            (pal_lab[ia].b + pal_lab[ib].b) * 0.5f,
        };
        std::vector<color_space::OKLab> reseed;
        if (!find_worst_cluster_centroid(reseed)) break;
        candidate[ib] = reseed[0];

        // Re-converge with k-means (5 iterations is plenty when starting
        // from a near-optimal palette).
        auto refined = kmeans_refine(samples_lab, candidate, 5);

        // Optionally snap each entry to OCS precision for OCS modes, so the
        // SSE we measure reflects what the hardware will actually display.
        if (snap_to_ocs) {
            for (auto& c : refined) {
                auto rgb = color_space::oklab_to_linear(c).clamped();
                rgb = palette::quantize_to_ocs(rgb);
                c = color_space::linear_to_oklab(rgb);
            }
        }

        // Guard against OCS collisions: if the refined palette ends up with
        // fewer unique colors than we started with, the "improvement" came
        // from losing a palette slot, which hurts the final dithered output.
        auto count_unique_ocs =
            [](std::span<const color_space::OKLab> p) {
                std::vector<std::uint16_t> ocs;
                ocs.reserve(p.size());
                for (auto& c : p) {
                    auto rgb = color_space::oklab_to_linear(c).clamped();
                    ocs.push_back(palette::linear_to_ocs(rgb));
                }
                std::sort(ocs.begin(), ocs.end());
                ocs.erase(std::unique(ocs.begin(), ocs.end()), ocs.end());
                return ocs.size();
            };

        if (snap_to_ocs &&
            count_unique_ocs(refined) < count_unique_ocs(pal_lab)) {
            // Refuse the swap: it would collapse colors.
            break;
        }

        float new_sse = palette_total_sse(samples_lab, refined);
        if (new_sse < best_sse) {
            best_sse = new_sse;
            pal_lab = std::move(refined);
            ++committed;
        } else {
            // The merge didn't help; stop — further swaps are unlikely to help.
            break;
        }
    }

    if (committed == 0) return;

    // Write back the refined palette
    for (std::size_t i = 0; i < palette.colors.size(); ++i) {
        auto rgb = color_space::oklab_to_linear(pal_lab[i]).clamped();
        if (snap_to_ocs) rgb = palette::quantize_to_ocs(rgb);
        palette.colors[i] = rgb;
    }
}

} // namespace

// ===========================================================================
// quantize() entry point
// ===========================================================================

Result<Palette> quantize(const Image& image, std::size_t max_colors,
                         Algorithm algo, int palette_diversity) {
    if (max_colors == 0 || max_colors > 256) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("Palette size must be 1-256, got {}", max_colors),
        }};
    }

    if (image.width() == 0 || image.height() == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Image dimensions must be non-zero",
        }};
    }

    switch (algo) {
    case Algorithm::median_cut:
        return median_cut(image.pixels(), max_colors, palette_diversity);
    case Algorithm::ocs_bruteforce:
        return ocs_bruteforce_quantize(image.pixels(), max_colors,
                                       palette_diversity);
    case Algorithm::pnn:
        // Continuous-space PNN for AGA, OCS-snapped PNN for OCS/STF.
        // Detection via an OCS snap test isn't available here, so the
        // caller selects snap via the helper overload. Default: continuous.
        return pnn_quantize(image.pixels(), max_colors, palette_diversity,
                            /*snap_to_ocs=*/false);
    }

    return median_cut(image.pixels(), max_colors, palette_diversity);
}

// ===========================================================================
// Public wrapper for the diversity pass
// ===========================================================================

void diversify_palette(Palette& palette,
                       std::span<const Color3f> pixels,
                       int diversity_level,
                       bool snap_to_ocs) {
    apply_palette_diversity(palette, pixels, diversity_level, snap_to_ocs);
}

// ===========================================================================
// Dither-aware palette refinement
// ===========================================================================

Result<Palette> refine_with_dither(
    const Image& image,
    const Palette& initial_palette,
    const dither::Settings& dither_settings,
    amiga::Chipset chipset,
    amiga::Mode mode,
    std::size_t max_iterations,
    const std::vector<bool>& locked) {

    if (dither_settings.method == dither::Method::none)
        return initial_palette;  // nothing to refine against

    // STF 9-bit gamut (512 colors) is too coarse for centroid-based
    // refinement — the brute-force quantizer already finds the optimal
    // discrete palette, and snapping continuous centroids to 9-bit can
    // collapse nearby colors to the same value.
    if (amiga::is_stf(mode))
        return initial_palette;

    auto pal = initial_palette;
    auto num_colors = pal.colors.size();
    if (num_colors < 2) return pal;

    bool is_stf = amiga::is_stf(mode);
    bool snap_to_discrete = (chipset != amiga::Chipset::aga) || is_stf;

    auto w = image.width();
    auto h = image.height();

    for (std::size_t iter = 0; iter < max_iterations; ++iter) {
        // Run the ditherer with the current palette
        auto dith = dither::apply(image, pal.colors, dither_settings);

        // Spatial Color Quantization: weight each pixel's contribution to
        // its cluster centroid by how many of its 4-neighbors share the
        // same palette assignment. Pixels in contiguous same-color regions
        // pull harder on the centroid; isolated single-pixel "islands"
        // pull less. This naturally pushes the palette toward colors that
        // serve large visual regions and away from scattered outliers.
        //
        // Weight = 1.0 + matching_neighbors * 0.5
        //   0 matching neighbors → weight 1.0  (isolated pixel)
        //   4 matching neighbors → weight 3.0  (interior of region)
        struct Acc { double L{}, a{}, b{}, w{}; };
        std::vector<Acc> acc(num_colors);

        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto idx = dith.indices[y * w + x];
                if (idx >= num_colors) continue;

                // Count 4-neighbors with the same assignment
                int neighbors = 0;
                if (x > 0     && dith.indices[y * w + (x - 1)] == idx) ++neighbors;
                if (x + 1 < w && dith.indices[y * w + (x + 1)] == idx) ++neighbors;
                if (y > 0     && dith.indices[(y - 1) * w + x] == idx) ++neighbors;
                if (y + 1 < h && dith.indices[(y + 1) * w + x] == idx) ++neighbors;

                double weight = 1.0 + static_cast<double>(neighbors) * 0.5;
                auto lab = color_space::linear_to_oklab(image[x, y]);
                acc[idx].L += static_cast<double>(lab.L) * weight;
                acc[idx].a += static_cast<double>(lab.a) * weight;
                acc[idx].b += static_cast<double>(lab.b) * weight;
                acc[idx].w += weight;
            }
        }

        // Update palette: move each unlocked slot to its weighted centroid
        bool changed = false;
        for (std::size_t k = 0; k < num_colors; ++k) {
            if (!locked.empty() && k < locked.size() && locked[k]) continue;
            if (acc[k].w < 1e-9) continue;

            auto dn = acc[k].w;
            auto lab = color_space::OKLab{
                static_cast<float>(acc[k].L / dn),
                static_cast<float>(acc[k].a / dn),
                static_cast<float>(acc[k].b / dn),
            };
            auto new_color = color_space::oklab_to_linear(lab).clamped();
            if (is_stf) new_color = palette::quantize_to_stf(new_color);
            else if (snap_to_discrete) new_color = palette::quantize_to_ocs(new_color);

            // Check convergence (discrete: exact comparison; AGA: epsilon)
            auto& old_color = pal.colors[k];
            if (snap_to_discrete) {
                auto oh = is_stf ? palette::linear_to_stf(old_color)
                                 : palette::linear_to_ocs(old_color);
                auto nh = is_stf ? palette::linear_to_stf(new_color)
                                 : palette::linear_to_ocs(new_color);
                if (oh != nh) { old_color = new_color; changed = true; }
            } else {
                auto d = oklab_dist_sq(
                    color_space::linear_to_oklab(old_color),
                    color_space::linear_to_oklab(new_color));
                if (d > 1e-10f) { old_color = new_color; changed = true; }
            }
        }

        if (!changed) break;
    }

    return pal;
}

} // namespace png2amiga::quantize
