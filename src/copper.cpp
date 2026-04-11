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
    const std::vector<bool>& excluded = {},
    std::span<const float> column_weights = {}) {

    auto num_colors = current_pal.size();
    auto width = rows[0].size();

    // Assign each pixel to nearest palette color and track per-slot error.
    // Column weights boost pixels in high-error vertical regions so copper
    // moves preferentially fix persistent problem areas.
    struct SlotStats {
        double sum_L{}, sum_a{}, sum_b{};
        double total_error{};
        double count{};
    };
    std::vector<SlotStats> stats(num_colors);

    // Cache assignments and weighted OKLab values for reuse
    auto total_pixels = rows.size() * width;
    std::vector<std::uint8_t> assignments(total_pixels);
    std::vector<float> pixel_weights(total_pixels);

    for (std::size_t r = 0; r < rows.size(); ++r) {
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
            assignments[base + x] = static_cast<std::uint8_t>(best_k);
            pixel_weights[base + x] = w;
            stats[best_k].sum_L += static_cast<double>(rl[x].L) * wd;
            stats[best_k].sum_a += static_cast<double>(rl[x].a) * wd;
            stats[best_k].sum_b += static_cast<double>(rl[x].b) * wd;
            stats[best_k].total_error += static_cast<double>(best_d) * wd;
            stats[best_k].count += wd;
        }
    }

    // For each slot, compute what happens if we replace it with its
    // assigned pixels' centroid (the ideal color for that cluster).
    // The slot with the highest error reduction wins.
    // Skip slot 0 — it's COLOR00 (background/border color)
    SwapCandidate best{0, {}, -1.0f};

    for (std::size_t k = 1; k < num_colors; ++k) {
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
        for (std::size_t r = 0; r < rows.size(); ++r) {
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
                                   const std::vector<Color3f>* user_palette) {
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
    auto max_swappable = num_colors > 1 ? num_colors - 1 : std::size_t{0};  // slot 0 reserved

    // --copper-changes is a pure user escape hatch and bypasses the budget
    // check entirely — the user is telling us "emit exactly this K, I know
    // what I'm doing".
    auto base_k = std::min(
        max_changes_per_line(depth, false, false, chipset), max_swappable);

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
                                         stretch_k, user_palette);
            if (!stretch) return std::unexpected{stretch.error()};
            if (stretch->max_moves_per_line <= MOVE_BUDGET_PER_LINE) return stretch;
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
        // Auto-quantize: N-1 colors + black at index 0
        auto algo = (chipset == amiga::Chipset::aga)
            ? quantize::Algorithm::median_cut
            : quantize::Algorithm::ocs_bruteforce;
        auto reserve = (num_colors > 1) ? num_colors - 1 : std::size_t{1};
        auto base_result = quantize::quantize(image, reserve, algo);
        if (!base_result) return std::unexpected{base_result.error()};
        base_pal = std::move(base_result->colors);
        if (chipset != amiga::Chipset::aga) {
            for (auto& c : base_pal) c = palette::quantize_to_ocs(c);
        }
        base_pal.insert(base_pal.begin(), Color3f{0.0f, 0.0f, 0.0f});
        while (base_pal.size() < num_colors)
            base_pal.push_back(Color3f{0.0f, 0.0f, 0.0f});

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
    std::vector<Color3f> current_pal = base_pal;
    // Reset column error accumulator for this pass (seeded from previous
    // iteration's dithered feedback, or zeros on first iteration)
    auto pass1_column_error = column_error;

    for (std::size_t y = 0; y < height; ++y) {
        auto row = image.row(y);

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

        for (std::size_t s = 0; s < changes_per_line; ++s) {
            // Precompute current palette in OKLab
            std::vector<color_space::OKLab> pal_lab(num_colors);
            for (std::size_t i = 0; i < num_colors; ++i) {
                pal_lab[i] = color_space::linear_to_oklab(current_pal[i]);
            }

            auto swap = find_best_swap(
                current_pal, pal_lab, rows, rows_lab, weights, chipset, swapped,
                col_weights);

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

            // Apply the swap
            swapped[swap.slot] = true;
            current_pal[swap.slot] = swap.new_color;
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
            // Compute current palette in OKLab for nearest-color lookup
            std::vector<color_space::OKLab> pal_lab_sort(num_colors);
            for (std::size_t i = 0; i < num_colors; ++i)
                pal_lab_sort[i] = color_space::linear_to_oklab(current_pal[i]);

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
        // per-pixel error against the effective palette.
        {
            std::vector<color_space::OKLab> pal_lab_tmp(num_colors);
            for (std::size_t i = 0; i < num_colors; ++i)
                pal_lab_tmp[i] = color_space::linear_to_oklab(current_pal[i]);
            for (std::size_t x = 0; x < width; ++x) {
                auto pixel_lab = all_lab[y][x];
                float best_d = std::numeric_limits<float>::max();
                for (std::size_t k = 0; k < num_colors; ++k) {
                    float dL = pixel_lab.L - pal_lab_tmp[k].L;
                    float da = pixel_lab.a - pal_lab_tmp[k].a;
                    float db = pixel_lab.b - pal_lab_tmp[k].b;
                    float d = dL * dL + da * da + db * db;
                    if (d < best_d) best_d = d;
                }
                pass1_column_error[x] = pass1_column_error[x] * col_decay + best_d;
            }
        }
    }

    // --- Vertical palette smoothing ---
    // Anti-alias palette transitions between scanlines by blending each
    // register's color with its vertical neighbors in OKLab. This prevents
    // the harsh "band" effect when a register snaps from one color to
    // another across a single scanline boundary.
    // Kernel: 5-tap [0.1, 0.2, 0.4, 0.2, 0.1] — gentle, preserves detail.
    {
        constexpr float kw[] = {0.1f, 0.2f, 0.4f, 0.2f, 0.1f};
        constexpr int krad = 2;
        bool is_ocs = (chipset != amiga::Chipset::aga);

        auto smoothed = scanline_palettes;  // copy
        for (std::size_t r = 1; r < num_colors; ++r) {  // skip reg 0 (black)
            for (std::size_t y = 0; y < height; ++y) {
                float sL = 0, sa = 0, sb = 0, sw = 0;
                for (int d = -krad; d <= krad; ++d) {
                    auto ny = static_cast<int>(y) + d;
                    if (ny < 0 || static_cast<std::size_t>(ny) >= height) continue;
                    auto lab = color_space::linear_to_oklab(
                        scanline_palettes[static_cast<std::size_t>(ny)][r]);
                    float w = kw[d + krad];
                    sL += lab.L * w;
                    sa += lab.a * w;
                    sb += lab.b * w;
                    sw += w;
                }
                auto blended = color_space::oklab_to_linear(
                    color_space::OKLab{sL / sw, sa / sw, sb / sw}).clamped();
                if (is_ocs) blended = palette::quantize_to_ocs(blended);
                smoothed[y][r] = blended;
            }
        }
        scanline_palettes = std::move(smoothed);
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
                std::size_t best_k = 0;
                for (std::size_t k = 0; k < num_colors; ++k) {
                    float dL = pixel_lab.L - pal_lab[k].L;
                    float da = pixel_lab.a - pal_lab[k].a;
                    float db = pixel_lab.b - pal_lab[k].b;
                    float d = dL * dL + da * da + db * db;
                    if (d < best_d) { best_d = d; best_k = k; }
                }
                all_indices[y * width + x] = static_cast<std::uint8_t>(best_k);
                total_error += best_d;

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
                        err_buf[idx].L += qerr.L * kw;
                        err_buf[idx].a += qerr.a * kw;
                        err_buf[idx].b += qerr.b * kw;
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
