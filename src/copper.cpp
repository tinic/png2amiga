#include "copper.hpp"
#include "bitplane.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "oklab_simd.hpp"
#include "palette.hpp"
#include "palette_locks.hpp"
#include "palette_pairs.hpp"
#include "pipeline.hpp"
#include "quantize.hpp"
#include "quantize_metal.hpp"
#include "ssimulacra2.hpp"
#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace png2amiga::copper {

namespace {

// Find the color in the row that has the highest error against the palette,
// and return the ideal replacement color (the row pixel's nearest unused hue).
// Actually: find the ideal color that would reduce error the most if it
// replaced one palette slot. We do this greedily:
//   1. For each pixel, compute its error against current palette
//   2. Group pixels by their nearest palette entry
//   3. For each palette slot, compute the total error of its assigned pixels
//   4. Find the palette slot whose replacement would save the most error
//   5. The replacement color = centroid of the highest-error unserved cluster

struct SwapCandidate {
    std::size_t slot;       // palette register to replace
    Color3f new_color;      // replacement color
    float error_reduction;  // how much error this swap saves
};

// find_best_swap's per-slot results for one SwapScratch state. A slot's
// centroid swap does not depend on which other slots are excluded, so
// repeated calls between scratch refreshes reuse it.
struct SlotSwapCache {
    struct Entry {
        bool have_reduction = false;
        bool have_color = false;
        float reduction = 0.0f;
        Color3f color{};
    };
    std::vector<Entry> slots;
    void clear() { slots.clear(); }
};

// Per-slot cluster stats (weighted sums + error) from a pass of
// nearest-color assignment. Caller-owned so we can cache across multiple
// find_best_swap calls within a row and only touch the slots a swap
// actually invalidates.
struct SlotStats {
    double sum_L{}, sum_a{}, sum_b{};
    double total_error{};
    double count{};
};

struct SwapScratch {
    std::vector<SlotStats> stats;
    std::vector<std::uint8_t> assignments;
    std::vector<float> pixel_weights;
    std::vector<float> best_dist;            // per-pixel squared distance to its slot
    std::uint64_t valid_for_swap_count = 0;  // sentinel: 0 = invalid
};

// Build assignments + per-slot stats from scratch. Called once per row;
// subsequent swaps within the same row update incrementally via
// refresh_swap_scratch.
void build_swap_scratch(SwapScratch& sc,
                        std::span<const color_space::OKLab> current_lab,
                        std::span<const std::span<const color_space::OKLab>> rows_lab,
                        std::span<const float> weights,
                        std::size_t width,
                        std::span<const float> column_weights,
                        const std::vector<bool>& excluded = {}) {

    auto num_colors = current_lab.size();
    auto total_pixels = rows_lab.size() * width;
    sc.stats.assign(num_colors, {});
    sc.assignments.assign(total_pixels, 0);
    sc.pixel_weights.assign(total_pixels, 0.0f);
    sc.best_dist.assign(total_pixels, 0.0f);

    // SoA SIMD argmin — 8-wide AVX2 / 4-wide WASM SIMD over the
    // palette, broadcast pixel, FMA distances, blend min/index. Built
    // once per call from the current palette + exclusion mask;
    // amortised across all `total_pixels` argmin evaluations.
    oklab_simd::PaletteSoA pal_soa{};
    oklab_simd::fill(current_lab, excluded, pal_soa);
    for (std::size_t r = 0; r < rows_lab.size(); ++r) {
        auto row_w = weights[r];
        auto& rl = rows_lab[r];
        auto base = r * width;
        for (std::size_t x = 0; x < width; ++x) {
            auto am = oklab_simd::argmin(rl[x], pal_soa);
            float col_w = column_weights.empty() ? 1.0f : column_weights[x];
            float w = row_w * col_w;
            auto wd = static_cast<double>(w);
            sc.assignments[base + x] = static_cast<std::uint8_t>(am.index);
            sc.pixel_weights[base + x] = w;
            sc.best_dist[base + x] = am.dist_sq;
            auto& s = sc.stats[am.index];
            s.sum_L += static_cast<double>(rl[x].L) * wd;
            s.sum_a += static_cast<double>(rl[x].a) * wd;
            s.sum_b += static_cast<double>(rl[x].b) * wd;
            s.total_error += static_cast<double>(am.dist_sq) * wd;
            s.count += wd;
        }
    }
}

// After the palette slot `changed_slot` was replaced with a new color,
// refresh only the pixels that need re-evaluation:
//   (a) pixels previously assigned to `changed_slot` — their old slot is
//       now a different color, so they might prefer a different slot.
//   (b) pixels whose current slot is distant but the NEW `changed_slot`
//       color is now their nearest.
// For speed we use `best_dist` to skip (b) whenever the new color is
// farther than the pixel's current distance — common when replacing a
// barely-used slot with something that serves a nearby pixel better.
void refresh_swap_scratch(SwapScratch& sc,
                          std::span<const color_space::OKLab> current_lab,
                          std::span<const std::span<const color_space::OKLab>> rows_lab,
                          std::size_t width,
                          std::uint8_t changed_slot,
                          const std::vector<bool>& excluded = {}) {

    auto num_colors = current_lab.size();
    auto& new_lab = current_lab[changed_slot];

    // Hoist SoA palette build out of the per-pixel inner — current_lab
    // doesn't change inside this call. Used in the was_in_changed
    // branch below to argmin against the full updated palette.
    oklab_simd::PaletteSoA pal_soa{};
    oklab_simd::fill(current_lab, excluded, pal_soa);

    for (std::size_t r = 0; r < rows_lab.size(); ++r) {
        auto& rl = rows_lab[r];
        auto base = r * width;
        for (std::size_t x = 0; x < width; ++x) {
            auto idx = base + x;
            float d_to_new;
            {
                float dL = rl[x].L - new_lab.L;
                float da = rl[x].a - new_lab.a;
                float db = rl[x].b - new_lab.b;
                d_to_new = color_space::fma_dist_sq(dL, da, db);
            }

            bool was_in_changed = sc.assignments[idx] == changed_slot;
            if (!was_in_changed && d_to_new >= sc.best_dist[idx]) {
                // New color doesn't beat current assignment — nothing to do.
                continue;
            }

            // Remove old stats contribution
            auto wd = static_cast<double>(sc.pixel_weights[idx]);
            auto& old_stat = sc.stats[sc.assignments[idx]];
            old_stat.sum_L -= static_cast<double>(rl[x].L) * wd;
            old_stat.sum_a -= static_cast<double>(rl[x].a) * wd;
            old_stat.sum_b -= static_cast<double>(rl[x].b) * wd;
            old_stat.total_error -= static_cast<double>(sc.best_dist[idx]) * wd;
            old_stat.count -= wd;

            // Recompute nearest for this pixel against the full palette
            // (needed only for case (a); for (b) we could take the shortcut
            // "new is better than old" but re-running full nearest-color
            // is simpler and correct when the old slot is gone).
            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            if (was_in_changed) {
                auto am = oklab_simd::argmin(rl[x], pal_soa);
                best_d = am.dist_sq;
                best_k = am.index;
                (void)num_colors;
            } else {
                // The new color beats the old assignment.
                best_d = d_to_new;
                best_k = changed_slot;
            }

            sc.assignments[idx] = static_cast<std::uint8_t>(best_k);
            sc.best_dist[idx] = best_d;
            auto& new_stat = sc.stats[best_k];
            new_stat.sum_L += static_cast<double>(rl[x].L) * wd;
            new_stat.sum_a += static_cast<double>(rl[x].a) * wd;
            new_stat.sum_b += static_cast<double>(rl[x].b) * wd;
            new_stat.total_error += static_cast<double>(best_d) * wd;
            new_stat.count += wd;
        }
    }
}

// Find the best single swap for a scanline.
// current_pal: the current palette (in linear RGB)
// current_lab: precomputed OKLab of current palette
// rows: scanline pixels (current + optional neighbors for smoothing)
// rows_lab: precomputed OKLab of row pixels
// weights: per-row weight (1.0 for current, less for neighbors)
// hist_pool: top-N most-frequent source colors (RGB444-bucketed) from
//   the whole image. When non-empty, the centroid candidate is
//   *snapped to the nearest histogram entry* before being scored —
//   guarantees the chip ends up displaying an actual source color
//   rather than an arbitrary OCS-grid centroid that may match no
//   real pixel. Per ham_convert 1.0.x ("source palette size for
//   lines>0 increased from 16 to 256 — much better picture quality").
SwapCandidate find_best_swap(std::span<const Color3f> current_pal,
                             std::span<const color_space::OKLab> current_lab,
                             std::span<const std::span<const Color3f>> rows,
                             std::span<const std::span<const color_space::OKLab>> rows_lab,
                             std::span<const float> weights,
                             amiga::Chipset chipset,
                             SwapScratch& sc,
                             const std::vector<bool>& excluded = {},
                             std::span<const float> column_weights = {},
                             std::span<const Color3f> hist_pool = {},
                             std::span<const color_space::OKLab> hist_pool_lab = {},
                             SlotSwapCache* cache = nullptr) {

    (void)rows;
    (void)current_lab;
    (void)weights;
    (void)column_weights;
    auto num_colors = current_pal.size();
    auto width = rows_lab[0].size();
    auto& stats = sc.stats;
    auto& assignments = sc.assignments;
    auto& pixel_weights = sc.pixel_weights;

    // For each slot, compute what happens if we replace it with its
    // assigned pixels' centroid (the ideal color for that cluster).
    // The slot with the highest error reduction wins.
    SwapCandidate best{0, {}, -1.0f};
    if (cache && cache->slots.size() != num_colors) cache->slots.assign(num_colors, {});

    for (std::size_t k = 0; k < num_colors; ++k) {
        if (stats[k].count < 0.001) continue;
        if (!excluded.empty() && excluded[k]) continue;
        SlotSwapCache::Entry* ce = cache ? &cache->slots[k] : nullptr;

        // Ideal centroid for this cluster (weighted by neighbor rows)
        auto n = stats[k].count;
        color_space::OKLab centroid{
            static_cast<float>(stats[k].sum_L / n),
            static_cast<float>(stats[k].sum_a / n),
            static_cast<float>(stats[k].sum_b / n),
        };

        float reduction = 0.0f;
        if (ce && ce->have_reduction) {
            reduction = ce->reduction;
        } else {
            // Error with the centroid — use cached assignments
            double new_error = 0.0;
            for (std::size_t r = 0; r < rows_lab.size(); ++r) {
                auto& rl = rows_lab[r];
                auto base = r * width;
                for (std::size_t x = 0; x < width; ++x) {
                    if (assignments[base + x] != k) continue;
                    float dL = rl[x].L - centroid.L;
                    float da = rl[x].a - centroid.a;
                    float db = rl[x].b - centroid.b;
                    new_error += static_cast<double>(color_space::fma_dist_sq(dL, da, db)) *
                                 static_cast<double>(pixel_weights[base + x]);
                }
            }
            reduction = static_cast<float>(stats[k].total_error - new_error);
            if (ce) {
                ce->have_reduction = true;
                ce->reduction = reduction;
            }
        }

        if (reduction > best.error_reduction) {
            Color3f linear{};
            if (ce && ce->have_color) {
                linear = ce->color;
            } else {
                linear = color_space::oklab_to_linear(centroid).clamped();
                // Snap to chipset precision
                if (chipset != amiga::Chipset::aga) {
                    linear = palette::quantize_to_ocs(linear);
                }
                if (ce) {
                    ce->have_color = true;
                    ce->color = linear;
                }
            }
            // [Histogram-pool snap experiment: tested and rejected.
            //  Snapping centroid to the nearest source color in a
            //  256-entry histogram regressed lores+sliced by ~0.5 dB
            //  averaged across 4 images. Reason: centroid is the
            //  optimal-for-cluster color; snapping it onto a strict
            //  256-color subset of the 4096-RGB444-gamut loses
            //  optimization headroom. The OCS snap above already
            //  produces a chip-displayable color. Argument kept on
            //  the signature for future use; unused at default empty
            //  span.]
            (void)hist_pool;
            (void)hist_pool_lab;
            best = {k, linear, reduction};
        }
    }

    return best;
}

// EHB-aware swap planner. SwapScratch and current_pal/current_lab are
// sized 64 (32 base + 32 hardware half-brite). This function only
// considers swaps to base slots 0..31 — half-brite slots 32..63 are
// hardware-derived from the base they mirror, so they're never directly
// modifiable. A swap on base slot k cascades to slot k+32 = halve(C),
// and the score reflects that: cur error from cluster k AND cluster
// k+32 vs new error from cluster k against C plus cluster k+32 against
// halve(C).
//
// Two candidate colors are tried per slot: the base cluster's centroid
// (best for cluster k alone), and the half-brite cluster's centroid
// doubled in sRGB (best for cluster k+32 alone). Whichever gives the
// larger combined-cluster error reduction wins. If cluster k+32 is
// empty, only the base centroid is tried; if cluster k is (near-)empty,
// the slot is left alone (see kTwinCaptureMinBase).
//
// A third candidate returns a moved slot to its seed colour (seed_pal[k],
// the whole-image EHB palette). Centroid candidates can only follow the
// pixels a slot already owns, so a colour evicted on some rows never
// comes back once those rows end: its pixels now sit in a neighbour's
// cluster. DIV2K 0779: the sky highlight #DEF (slot 31) went to the beak
// at row 63 and never returned, so the lighter sky below lost its upper
// dither bracket while slot 30 crept CDE->CEF->DEF one band at a time.
// The seed candidate is scored with re-assignment (pixels may move into
// it from any slot, and pixels leaving slot k fall back to their next
// nearest entry), since the cluster-local score cannot see those pixels.
// reserved64 flags reserve slots (and their twins) no pixel may use.
SwapCandidate find_best_swap_ehb(std::span<const color_space::OKLab> current_lab,
                                 std::span<const std::span<const color_space::OKLab>> rows_lab,
                                 SwapScratch& sc,
                                 const std::vector<bool>& excluded,
                                 std::span<const Color3f> seed_pal,
                                 const std::vector<bool>& reserved64) {

    constexpr std::size_t kBase = 32;
    auto width = rows_lab[0].size();
    auto& stats = sc.stats;
    auto& assignments = sc.assignments;
    auto& pixel_weights = sc.pixel_weights;

    SwapCandidate best{0, {}, -1.0f};

    for (std::size_t k = 0; k < kBase; ++k) {
        if (!excluded.empty() && k < excluded.size() && excluded[k]) continue;
        auto count_b = stats[k].count;
        auto count_h = stats[k + kBase].count;
        if (count_b < 0.001 && count_h < 0.001) continue;

        if (k < seed_pal.size()) {
            auto seed_lin = palette::quantize_to_ocs(seed_pal[k]);
            auto c_lab = color_space::linear_to_oklab(seed_lin);
            const auto& cur = current_lab[k];
            if (color_space::fma_dist_sq(c_lab.L - cur.L, c_lab.a - cur.a, c_lab.b - cur.b) >
                1e-8f) {
                auto h_lab = color_space::linear_to_oklab(palette::half_brite(seed_lin));
                double reduction = 0.0;
                for (std::size_t r = 0; r < rows_lab.size(); ++r) {
                    auto& rl = rows_lab[r];
                    auto base = r * width;
                    for (std::size_t x = 0; x < width; ++x) {
                        auto i = base + x;
                        const auto& p = rl[x];
                        float d = std::min(
                            color_space::fma_dist_sq(p.L - c_lab.L, p.a - c_lab.a, p.b - c_lab.b),
                            color_space::fma_dist_sq(p.L - h_lab.L, p.a - h_lab.a, p.b - h_lab.b));
                        auto a = assignments[i];
                        if (a == k || a == k + kBase) {
                            for (std::size_t j = 0; j < current_lab.size(); ++j) {
                                if (j == k || j == k + kBase) continue;
                                if (!reserved64.empty() && reserved64[j]) continue;
                                const auto& e = current_lab[j];
                                d = std::min(
                                    d, color_space::fma_dist_sq(p.L - e.L, p.a - e.a, p.b - e.b));
                            }
                        } else {
                            d = std::min(d, sc.best_dist[i]);
                        }
                        reduction += static_cast<double>(sc.best_dist[i] - d) *
                                     static_cast<double>(pixel_weights[i]);
                    }
                }
                if (static_cast<float>(reduction) > best.error_reduction) {
                    best = {k, seed_lin, static_cast<float>(reduction)};
                }
            }
        }

        double cur_err = stats[k].total_error + stats[k + kBase].total_error;

        auto try_candidate = [&](Color3f c_lin) {
            c_lin = palette::quantize_to_ocs(c_lin);
            auto c_lab = color_space::linear_to_oklab(c_lin);
            auto h_lab = color_space::linear_to_oklab(palette::half_brite(c_lin));
            double new_err = 0.0;
            for (std::size_t r = 0; r < rows_lab.size(); ++r) {
                auto& rl = rows_lab[r];
                auto base = r * width;
                for (std::size_t x = 0; x < width; ++x) {
                    auto a = assignments[base + x];
                    if (a != k && a != k + kBase) continue;
                    const auto& ref = (a == k) ? c_lab : h_lab;
                    float dL = rl[x].L - ref.L;
                    float da = rl[x].a - ref.a;
                    float db = rl[x].b - ref.b;
                    new_err += static_cast<double>(color_space::fma_dist_sq(dL, da, db)) *
                               static_cast<double>(pixel_weights[base + x]);
                }
            }
            float reduction = static_cast<float>(cur_err - new_err);
            if (reduction > best.error_reduction) {
                best = {k, c_lin, reduction};
            }
        };

        if (count_b > 0.001) {
            color_space::OKLab cb{static_cast<float>(stats[k].sum_L / count_b),
                                  static_cast<float>(stats[k].sum_a / count_b),
                                  static_cast<float>(stats[k].sum_b / count_b)};
            try_candidate(color_space::oklab_to_linear(cb).clamped());
        }
        // The half-brite twin may only re-aim a slot whose base half still
        // serves pixels here. Otherwise a slot idle on these rows (e.g. the
        // white highlight base) gets captured for its twin's cluster, and
        // since candidates only come from cluster centroids, nothing ever
        // proposes the base colour again: later rows lose it and dither
        // clips to the next-darker entry (DIV2K 0041: S2 -17 vs plain EHB).
        // Same 0.5 weighted-px emptiness bar as kStealEmptyThreshold.
        constexpr double kTwinCaptureMinBase = 0.5;
        if (count_h > 0.001 && count_b >= kTwinCaptureMinBase) {
            color_space::OKLab ch{static_cast<float>(stats[k + kBase].sum_L / count_h),
                                  static_cast<float>(stats[k + kBase].sum_a / count_h),
                                  static_cast<float>(stats[k + kBase].sum_b / count_h)};
            auto h_lin = color_space::oklab_to_linear(ch).clamped();
            auto h_srgb = color_space::linear_to_srgb(h_lin).clamped();
            Color3f doubled_srgb{std::min(2.0f * h_srgb.r, 1.0f),
                                 std::min(2.0f * h_srgb.g, 1.0f),
                                 std::min(2.0f * h_srgb.b, 1.0f)};
            try_candidate(color_space::srgb_to_linear(doubled_srgb).clamped());
        }
        // Suppress unused-parameter warning when current_lab path drops.
        (void)current_lab;
    }
    return best;
}

// Dithered-result check for lores sliced swaps. find_best_swap scores a swap
// by nearest-colour error of the slot's own cluster, which cannot see that
// the ditherer mixes a flat area from two or more entries: the second entry
// owns few nearest pixels, looks free, and gets taken. The area then mixes
// with a far colour (red speckle in 0256's black, a light dot band in 0386's
// sky) while the planner's nearest-colour error goes down.
//
// This window runs the real sliced picker (dither::DiffuseRows, the row form
// of diffuse_raw_buffer, with pass 2's nearest pick) over rows y-kWarm .. y+kAhead.
// Rows above y keep the palettes already planned, so error arrives at row
// y as it does in the output. Rows from y on use the trial palette, since a
// swap stays in effect until a later row changes the slot again. Interlace:
// rows of the other field use that field's current palette.
//
// A trial is scored by two terms over rows y.. (weighted decay^(row-y)):
//   blurred  error of the result against the source after a horizontal box
//            (half-widths 1, 3, 7) and a vertical [1 2 1] filter, summed
//            over the three scales: what the eye averages the dither into.
//   raw      per-pixel error, the dither noise.
// A swap is kept only if blurred drops and raw does not rise. Blurred alone
// accepts trades of a small mean shift for new noise in flat areas (0718's
// dark background: B-channel noise for a 2/255 mean gain, S2 -7).
//
// Applied (with the refined seed below) up to 32 colours. Each trial is a
// window dither whose cost grows with the palette; at lores d8 AGA a frame
// took 45-60 s instead of 3 s, and the old planner already scores above plain
// there (0779: 84.8 vs 82.5).
constexpr std::size_t kDitherCheckMaxColors = 32;

// Trials DitherWindow::try_batch scores in parallel. The result does not
// depend on it.
std::size_t trial_batch() {
    static const std::size_t n = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
    return n;
}

class DitherWindow {
public:
    static constexpr std::size_t kWarm = 4;
    static constexpr std::size_t kAhead = 8;

