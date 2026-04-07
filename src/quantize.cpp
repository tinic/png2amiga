#include "quantize.hpp"
#include "color_space.hpp"
#include "palette.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <span>
#include <thread>
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
                                std::size_t max_colors) {
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

    // Step 2: Weighted median-cut on OCS colors to get initial palette
    // Build a weighted color array (expand by sqrt of weight for rough
    // approximation, then median-cut). Better: do proper weighted median-cut.
    // For simplicity and correctness, use the histogram entries directly.

    // Initial palette via weighted k-means++ seeding
    std::vector<std::uint16_t> palette_ocs(max_colors);

    // Seed first center: the OCS color with the most pixels
    auto max_it = std::max_element(entries.begin(), entries.end(),
        [](auto& a, auto& b) { return a.weight < b.weight; });
    palette_ocs[0] = max_it->ocs_index;

    // k-means++ seeding: pick subsequent centers weighted by distance
    std::vector<float> min_dists(entries.size(),
                                 std::numeric_limits<float>::max());
    for (std::size_t k = 1; k < max_colors; ++k) {
        // Update min distances to nearest existing center
        auto prev_lab = lut.oklab[palette_ocs[k - 1]];
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto lab = lut.oklab[entries[i].ocs_index];
            float dL = lab.L - prev_lab.L;
            float da = lab.a - prev_lab.a;
            float db = lab.b - prev_lab.b;
            float dist = dL * dL + da * da + db * db;
            min_dists[i] = std::min(min_dists[i], dist);
        }
        // Pick the entry with max weighted distance
        float best_score = -1.0f;
        std::size_t best_idx = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            float score = min_dists[i] * static_cast<float>(entries[i].weight);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        palette_ocs[k] = entries[best_idx].ocs_index;
    }

    // Step 3: K-means refinement with brute-force OCS search
    // Each iteration: assign entries to nearest palette color, then for
    // each cluster find the OCS color minimizing total weighted error.
    constexpr int max_iterations = 15;
    std::vector<std::size_t> assignments(entries.size());

    for (int iter = 0; iter < max_iterations; ++iter) {
        // Assign each entry to nearest palette color
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto lab = lut.oklab[entries[i].ocs_index];
            float best_dist = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            for (std::size_t k = 0; k < max_colors; ++k) {
                auto pal_lab = lut.oklab[palette_ocs[k]];
                float dL = lab.L - pal_lab.L;
                float da = lab.a - pal_lab.a;
                float db = lab.b - pal_lab.b;
                float dist = dL * dL + da * da + db * db;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_k = k;
                }
            }
            assignments[i] = best_k;
        }

        // For each cluster, brute-force find the best OCS color
        // Parallelize across clusters
        std::vector<std::uint16_t> new_palette(max_colors);
        bool changed = false;

        auto refine_cluster = [&](std::size_t k) {
            // Collect cluster members
            std::vector<std::size_t> members;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (assignments[i] == k) members.push_back(i);
            }
            if (members.empty()) {
                new_palette[k] = palette_ocs[k];
                return;
            }

            // Try all 4096 OCS colors, find the one minimizing
            // total weighted perceptual error for this cluster
            float best_error = std::numeric_limits<float>::max();
            std::uint16_t best_ocs = palette_ocs[k];

            for (std::uint16_t candidate = 0; candidate < 4096; ++candidate) {
                auto cand_lab = lut.oklab[candidate];
                float total_err = 0.0f;

                for (auto idx : members) {
                    auto lab = lut.oklab[entries[idx].ocs_index];
                    float dL = lab.L - cand_lab.L;
                    float da = lab.a - cand_lab.a;
                    float db = lab.b - cand_lab.b;
                    total_err += (dL * dL + da * da + db * db)
                                 * static_cast<float>(entries[idx].weight);
                }

                if (total_err < best_error) {
                    best_error = total_err;
                    best_ocs = candidate;
                }
            }

            new_palette[k] = best_ocs;
        };

        // Launch threads for cluster refinement
        auto num_threads = std::min(
            static_cast<std::size_t>(std::thread::hardware_concurrency()),
            max_colors);
        if (num_threads < 1) num_threads = 1;

        std::vector<std::thread> threads;
        std::size_t k = 0;

        while (k < max_colors) {
            threads.clear();
            auto batch = std::min(num_threads, max_colors - k);
            for (std::size_t t = 0; t < batch; ++t) {
                threads.emplace_back(refine_cluster, k + t);
            }
            for (auto& t : threads) t.join();
            k += batch;
        }

        // Check for convergence
        for (std::size_t i = 0; i < max_colors; ++i) {
            if (new_palette[i] != palette_ocs[i]) changed = true;
        }
        palette_ocs = new_palette;
        if (!changed) break;
    }

    // Build result palette
    Palette result;
    result.name = "ocs-optimal";
    result.colors.reserve(max_colors);
    for (auto ocs : palette_ocs) {
        result.colors.push_back(lut.linear[ocs]);
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
    auto prefix_sum_3d = [](std::vector<auto>& t) {
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
                   std::size_t max_colors) {
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
                float dL = samples_lab[i].L - centroids[k].L;
                float da = samples_lab[i].a - centroids[k].a;
                float db = samples_lab[i].b - centroids[k].b;
                float d = dL * dL + da * da + db * db;
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
            float dL = nlab.L - centroids[k].L;
            float da = nlab.a - centroids[k].a;
            float db = nlab.b - centroids[k].b;
            if (dL * dL + da * da + db * db > 1e-12f) {
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

    // Sort palette by perceptual luminance (OKLab L) for consistent ordering
    std::sort(result.colors.begin(), result.colors.end(),
              [](const Color3f& a, const Color3f& b) {
                  return color_space::linear_to_oklab(a).L <
                         color_space::linear_to_oklab(b).L;
              });

    return result;
}

// ===========================================================================
// quantize() entry point
// ===========================================================================

Result<Palette> quantize(const Image& image, std::size_t max_colors,
                         Algorithm algo) {
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
        return median_cut(image.pixels(), max_colors);
    case Algorithm::ocs_bruteforce:
        return ocs_bruteforce_quantize(image.pixels(), max_colors);
    }

    return median_cut(image.pixels(), max_colors);
}

} // namespace png2amiga::quantize
