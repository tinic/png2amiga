#include "copper.hpp"
#include "bitplane.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "quantize.hpp"
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
                float d = dL * dL + da * da + db * db;
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
                d_to_new = dL * dL + da * da + db * db;
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
                    float d = dL * dL + da * da + db * db;
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
SwapCandidate find_best_swap(
    std::span<const Color3f> current_pal,
    std::span<const color_space::OKLab> current_lab,
    std::span<const std::span<const Color3f>> rows,
    std::span<const std::span<const color_space::OKLab>> rows_lab,
    std::span<const float> weights,
    amiga::Chipset chipset,
    SwapScratch& sc,
    const std::vector<bool>& excluded = {},
    std::span<const float> column_weights = {}) {

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
                new_error += static_cast<double>(dL * dL + da * da + db * db)
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
            best = {k, linear, reduction};
        }
    }

    return best;
}

// Dither a single row with correct Y coordinate for ordered dithering
void dither_row(std::span<const Color3f> row,
                std::span<const color_space::OKLab> pal_lab,
                std::size_t y,
                const dither::Settings& settings,
                std::vector<std::uint8_t>& out_indices,
                std::size_t out_offset,
                float& out_error) {
    auto width = row.size();
    auto num_colors = pal_lab.size();

    if (dither::is_ordered(settings.method)) {
        for (std::size_t x = 0; x < width; ++x) {
            auto pixel_lab = color_space::linear_to_oklab(row[x]);
            float threshold = dither::ordered_threshold(settings.method, x, y);
            pixel_lab.L += threshold * settings.strength * 0.15f;
            pixel_lab.a += threshold * settings.strength * 0.03f;
            pixel_lab.b += threshold * settings.strength * 0.03f;

            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            for (std::size_t k = 0; k < num_colors; ++k) {
                float dL = pixel_lab.L - pal_lab[k].L;
                float da = pixel_lab.a - pal_lab[k].a;
                float db = pixel_lab.b - pal_lab[k].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best_k = k; }
            }
            out_indices[out_offset + x] = static_cast<std::uint8_t>(best_k);
            out_error += best_d;
        }
    } else {
        // Error diffusion on 1-row image
        Image row_img(width, 1,
            std::vector<Color3f>(row.begin(), row.end()));
        // Build Color3f palette from OKLab
        std::vector<Color3f> pal_linear(num_colors);
        for (std::size_t i = 0; i < num_colors; ++i) {
            pal_linear[i] = color_space::oklab_to_linear(pal_lab[i]).clamped();
        }
        auto result = dither::apply(row_img, pal_linear, settings);
        std::copy(result.indices.begin(), result.indices.end(),
                  out_indices.begin()
                      + static_cast<std::ptrdiff_t>(out_offset));
        out_error += result.total_error;
    }
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
                                   bool reserve_color0,
                                   const std::vector<std::pair<std::size_t, Color3f>>& locked,
                                   int palette_diversity,
                                   std::size_t skip_initial_swap_rows,
                                   bool is_lace) {
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
    auto max_swappable = reserve_color0
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
                                         stretch_k, user_palette, reserve_color0,
                                         locked, palette_diversity,
                                         skip_initial_swap_rows, is_lace);
            if (!stretch) return std::unexpected{stretch.error()};
            if (stretch->max_moves_per_line <= move_budget) return stretch;
            // Stretch overshot — try the next-smaller bump, or fall through.
        }
    }

    auto changes_per_line = std::min(
        override_changes == 0 ? base_k : override_changes, max_swappable);

    // Step 1: Base palette — user-provided or auto-quantized
    std::vector<Color3f> base_pal;
    if (user_palette && !user_palette->empty()) {
        // Use user palette as-is (already snapped to chipset by caller)
        base_pal = *user_palette;
        if (base_pal.size() > num_colors)
            base_pal.resize(num_colors);
        while (base_pal.size() < num_colors)
            base_pal.push_back(Color3f{0.0f, 0.0f, 0.0f});
    } else {
        auto algo = (chipset == amiga::Chipset::aga)
            ? quantize::Algorithm::median_cut
            : quantize::Algorithm::ocs_bruteforce;
        auto reserve = reserve_color0
            ? ((num_colors > 1) ? num_colors - 1 : std::size_t{1})
            : num_colors;
        auto base_result = quantize::quantize(image, reserve, algo);
        if (!base_result) return std::unexpected{base_result.error()};
        base_pal = std::move(base_result->colors);
        if (chipset != amiga::Chipset::aga) {
            for (auto& c : base_pal) c = palette::quantize_to_ocs(c);
        }
        if (palette_diversity > 0) {
            Palette tmp;
            tmp.colors = base_pal;
            quantize::diversify_palette(tmp, image.pixels(),
                                        palette_diversity,
                                        chipset != amiga::Chipset::aga);
            base_pal = std::move(tmp.colors);
        }
        if (reserve_color0)
            base_pal.insert(base_pal.begin(), Color3f{0.0f, 0.0f, 0.0f});
        while (base_pal.size() < num_colors)
            base_pal.push_back(Color3f{0.0f, 0.0f, 0.0f});

    }

    // Apply locked palette slots (e.g., for blitter objects)
    for (auto& [idx, color] : locked) {
        if (idx < base_pal.size())
            base_pal[idx] = color;
    }

    // Step 2: Iterative two-pass predict+dither loop.
    //
    // Iteration 1: pass 1 predicts per-scanline palettes using raw-pixel
    //              error for column priority; pass 2 dithers with those
    //              palettes and measures DITHERED per-column error.
    // Iteration 2: pass 1 re-predicts using the dithered error map from
    //              iteration 1 as column weights — swaps now target where
    //              the ditherer actually struggled, not just where raw
    //              pixel-to-palette distance was high.
    //
    // The feedback loop closes the gap between "what the optimizer thinks
    // matters" and "what the dithered output actually looks like."

    std::vector<std::uint8_t> all_indices(width * height);
    std::vector<std::vector<CopperChange>> scanline_changes(height);
    std::vector<std::vector<Color3f>> scanline_palettes(height);
    float total_error = 0.0f;

    bool use_diffusion = dither_settings.method != dither::Method::none &&
                         !dither::is_ordered(dither_settings.method);
    std::vector<color_space::OKLab> err_buf;

    // Precompute all rows in OKLab for neighbor lookups
    std::vector<std::vector<color_space::OKLab>> all_lab(height);
    for (std::size_t y = 0; y < height; ++y) {
        all_lab[y].resize(width);
        auto row = image.row(y);
        for (std::size_t x = 0; x < width; ++x)
            all_lab[y][x] = color_space::linear_to_oklab(row[x]);
    }

    constexpr float col_decay = 0.85f;
    constexpr float col_scale = 2.0f;
    constexpr int predict_dither_iterations = 2;

    // Column error seeded as zeros for iteration 1; fed back from
    // dithered output for iteration 2+.
    std::vector<float> column_error(width, 0.0f);

    for (int pd_iter = 0; pd_iter < predict_dither_iterations; ++pd_iter) {

    // --- Pass 1: predict per-scanline palettes ---
    // For interlace, each field has its own palette state since field 1
    // (rows 0,2,4,...) and field 2 (rows 1,3,5,...) accumulate independently.
    // The per-field K-swap budget then only has to cover one row's diff.
    std::vector<Color3f> current_pal_f1 = base_pal;
    std::vector<Color3f> current_pal_f2 = base_pal;
    // Reset column error accumulator for this pass (seeded from previous
    // iteration's dithered feedback, or zeros on first iteration)
    auto pass1_column_error = column_error;

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
        // Current row weight=1.0, neighbors decay with distance.
        constexpr std::size_t neighbor_radius = 4;
        constexpr float decay = 0.5f;  // weight halves per row of distance
        std::vector<std::span<const Color3f>> rows;
        std::vector<std::span<const color_space::OKLab>> rows_lab;
        std::vector<float> weights;
        for (std::size_t dy = 0; dy <= neighbor_radius; ++dy) {
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
        if (reserve_color0) swapped[0] = true;  // don't swap COLOR00
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
        std::vector<color_space::OKLab> pal_lab(num_colors);
        for (std::size_t i = 0; i < num_colors; ++i) {
            pal_lab[i] = color_space::linear_to_oklab(current_pal[i]);
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
            auto swap = find_best_swap(
                current_pal, pal_lab, rows, rows_lab, weights, chipset, sc,
                swapped, col_weights);

            if (swap.error_reduction <= 0.0f) break;

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

            // Apply the swap (update pal_lab and scratch incrementally)
            swapped[swap.slot] = true;
            current_pal[swap.slot] = swap.new_color;
            pal_lab[swap.slot] = color_space::linear_to_oklab(swap.new_color);
            refresh_swap_scratch(sc, pal_lab, rows_lab, width,
                                 static_cast<std::uint8_t>(swap.slot));
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
                        float d = dL * dL + da * da + db * db;
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
                float d = dL * dL + da * da + db * db;
                if (d < best_d) best_d = d;
            }
            pass1_column_error[x] = pass1_column_error[x] * col_decay + best_d;
        }
    }

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

        for (std::size_t r = 1; r < num_colors; ++r) {
            Color3f committed = scanline_palettes[0][r];
            Color3f candidate = committed;
            int candidate_count = 0;

            for (std::size_t y = 1; y < height; ++y) {
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
                float dist = dL * dL + da * da + db * db;

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
                float threshold = std::fmod(
                    static_cast<float>(y) * phi + 0.5f, 1.0f);
                scanline_palettes[y][r] =
                    (ramp > threshold) ? candidate : committed;

                if (candidate_count >= eff_spread) {
                    committed = candidate;
                    candidate_count = 0;
                }
            }
        }
    }

    // Reset dithering state for this iteration
    total_error = 0.0f;
    if (use_diffusion) {
        err_buf.assign(width * height, color_space::OKLab{0, 0, 0});
    }

    // --- Pass 2: Dither with the predetermined per-scanline palettes ---
    //
    // Now that every scanline's effective palette is known, error diffusion
    // can flow correctly across scanline boundaries — each row dithers
    // against its own palette, and the error propagated to the next row
    // is applied against that row's (different) palette.
    //
    // In the old single-pass approach, swaps and dithering were interleaved
    // so error from scanline Y (palette A) bled into scanline Y+1 (palette
    // B) without the ditherer knowing the palette had changed.
    // ===================================================================

    for (std::size_t y = 0; y < height; ++y) {
        auto row = image.row(y);
        auto& pal = scanline_palettes[y];

        std::vector<color_space::OKLab> pal_lab(num_colors);
        for (std::size_t i = 0; i < num_colors; ++i)
            pal_lab[i] = color_space::linear_to_oklab(pal[i]);

        if (use_diffusion) {
            auto kernel = dither::error_diffusion_kernel(dither_settings.method);
            // Ostromoukhov uses F-S kernel with variable scaling
            bool is_ostro = (dither_settings.method == dither::Method::ostromoukhov);
            if (is_ostro)
                kernel = dither::error_diffusion_kernel(dither::Method::floyd_steinberg);
            bool reverse = (y % 2 == 1);
            auto ec = dither_settings.error_clamp;
            auto str = dither_settings.strength;
            for (std::size_t step = 0; step < width; ++step) {
                std::size_t x = reverse ? (width - 1 - step) : step;
                auto pixel_lab = color_space::linear_to_oklab(row[x]);

                auto& e = err_buf[y * width + x];
                pixel_lab.L += std::clamp(e.L, -ec, ec);
                pixel_lab.a += std::clamp(e.a, -ec, ec);
                pixel_lab.b += std::clamp(e.b, -ec, ec);

                float best_d = std::numeric_limits<float>::max();
                float second_d = std::numeric_limits<float>::max();
                std::size_t best_k = 0;
                for (std::size_t k = 0; k < num_colors; ++k) {
                    float dL = pixel_lab.L - pal_lab[k].L;
                    float da = pixel_lab.a - pal_lab[k].a;
                    float db = pixel_lab.b - pal_lab[k].b;
                    float d = dL * dL + da * da + db * db;
                    if (d < best_d) {
                        second_d = best_d;
                        best_d = d; best_k = k;
                    } else if (d < second_d) {
                        second_d = d;
                    }
                }
                all_indices[y * width + x] = static_cast<std::uint8_t>(best_k);
                total_error += best_d;

                // Ostromoukhov: scale diffusion by threshold level
                float ostro_scale = 1.0f;
                if (is_ostro && second_d > 1e-12f) {
                    float threshold = std::sqrt(best_d) /
                        (std::sqrt(best_d) + std::sqrt(second_d));
                    ostro_scale = 0.6f + 0.8f * threshold;
                }

                auto orig_lab = color_space::linear_to_oklab(row[x]);
                color_space::OKLab qerr = {
                    (orig_lab.L - pal_lab[best_k].L) * str,
                    (orig_lab.a - pal_lab[best_k].a) * str,
                    (orig_lab.b - pal_lab[best_k].b) * str,
                };
                for (auto& [kdx, kdy, kw] : kernel) {
                    auto nx = static_cast<std::ptrdiff_t>(x) + (reverse ? -kdx : kdx);
                    auto ny = static_cast<std::ptrdiff_t>(y) + kdy;
                    if (nx >= 0 && static_cast<std::size_t>(nx) < width &&
                        ny >= 0 && static_cast<std::size_t>(ny) < height) {
                        auto idx = static_cast<std::size_t>(ny) * width +
                                   static_cast<std::size_t>(nx);
                        err_buf[idx].L += qerr.L * kw * ostro_scale;
                        err_buf[idx].a += qerr.a * kw * ostro_scale;
                        err_buf[idx].b += qerr.b * kw * ostro_scale;
                    }
                }
            }
        } else {
            dither_row(row, pal_lab, y, dither_settings,
                       all_indices, y * width, total_error);
        }
    }

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
                column_error[x] += dL * dL + da * da + db * db;
            }
        }
    }

    } // end predict_dither_iterations loop

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

} // namespace png2amiga::copper