    struct Score {
        double blurred{};
        double raw{};
    };

    // `source`: dither::diffuse_source(image).
    DitherWindow(const Image& image,
                 const std::vector<std::vector<color_space::OKLab>>& all_lab,
                 const dither::DiffuseSource& source,
                 const dither::Settings& settings,
                 std::size_t y,
                 bool is_lace,
                 float decay)
        : all_lab_(all_lab), settings_(settings), width_(image.width()), y_(y), lace_(is_lace) {
        auto height = image.height();
        y0_ = (!is_lace && y >= kWarm) ? y - kWarm
                                       : (is_lace && y >= 2 * kWarm ? y - 2 * kWarm : 0);
        auto y1 = std::min(height - 1, y + kAhead);
        rows_ = y1 - y0_ + 1;
        std::vector<Color3f> px(width_ * rows_);
        for (std::size_t r = 0; r < rows_; ++r) {
            auto src = image.row(y0_ + r);
            std::copy(src.begin(), src.end(), px.begin() + static_cast<std::ptrdiff_t>(r * width_));
        }
        win_ = Image(width_, rows_, std::move(px));
        diffuser_.emplace(win_, settings_, &source, y0_);
        row_w_.resize(rows_);
        for (std::size_t r = 0; r < rows_; ++r) {
            auto gy = y0_ + r;
            row_w_[r] = gy < y ? 0.0f : std::pow(decay, static_cast<float>(gy - y));
        }
        first_trial_ = y - y0_;
        nearest_ = !dither::is_yliluoma(settings.method);
        fixed_.resize(rows_);
        fixed_s_.resize(rows_);
        init(inc_);
    }

    // Palette (usable entries only, OKLab) for window row r when that row is
    // not driven by the trial palette: a row above y, or the other field.
    void set_fixed(std::size_t r, std::vector<color_space::OKLab> lab) {
        fixed_s_[r] = to_srgb(lab);
        fixed_[r] = std::move(lab);
    }
    std::size_t rows() const { return rows_; }
    std::size_t row_y(std::size_t r) const { return y0_ + r; }
    bool uses_trial(std::size_t r) const {
        auto gy = y0_ + r;
        return gy >= y_ && (!lace_ || ((gy ^ y_) & 1) == 0);
    }

    // The palette from row y on before any swap. Rows above y do not depend
    // on it: they are dithered here, once. Its rows from y on run with the
    // first batch of trials.
    void set_incumbent(std::span<const color_space::OKLab> trial) {
        inc_pal_.assign(trial.begin(), trial.end());
        inc_pal_s_ = to_srgb(inc_pal_);
        auto state = diffuser_->start();
        Score unused;
        scratch_.init(width_);
        run_into(inc_, scratch_, 0, first_trial_, state, inc_pal_, inc_pal_s_, nullptr, unused);
        diffuser_->save(state, first_trial_, inc_.snap[first_trial_]);
        pending_ = true;
    }

    // Trials are judged as a full window dither would judge them: kept
    // only if blurred drops below the incumbent's blurred and raw does not
    // rise above its raw. The work is less:
    //   - rows before the first one whose picks can change are the
    //     incumbent's: resume from its saved error state there.
    //   - both terms are sums of non-negative row terms in a fixed order,
    //     so a partial sum that already fails the test rejects the trial.
    //   - all trials of a batch are scored against the same incumbent in
    //     parallel; the first kept one wins, as in a one-by-one scan.
    // Returns the index of the first trial kept (now the incumbent), or
    // trials.size() if none is.
    std::size_t try_batch(std::span<const std::vector<color_space::OKLab>> trials) {
        const auto n = trials.size();
        // Trial buffers outlive the window (one per row): kept per thread
        // and only resized.
        thread_local std::vector<std::unique_ptr<Work>> pool;
        while (pool.size() < n)
            pool.push_back(std::make_unique<Work>());
        for (; ready_ < n; ++ready_) {
            init(pool[ready_]->t);
            pool[ready_]->scratch.init(width_);
        }
        auto& work_ = pool;
        if (pending_) {
            // Incumbent rows from y on in parallel with the trials, which
            // then start at row y and have no bound to stop early against.
            pipeline::parallel_for(n + 1, [&](std::size_t i) {
                if (i == n) {
                    diffuser_->restore(inc_.snap[first_trial_], first_trial_, inc_state_);
                    auto& state = inc_state_;
                    run_into(inc_,
                             scratch_,
                             first_trial_,
                             rows_,
                             state,
                             inc_pal_,
                             inc_pal_s_,
                             nullptr,
                             cur_);
                } else {
                    evaluate(*work_[i], trials[i], nullptr);
                }
            });
            pending_ = false;
            for (std::size_t i = 0; i < n; ++i) {
                auto& w = *work_[i];
                w.kept = w.done && w.score.blurred < cur_.blurred && w.score.raw <= cur_.raw;
            }
        } else if (n == 1) {
            evaluate(*work_[0], trials[0], &cur_);
        } else {
            pipeline::parallel_for(n,
                                   [&](std::size_t i) { evaluate(*work_[i], trials[i], &cur_); });
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!work_[i]->kept) continue;
            adopt(*work_[i], trials[i]);
            cur_ = work_[i]->score;
            return i;
        }
        return n;
    }

private:
    struct Trace {
        // Per pixel: picker target, its distance to the picked entry and
        // that entry's index.
        std::vector<color_space::OKLab> target;
        std::vector<float> best_d;
        std::vector<std::uint8_t> best_k;
        // Horizontal box of the error at half-widths 1, 3, 7.
        std::array<std::vector<color_space::OKLab>, 3> box;
        // Weighted per-row terms of raw and of blurred (per box width).
        std::vector<double> raw_t;
        std::array<std::vector<double>, 3> blur_t;
        // Diffusion state before each row from the first trial row on
        // (DiffuseRows::save form).
        std::vector<dither::DiffuseState> snap;
    };
    // Per-row buffers of one window dither.
    struct Scratch {
        std::vector<color_space::OKLab> err_row;
        std::vector<double> pL, pa, pb;
        void init(std::size_t width) {
            err_row.resize(width);
            pL.resize(width + 1);
            pa.resize(width + 1);
            pb.resize(width + 1);
        }
    };
    // One trial's scratch.
    struct Work {
        Trace t;
        Scratch scratch;
        std::vector<Color3f> trial_s;
        std::size_t r0{};
        dither::DiffuseState state;
        bool done{};  // dithered to the last row (score is final)
        bool kept{};
        Score score;
    };
    static constexpr std::array<std::size_t, 3> kHalfWidths{1, 3, 7};

    void init(Trace& t) const {
        t.target.resize(width_ * rows_);
        t.best_d.resize(width_ * rows_);
        t.best_k.resize(width_ * rows_);
        for (auto& b : t.box)
            b.resize(width_ * rows_);
        t.raw_t.assign(rows_, 0.0);
        for (auto& b : t.blur_t)
            b.assign(rows_, 0.0);
        t.snap.resize(rows_);
    }

    static std::vector<Color3f> to_srgb(std::span<const color_space::OKLab> lab) {
        std::vector<Color3f> s(lab.size());
        for (std::size_t k = 0; k < lab.size(); ++k)
            s[k] = color_space::linear_to_srgb(color_space::oklab_to_linear(lab[k])).clamped();
        return s;
    }

    // Scores one trial into w (reads only shared state, so trials run in
    // parallel). With `cur`, the incumbent is complete: the trial resumes
    // where its picks can first differ, stops once it cannot beat cur, and
    // w.kept is the verdict. Without, it starts at row y and runs to the end.
    void evaluate(Work& w, std::span<const color_space::OKLab> trial, const Score* cur) const {
        w.kept = false;
        w.done = false;
        w.trial_s = to_srgb(trial);
        std::size_t r0 = first_trial_;
        if (cur && nearest_ && trial.size() == inc_pal_.size()) r0 = first_divergent_row(trial);
        w.r0 = r0;
        if (r0 >= rows_) return;  // same picks everywhere: same score, not lower
        auto& t = w.t;
        // Rows before r0 are the incumbent's; their box rows feed the
        // vertical filter of rows r0-1 and r0.
        for (std::size_t r = (r0 >= 2 ? r0 - 2 : 0); r < r0; ++r)
            for (std::size_t s = 0; s < 3; ++s)
                std::copy_n(inc_.box[s].begin() + static_cast<std::ptrdiff_t>(r * width_),
                            width_,
                            t.box[s].begin() + static_cast<std::ptrdiff_t>(r * width_));
        for (std::size_t r = 0; r < r0; ++r)
            t.raw_t[r] = inc_.raw_t[r];
        for (std::size_t s = 0; s < 3; ++s)
            for (std::size_t r = 0; r + 1 < r0; ++r)
                t.blur_t[s][r] = inc_.blur_t[s][r];
        Score sc;
        diffuser_->restore(inc_.snap[r0], r0, w.state);
        auto& state = w.state;
        if (!run_into(t, w.scratch, r0, rows_, state, trial, w.trial_s, cur, sc)) return;
        w.done = true;
        w.score = sc;
        if (cur) w.kept = sc.blurred < cur->blurred && sc.raw <= cur->raw;
    }

