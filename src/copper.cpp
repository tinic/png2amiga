#include "copper.hpp"
#include "bitplane.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "palette_locks.hpp"
#include "quantize.hpp"
#include "quantize_metal.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
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
    std::vector<float> best_dist;  // per-pixel squared distance to its slot
    std::uint64_t valid_for_swap_count = 0;  // sentinel: 0 = invalid
};

// Build assignments + per-slot stats from scratch. Called once per row;
// subsequent swaps within the same row update incrementally via
// refresh_swap_scratch.
void build_swap_scratch(
    SwapScratch& sc,
    std::span<const color_space::OKLab> current_lab,
    std::span<const std::span<const color_space::OKLab>> rows_lab,
    std::span<const float> weights,
    std::size_t width,
    std::span<const float> column_weights) {

    auto num_colors = current_lab.size();
    auto total_pixels = rows_lab.size() * width;
    sc.stats.assign(num_colors, {});
    sc.assignments.assign(total_pixels, 0);
    sc.pixel_weights.assign(total_pixels, 0.0f);
    sc.best_dist.assign(total_pixels, 0.0f);

    for (std::size_t r = 0; r < rows_lab.size(); ++r) {
        auto row_w = weights[r];
        auto& rl = rows_lab[r];
        auto base = r * width;
        for (std::size_t x = 0; x < width; ++x) {
            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            for (std::size_t k = 0; k < num_colors; ++k) {
                float dL = rl[x].L - current_lab[k].L;
                float da = rl[x].a - current_lab[k].a;
                float db = rl[x].b - current_lab[k].b;
                float d = color_space::fma_dist_sq(dL, da, db);
                if (d < best_d) { best_d = d; best_k = k; }
            }
            float col_w = column_weights.empty() ? 1.0f : column_weights[x];
            float w = row_w * col_w;
            auto wd = static_cast<double>(w);
            sc.assignments[base + x] = static_cast<std::uint8_t>(best_k);
            sc.pixel_weights[base + x] = w;
            sc.best_dist[base + x] = best_d;
            auto& s = sc.stats[best_k];
            s.sum_L += static_cast<double>(rl[x].L) * wd;
            s.sum_a += static_cast<double>(rl[x].a) * wd;
            s.sum_b += static_cast<double>(rl[x].b) * wd;
            s.total_error += static_cast<double>(best_d) * wd;
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
void refresh_swap_scratch(
    SwapScratch& sc,
    std::span<const color_space::OKLab> current_lab,
    std::span<const std::span<const color_space::OKLab>> rows_lab,
    std::size_t width,
    std::uint8_t changed_slot) {

    auto num_colors = current_lab.size();
    auto& new_lab = current_lab[changed_slot];

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
                for (std::size_t k = 0; k < num_colors; ++k) {
                    float dL = rl[x].L - current_lab[k].L;
                    float da = rl[x].a - current_lab[k].a;
                    float db = rl[x].b - current_lab[k].b;
                    float d = color_space::fma_dist_sq(dL, da, db);
                    if (d < best_d) { best_d = d; best_k = k; }
                }
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
// hist_pool: top-N most-frequent source colours (RGB444-bucketed) from
//   the whole image. When non-empty, the centroid candidate is
//   *snapped to the nearest histogram entry* before being scored —
//   guarantees the chip ends up displaying an actual source colour
//   rather than an arbitrary OCS-grid centroid that may match no
//   real pixel. Per ham_convert 1.0.x ("source palette size for
//   lines>0 increased from 16 to 256 — much better picture quality").
SwapCandidate find_best_swap(
    std::span<const Color3f> current_pal,
    std::span<const color_space::OKLab> current_lab,
    std::span<const std::span<const Color3f>> rows,
    std::span<const std::span<const color_space::OKLab>> rows_lab,
    std::span<const float> weights,
    amiga::Chipset chipset,
    SwapScratch& sc,
    const std::vector<bool>& excluded = {},
    std::span<const float> column_weights = {},
    std::span<const Color3f> hist_pool = {},
    std::span<const color_space::OKLab> hist_pool_lab = {}) {

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

    for (std::size_t k = 0; k < num_colors; ++k) {
        if (stats[k].count < 0.001) continue;
        if (!excluded.empty() && excluded[k]) continue;

        // Ideal centroid for this cluster (weighted by neighbor rows)
        auto n = stats[k].count;
        color_space::OKLab centroid{
            static_cast<float>(stats[k].sum_L / n),
            static_cast<float>(stats[k].sum_a / n),
            static_cast<float>(stats[k].sum_b / n),
        };

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
                new_error += static_cast<double>(color_space::fma_dist_sq(dL, da, db))
                           * static_cast<double>(pixel_weights[base + x]);
            }
        }

        auto reduction = static_cast<float>(stats[k].total_error - new_error);
        if (reduction > best.error_reduction) {
            auto linear = color_space::oklab_to_linear(centroid).clamped();
            // Snap to chipset precision
            if (chipset != amiga::Chipset::aga) {
                linear = palette::quantize_to_ocs(linear);
            }
            // [Histogram-pool snap experiment: tested and rejected.
            //  Snapping centroid to the nearest source colour in a
            //  256-entry histogram regressed lores+sliced by ~0.5 dB
            //  averaged across 4 images. Reason: centroid is the
            //  optimal-for-cluster colour; snapping it onto a strict
            //  256-color subset of the 4096-RGB444-gamut loses
            //  optimization headroom. The OCS snap above already
            //  produces a chip-displayable colour. Argument kept on
            //  the signature for future use; unused at default empty
            //  span.]
            (void)hist_pool; (void)hist_pool_lab;
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
// Two candidate colours are tried per slot: the base cluster's centroid
// (best for cluster k alone), and the half-brite cluster's centroid
// doubled in sRGB (best for cluster k+32 alone). Whichever gives the
// larger combined-cluster error reduction wins. With one tiebreak: if
// cluster k+32 is empty, only the base centroid is tried; if cluster k
// is empty, only the doubled half centroid.
SwapCandidate find_best_swap_ehb(
    std::span<const color_space::OKLab> current_lab,
    std::span<const std::span<const color_space::OKLab>> rows_lab,
    SwapScratch& sc,
    const std::vector<bool>& excluded = {}) {

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
                    new_err +=
                        static_cast<double>(color_space::fma_dist_sq(dL, da, db)) *
                        static_cast<double>(pixel_weights[base + x]);
                }
            }
            float reduction = static_cast<float>(cur_err - new_err);
            if (reduction > best.error_reduction) {
                best = {k, c_lin, reduction};
            }
        };

        if (count_b > 0.001) {
            color_space::OKLab cb{
                static_cast<float>(stats[k].sum_L / count_b),
                static_cast<float>(stats[k].sum_a / count_b),
                static_cast<float>(stats[k].sum_b / count_b)};
            try_candidate(color_space::oklab_to_linear(cb).clamped());
        }
        if (count_h > 0.001) {
            color_space::OKLab ch{
                static_cast<float>(stats[k + kBase].sum_L / count_h),
                static_cast<float>(stats[k + kBase].sum_a / count_h),
                static_cast<float>(stats[k + kBase].sum_b / count_h)};
            auto h_lin = color_space::oklab_to_linear(ch).clamped();
            auto h_srgb = color_space::linear_to_srgb(h_lin).clamped();
            Color3f doubled_srgb{
                std::min(2.0f * h_srgb.r, 1.0f),
                std::min(2.0f * h_srgb.g, 1.0f),
                std::min(2.0f * h_srgb.b, 1.0f)};
            try_candidate(
                color_space::srgb_to_linear(doubled_srgb).clamped());
        }
        // Suppress unused-parameter warning when current_lab path drops.
        (void)current_lab;
    }
    return best;
}

} // namespace

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
                                   std::function<void(float, std::string_view)>
                                       on_progress,
                                   std::size_t neighbor_radius,
                                   float neighbor_decay,
                                   const std::vector<std::size_t>& dither_excluded,
                                   std::optional<quantize::Algorithm> quantizer_override) {
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
    auto max_swappable = lock_color0
        ? (num_colors > 1 ? num_colors - 1 : std::size_t{0})
        : num_colors;

    // --copper-changes is a pure user escape hatch and bypasses the budget
    // check entirely — the user is telling us "emit exactly this K, I know
    // what I'm doing".
    auto base_k = std::min(
        max_changes_per_line(depth, false, false, chipset, is_lace), max_swappable);
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
    if (override_changes == 0 && chipset == amiga::Chipset::aga) {
        for (std::size_t bump = 3; bump >= 1; --bump) {
            auto stretch_k = base_k + bump;
            if (stretch_k > max_swappable) continue;
            auto stretch = encode_copper(image, depth, dither_settings, chipset,
                                         stretch_k, user_palette, lock_color0,
                                         locked, palette_diversity,
                                         skip_initial_swap_rows, is_lace, is_ehb,
                                         on_progress, neighbor_radius,
                                         neighbor_decay, dither_excluded);
            if (!stretch) return std::unexpected{stretch.error()};
            if (stretch->max_moves_per_line <= move_budget) return stretch;
            // Stretch overshot — try the next-smaller bump, or fall through.
        }
    }

    auto changes_per_line = std::min(
        override_changes == 0 ? base_k : override_changes, max_swappable);

    // Step 1: Base palette — user-provided or auto-quantized
    std::vector<Color3f> base_pal;
    std::string base_pal_name;
    if (user_palette && !user_palette->empty()) {
        // Use user palette as-is (already snapped to chipset by caller).
        // Locks overlay on top — user-palette case preserves its own
        // colour set; locks are point fixes from --lock-index /
        // --reserve-range.
        base_pal = *user_palette;
        if (base_pal.size() > num_colors)
            base_pal.resize(num_colors);
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
        auto qfn = [&](std::size_t k) -> Result<Palette> {
            auto r = quantize::quantize(image, k, algo);
            if (!r) return std::unexpected{r.error()};
            Palette p = std::move(*r);
            if (chipset != amiga::Chipset::aga) {
                for (auto& c : p.colors) c = palette::quantize_to_ocs(c);
            }
            if (palette_diversity > 0) {
                quantize::diversify_palette(p, image.pixels(),
                                            palette_diversity,
                                            chipset != amiga::Chipset::aga);
            }
            return p;
        };
        // Subtract locked.size() from k1 so the quantizer optimises for
        // the slots it'll actually fill (= unlocked + non-zero count).
        // Without this, the quantizer's "first num_colors-1" output
        // includes colours that land at locked indices and get
        // overwritten below, displacing those colours entirely. For
        // 5bpp + 15 reserves at slots 1-15: quantizer at K=31 puts the
        // dark tones at slots 1-15 (sorted by L), the locked overlay
        // overwrites them with reserve colours, and the dither's
        // candidate set ends up with only the bright tail (slots
        // 16-31) plus slot 0 — image renders with no dark/mid colours.
        // Floor at 1.
        std::size_t k1 = num_colors;
        if (lock_color0 && k1 > 1) --k1;
        if (k1 > locked.size()) k1 -= locked.size();
        else                     k1 = 1;
        auto kfallback = std::min(num_colors,
            k1 + (lock_color0 ? std::size_t{1} : std::size_t{0}));
        auto pr = palette_locks::two_pass_quantize(
            qfn, k1, kfallback, lock_color0);
        if (!pr) return std::unexpected{pr.error()};
        base_pal_name = std::move(pr->name);
        // Fill base_pal: lock_zero at slot 0 (if requested), locks at
        // their indices, quantized in the remaining unlocked slots in
        // order. Skipping locked indices when filling means the
        // quantizer's colours land specifically where the dither will
        // actually be able to use them.
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
        std::size_t qi = 0;
        for (std::size_t i = 0; i < num_colors; ++i) {
            if (base_locked[i]) continue;
            if (qi < pr->colors.size()) base_pal[i] = pr->colors[qi++];
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

    // [Top-N source colour histogram pool experiment: tested and
    //  rejected — snapping centroid to nearest source colour in a
    //  256-entry RGB444 histogram regressed lores+sliced by ~0.5 dB.
    //  Centroid → OCS-grid snap is already enough; further
    //  constraining to a histogram subset loses optimization
    //  headroom. find_best_swap accepts the pool args as a
    //  no-op default; pass {} from the call site.]

    // Locate the "anchor slot" — the base-palette index that the most
    // pixels assign to. Per ham_convert 1.2.0 ("most common color is
    // never changed to reduce horizontal blocking artefacts"), holding
    // this slot fixed across all scanlines avoids wasting per-row
    // copper bandwidth re-introducing a colour that's already there
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
                    float dL = rl[x].L - base_lab[k].L;
                    float da = rl[x].a - base_lab[k].a;
                    float db = rl[x].b - base_lab[k].b;
                    float d = color_space::fma_dist_sq(dL, da, db);
                    if (d < best_d) { best_d = d; best_k = k; }
                }
                ++freq[best_k];
            }
        }
        // Pick the most-frequent slot, BUT skip slot 0 if it's already
        // reserve-color0-locked — picking it would just be redundant.
        std::size_t best_k = 0;
        std::size_t best_n = 0;
        for (std::size_t k = (lock_color0 ? std::size_t{1} : std::size_t{0});
             k < num_colors; ++k) {
            if (freq[k] > best_n) { best_n = freq[k]; best_k = k; }
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
    float pd_lo = jlo + (jhi - jlo) *
        (static_cast<float>(pd_iter) /
         static_cast<float>(predict_dither_iterations));
    float pd_hi = jlo + (jhi - jlo) *
        (static_cast<float>(pd_iter + 1) /
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
    struct SpreadDefault { std::size_t radius; float decay; };
    constexpr SpreadDefault kSpreadEHB    {4, 0.30f};
    constexpr SpreadDefault kSpreadDPF    {3, 0.85f};
    constexpr SpreadDefault kSpreadLoresD5{2, 0.85f};
    constexpr SpreadDefault kSpreadOther  {4, 0.85f};
    SpreadDefault sd =
        is_ehb       ? kSpreadEHB     :
        depth <= 3   ? kSpreadDPF     :
        depth == 5   ? kSpreadLoresD5 :
                       kSpreadOther;
    const std::size_t resolved_radius =
        (neighbor_radius == std::numeric_limits<std::size_t>::max())
        ? sd.radius : neighbor_radius;
    const float resolved_decay =
        (neighbor_decay < 0.0f) ? sd.decay : neighbor_decay;

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
                rows_lab.push_back(all_lab[y]);
                weights.push_back(w);
            } else {
                if (y >= dy) {
                    rows.push_back(image.row(y - dy));
                    rows_lab.push_back(all_lab[y - dy]);
                    weights.push_back(w);
                }
                if (y + dy < height) {
                    rows.push_back(image.row(y + dy));
                    rows_lab.push_back(all_lab[y + dy]);
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
                swapped[idx] = true;  // don't swap locked slots
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
        build_swap_scratch(sc, pal_lab, rows_lab, weights, width, col_weights);

        // Skip swap-finding for initial rows (interlace: row 0 is field 1's
        // first displayed line and row 1 is field 2's — neither has a prior
        // scanline on which to pre-apply palette changes, so both must display
        // with the base palette as-is).
        auto this_row_changes = (y < skip_initial_swap_rows)
            ? std::size_t{0} : changes_per_line;
        for (std::size_t s = 0; s < this_row_changes; ++s) {
            SwapCandidate swap{0, {}, -1.0f};
            if (is_ehb) {
                swap = find_best_swap_ehb(pal_lab, rows_lab, sc, swapped);
            } else {
                swap = find_best_swap(
                    current_pal, pal_lab, rows, rows_lab, weights, chipset, sc,
                    swapped, col_weights);
            }

            if (swap.error_reduction <= 0.0f) break;

            // Per-slot drift cap: reject swaps that take a slot farther
            // than kMaxDriftSq (OKLab²) from its base_pal value. The
            // greedy planner can pick centroids that are perceptually
            // far from base when consecutive rows have varied content;
            // accumulating those moves over many rows produces a slot
            // whose colour is wildly different from base_pal[slot]. A
            // pixel near base_pal[slot]'s value in subsequent rows then
            // finds the drifted slot in OKLab terms (very dark vs
            // slightly-brighter is a near-tie in OKLab L) and renders
            // with the drifted colour, producing visible cross-row
            // banding (chuck31: black-space pixels rendering as #444433
            // because slot 1 had drifted from base #111 to #444).
            // The cap keeps each slot perceptually within ΔE ~12 of base.
            constexpr float kMaxDriftSq = 0.03f;
            auto base_lab = color_space::linear_to_oklab(base_pal[swap.slot]);
            auto new_lab = color_space::linear_to_oklab(swap.new_color);
            float ddL = base_lab.L - new_lab.L;
            float dda = base_lab.a - new_lab.a;
            float ddb = base_lab.b - new_lab.b;
            if (color_space::fma_dist_sq(ddL, dda, ddb) > kMaxDriftSq) break;

            // Nibble-skip optimization (AGA only): if the new color shares its
            // high 4-bit nibble with the slot's previous value, the LOCT=0 (high)
            // write is unnecessary. The viewer detects skip_hi via a 0xFFFF
            // sentinel in the hi-table reg field.
            bool skip_hi_flag = false;
            bool skip_lo_flag = false;
            if (chipset == amiga::Chipset::aga) {
                auto old_hilo = palette::linear_to_aga_hilo(current_pal[swap.slot]);
                auto new_hilo = palette::linear_to_aga_hilo(swap.new_color);
                skip_hi_flag = (old_hilo.hi == new_hilo.hi);
                skip_lo_flag = (old_hilo.lo == new_hilo.lo);
            }

            // Apply the swap. For EHB, also update the half-brite mirror
            // slot (k+32) and refresh the scratch for both — pixels can
            // migrate in or out of either cluster as a result.
            swapped[swap.slot] = true;
            current_pal[swap.slot] = swap.new_color;
            pal_lab[swap.slot] = color_space::linear_to_oklab(swap.new_color);
            if (is_ehb) {
                pal_lab[num_colors + swap.slot] =
                    color_space::linear_to_oklab(palette::half_brite(swap.new_color));
            }
            refresh_swap_scratch(sc, pal_lab, rows_lab, width,
                                 static_cast<std::uint8_t>(swap.slot));
            if (is_ehb) {
                refresh_swap_scratch(sc, pal_lab, rows_lab, width,
                                     static_cast<std::uint8_t>(
                                         num_colors + swap.slot));
            }
            changes.push_back(CopperChange{
                static_cast<std::uint8_t>(swap.slot),
                swap.new_color,
                skip_hi_flag,
                skip_lo_flag,
            });
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
                        float dL = all_lab[y][x].L - pal_lab_sort[k].L;
                        float da = all_lab[y][x].a - pal_lab_sort[k].a;
                        float db = all_lab[y][x].b - pal_lab_sort[k].b;
                        float d = color_space::fma_dist_sq(dL, da, db);
                        if (d < best_d) { best_d = d; best_k = k; }
                    }
                    if (best_k == ch.reg) {
                        ch.avg_x = static_cast<float>(x);
                        break;  // first occurrence found
                    }
                }
            }
            std::sort(changes.begin(), changes.end(),
                      [](const CopperChange& a, const CopperChange& b) {
                          return a.avg_x < b.avg_x;
                      });
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
            pd_progress(0.5f *
                static_cast<float>(y + 1) / static_cast<float>(height));
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
    if (depth <= 4 && chipset != amiga::Chipset::aga) {
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
        int max_spread = 6 + gap * 3;           // d1:18 d2:15 d3:12 d4:9
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
                    int eff_spread = std::max(2, static_cast<int>(
                        static_cast<float>(max_spread) * (1.0f - norm)));

                    float ramp = std::min(
                        1.0f,
                        static_cast<float>(candidate_count) /
                            static_cast<float>(eff_spread));
                    // Use the field-local row index for the golden-ratio
                    // phase so successive in-field rows step through the
                    // sequence cleanly. Using raw y under stride=2 would
                    // skip every other φ value, producing a 50/50 phase
                    // that looks like noise rather than a smooth ramp.
                    float threshold = std::fmod(
                        static_cast<float>(seq_idx) * phi + 0.5f, 1.0f);
                    scanline_palettes[y][r] =
                        (ramp > threshold) ? candidate : committed;

                    if (candidate_count >= eff_spread) {
                        committed = candidate;
                        candidate_count = 0;
                    }
                }
            }
        };

        if (is_lace) {
            run_pass(0, 2);                       // field 1: rows 0, 2, 4, …
            if (height >= 2) run_pass(1, 2);      // field 2: rows 1, 3, 5, …
        } else {
            run_pass(0, 1);                       // progressive: every row
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

    // Pre-convert each row's sliced palette to OKLab once. When --reserve-range
    // is active, drop excluded slots from the candidate set (cand_to_full[y]
    // maps the filtered index back to the actual palette slot).
    std::vector<bool> excluded_mask;
    if (!dither_excluded.empty()) {
        excluded_mask.assign(num_colors, false);
        for (auto i : dither_excluded)
            if (i < num_colors) excluded_mask[i] = true;
    }
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
                pal_lab_per_row[y].push_back(
                    color_space::linear_to_oklab(pal[i]));
                cand_to_full_per_row[y].push_back(static_cast<std::uint8_t>(i));
            }
        }
    }

    total_error = dither::diffuse_raw_buffer(
        image, dither_settings,
        [&](const color_space::OKLab& target,
            std::size_t x, std::size_t y) -> dither::PickResult {
            auto& pal_lab = pal_lab_per_row[y];
            std::size_t k = 0;
            color_space::OKLab chosen{};
            float thr = dither::pick_palette_index_with_ostro(
                dither_settings.method, target, pal_lab, x, y,
                dither_settings.strength, /*k_min=*/0, k, chosen);
            std::uint8_t full_idx = excluded_mask.empty()
                ? static_cast<std::uint8_t>(k)
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
                image, all_indices,
                [&](std::size_t /*x*/, std::size_t y)
                    -> std::span<const color_space::OKLab> {
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
                image, cand_indices,
                [&](std::size_t /*x*/, std::size_t y)
                    -> std::span<const color_space::OKLab> {
                    return pal_lab_per_row[y];
                });
            for (std::size_t i = 0; i < cand_indices.size(); ++i) {
                auto y = i / width;
                all_indices[i] = cand_to_full_per_row[y][cand_indices[i]];
            }
        }
    }
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

    } // end predict_dither_iterations loop

    if (on_progress) on_progress(1.0f, "done");

    // Encode to bitplanes
    auto planes = bitplane::encode(all_indices, width, height, depth);
    if (!planes) return std::unexpected{planes.error()};

    // Compute average actual changes per line
    std::size_t total_changes = 0;
    for (auto& ch : scanline_changes) total_changes += ch.size();
    float avg_changes = height > 0
        ? static_cast<float>(total_changes) / static_cast<float>(height)
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
            std::format("Expected {} scanline palettes, got {}",
                        height, scanline_palettes.size()),
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
            std::format("Expected {} scanline palettes, got {}",
                        height, scanline_palettes.size()),
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

    auto pick_topk_diffs = [&](const std::vector<Color3f>& prev,
                               const std::vector<Color3f>& cur) {
        struct Cand { std::size_t reg; float dist; Color3f color; };
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
            std::partial_sort(
                cands.begin(),
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
        const auto& prev_target = is_lace
            ? (y < 2 ? base_vec : scanline_palettes[y - 2])
            : (y == 0 ? base_vec : scanline_palettes[y - 1]);
        auto cands = pick_topk_diffs(prev_target, target);
        for (auto& c : cands) state[c.reg] = c.color;
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

} // namespace png2amiga::copper