    // Makes a kept trial the incumbent from its first changed row on.
    void adopt(Work& w, std::span<const color_space::OKLab> trial) {
        auto& c = w.t;
        const auto r0 = w.r0;
        auto off = static_cast<std::ptrdiff_t>(r0 * width_);
        std::copy(c.target.begin() + off, c.target.end(), inc_.target.begin() + off);
        std::copy(c.best_d.begin() + off, c.best_d.end(), inc_.best_d.begin() + off);
        std::copy(c.best_k.begin() + off, c.best_k.end(), inc_.best_k.begin() + off);
        for (std::size_t s = 0; s < 3; ++s) {
            std::copy(c.box[s].begin() + off, c.box[s].end(), inc_.box[s].begin() + off);
            inc_.blur_t[s] = c.blur_t[s];
        }
        inc_.raw_t = c.raw_t;
        for (std::size_t r = r0 + 1; r < rows_; ++r)
            std::swap(inc_.snap[r], c.snap[r]);
        inc_pal_.assign(trial.begin(), trial.end());
        std::swap(inc_pal_s_, w.trial_s);
    }

    // First trial row with a pixel whose pick can differ from the
    // incumbent's: it picked a changed entry, or a changed entry is as near
    // to its target (with margin for rounding) as the pick.
    std::size_t first_divergent_row(std::span<const color_space::OKLab> trial) const {
        std::array<std::size_t, 256> changed{};
        std::size_t n_changed = 0;
        for (std::size_t k = 0; k < trial.size() && k < changed.size(); ++k) {
            const auto& a = trial[k];
            const auto& b = inc_pal_[k];
            if (a.L != b.L || a.a != b.a || a.b != b.b) changed[n_changed++] = k;
        }
        if (n_changed == 0) return rows_;
        for (std::size_t r = first_trial_; r < rows_; ++r) {
            if (!uses_trial(r)) continue;
            for (std::size_t x = 0; x < width_; ++x) {
                auto i = r * width_ + x;
                const auto& t = inc_.target[i];
                float bound = inc_.best_d[i] * 1.0001f + 1e-12f;
                for (std::size_t c = 0; c < n_changed; ++c) {
                    auto k = changed[c];
                    if (inc_.best_k[i] == k) return r;
                    const auto& e = trial[k];
                    if (color_space::fma_dist_sq(t.L - e.L, t.a - e.a, t.b - e.b) <= bound)
                        return r;
                }
            }
        }
        return rows_;
    }

    // Lower bounds of the final sums: the same ordered sums over the terms
    // known so far (unknown terms are >= 0).
    Score partial(const Trace& t, std::size_t raw_rows, std::size_t blur_rows) const {
        Score sc;
        for (std::size_t r = 0; r < raw_rows; ++r)
            if (row_w_[r] != 0.0f) sc.raw += t.raw_t[r];
        for (std::size_t s = 0; s < 3; ++s)
            for (std::size_t r = 0; r < blur_rows; ++r)
                if (row_w_[r] != 0.0f) sc.blurred += t.blur_t[s][r];
        return sc;
    }

    void blur_term(Trace& t, std::size_t r) const {
        if (row_w_[r] == 0.0f) return;
        for (std::size_t s = 0; s < 3; ++s) {
            const auto& box = t.box[s];
            double acc = 0.0;
            for (std::size_t x = 0; x < width_; ++x) {
                const auto& c = box[r * width_ + x];
                float L = 2.0f * c.L, a = 2.0f * c.a, b = 2.0f * c.b, tw = 2.0f;
                if (r > 0) {
                    const auto& u = box[(r - 1) * width_ + x];
                    L += u.L;
                    a += u.a;
                    b += u.b;
                    tw += 1.0f;
                }
                if (r + 1 < rows_) {
                    const auto& d = box[(r + 1) * width_ + x];
                    L += d.L;
                    a += d.a;
                    b += d.b;
                    tw += 1.0f;
                }
                acc += static_cast<double>(color_space::fma_dist_sq(L / tw, a / tw, b / tw));
            }
            t.blur_t[s][r] = static_cast<double>(row_w_[r]) * acc;
        }
    }

    // Dithers rows r0..r1-1 into t from `state` (the state before row r0;
    // on return the state before r1). Saves the state before each row after
    // r0 from the first trial row on. With `bound`, returns false as soon as
    // the partial sums show the trial cannot beat it. Otherwise returns true
    // and, if r1 is the last row, fills `out`.
    bool run_into(Trace& t,
                  Scratch& scratch,
                  std::size_t r0,
                  std::size_t r1,
                  dither::DiffuseState& state,
                  std::span<const color_space::OKLab> trial,
                  std::span<const Color3f> trial_s,
                  const Score* bound,
                  Score& out) const {
        auto& err_row = scratch.err_row;
        std::span<const color_space::OKLab> pal;
        std::span<const Color3f> pal_s;
        auto pick = [&](const color_space::OKLab& target,
                        std::size_t x,
                        std::size_t r) -> dither::PickResult {
            std::size_t k = 0;
            color_space::OKLab chosen{};
            // The diffuser reads only the chosen colour, so a nearest pick
            // skips the threshold's second-nearest search.
            float thr = 0.5f;
            if (nearest_) {
                k = dither::nearest_palette_index(target, pal);
                chosen = pal[k];
            } else {
                thr = dither::pick_palette_index_with_ostro(settings_.method,
                                                            target,
                                                            pal,
                                                            x,
                                                            y0_ + r,
                                                            settings_.strength,
                                                            /*k_min=*/0,
                                                            k,
                                                            chosen);
            }
            const auto& s = all_lab_[y0_ + r][x];
            err_row[x] = {chosen.L - s.L, chosen.a - s.a, chosen.b - s.b};
            auto i = r * width_ + x;
            t.target[i] = target;
            t.best_d[i] = color_space::fma_dist_sq(
                target.L - chosen.L, target.a - chosen.a, target.b - chosen.b);
            t.best_k[i] = static_cast<std::uint8_t>(k);
            return {chosen, thr, &pal_s[k]};
        };
        dither::PixelPicker picker = pick;
        auto& pL = scratch.pL;
        auto& pa = scratch.pa;
        auto& pb = scratch.pb;
        for (std::size_t row = r0; row < r1; ++row) {
            if (row >= first_trial_ && row > r0) diffuser_->save(state, row, t.snap[row]);
            if (uses_trial(row)) {
                pal = trial;
                pal_s = trial_s;
            } else {
                pal = fixed_[row];
                pal_s = fixed_s_[row];
            }
            diffuser_->row(state, row, picker);

            if (row_w_[row] != 0.0f) {
                double acc = 0.0;
                for (std::size_t x = 0; x < width_; ++x) {
                    const auto& e = err_row[x];
                    acc += static_cast<double>(color_space::fma_dist_sq(e.L, e.a, e.b));
                }
                t.raw_t[row] = static_cast<double>(row_w_[row]) * acc;
            }
            pL[0] = pa[0] = pb[0] = 0.0;
            for (std::size_t x = 0; x < width_; ++x) {
                const auto& e = err_row[x];
                pL[x + 1] = pL[x] + static_cast<double>(e.L);
                pa[x + 1] = pa[x] + static_cast<double>(e.a);
                pb[x + 1] = pb[x] + static_cast<double>(e.b);
            }
            for (std::size_t s = 0; s < 3; ++s) {
                auto hw = kHalfWidths[s];
                auto& box = t.box[s];
                for (std::size_t x = 0; x < width_; ++x) {
                    auto x0 = x >= hw ? x - hw : 0;
                    auto x1 = std::min(width_, x + hw + 1);
                    auto n = static_cast<double>(x1 - x0);
                    box[row * width_ + x] = {static_cast<float>((pL[x1] - pL[x0]) / n),
                                             static_cast<float>((pa[x1] - pa[x0]) / n),
                                             static_cast<float>((pb[x1] - pb[x0]) / n)};
                }
            }
            if (row > 0) blur_term(t, row - 1);
            if (row + 1 == rows_) blur_term(t, row);
            if (bound) {
                auto lb = partial(t, row + 1, row + 1 == rows_ ? rows_ : row);
                if (lb.blurred >= bound->blurred || lb.raw > bound->raw) return false;
            }
        }
        if (r1 == rows_) out = partial(t, rows_, rows_);
        return true;
    }

    const std::vector<std::vector<color_space::OKLab>>& all_lab_;
    const dither::Settings& settings_;
    std::size_t width_;
    std::size_t y_;
    bool lace_;
    bool nearest_{};
    std::size_t y0_{};
    std::size_t rows_{};
    std::size_t first_trial_{};
    Image win_;
    std::optional<dither::DiffuseRows> diffuser_;
    std::vector<float> row_w_;
    std::vector<std::vector<color_space::OKLab>> fixed_;
    std::vector<std::vector<Color3f>> fixed_s_;
    Scratch scratch_;
    std::vector<color_space::OKLab> inc_pal_;
    std::vector<Color3f> inc_pal_s_;
    Trace inc_;
    Score cur_;       // incumbent score
    bool pending_{};  // incumbent rows from y on not dithered yet
    dither::DiffuseState inc_state_;
    std::size_t ready_{};  // pooled trial buffers sized for this window
};

// Sliced pass 2: dither every row against its own palette, never picking an
// excluded slot, then run the DBS post-pass when selected. Fills
// `all_indices` (width x height) and returns the diffuser's total error.
float dither_rows(const Image& image,
                  const dither::Settings& dither_settings,
                  const std::vector<std::vector<Color3f>>& scanline_palettes,
                  const std::vector<bool>& excluded_mask,
                  std::size_t num_colors,
                  std::vector<std::uint8_t>& all_indices) {
    const auto width = image.width();
    const auto height = image.height();
    // Pre-convert each row's sliced palette to OKLab once. excluded_mask
    // (built once at the top of encode_copper) flags slots the dither
    // pass refuses to pick — same set the swap planner ignored, so pass
    // 1 and pass 2 share a single coherent view of "usable slots."
    std::vector<std::vector<color_space::OKLab>> pal_lab_per_row(height);
    std::vector<std::vector<std::uint8_t>> cand_to_full_per_row;
    if (!excluded_mask.empty()) cand_to_full_per_row.resize(height);
    for (std::size_t y = 0; y < height; ++y) {
        auto& pal = scanline_palettes[y];
        if (excluded_mask.empty()) {
            pal_lab_per_row[y].resize(num_colors);
            for (std::size_t i = 0; i < num_colors; ++i)
                pal_lab_per_row[y][i] = color_space::linear_to_oklab(pal[i]);
        } else {
            pal_lab_per_row[y].reserve(num_colors);
            cand_to_full_per_row[y].reserve(num_colors);
            for (std::size_t i = 0; i < num_colors; ++i) {
                if (excluded_mask[i]) continue;
                pal_lab_per_row[y].push_back(color_space::linear_to_oklab(pal[i]));
                cand_to_full_per_row[y].push_back(static_cast<std::uint8_t>(i));
            }
        }
    }

    float total_error = dither::diffuse_raw_buffer(
        image,
        dither_settings,
        [&](const color_space::OKLab& target,
            std::size_t x,
            std::size_t y) -> dither::PickResult {
            auto& pal_lab = pal_lab_per_row[y];
            std::size_t k = 0;
            color_space::OKLab chosen{};
            float thr = dither::pick_palette_index_with_ostro(dither_settings.method,
                                                              target,
                                                              pal_lab,
                                                              x,
                                                              y,
                                                              dither_settings.strength,
                                                              /*k_min=*/0,
                                                              k,
                                                              chosen);
            std::uint8_t full_idx = excluded_mask.empty() ? static_cast<std::uint8_t>(k)
                                                          : cand_to_full_per_row[y][k];
            all_indices[y * width + x] = full_idx;
            return {chosen, thr};
        });

    // DBS post-pass refinement for sliced. The base picker above already
    // picked a sensible per-row palette index per pixel; DBS sweeps
    // and toggles indices to lower the HVS-blurred OKLab cost,
    // respecting that each row has a different palette.
    if (dither_settings.method == dither::Method::dbs) {
        if (excluded_mask.empty()) {
            dither::apply_dbs_post_pass(
                image,
                all_indices,
                [&](std::size_t /*x*/, std::size_t y) -> std::span<const color_space::OKLab> {
                    return pal_lab_per_row[y];
                });
        } else {
            // Translate full indices → candidate-space, run DBS, translate back.
            std::vector<std::vector<std::uint8_t>> full_to_cand_per_row(height);
            for (std::size_t y = 0; y < height; ++y) {
                full_to_cand_per_row[y].assign(num_colors, 255);
                auto& cand = cand_to_full_per_row[y];
                for (std::size_t k = 0; k < cand.size(); ++k)
                    full_to_cand_per_row[y][cand[k]] = static_cast<std::uint8_t>(k);
            }
            std::vector<std::uint8_t> cand_indices(all_indices.size());
            for (std::size_t i = 0; i < all_indices.size(); ++i) {
                auto y = i / width;
                cand_indices[i] = full_to_cand_per_row[y][all_indices[i]];
            }
            dither::apply_dbs_post_pass(
                image,
                cand_indices,
                [&](std::size_t /*x*/, std::size_t y) -> std::span<const color_space::OKLab> {
                    return pal_lab_per_row[y];
                });
            for (std::size_t i = 0; i < cand_indices.size(); ++i) {
                auto y = i / width;
                all_indices[i] = cand_to_full_per_row[y][cand_indices[i]];
            }
        }
    }
    return total_error;
}

// Exact-colour plan for hardware-ready pixel art. The clustering planner in
// encode_copper minimises error and never aims for identity, so it merges
// distinct colours even when every row's colours fit the palette (an 8-colour
// row at depth 3 came out with 5.5 slots in use). Here the free slots act as
// a cache of the row colours snapped to the chipset grid: a row's colours must
// be resident, each load costs one change, and the base palette loads for
// free. A row that cannot afford its loads gets them earlier, in the spare
// changes of the rows above, evicting the colour needed farthest ahead
// (Belady). Rows with more colours than slots, or loads no row above could
// take, keep the nearest colours via the dither pass.
//
// Interlace plans each field on its own (a row follows the row two above it
// on the beam). Rows before `skip_initial_swap_rows` take no changes.
struct ExactPlan {
    std::vector<Color3f> base_pal;
    std::vector<std::vector<Color3f>> scanline_palettes;
    std::size_t inexact_rows = 0;
    bool off_grid = false;  // some source pixel is not a chipset colour
};

std::optional<ExactPlan> plan_exact_copper(
    const Image& image,
    std::size_t num_colors,
    std::size_t changes_per_line,
    amiga::Chipset chipset,
    bool lock_color0,
    const std::vector<std::pair<std::size_t, Color3f>>& locked,
    const std::vector<bool>& excluded_mask,
    std::size_t skip_initial_swap_rows,
    bool is_lace) {
    const auto width = image.width();
    const auto height = image.height();
    const bool aga = chipset == amiga::Chipset::aga;
    auto key_of = [aga](Color3f c) -> std::uint32_t {
        return aga ? palette::linear_to_aga(c) : palette::linear_to_ocs(c);
    };
    auto color_of = [aga](std::uint32_t key) {
        return aga ? palette::aga_to_linear(key)
                   : palette::ocs_to_linear(static_cast<std::uint16_t>(key));
    };

    // Slot roles: fixed slots hold a colour for the whole frame (colour 0
    // black, --lock-index); excluded slots are never shown; the rest are free.
    std::vector<Color3f> fixed_color(num_colors, Color3f{0.0f, 0.0f, 0.0f});
    std::vector<bool> fixed(num_colors, false);
    if (lock_color0 && num_colors > 1) fixed[0] = true;
    for (auto& [idx, color] : locked) {
        if (idx < num_colors) {
            fixed[idx] = true;
            fixed_color[idx] = color;
        }
    }
    std::vector<std::size_t> free_slots;
    std::vector<std::uint32_t> fixed_keys;
    for (std::size_t k = 0; k < num_colors; ++k) {
        bool excluded = k < excluded_mask.size() && excluded_mask[k];
        if (fixed[k] && !excluded) fixed_keys.push_back(key_of(fixed_color[k]));
        if (!fixed[k] && !excluded) free_slots.push_back(k);
    }
    const std::size_t free_count = free_slots.size();

    // Per row: the distinct snapped colours a free slot must hold, most
    // pixels first. Colours get dense ids.
    std::vector<std::uint32_t> color_keys;
    std::vector<std::vector<std::uint32_t>> needs(height);
    std::size_t rows_fit = 0;
    bool off_grid = false;
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> row_keys;  // key, count
        std::vector<std::pair<std::uint32_t, std::uint32_t>> key_to_id;  // sorted
        for (std::size_t y = 0; y < height; ++y) {
            row_keys.clear();
            auto row = image.row(y);
            for (std::size_t x = 0; x < width; ++x) {
                auto key = key_of(row[x]);
                if (!aga && !off_grid)
                    off_grid = palette::linear_to_aga(row[x]) !=
                               palette::linear_to_aga(color_of(key));
                if (std::ranges::find(fixed_keys, key) != fixed_keys.end()) continue;
                auto it = std::ranges::find(row_keys, key, &std::pair<std::uint32_t, std::uint32_t>::first);
                if (it != row_keys.end()) {
                    ++it->second;
                    continue;
                }
                // A photo row has hundreds of colours: give up early.
                if (row_keys.size() >= 4 * num_colors) return std::nullopt;
                row_keys.emplace_back(key, 1);
            }
            std::ranges::stable_sort(row_keys, [](auto& a, auto& b) { return a.second > b.second; });
            if (row_keys.size() <= free_count) ++rows_fit;
            for (auto& [key, count] : row_keys) {
                auto it = std::ranges::lower_bound(
                    key_to_id, key, {}, &std::pair<std::uint32_t, std::uint32_t>::first);
                std::uint32_t id = 0;
                if (it != key_to_id.end() && it->first == key) {
                    id = it->second;
                } else {
                    id = static_cast<std::uint32_t>(color_keys.size());
                    color_keys.push_back(key);
                    key_to_id.insert(it, {key, id});
                }
                needs[y].push_back(id);
            }
        }
    }
    if (free_count == 0 || rows_fit * 2 < height) return std::nullopt;

    constexpr std::uint32_t kEmpty = std::numeric_limits<std::uint32_t>::max();
    constexpr std::size_t kNever = std::numeric_limits<std::size_t>::max();
    const std::size_t n_ids = color_keys.size();

    // Base palette: free slots take colours in order of first appearance.
    std::vector<std::uint32_t> base_ids(free_count, kEmpty);
    {
        std::vector<bool> placed(n_ids, false);
        std::size_t filled = 0;
        for (std::size_t y = 0; y < height && filled < free_count; ++y)
            for (auto id : needs[y])
                if (!placed[id] && filled < free_count) {
                    placed[id] = true;
                    base_ids[filled++] = id;
                }
    }

    ExactPlan plan;
    plan.off_grid = off_grid;
    plan.base_pal = fixed_color;
    for (std::size_t s = 0; s < free_count; ++s)
        plan.base_pal[free_slots[s]] =
            base_ids[s] == kEmpty ? Color3f{0.0f, 0.0f, 0.0f} : color_of(color_keys[base_ids[s]]);
    plan.scanline_palettes.assign(height, plan.base_pal);

    // Lookahead for preloads, in rows of the same field.
    constexpr std::size_t kLookahead = 64;
    const std::size_t stride = is_lace ? 2 : 1;
    for (std::size_t start = 0; start < std::min(stride, height); ++start) {
        std::vector<std::size_t> seq;
        for (std::size_t y = start; y < height; y += stride)
            seq.push_back(y);
        // Positions (in seq) where each colour is needed.
        std::vector<std::vector<std::size_t>> uses(n_ids);
        for (std::size_t i = 0; i < seq.size(); ++i)
            for (auto id : needs[seq[i]])
                uses[id].push_back(i);
        auto next_use = [&](std::uint32_t id, std::size_t from) {
            if (id == kEmpty) return kNever;
            auto it = std::ranges::lower_bound(uses[id], from);
            return it == uses[id].end() ? kNever : *it;
        };

        std::vector<std::uint32_t> slots = base_ids;
        std::vector<std::size_t> slot_of(n_ids, kNever);
        for (std::size_t s = 0; s < free_count; ++s)
            if (slots[s] != kEmpty) slot_of[slots[s]] = s;

        // Free slot to overwrite for a colour needed at position `need_at`:
        // the one whose colour is next needed farthest ahead, and only after
        // need_at. kNever when no slot qualifies.
        auto pick_victim = [&](std::size_t i, std::size_t need_at) {
            std::size_t best = kNever;
            std::size_t best_use = 0;
            for (std::size_t s = 0; s < free_count; ++s) {
                auto use = next_use(slots[s], i);  // kNever sorts last
                if (use <= need_at) continue;
                if (best == kNever || use > best_use) {
                    best = s;
                    best_use = use;
                    if (use == kNever) break;
                }
            }
            return best;
        };
        auto load = [&](std::size_t s, std::uint32_t id) {
            if (slots[s] != kEmpty) slot_of[slots[s]] = kNever;
            slots[s] = id;
            slot_of[id] = s;
        };

        for (std::size_t i = 0; i < seq.size(); ++i) {
            const auto y = seq[i];
            std::size_t budget = y < skip_initial_swap_rows ? 0 : changes_per_line;
            bool exact = true;
            for (auto id : needs[y]) {
                if (slot_of[id] != kNever) continue;
                auto s = budget > 0 ? pick_victim(i, i) : kNever;
                if (s == kNever) {
                    exact = false;
                    continue;
                }
                load(s, id);
                --budget;
            }
            if (!exact) ++plan.inexact_rows;
            // Spare changes preload the colours the next rows need soonest.
            for (std::size_t j = i + 1; budget > 0 && j < seq.size() && j <= i + kLookahead; ++j) {
                for (auto id : needs[seq[j]]) {
                    if (budget == 0) break;
                    if (slot_of[id] != kNever) continue;
                    auto s = pick_victim(i, j);
                    if (s == kNever) continue;
                    load(s, id);
                    --budget;
                }
            }
            auto& pal = plan.scanline_palettes[y];
            for (std::size_t s = 0; s < free_count; ++s)
                if (slots[s] != kEmpty) pal[free_slots[s]] = color_of(color_keys[slots[s]]);
        }
    }
    return plan;
}

// Per-row register writes for a planned palette sequence: each row against
// the row above it on the beam (two above under interlace; the base palette
// for a field's first row), leftmost first use first so a reduced K drops
// the rightmost writes. AGA marks writes whose hi or lo nibbles don't change.
std::vector<std::vector<CopperChange>> changes_from_palettes(
    const std::vector<Color3f>& base_pal,
    const std::vector<std::vector<Color3f>>& scanline_palettes,
    const std::vector<std::uint8_t>& all_indices,
    std::size_t width,
    bool is_lace,
    amiga::Chipset chipset) {
    const auto height = scanline_palettes.size();
    const std::size_t stride = is_lace ? 2 : 1;
    std::vector<std::vector<CopperChange>> out(height);
    for (std::size_t y = 0; y < height; ++y) {
        const auto& prev = y >= stride ? scanline_palettes[y - stride] : base_pal;
        const auto& cur = scanline_palettes[y];
        for (std::size_t k = 0; k < cur.size(); ++k) {
            if (cur[k] == prev[k]) continue;
            CopperChange ch{static_cast<std::uint8_t>(k), cur[k]};
            if (chipset == amiga::Chipset::aga) {
                auto old_hilo = palette::linear_to_aga_hilo(prev[k]);
                auto new_hilo = palette::linear_to_aga_hilo(cur[k]);
                ch.skip_hi = old_hilo.hi == new_hilo.hi;
                ch.skip_lo = old_hilo.lo == new_hilo.lo;
            }
            ch.avg_x = static_cast<float>(width);
            for (std::size_t x = 0; x < width; ++x) {
                if (all_indices[y * width + x] == k) {
                    ch.avg_x = static_cast<float>(x);
                    break;
                }
            }
            out[y].push_back(ch);
        }
        std::ranges::stable_sort(out[y], {}, &CopperChange::avg_x);
    }
    return out;
}

// Bitplanes plus the per-line statistics of a finished sliced encode.
Result<CopperResult> assemble_result(const std::vector<std::uint8_t>& all_indices,
                                     std::size_t width,
                                     std::size_t depth,
                                     std::vector<Color3f> base_pal,
                                     std::vector<std::vector<CopperChange>> scanline_changes,
                                     std::vector<std::vector<Color3f>> scanline_palettes,
                                     std::size_t changes_per_line,
                                     float total_error,
                                     amiga::Chipset chipset,
                                     std::string base_pal_name) {
    const auto height = scanline_palettes.size();
    const auto num_colors = std::size_t{1} << depth;
    auto planes = bitplane::encode(all_indices, width, height, depth);
    if (!planes) return std::unexpected{planes.error()};

    // Compute average actual changes per line
    std::size_t total_changes = 0;
    for (auto& ch : scanline_changes)
        total_changes += ch.size();
    float avg_changes = height > 0 ? static_cast<float>(total_changes) / static_cast<float>(height)
                                   : 0.0f;

    // Compute worst-case MOVE count per scanline (post-bank-clustering).
    // The viewer's emission strategy determines the formula; see moves_for_line.
    bool aga = (chipset == amiga::Chipset::aga);
    bool aga_banks = aga && num_colors > 32;
    std::size_t max_moves = 0;
    for (auto& ch : scanline_changes) {
        max_moves = std::max(max_moves, moves_for_line(ch, aga, aga_banks));
    }

    return CopperResult{
        *std::move(planes),
        std::move(base_pal),
        std::move(scanline_changes),
        std::move(scanline_palettes),
        num_colors,
        changes_per_line,
        avg_changes,
        total_error,
        max_moves,
        std::move(base_pal_name),
    };
}

}  // namespace

// ===========================================================================
// encode_copper
// ===========================================================================

Result<CopperResult> encode_copper(const Image& image,
                                   std::size_t depth,
                                   const dither::Settings& dither_settings,
                                   amiga::Chipset chipset,
                                   std::size_t override_changes,
                                   const std::vector<Color3f>* user_palette,
                                   bool lock_color0,
                                   const std::vector<std::pair<std::size_t, Color3f>>& locked,
                                   int palette_diversity,
                                   std::size_t skip_initial_swap_rows,
                                   bool is_lace,
                                   bool is_ehb,
                                   std::function<void(float, std::string_view)> on_progress,
                                   std::size_t neighbor_radius,
                                   float neighbor_decay,
                                   bool vertical_dither,
                                   const std::vector<std::size_t>& dither_excluded,
                                   std::optional<quantize::Algorithm> quantizer_override,
                                   bool sliced_beam,
                                   bool dither_checked) {
    if (depth < 1 || depth > 8) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("Copper mode depth must be 1-8, got {}", depth),
        }};
    }

    auto width = image.width();
    auto height = image.height();

    if (width == 0 || height == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Image dimensions must be non-zero",
        }};
    }

    auto num_colors = std::size_t{1} << depth;
    auto max_swappable = lock_color0 ? (num_colors > 1 ? num_colors - 1 : std::size_t{0})
                                     : num_colors;

    // --copper-changes is a pure user escape hatch and bypasses the budget
    // check entirely — the user is telling us "emit exactly this K, I know
    // what I'm doing".
    auto base_k = std::min(max_changes_per_line(depth, false, false, chipset, is_lace),
                           max_swappable);
    auto move_budget = is_lace ? MOVE_BUDGET_PER_LINE_LACE : MOVE_BUDGET_PER_LINE;

    // Auto mode stretch: the static AGA base K=3 leaves 2 MOVEs of headroom
    // (worst case 4*3=12 vs 14 budget). Walk K down from K+3 to K+1, return
    // the first encoding that fits the 14-MOVE budget, else fall back to
    // base K. Per the budget math:
    //   d<=5: K+3=6 worst 2*6+2 = 14  → fits (≤32 colors, exactly at limit)
    //   d6:   K+3=6 worst 2*6+4 = 16  → fails, K+2=5 worst 2*5+4 = 14 → fits
    //   d7:   K+3=6 worst 4*6   = 24  → fails, K+2/K+1 depend on clustering
    //   d8:   same as d7 (8 banks, K<=B unsaturated)
    // OCS base K=14 already saturates the budget (1 MOVE per change), no room.
    int aga_speculatives_done = 0;
    constexpr float kPerSlot = 0.25f;
    if (override_changes == 0 && chipset == amiga::Chipset::aga) {
        // Stretch passes are NOT silent — at d=7/8 the planner may need
        // to run up to 3 full encodes to find a K that fits the MOVE
        // budget. With progress disabled the user stares at 0% for one
        // or more full encodes' worth of work. Map each attempt into a
        // sub-range of the bar so the bar still moves smoothly:
        //   attempt 0 (bump=3): [0.00, 0.25]
        //   attempt 1 (bump=2): [0.25, 0.50]
        //   attempt 2 (bump=1): [0.50, 0.75]
        //   accepted replay / fall-through to base K: [last_hi, 1.00]
        // Suppress the inner "done" so the speculative pass doesn't
        // collapse the bar to 100% before the next slot opens.
        for (std::size_t bump = 3; bump >= 1; --bump) {
            auto stretch_k = base_k + bump;
            if (stretch_k > max_swappable) continue;
            float slot_lo = static_cast<float>(aga_speculatives_done) * kPerSlot;
            float slot_hi = slot_lo + kPerSlot;
            std::function<void(float, std::string_view)> stretch_progress;
            if (on_progress) {
                stretch_progress = [outer = on_progress, slot_lo, slot_hi](float p,
                                                                           std::string_view s) {
                    if (s == "done") return;
                    outer(slot_lo + (slot_hi - slot_lo) * std::clamp(p, 0.0f, 1.0f), "encoding");
                };
            }
            auto stretch = encode_copper(image,
                                         depth,
                                         dither_settings,
                                         chipset,
                                         stretch_k,
                                         user_palette,
                                         lock_color0,
                                         locked,
                                         palette_diversity,
                                         skip_initial_swap_rows,
                                         is_lace,
                                         is_ehb,
                                         stretch_progress,
                                         neighbor_radius,
                                         neighbor_decay,
                                         vertical_dither,
                                         dither_excluded,
                                         /*quantizer_override=*/std::nullopt,
                                         sliced_beam,
                                         dither_checked);
            if (!stretch) return std::unexpected{stretch.error()};
            ++aga_speculatives_done;
            if (stretch->max_moves_per_line <= move_budget) {
                // Replay the accepted K, mapping its progress into the
                // remainder of the bar so the user sees a continuous
                // sweep from where the last speculative ended up to 100%.
                if (on_progress) {
                    float replay_lo = static_cast<float>(aga_speculatives_done) * kPerSlot;
                    auto replay_progress = [outer = on_progress, replay_lo](float p,
                                                                            std::string_view s) {
                        if (s == "done") {
                            outer(1.0f, s);
                            return;
                        }
                        outer(replay_lo + (1.0f - replay_lo) * std::clamp(p, 0.0f, 1.0f), s);
                    };
                    return encode_copper(image,
                                         depth,
                                         dither_settings,
                                         chipset,
                                         stretch_k,
                                         user_palette,
                                         lock_color0,
                                         locked,
                                         palette_diversity,
                                         skip_initial_swap_rows,
                                         is_lace,
                                         is_ehb,
                                         replay_progress,
                                         neighbor_radius,
                                         neighbor_decay,
                                         vertical_dither,
                                         dither_excluded,
                                         /*quantizer_override=*/std::nullopt,
                                         sliced_beam,
                                         dither_checked);
                }
                return stretch;
            }
            // Stretch overshot — try the next-smaller bump, or fall through.
        }
        // Fall-through: no speculative K fit, run base_k inline below.
        // Rescale the inline body's progress into the remaining bar
        // [aga_speculatives_done * kPerSlot, 1.0] so we don't reset to 0%.
        if (on_progress && aga_speculatives_done > 0) {
            float body_lo = static_cast<float>(aga_speculatives_done) * kPerSlot;
            on_progress = [outer = std::move(on_progress), body_lo](float p, std::string_view s) {
                if (s == "done") {
                    outer(1.0f, s);
                    return;
                }
                outer(body_lo + (1.0f - body_lo) * std::clamp(p, 0.0f, 1.0f), s);
            };
        }
    }

    auto changes_per_line = std::min(override_changes == 0 ? base_k : override_changes,
                                     max_swappable);

    // Hardware-ready pixel art: try the exact-colour plan first. All rows
    // exact on chipset colours ends the encode; otherwise (some row misses,
    // or the source is off the chipset grid, where dithering can beat the
    // snapped colours) it competes with the clustering planner on S2. EHB (half-brite mirrors), a user palette and
    // vertical palette dither keep the clustering planner.
    std::optional<CopperResult> exact_fallback;
    if (!is_ehb && !vertical_dither && !(user_palette && !user_palette->empty())) {
        std::vector<bool> exact_excluded;
        if (!dither_excluded.empty()) {
            exact_excluded.assign(num_colors, false);
            for (auto i : dither_excluded)
                if (i < num_colors) exact_excluded[i] = true;
        }
        auto plan = plan_exact_copper(image,
                                      num_colors,
                                      changes_per_line,
                                      chipset,
                                      lock_color0,
                                      locked,
                                      exact_excluded,
                                      skip_initial_swap_rows,
                                      is_lace);
        if (plan) {
            std::vector<std::uint8_t> indices(width * height);
            float err = dither_rows(image,
                                    dither_settings,
                                    plan->scanline_palettes,
                                    exact_excluded,
                                    num_colors,
                                    indices);
            auto changes = changes_from_palettes(
                plan->base_pal, plan->scanline_palettes, indices, width, is_lace, chipset);
            auto exact = assemble_result(indices,
                                         width,
                                         depth,
                                         std::move(plan->base_pal),
                                         std::move(changes),
                                         std::move(plan->scanline_palettes),
                                         changes_per_line,
                                         err,
                                         chipset,
                                         "exact");
            if (!exact) return std::unexpected{exact.error()};
            if (plan->inexact_rows == 0 && !plan->off_grid) {
                if (on_progress) on_progress(1.0f, "done");
                return exact;
            }
            exact_fallback = std::move(*exact);
        }
    }

    // Step 1: Base palette — user-provided or auto-quantized
    std::vector<Color3f> base_pal;
    std::string base_pal_name;
    if (user_palette && !user_palette->empty()) {
        // Use user palette as-is (already snapped to chipset by caller).
        // Locks overlay on top — user-palette case preserves its own
        // color set; locks are point fixes from --lock-index /
        // --reserve-range.
        base_pal = *user_palette;
        if (base_pal.size() > num_colors) base_pal.resize(num_colors);
        while (base_pal.size() < num_colors)
            base_pal.push_back(Color3f{0.0f, 0.0f, 0.0f});
        for (auto& [idx, color] : locked) {
            if (idx < base_pal.size()) base_pal[idx] = color;
        }
        base_pal_name = "user-supplied";
    } else {
        // Quantizer: caller override wins; otherwise the centralised
        // resolver picks the AGA / OCS default. The resolver mirrors
        // every other dispatch site — single source of truth.
        auto algo = quantizer_override.has_value()
                        ? *quantizer_override
                        : quantize::resolve_algorithm(amiga::Mode::lores, chipset, "");
        // AGA 16+ colours: 48 PNN bins, as plain lores/hires. Mean S2 on
        // examples/*.jpg at 320 wide, 24 -> 48 bins: sliced d5 67.87 ->
        // 68.32, d8 83.62 -> 84.85.
        const int pnn_bins = quantize::plain_pnn_bins(amiga::Mode::lores, chipset, num_colors);
        auto qfn = [&](std::size_t k) -> Result<Palette> {
            auto r = quantize::quantize(image, k, algo, 0, pnn_bins);
            if (!r) return std::unexpected{r.error()};
            Palette p = std::move(*r);
            if (chipset != amiga::Chipset::aga) {
                for (auto& c : p.colors)
                    c = palette::quantize_to_ocs(c);
            }
            if (palette_diversity > 0) {
                quantize::diversify_palette(
                    p, image.pixels(), palette_diversity, chipset != amiga::Chipset::aga);
            }
            return p;
        };
        // Subtract locked.size() from k1 so the quantizer optimises for
        // the slots it'll actually fill (= unlocked + non-zero count).
        // Without this, the quantizer's "first num_colors-1" output
        // includes colors that land at locked indices and get
        // overwritten below, displacing those colors entirely. For
        // 5bpp + 15 reserves at slots 1-15: quantizer at K=31 puts the
        // dark tones at slots 1-15 (sorted by L), the locked overlay
        // overwrites them with reserve colors, and the dither's
        // candidate set ends up with only the bright tail (slots
        // 16-31) plus slot 0 — image renders with no dark/mid colors.
        // Floor at 1.
        std::size_t k1 = num_colors;
        if (lock_color0 && k1 > 1) --k1;
        if (k1 > locked.size())
            k1 -= locked.size();
        else
            k1 = 1;
        auto kfallback = std::min(num_colors, k1 + (lock_color0 ? std::size_t{1} : std::size_t{0}));
        auto pr = palette_locks::two_pass_quantize(qfn, k1, kfallback, lock_color0);
        if (!pr) return std::unexpected{pr.error()};
        base_pal_name = std::move(pr->name);
        // Fill base_pal: lock_zero at slot 0 (if requested), locks at
        // their indices, quantized in the remaining unlocked slots in
        // order. Skipping locked indices when filling means the
        // quantizer's colors land specifically where the dither will
        // actually be able to use them. Quantizer picks that bit-match a
        // locked colour are dropped here; find_best_swap below catches the
        // per-line variant by rejecting centroids that OCS-snap onto a
        // reserved color.
        base_pal.assign(num_colors, Color3f{0.0f, 0.0f, 0.0f});
        std::vector<bool> base_locked(num_colors, false);
        if (lock_color0) {
            base_pal[0] = Color3f{0.0f, 0.0f, 0.0f};
            base_locked[0] = true;
        }
        for (auto& [idx, color] : locked) {
            if (idx < num_colors) {
                base_pal[idx] = color;
                base_locked[idx] = true;
            }
        }
        // The K retry in two_pass_quantize returns the black slot 0 already
        // holds; an in-order fill duplicated it at slot 1 and dropped the
        // brightest colour (DIV2K 0041: S2 56 -> 27).
        palette_locks::fill_unlocked_slots(base_pal, base_locked, pr->colors);

        // Give the seed the same dither-aware refinement the plain lores
        // palette gets (main.cpp / api.cpp: refine_with_dither, joint_refit,
        // pair refine). The planner below only moves a slot toward the
        // nearest-colour centroid of pixels it already owns, so it cannot
        // find mixing partners a cluster centroid never lands on (DIV2K
        // 0256: plain's #010 next to #100 dithers the brown-black evenly; the
        // unrefined seed has #111 instead and the black speckles red, S2 -6).
        // With no swaps the sliced output then equals plain lores. Reserve
        // slots are left to the existing path.
        if (dither_checked && !is_ehb && num_colors <= kDitherCheckMaxColors &&
            dither_excluded.empty()) {
            constexpr std::size_t kRefineIterations = 8;  // plain's --refine default
            auto refined = quantize::refine_with_dither(image,
                                                        Palette{"refined", base_pal},
                                                        dither_settings,
                                                        chipset,
                                                        amiga::Mode::lores,
                                                        kRefineIterations,
                                                        base_locked);
            if (refined && refined->colors.size() == num_colors)
                base_pal = std::move(refined->colors);
            auto seed_dither = dither::apply(
                image, std::span<const Color3f>(base_pal), dither_settings);
            palette_pairs::joint_refit(
                image,
                base_pal,
                seed_dither,
                base_locked,
                palette_pairs::grid_for(chipset),
                /*ehb=*/false,
                [&](const std::vector<Color3f>& p) {
                    return dither::apply(image, std::span<const Color3f>(p), dither_settings);
                },
                /*concurrent=*/true);
            palette_pairs::refine(
                image, base_pal, seed_dither, dither_settings, base_locked, chipset);
        }
    }

    // Step 2: Iterative two-pass predict+dither loop.
    //
    // pd_iter 1: pass 1 predicts per-scanline palettes using raw-pixel
    //            error for column priority; pass 2 dithers with those
    //            palettes and measures DITHERED per-column error.
    // pd_iter 2: pass 1 re-predicts using the dithered error map from
    //            iteration 1 as column weights — swaps now target where
    //            the ditherer actually struggled.
    //
    // Joint base-palette refinement (the --best HAM strategy) was
    // tried here and gave ≤+0.10 dB on indexed copper modes — the
    // existing pd_iter feedback already finds a near-local-optimum so
    // OKLab-centroid re-seeding doesn't dislodge it. Removed; --best
    // is HAM-only.

    std::vector<std::uint8_t> all_indices(width * height);
    std::vector<std::vector<CopperChange>> scanline_changes(height);
    std::vector<std::vector<Color3f>> scanline_palettes(height);
    float total_error = 0.0f;

    auto report = [&](float p) {
        if (on_progress) on_progress(std::clamp(p, 0.0f, 1.0f), "encoding");
    };
    report(0.0f);
    constexpr float jlo = 0.0f;
    constexpr float jhi = 1.0f;

    // Precompute all rows in OKLab for neighbor lookups.
    // [Source pre-quantize to RGB444 experiment: tested and rejected.
    //  Snapping pixels to OCS grid before palette planning gave a
    //  net -0.03 dB across 5 modes × 4 images: lores+sliced +0.39,
    //  EHB+sliced -0.57, EHB+strips -0.03, DPF+strips +0.18. EHB+sliced loss
    //  outweighs marginal gains elsewhere — keep continuous-precision
    //  pixels for the swap planner so cluster centroids stay accurate.]
    std::vector<std::vector<color_space::OKLab>> all_lab(height);
    for (std::size_t y = 0; y < height; ++y) {
        all_lab[y].resize(width);
        auto row = image.row(y);
        for (std::size_t x = 0; x < width; ++x)
            all_lab[y][x] = color_space::linear_to_oklab(row[x]);
    }
    // Source conversions for the DitherWindow scans, built on first use.
    dither::DiffuseSource window_source;

    // Reserve mask: slots the dither pass refuses to pick, AND that the
    // swap planner must pretend don't exist when assigning pixels to
    // clusters. Without skipping reserves in build_swap_scratch / anchor
    // selection, pixels close to a reserve color get assigned to that
    // locked slot; find_best_swap then can't help that cluster, and pass
    // 2 dither has to route those pixels to a worse slot. Net effect:
    // reserve color leaks into output quality even though the encoder
    // never writes those slots.
    std::vector<bool> excluded_mask;
    if (!dither_excluded.empty()) {
        excluded_mask.assign(num_colors, false);
        for (auto i : dither_excluded)
            if (i < num_colors) excluded_mask[i] = true;
    }
    // EHB-aware mask: pal_lab is sized 64 (32 base + 32 half-brite). When
    // base slot k is reserved, its hardware-derived sibling k+32 = halve(k)
    // ALSO holds a reserve-derived color the encoder mustn't route pixels
    // through. Build a 64-entry mask flagging both. Used by
    // build_swap_scratch / refresh_swap_scratch / change-sort /
    // column-error feedback under is_ehb=true so reserve color can't
    // bias EHB cluster stats either.
    std::vector<bool> excluded_mask_ehb;
    if (is_ehb && !excluded_mask.empty()) {
        excluded_mask_ehb.assign(num_colors * 2, false);
        for (std::size_t k = 0; k < num_colors; ++k) {
            if (excluded_mask[k]) {
                excluded_mask_ehb[k] = true;
                excluded_mask_ehb[num_colors + k] = true;
            }
        }
    }
    auto& planner_excluded = is_ehb ? excluded_mask_ehb : excluded_mask;

    // [Top-N source color histogram pool experiment: tested and
    //  rejected — snapping centroid to nearest source color in a
    //  256-entry RGB444 histogram regressed lores+sliced by ~0.5 dB.
    //  Centroid → OCS-grid snap is already enough; further
    //  constraining to a histogram subset loses optimization
    //  headroom. find_best_swap accepts the pool args as a
    //  no-op default; pass {} from the call site.]

    // Locate the "anchor slot" — the base-palette index that the most
    // pixels assign to. Per ham_convert 1.2.0 ("most common color is
    // never changed to reduce horizontal blocking artefacts"), holding
    // this slot fixed across all scanlines avoids wasting per-row
    // copper bandwidth re-introducing a color that's already there
    // and reduces vertical "sliced look" on uniform regions.
    // Skipped when lock_color0 already locks slot 0 AND num_colors
    // is small (≤4) — locking 50% of a tiny palette starves the
    // planner.
    std::size_t anchor_slot = std::numeric_limits<std::size_t>::max();
    if (num_colors >= 8) {
        std::vector<color_space::OKLab> base_lab(num_colors);
        for (std::size_t k = 0; k < num_colors; ++k)
            base_lab[k] = color_space::linear_to_oklab(base_pal[k]);
        std::vector<std::size_t> freq(num_colors, 0);
        for (std::size_t y = 0; y < height; ++y) {
            auto& rl = all_lab[y];
            for (std::size_t x = 0; x < width; ++x) {
                float best_d = std::numeric_limits<float>::max();
                std::size_t best_k = 0;
                for (std::size_t k = 0; k < num_colors; ++k) {
                    if (!excluded_mask.empty() && excluded_mask[k]) continue;
                    float dL = rl[x].L - base_lab[k].L;
                    float da = rl[x].a - base_lab[k].a;
                    float db = rl[x].b - base_lab[k].b;
                    float d = color_space::fma_dist_sq(dL, da, db);
                    if (d < best_d) {
                        best_d = d;
                        best_k = k;
                    }
                }
                ++freq[best_k];
            }
        }
        // Pick the most-frequent slot, BUT skip slot 0 if it's already
        // reserve-color0-locked — picking it would just be redundant.
        std::size_t best_k = 0;
        std::size_t best_n = 0;
        for (std::size_t k = (lock_color0 ? std::size_t{1} : std::size_t{0}); k < num_colors; ++k) {
            if (freq[k] > best_n) {
                best_n = freq[k];
                best_k = k;
            }
        }
        if (best_n > 0) anchor_slot = best_k;
    }

    constexpr float col_decay = 0.85f;
    constexpr float col_scale = 2.0f;
    constexpr int predict_dither_iterations = 2;

    // Column error seeded as zeros for iteration 1; fed back from
    // dithered output for iteration 2+. Reset for each joint iteration.
    std::vector<float> column_error(width, 0.0f);
    std::fill(column_error.begin(), column_error.end(), 0.0f);

    for (int pd_iter = 0; pd_iter < predict_dither_iterations; ++pd_iter) {
        float pd_lo = jlo + (jhi - jlo) * (static_cast<float>(pd_iter) /
                                           static_cast<float>(predict_dither_iterations));
        float pd_hi = jlo + (jhi - jlo) * (static_cast<float>(pd_iter + 1) /
                                           static_cast<float>(predict_dither_iterations));
        auto pd_progress = [&](float local) {
            report(pd_lo + (pd_hi - pd_lo) * std::clamp(local, 0.0f, 1.0f));
        };

        // --- Pass 1: predict per-scanline palettes ---
        // For interlace, each field has its own palette state since field 1
        // (rows 0,2,4,...) and field 2 (rows 1,3,5,...) accumulate independently.
        // The per-field K-swap budget then only has to cover one row's diff.
        std::vector<Color3f> current_pal_f1 = base_pal;
        std::vector<Color3f> current_pal_f2 = base_pal;
        // Reset column error accumulator for this pass (seeded from previous
        // iteration's dithered feedback, or zeros on first iteration)
        auto pass1_column_error = column_error;

        // Depth/is_ehb-aware spread defaults from the 25-config A/B sweep
        // on FS encodes (320×213, 4 hero images). User-supplied values via
        // --slice-spread-radius / --slice-spread-decay override.
        //
        //   key  → (radius, decay)   Δ vs r=4,d=0.85 (was prior global default)
        //   --------------------------------------------------------------
        //   EHB+sliced                    r=4, d=0.30    +1.23 dB
        //   depth ≤ 3 (DPF+sliced)        r=3, d=0.85    +0.71
        //   depth = 5 (lores+sliced d5)   r=2, d=0.85    +0.54
        //   else (HAM6+sliced / strips)     r=4, d=0.85    marginal (≤±0.15)
        struct SpreadDefault {
            std::size_t radius;
            float decay;
        };
        constexpr SpreadDefault kSpreadEHB{4, 0.30f};
        constexpr SpreadDefault kSpreadDPF{3, 0.85f};
        constexpr SpreadDefault kSpreadLoresD5{2, 0.85f};
        constexpr SpreadDefault kSpreadOther{4, 0.85f};
        SpreadDefault sd = is_ehb       ? kSpreadEHB
                           : depth <= 3 ? kSpreadDPF
                           : depth == 5 ? kSpreadLoresD5
                                        : kSpreadOther;
        const std::size_t resolved_radius = (neighbor_radius ==
                                             std::numeric_limits<std::size_t>::max())
                                                ? sd.radius
                                                : neighbor_radius;
        const float resolved_decay = (neighbor_decay < 0.0f) ? sd.decay : neighbor_decay;

        for (std::size_t y = 0; y < height; ++y) {
            auto row = image.row(y);
            auto& current_pal = (is_lace && (y & 1)) ? current_pal_f2 : current_pal_f1;

            std::vector<float> col_weights(width, 1.0f);
            float max_col_err = 0.0f;
            for (std::size_t x = 0; x < width; ++x)
                max_col_err = std::max(max_col_err, pass1_column_error[x]);
            if (max_col_err > 1e-6f) {
                for (std::size_t x = 0; x < width; ++x)
                    col_weights[x] = 1.0f + (pass1_column_error[x] / max_col_err) * col_scale;
            }

            // Build neighbor rows with weights for smoothing.
            const float decay = resolved_decay;
            std::vector<std::span<const Color3f>> rows;
            std::vector<std::span<const color_space::OKLab>> rows_lab;
            std::vector<float> weights;
            for (std::size_t dy = 0; dy <= resolved_radius; ++dy) {
                float w = (dy == 0) ? 1.0f : std::pow(decay, static_cast<float>(dy));
                if (dy == 0) {
                    rows.push_back(row);
                    rows_lab.emplace_back(all_lab[y]);
                    weights.push_back(w);
                } else {
                    if (y >= dy) {
                        rows.push_back(image.row(y - dy));
                        rows_lab.emplace_back(all_lab[y - dy]);
                        weights.push_back(w);
                    }
                    if (y + dy < height) {
                        rows.push_back(image.row(y + dy));
                        rows_lab.emplace_back(all_lab[y + dy]);
                        weights.push_back(w);
                    }
                }
            }

            // Greedily find the best K swaps (each slot swapped at most once per line)
            std::vector<CopperChange> changes;
            changes.reserve(changes_per_line);
            std::vector<bool> swapped(num_colors, false);
            if (lock_color0 && !swapped.empty()) swapped[0] = true;
            if (anchor_slot < num_colors && anchor_slot < swapped.size()) {
                // Hold most-frequent slot fixed across rows.
                swapped[anchor_slot] = true;
            }
            for (auto& [idx, color] : locked) {
                if (idx < num_colors) {
                    swapped[idx] = true;       // don't swap locked slots
                    current_pal[idx] = color;  // enforce locked color each line
                }
            }

            // Hoist pal_lab out and build the per-row swap scratch ONCE. Each
            // inner swap iteration only mutates ONE palette slot, so we
            // update pal_lab in place and incrementally refresh only the
            // assignments for pixels affected by the swap — big win for
            // high-color modes (e.g. lores8 AGA, 256 colors: full-rebuild
            // dropped from ~4.7 B ops to ~300 M).
            //
            // EHB-aware mode: pal_lab is sized 64 (32 base + 32 hardware
            // half-brite). The planner scores swaps against this 64-vector
            // so candidates that hurt half-brite-bound pixels get penalised
            // even though the underlying register write only touches a base
            // slot. Output (changes / scanline_palettes) stays 32-base.
            const std::size_t pal_n = is_ehb ? std::size_t{64} : num_colors;
            std::vector<color_space::OKLab> pal_lab(pal_n);
            for (std::size_t i = 0; i < num_colors; ++i) {
                pal_lab[i] = color_space::linear_to_oklab(current_pal[i]);
            }
            if (is_ehb) {
                for (std::size_t k = 0; k < num_colors; ++k) {
                    pal_lab[num_colors + k] = color_space::linear_to_oklab(
                        palette::half_brite(current_pal[k]));
                }
            }
            SwapScratch sc;
            SlotSwapCache slot_cache;
            build_swap_scratch(
                sc, pal_lab, rows_lab, weights, width, col_weights, planner_excluded);

            // Skip swap-finding for initial rows (interlace: row 0 is field 1's
            // first displayed line and row 1 is field 2's — neither has a prior
            // scanline on which to pre-apply palette changes, so both must display
            // with the base palette as-is).
            auto this_row_changes = (y < skip_initial_swap_rows) ? std::size_t{0}
                                                                 : changes_per_line;

            // Applies one register write for this row. EHB also refreshes the
            // half-brite mirror slot (k+32): pixels can migrate in or out of
            // either cluster.
            auto apply_change = [&](std::size_t slot, Color3f color) {
                // Nibble-skip optimization (AGA only): if the new color shares
                // its high 4-bit nibble with the slot's previous value, the
                // LOCT=0 (high) write is unnecessary. The viewer detects
                // skip_hi via a 0xFFFF sentinel in the hi-table reg field.
                bool skip_hi_flag = false;
                bool skip_lo_flag = false;
                if (chipset == amiga::Chipset::aga) {
                    auto old_hilo = palette::linear_to_aga_hilo(current_pal[slot]);
                    auto new_hilo = palette::linear_to_aga_hilo(color);
                    skip_hi_flag = (old_hilo.hi == new_hilo.hi);
                    skip_lo_flag = (old_hilo.lo == new_hilo.lo);
                }
                swapped[slot] = true;
                current_pal[slot] = color;
                slot_cache.clear();
                pal_lab[slot] = color_space::linear_to_oklab(color);
                if (is_ehb) {
                    pal_lab[num_colors + slot] = color_space::linear_to_oklab(
                        palette::half_brite(color));
                }
                refresh_swap_scratch(sc,
                                     pal_lab,
                                     rows_lab,
                                     width,
                                     static_cast<std::uint8_t>(slot),
                                     planner_excluded);
                if (is_ehb) {
                    refresh_swap_scratch(sc,
                                         pal_lab,
                                         rows_lab,
                                         width,
                                         static_cast<std::uint8_t>(num_colors + slot),
                                         planner_excluded);
                }
                changes.push_back(CopperChange{
                    static_cast<std::uint8_t>(slot),
                    color,
                    skip_hi_flag,
                    skip_lo_flag,
                });
            };

            // Non-EHB: every write must improve the dithered result in the
            // window below (see DitherWindow). EHB keeps its own planner.
            auto usable_lab = [&](const std::vector<Color3f>& pal) {
                std::vector<color_space::OKLab> lab;
                lab.reserve(num_colors);
                for (std::size_t k = 0; k < num_colors; ++k) {
                    if (!excluded_mask.empty() && excluded_mask[k]) continue;
                    lab.push_back(color_space::linear_to_oklab(pal[k]));
                }
                return lab;
            };
            std::optional<DitherWindow> window;
            // Per-slot drift cap (see the planner loop below).
            constexpr float kMaxDriftSq = 0.03f;
            const std::size_t kTrialBatch = trial_batch();
            if (dither_checked && !is_ehb && num_colors <= kDitherCheckMaxColors &&
                this_row_changes > 0) {
                if (window_source.lab.empty()) window_source = dither::diffuse_source(image);
                window.emplace(
                    image, all_lab, window_source, dither_settings, y, is_lace, resolved_decay);
                const auto& other_field = (is_lace && (y & 1)) ? current_pal_f1 : current_pal_f2;
                for (std::size_t r = 0; r < window->rows(); ++r) {
                    if (window->uses_trial(r)) continue;
                    auto gy = window->row_y(r);
                    window->set_fixed(r, usable_lab(gy < y ? scanline_palettes[gy] : other_field));
                }
                window->set_incumbent(usable_lab(current_pal));
            }
            // Seed return. Centroid proposals only follow pixels a slot
            // already owns, so a seed colour moved away on some rows never
            // comes back once those rows end (0386: slot 25 holds the sky's
            // #8AD in the seed and #999 at row 84; the sky there dithered
            // #79C with light greys into a band of dots). Offer each moved
            // slot its seed
            // colour first, ranked by the nearest-colour gain with pixels
            // re-assigned (the cluster-local score cannot see pixels moving
            // in from other slots), and keep it if the dither improves.
            if (window) {
                std::vector<std::pair<double, std::size_t>> returns;
                for (std::size_t k = 0; k < num_colors; ++k) {
                    if (swapped[k] || current_pal[k] == base_pal[k]) continue;
                    if (!excluded_mask.empty() && excluded_mask[k]) continue;
                    auto seed_lab = color_space::linear_to_oklab(base_pal[k]);
                    double gain = 0.0;
                    for (std::size_t r = 0; r < rows_lab.size(); ++r) {
                        const auto& rl = rows_lab[r];
                        for (std::size_t x = 0; x < width; ++x) {
                            auto i = r * width + x;
                            const auto& p = rl[x];
                            float d = color_space::fma_dist_sq(
                                p.L - seed_lab.L, p.a - seed_lab.a, p.b - seed_lab.b);
                            if (sc.assignments[i] == k) {
                                for (std::size_t j = 0; j < num_colors; ++j) {
                                    if (j == k) continue;
                                    if (!excluded_mask.empty() && excluded_mask[j]) continue;
                                    const auto& e = pal_lab[j];
                                    d = std::min(
                                        d,
                                        color_space::fma_dist_sq(p.L - e.L, p.a - e.a, p.b - e.b));
                                }
                            } else {
                                d = std::min(d, sc.best_dist[i]);
                            }
                            gain += static_cast<double>(sc.best_dist[i] - d) *
                                    static_cast<double>(sc.pixel_weights[i]);
                        }
                    }
                    if (gain > 0.0) returns.emplace_back(gain, k);
                }
                std::sort(returns.begin(), returns.end(), std::greater<>{});
                // Trials in batches against the same incumbent; the first
                // kept one is applied and the rest are re-tried after it,
                // as the one-by-one scan would.
                std::size_t next = 0;
                std::vector<std::vector<color_space::OKLab>> trials;
                while (next < returns.size() && changes.size() < this_row_changes) {
                    auto nb = std::min(kTrialBatch, returns.size() - next);
                    trials.clear();
                    for (std::size_t j = 0; j < nb; ++j) {
                        auto trial = current_pal;
                        auto k = returns[next + j].second;
                        trial[k] = base_pal[k];
                        trials.push_back(usable_lab(trial));
                    }
                    auto hit = window->try_batch(trials);
                    if (hit < nb) {
                        auto k = returns[next + hit].second;
                        apply_change(k, base_pal[k]);
                    }
                    next += std::min(nb, hit + 1);
                }
            }

            // Dither-checked: plan the next proposals as if every trial
            // were rejected, score their trials as a batch, then replay
            // up to the first kept one. Same choices as one at a time.
            while (window && changes.size() < this_row_changes) {
                struct Step {
                    std::size_t slot;
                    Color3f color;
                    bool trial;
                };
                std::vector<Step> steps;
                std::vector<std::vector<color_space::OKLab>> trials;
                auto sw = swapped;
                bool ended = false;
                while (trials.size() < kTrialBatch) {
                    auto swap = find_best_swap(current_pal,
                                               pal_lab,
                                               rows,
                                               rows_lab,
                                               weights,
                                               chipset,
                                               sc,
                                               sw,
                                               col_weights,
                                               {},
                                               {},
                                               &slot_cache);
                    if (swap.error_reduction <= 0.0f) {
                        ended = true;
                        break;
                    }
                    sw[swap.slot] = true;
                    // Drift cap and snap-back as in the loop below.
                    auto base_lab = color_space::linear_to_oklab(base_pal[swap.slot]);
                    auto new_lab = color_space::linear_to_oklab(swap.new_color);
                    float ddL = base_lab.L - new_lab.L;
                    float dda = base_lab.a - new_lab.a;
                    float ddb = base_lab.b - new_lab.b;
                    if (color_space::fma_dist_sq(ddL, dda, ddb) > kMaxDriftSq ||
                        swap.new_color == current_pal[swap.slot]) {
                        steps.push_back({swap.slot, {}, false});
                        continue;
                    }
                    steps.push_back({swap.slot, swap.new_color, true});
                    auto trial = current_pal;
                    trial[swap.slot] = swap.new_color;
                    trials.push_back(usable_lab(trial));
                }
                auto hit = trials.empty() ? std::size_t{0} : window->try_batch(trials);
                bool kept = false;
                std::size_t ti = 0;
                for (const auto& st : steps) {
                    if (st.trial && ti++ == hit) {
                        apply_change(st.slot, st.color);
                        kept = true;
                        break;
                    }
                    swapped[st.slot] = true;
                }
                if (!kept && ended) break;
            }

            while (!window && changes.size() < this_row_changes) {
                SwapCandidate swap{0, {}, -1.0f};
                if (is_ehb) {
                    swap = find_best_swap_ehb(
                        pal_lab, rows_lab, sc, swapped, base_pal, planner_excluded);
                } else {
                    swap = find_best_swap(current_pal,
                                          pal_lab,
                                          rows,
                                          rows_lab,
                                          weights,
                                          chipset,
                                          sc,
                                          swapped,
                                          col_weights);
                }

                if (swap.error_reduction <= 0.0f) break;

                // Per-slot drift cap: reject swaps that take a slot farther
                // than kMaxDriftSq (OKLab²) from its base_pal value. The
                // greedy planner can pick centroids that are perceptually
                // far from base when consecutive rows have varied content;
                // accumulating those moves over many rows produces a slot
                // whose color is wildly different from base_pal[slot]. A
                // pixel near base_pal[slot]'s value in subsequent rows then
                // finds the drifted slot in OKLab terms (very dark vs
                // slightly-brighter is a near-tie in OKLab L) and renders
                // with the drifted color, producing visible cross-row
                // banding (chuck31: black-space pixels rendering as #444433
                // because slot 1 had drifted from base #111 to #444).
                // The cap keeps each slot perceptually within ΔE ~12 of base.
                // A capped slot is skipped, not the rest of the row: ending
                // the row here left fantasy1.jpg's later swaps unplanned.
                auto base_lab = color_space::linear_to_oklab(base_pal[swap.slot]);
                auto new_lab = color_space::linear_to_oklab(swap.new_color);
                float ddL = base_lab.L - new_lab.L;
                float dda = base_lab.a - new_lab.a;
                float ddb = base_lab.b - new_lab.b;
                if (color_space::fma_dist_sq(ddL, dda, ddb) > kMaxDriftSq) {
                    if (is_ehb) break;
                    swapped[swap.slot] = true;
                    continue;
                }
                apply_change(swap.slot, swap.new_color);
            }

            // ---- PHASE B: forward-look beam fill (PNG2AMIGA_SLICED_BEAM=1) ----
            // Phase A (greedy) exits at the first non-positive ER swap. On
            // high-diversity rows it converges in 2–3 swaps because most
            // palette slots have empty clusters (no pixels currently
            // assigned). The 25+ unused budget slots sit dormant while
            // the same row contains ~50 distinct source colors that the
            // dither has to map onto ~6 used slots.
            //
            // Phase B scavenges those dormant slots, scored against a
            // forward window of `kBeamLookahead` rows so the swap is only
            // accepted when it genuinely reduces total OKLab² error across
            // current AND future rows. The window evaluation reuses
            // `all_lab[y..y+K-1]` (the source row OKLab cache built once at
            // line 574) and a small inline nearest-palette scan per pixel.
            //
            // Gated behind PNG2AMIGA_SLICED_BEAM for A/B until we have a
            // regression sweep. The scoring is what makes this safe on
            // smooth images: a swap that hurts the window-residual fails
            // and the slot stays untouched, preserving cross-row palette
            // continuity that smooth images depend on.
            // Beam fires only on rows where the row content is genuinely
            // under-served by the current palette — i.e., many slots have
            // EMPTY clusters in stats[]. On smooth rows (game art,
            // photographs with limited per-row palette), the few-swap
            // greedy bail just means the row content is covered by a
            // small subset of slots; the unused slots still hold
            // perceptually-meaningful colors from other rows that beam's
            // residual-fill would clobber, breaking cross-row palette
            // continuity (shooter, photo, fromthe in early tests).
            //
            // Diverse rows like ocs_4096 (52 unique colors / row) have
            // most slots empty after greedy because the greedy can't even
            // propose centroids for slots with zero count — so empty-slot
            // share is the discriminator we want.
            std::size_t empty_slot_count = 0;
            for (std::size_t k = 0; k < num_colors; ++k) {
                if (swapped[k]) continue;
                if (!excluded_mask.empty() && excluded_mask[k]) continue;
                if (sc.stats[k].count < 0.5) ++empty_slot_count;
            }
            const bool beam_eligible = empty_slot_count >= num_colors / 2;
            if (!is_ehb && beam_eligible && sliced_beam) {
                constexpr std::size_t kBeamLookahead = 4;  // rows in the window
                // Window error of `all_lab[y0..y0+win-1]` against an OKLab
                // palette of size num_colors.
                auto window_error = [&](const std::vector<color_space::OKLab>& pal_lab_eval,
                                        std::size_t y0,
                                        std::size_t win) -> double {
                    double total = 0.0;
                    std::size_t y_end = std::min(y0 + win, height);
                    for (std::size_t yy = y0; yy < y_end; ++yy) {
                        auto& rl = all_lab[yy];
                        for (std::size_t x = 0; x < width; ++x) {
                            float best_d = std::numeric_limits<float>::max();
                            for (std::size_t k = 0; k < num_colors; ++k) {
                                if (!excluded_mask.empty() && excluded_mask[k]) continue;
                                float dL = rl[x].L - pal_lab_eval[k].L;
                                float da = rl[x].a - pal_lab_eval[k].a;
                                float db = rl[x].b - pal_lab_eval[k].b;
                                float d = color_space::fma_dist_sq(dL, da, db);
                                if (d < best_d) best_d = d;
                            }
                            total += static_cast<double>(best_d);
                        }
                    }
                    return total;
                };

                std::vector<color_space::OKLab> proposed_lab(num_colors);
                for (std::size_t k = 0; k < num_colors; ++k)
                    proposed_lab[k] = pal_lab[k];

                // Only target *genuinely empty* slots. Stealing a slot with a
                // small-but-non-empty cluster breaks cross-row palette
                // continuity that smooth images depend on (fromthe lost
                // -2.83 S2 at default budget; shooter -1.7; brick/chuck30/
                // chuck31 used to drift mid-sweep). The window-error gate
                // further below already rejects unhelpful steals — this
                // threshold prevents them from even being proposed.
                constexpr double kStealEmptyThreshold = 0.5;  // weighted px
                while (changes.size() < this_row_changes) {
                    // Pick the lowest-utility (= empty) unused slot.
                    std::size_t target_slot = std::numeric_limits<std::size_t>::max();
                    double min_count = std::numeric_limits<double>::infinity();
                    for (std::size_t k = 0; k < num_colors; ++k) {
                        if (swapped[k]) continue;
                        if (!excluded_mask.empty() && excluded_mask[k]) continue;
                        if (sc.stats[k].count >= kStealEmptyThreshold) continue;
                        if (sc.stats[k].count < min_count) {
                            min_count = sc.stats[k].count;
                            target_slot = k;
                        }
                    }
                    if (target_slot == std::numeric_limits<std::size_t>::max()) break;

                    // Find the source pixel in the forward window with the
                    // highest residual error against the current palette.
                    // Approximates "what color, placed at any slot, would
                    // most improve the window" — for that pixel specifically
                    // the residual collapses to zero.
                    float max_res = 0.0f;
                    std::size_t best_yy = 0, best_x = 0;
                    std::size_t y_end_r = std::min(y + kBeamLookahead, height);
                    for (std::size_t yy = y; yy < y_end_r; ++yy) {
                        auto& rl = all_lab[yy];
                        for (std::size_t x = 0; x < width; ++x) {
                            float best_d = std::numeric_limits<float>::max();
                            for (std::size_t k = 0; k < num_colors; ++k) {
                                if (!excluded_mask.empty() && excluded_mask[k]) continue;
                                float dL = rl[x].L - pal_lab[k].L;
                                float da = rl[x].a - pal_lab[k].a;
                                float db = rl[x].b - pal_lab[k].b;
                                float d = color_space::fma_dist_sq(dL, da, db);
                                if (d < best_d) best_d = d;
                            }
                            if (best_d > max_res) {
                                max_res = best_d;
                                best_yy = yy;
                                best_x = x;
                            }
                        }
                    }
                    if (max_res <= 0.0f) break;

                    auto pixel_lab = all_lab[best_yy][best_x];
                    auto pixel_linear = color_space::oklab_to_linear(pixel_lab).clamped();
                    if (chipset != amiga::Chipset::aga) {
                        pixel_linear = palette::quantize_to_ocs(pixel_linear);
                    }
                    auto pixel_lab_q = color_space::linear_to_oklab(pixel_linear);

                    // Drift cap (chuck31 fix) — refuse any individual swap
                    // that moves a slot too far from its base_pal anchor.
                    auto base_lab_b = color_space::linear_to_oklab(base_pal[target_slot]);
                    float ddLb = base_lab_b.L - pixel_lab_q.L;
                    float ddab = base_lab_b.a - pixel_lab_q.a;
                    float ddbb = base_lab_b.b - pixel_lab_q.b;
                    constexpr float kMaxDriftSqB = 0.03f;
                    if (color_space::fma_dist_sq(ddLb, ddab, ddbb) > kMaxDriftSqB) {
                        swapped[target_slot] = true;  // don't reconsider
                        continue;
                    }

                    // Score against the forward window — only accept if the
                    // proposed palette reduces total window error. This is
                    // the gate that protects smooth images: when no future
                    // benefit exists, the swap fails and the slot stays put.
                    proposed_lab[target_slot] = pixel_lab_q;
                    (void)window_error;  // helper kept for future tuning
                    double err_cur = 0.0;
                    double err_new = 0.0;
                    std::size_t y_end = std::min(y + kBeamLookahead, height);
                    for (std::size_t yy = y; yy < y_end; ++yy) {
                        auto& rl = all_lab[yy];
                        for (std::size_t x = 0; x < width; ++x) {
                            float best_cur = std::numeric_limits<float>::max();
                            float best_new = std::numeric_limits<float>::max();
                            for (std::size_t k = 0; k < num_colors; ++k) {
                                if (!excluded_mask.empty() && excluded_mask[k]) continue;
                                float dL = rl[x].L - pal_lab[k].L;
                                float da = rl[x].a - pal_lab[k].a;
                                float db = rl[x].b - pal_lab[k].b;
                                float d = color_space::fma_dist_sq(dL, da, db);
                                if (d < best_cur) best_cur = d;
                                float dL2 = rl[x].L - proposed_lab[k].L;
                                float da2 = rl[x].a - proposed_lab[k].a;
                                float db2 = rl[x].b - proposed_lab[k].b;
                                float d2 = color_space::fma_dist_sq(dL2, da2, db2);
                                if (d2 < best_new) best_new = d2;
                            }
                            err_cur += static_cast<double>(best_cur);
                            err_new += static_cast<double>(best_new);
                        }
                    }
                    if (err_new >= err_cur) {
                        // No net benefit; revert proposed_lab and skip.
                        proposed_lab[target_slot] = pal_lab[target_slot];
                        swapped[target_slot] = true;
                        continue;
                    }

                    // Accept.
                    bool skip_hi_flag2 = false;
                    bool skip_lo_flag2 = false;
                    if (chipset == amiga::Chipset::aga) {
                        auto old_hilo = palette::linear_to_aga_hilo(current_pal[target_slot]);
                        auto new_hilo = palette::linear_to_aga_hilo(pixel_linear);
                        skip_hi_flag2 = (old_hilo.hi == new_hilo.hi);
                        skip_lo_flag2 = (old_hilo.lo == new_hilo.lo);
                    }
                    swapped[target_slot] = true;
                    current_pal[target_slot] = pixel_linear;
                    pal_lab[target_slot] = pixel_lab_q;
                    proposed_lab[target_slot] = pixel_lab_q;
                    refresh_swap_scratch(sc,
                                         pal_lab,
                                         rows_lab,
                                         width,
                                         static_cast<std::uint8_t>(target_slot),
                                         planner_excluded);
                    changes.push_back(CopperChange{
                        static_cast<std::uint8_t>(target_slot),
                        pixel_linear,
                        skip_hi_flag2,
                        skip_lo_flag2,
                    });
                }
            }

            // Sort changes by the spatial position where each swapped register
            // is most used on this scanline. This way, reducing --copper-changes
            // drops the rightmost (least critical leftward) swaps first, giving
            // predictable hand-tuning behavior.
            {
                // Reuse the pal_lab we already maintain (up-to-date with the
                // swaps we just applied) instead of rebuilding it from scratch.
                auto& pal_lab_sort = pal_lab;

                // For each change, find the first (leftmost) X position on this
                // row where a pixel is assigned to that register. The first
                // occurrence is what matters — the copper swap must happen
                // before that pixel is displayed.
                for (auto& ch : changes) {
                    ch.avg_x = static_cast<float>(width);  // default: sort last
                    for (std::size_t x = 0; x < width; ++x) {
                        float best_d = std::numeric_limits<float>::max();
                        std::size_t best_k = 0;
                        for (std::size_t k = 0; k < num_colors; ++k) {
                            if (!excluded_mask.empty() && excluded_mask[k]) continue;
                            float dL = all_lab[y][x].L - pal_lab_sort[k].L;
                            float da = all_lab[y][x].a - pal_lab_sort[k].a;
                            float db = all_lab[y][x].b - pal_lab_sort[k].b;
                            float d = color_space::fma_dist_sq(dL, da, db);
                            if (d < best_d) {
                                best_d = d;
                                best_k = k;
                            }
                        }
                        if (best_k == ch.reg) {
                            ch.avg_x = static_cast<float>(x);
                            break;  // first occurrence found
                        }
                    }
                }
                std::sort(
                    changes.begin(),
                    changes.end(),
                    [](const CopperChange& a, const CopperChange& b) { return a.avg_x < b.avg_x; });
            }

            // Snapshot the effective palette for this scanline
            scanline_palettes[y] = current_pal;
            scanline_changes[y] = std::move(changes);

            // Update per-column error: decay old error, add this scanline's
            // per-pixel error against the effective palette. Reuse the
            // hoisted pal_lab (already reflects all swaps applied this row).
            for (std::size_t x = 0; x < width; ++x) {
                auto pixel_lab = all_lab[y][x];
                float best_d = std::numeric_limits<float>::max();
                for (std::size_t k = 0; k < num_colors; ++k) {
                    if (!excluded_mask.empty() && excluded_mask[k]) continue;
                    float dL = pixel_lab.L - pal_lab[k].L;
                    float da = pixel_lab.a - pal_lab[k].a;
                    float db = pixel_lab.b - pal_lab[k].b;
                    float d = color_space::fma_dist_sq(dL, da, db);
                    if (d < best_d) best_d = d;
                }
                pass1_column_error[x] = pass1_column_error[x] * col_decay + best_d;
            }
            // Pass 1 covers 0..50% of pd_iter range. Progress reports every
            // ~5% of rows to limit callback frequency.
            if (height > 0 && (y & 7) == 7) {
                pd_progress(0.5f * static_cast<float>(y + 1) / static_cast<float>(height));
            }
        }
        pd_progress(0.5f);

        // --- Vertical palette dithering ---
        // At low bitplane depths (≤4), each palette register covers a wide color
        // range.  When a copper swap changes a register between scanlines the
        // boundary is a visible horizontal band.  Fix: spread the transition over
        // several lines by alternating old/new color in a 1-D Bayer pattern.
        // The eye averages the alternating lines into a smooth gradient — using
        // only real palette colors, no intermediate blends that would hit OCS
        // 12-bit quantization artefacts.
        if (vertical_dither && chipset != amiga::Chipset::aga) {
            // Golden ratio (R1) sequence: fract(y·φ + ½).  Never repeats,
            // optimal gap-filling at any prefix length — no periodicity
            // artefacts unlike Bayer-8 which tiles every 8 lines.
            constexpr float phi = 0.6180339887f;  // (√5 − 1) / 2
            // Scale with depth: fewer colors → bigger swings, wider bands.
            // max_spread is the LONGEST transition (used for the smallest color
            // changes).  As the perceptual distance grows, effective_spread
            // shrinks linearly toward 2, so distant colors nearly hard-switch
            // instead of creating ugly stripes.
            int gap = 5 - static_cast<int>(depth);
            int max_spread = 6 + gap * 3;  // d1:18 d2:15 d3:12 d4:9
            constexpr float hard_switch_de2 = 0.50f;
            float merge_de2 = 0.01f + static_cast<float>(gap) * 0.02f;

            // Per-register dither walk parameterised by (start, stride). For
            // progressive: stride 1 over every row. For interlace: run twice —
            // stride 2 starting at row 0 (field 1) and stride 2 starting at row 1
            // (field 2). Without per-field segregation, the committed/candidate
            // state cross-leaks between fields, and the golden-ratio alternation
            // pattern uses linear y so its phase is wrong relative to either
            // field's actual scan order. The visible symptom is every transition
            // appearing ~half max_spread image rows too early (eg. d1 hires-lace
            // ⇒ ~9 image rows = ~8 CRT raster lines premature).
            auto run_pass = [&](std::size_t start, std::size_t stride) {
                for (std::size_t r = 1; r < num_colors; ++r) {
                    Color3f committed = scanline_palettes[start][r];
                    Color3f candidate = committed;
                    int candidate_count = 0;

                    std::size_t seq_idx = 0;  // field-local row count (post-increment per step)
                    for (std::size_t y = start + stride; y < height; y += stride) {
                        ++seq_idx;
                        Color3f ideal = scanline_palettes[y][r];

                        if (ideal == committed) {
                            candidate = committed;
                            candidate_count = 0;
                            continue;
                        }

                        // Track how long the candidate has been active
                        if (ideal == candidate) {
                            candidate_count++;
                        } else {
                            // Gradual drift (close to previous candidate)?  Keep ramping.
                            auto c_lab = color_space::linear_to_oklab(candidate);
                            auto i_lab = color_space::linear_to_oklab(ideal);
                            float dd = (c_lab.L - i_lab.L) * (c_lab.L - i_lab.L) +
                                       (c_lab.a - i_lab.a) * (c_lab.a - i_lab.a) +
                                       (c_lab.b - i_lab.b) * (c_lab.b - i_lab.b);
                            if (dd < merge_de2 && candidate != committed) {
                                candidate = ideal;
                                candidate_count++;
                            } else {
                                candidate = ideal;
                                candidate_count = 1;
                            }
                        }

                        // Distance committed → candidate
                        auto com_lab = color_space::linear_to_oklab(committed);
                        auto can_lab = color_space::linear_to_oklab(candidate);
                        float dL = com_lab.L - can_lab.L;
                        float da = com_lab.a - can_lab.a;
                        float db = com_lab.b - can_lab.b;
                        float dist = color_space::fma_dist_sq(dL, da, db);

                        if (dist >= hard_switch_de2) {
                            committed = candidate;
                            candidate_count = 0;
                            scanline_palettes[y][r] = committed;
                            continue;
                        }

                        // Effective spread: close colors → long transition,
                        // distant colors → short (almost hard-switch).
                        float norm = dist / hard_switch_de2;  // 0..1
                        int eff_spread = std::max(
                            2, static_cast<int>(static_cast<float>(max_spread) * (1.0f - norm)));

                        float ramp = std::min(1.0f,
                                              static_cast<float>(candidate_count) /
                                                  static_cast<float>(eff_spread));
                        // Use the field-local row index for the golden-ratio
                        // phase so successive in-field rows step through the
                        // sequence cleanly. Using raw y under stride=2 would
                        // skip every other φ value, producing a 50/50 phase
                        // that looks like noise rather than a smooth ramp.
                        float threshold = std::fmod(static_cast<float>(seq_idx) * phi + 0.5f, 1.0f);
                        scanline_palettes[y][r] = (ramp > threshold) ? candidate : committed;

                        if (candidate_count >= eff_spread) {
                            committed = candidate;
                            candidate_count = 0;
                        }
                    }
                }
            };

            if (is_lace) {
                run_pass(0, 2);                   // field 1: rows 0, 2, 4, …
                if (height >= 2) run_pass(1, 2);  // field 2: rows 1, 3, 5, …
            } else {
                run_pass(0, 1);  // progressive: every row
            }
        }

        // --- Pass 2: Dither with the predetermined per-scanline palettes ---
        //
        // Now that every scanline's effective palette is known, error diffusion
        // can flow correctly across scanline boundaries — each row dithers
        // against its own palette, and the error propagated to the next row
        // is applied against that row's (different) palette.
        //
        // The driver owns the ED scaffolding (kernel, serpentine, structure
        // bias, Riemersma scaling, ordered offsets). The picker
        // selects the per-row palette and yliluoma family / nearest-pair.
        // ===================================================================

        total_error = dither_rows(
            image, dither_settings, scanline_palettes, excluded_mask, num_colors, all_indices);
        pd_progress(1.0f);

        // --- Feedback: compute per-column dithered error for next iteration ---
        if (pd_iter + 1 < predict_dither_iterations) {
            std::fill(column_error.begin(), column_error.end(), 0.0f);
            for (std::size_t y = 0; y < height; ++y) {
                auto& pal = scanline_palettes[y];
                std::vector<color_space::OKLab> pal_lab_fb(num_colors);
                for (std::size_t i = 0; i < num_colors; ++i)
                    pal_lab_fb[i] = color_space::linear_to_oklab(pal[i]);
                for (std::size_t x = 0; x < width; ++x) {
                    auto idx = all_indices[y * width + x];
                    auto& pixel = all_lab[y][x];
                    auto& chosen = pal_lab_fb[idx];
                    float dL = pixel.L - chosen.L;
                    float da = pixel.a - chosen.a;
                    float db = pixel.b - chosen.b;
                    column_error[x] += color_space::fma_dist_sq(dL, da, db);
                }
            }
        }

    }  // end predict_dither_iterations loop

    if (on_progress) on_progress(1.0f, "done");

    auto result = assemble_result(all_indices,
                                  width,
                                  depth,
                                  std::move(base_pal),
                                  std::move(scanline_changes),
                                  std::move(scanline_palettes),
                                  changes_per_line,
                                  total_error,
                                  chipset,
                                  std::move(base_pal_name));
    if (result && exact_fallback) {
        // A partly exact plan against the clustering planner: rank by S2.
        // OKLab error preferred the exact plan on examples/grungy.png at
        // depth 5, which cost 1.6 S2.
        auto score = [&](const CopperResult& r) {
            auto shown = render_copper_capped(
                r.planes, r.scanline_palettes, r.base_palette, r.changes_per_line, is_lace, chipset);
            return shown ? ssimulacra2::compute(image.pixels(), shown->pixels(), width, height)
                         : -std::numeric_limits<float>::infinity();
        };
        if (score(*exact_fallback) > score(*result)) return *std::move(exact_fallback);
    }
    return result;
}

// ===========================================================================
// render_copper
// ===========================================================================

Result<Image> render_copper(const bitplane::BitplaneData& planes,
                            const std::vector<std::vector<Color3f>>& scanline_palettes) {
    auto width = planes.width;
    auto height = planes.height;

    if (scanline_palettes.size() < height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Expected {} scanline palettes, got {}", height, scanline_palettes.size()),
        }};
    }

    auto indices = bitplane::decode(planes);
    if (!indices) return std::unexpected{indices.error()};

    Image result(width, height);

    for (std::size_t y = 0; y < height; ++y) {
        auto& pal = scanline_palettes[y];
        for (std::size_t x = 0; x < width; ++x) {
            auto idx = (*indices)[y * width + x];
            if (idx < pal.size()) {
                result[x, y] = pal[idx];
            } else {
                result[x, y] = Color3f{0.0f, 0.0f, 0.0f};
            }
        }
    }

    return result;
}

// Simulates the cheader-side lace_rebuild: at each image row, computes
// the diff vs the previous SAME-FIELD row (y-2 in lace, y-1 in
// progressive), keeps only the top-K diffs by squared OKLab distance,
// and cascades the result. The eventual per-row palette is what the
// CHIP actually displays — which can differ from `scanline_palettes[y]`
// when the K budget is exceeded (typical for OCS depth ≤ 4 with
// vertical palette dithering active, since the dither spreads palette
// transitions across many rows and inflates the per-row-pair diff
// count past the swap budget).
Result<Image> render_copper_capped(const bitplane::BitplaneData& planes,
                                   const std::vector<std::vector<Color3f>>& scanline_palettes,
                                   std::span<const Color3f> base_palette,
                                   std::size_t sliced_changes_per_line,
                                   bool is_lace,
                                   amiga::Chipset chipset) {
    auto width = planes.width;
    auto height = planes.height;
    auto quantize = [&](const Color3f& c) {
        return chipset == amiga::Chipset::aga ? c : palette::quantize_to_ocs(c);
    };

    if (scanline_palettes.size() < height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Expected {} scanline palettes, got {}", height, scanline_palettes.size()),
        }};
    }

    auto indices = bitplane::decode(planes);
    if (!indices) return std::unexpected{indices.error()};

    // Reconstruct the chip-applied per-row palette by replaying the
    // top-K-clipped diff cascade. Each field carries its own state.
    std::vector<std::vector<Color3f>> applied(height);
    std::vector<Color3f> base_vec(base_palette.begin(), base_palette.end());
    std::vector<Color3f> state_f1 = base_vec;
    std::vector<Color3f> state_f2 = base_vec;

    auto pick_topk_diffs = [&](const std::vector<Color3f>& prev, const std::vector<Color3f>& cur) {
        struct Cand {
            std::size_t reg;
            float dist;
            Color3f color;
        };
        std::vector<Cand> cands;
        auto n_regs = std::min(prev.size(), cur.size());
        cands.reserve(n_regs);
        for (std::size_t r = 0; r < n_regs; ++r) {
            auto dr = cur[r].r - prev[r].r;
            auto dg = cur[r].g - prev[r].g;
            auto db = cur[r].b - prev[r].b;
            auto d2 = color_space::fma_dist_sq(dr, dg, db);
            if (d2 > 0.0f) cands.push_back({r, d2, cur[r]});
        }
        if (cands.size() > sliced_changes_per_line) {
            std::partial_sort(cands.begin(),
                              cands.begin() + static_cast<std::ptrdiff_t>(sliced_changes_per_line),
                              cands.end(),
                              [](auto& a, auto& b) { return a.dist > b.dist; });
            cands.resize(sliced_changes_per_line);
        }
        return cands;
    };

    // The diff source for the top-K pick must match cheader's lace_rebuild
    // exactly: previous TARGET-row palette (pals[y-2] in lace, pals[y-1]
    // progressive, base for the first row). Picking against running state
    // would let the preview "rescue" registers whose change got dropped at
    // an earlier row — the next row's diff (state → target) looks bigger
    // than (prev_target → target) and may sneak back into the top-K.
    // Hardware never gets that second chance: it ranks against prev_target
    // and permanently drops anything past K. Using prev_target here keeps
    // the preview faithful to the chip on K-exceeded rows (the failure
    // mode for OCS hires-lace + depth ≤ 4 with vertical palette dither).
    for (std::size_t y = 0; y < height; ++y) {
        auto& state = (is_lace && (y & 1)) ? state_f2 : state_f1;
        const auto& target = scanline_palettes[y];
        const auto& prev_target = is_lace ? (y < 2 ? base_vec : scanline_palettes[y - 2])
                                          : (y == 0 ? base_vec : scanline_palettes[y - 1]);
        auto cands = pick_topk_diffs(prev_target, target);
        for (auto& c : cands)
            state[c.reg] = c.color;
        applied[y] = state;
    }

    Image result(width, height);
    for (std::size_t y = 0; y < height; ++y) {
        auto& pal = applied[y];
        for (std::size_t x = 0; x < width; ++x) {
            auto idx = (*indices)[y * width + x];
            if (idx < pal.size()) {
                result[x, y] = quantize(pal[idx]);
            } else {
                result[x, y] = Color3f{0.0f, 0.0f, 0.0f};
            }
        }
    }

    return result;
}

}  // namespace png2amiga::copper
