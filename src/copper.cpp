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
    const std::vector<bool>& excluded = {}) {

    auto num_colors = current_pal.size();
    auto width = rows[0].size();

    // Assign each pixel to nearest palette color and track per-slot error
    struct SlotStats {
        double sum_L{}, sum_a{}, sum_b{};
        double total_error{};
        double count{};
    };
    std::vector<SlotStats> stats(num_colors);

    for (std::size_t r = 0; r < rows.size(); ++r) {
        auto w = static_cast<double>(weights[r]);
        auto& rl = rows_lab[r];
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
            stats[best_k].sum_L += static_cast<double>(rl[x].L) * w;
            stats[best_k].sum_a += static_cast<double>(rl[x].a) * w;
            stats[best_k].sum_b += static_cast<double>(rl[x].b) * w;
            stats[best_k].total_error += static_cast<double>(best_d) * w;
            stats[best_k].count += w;
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

        // Error with the centroid instead of current color
        double new_error = 0.0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            auto w = static_cast<double>(weights[r]);
            auto& rl = rows_lab[r];
            for (std::size_t x = 0; x < width; ++x) {
                // Check if pixel is assigned to this slot
                float best_d = std::numeric_limits<float>::max();
                std::size_t best_k = 0;
                for (std::size_t j = 0; j < num_colors; ++j) {
                    float dL = rl[x].L - current_lab[j].L;
                    float da = rl[x].a - current_lab[j].a;
                    float db = rl[x].b - current_lab[j].b;
                    float d = dL * dL + da * da + db * db;
                    if (d < best_d) { best_d = d; best_k = j; }
                }
                if (best_k != k) continue;
                float dL = rl[x].L - centroid.L;
                float da = rl[x].a - centroid.a;
                float db = rl[x].b - centroid.b;
                new_error += static_cast<double>(dL * dL + da * da + db * db) * w;
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
                                   bool is_ham,
                                   bool is_hires,
                                   std::size_t override_changes) {
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
    auto changes_per_line = (override_changes > 0) ? override_changes
        : max_changes_per_line(depth, is_ham, is_hires, chipset);

    // Step 1: Generate global base palette (N-1 colors + black at index 0)
    auto algo = (chipset == amiga::Chipset::aga)
        ? quantize::Algorithm::median_cut
        : quantize::Algorithm::ocs_bruteforce;
    auto reserve = (num_colors > 1) ? num_colors - 1 : std::size_t{1};
    auto base_result = quantize::quantize(image, reserve, algo);
    if (!base_result) return std::unexpected{base_result.error()};
    auto base_pal = std::move(base_result->colors);
    if (chipset != amiga::Chipset::aga) {
        for (auto& c : base_pal) c = palette::quantize_to_ocs(c);
    }
    base_pal.insert(base_pal.begin(), Color3f{0.0f, 0.0f, 0.0f});
    while (base_pal.size() < num_colors) {
        base_pal.push_back(Color3f{0.0f, 0.0f, 0.0f});
    }

    // Step 2: Process scanlines — accumulate palette changes
    std::vector<Color3f> current_pal = base_pal;
    std::vector<std::uint8_t> all_indices(width * height);
    std::vector<std::vector<CopperChange>> scanline_changes(height);
    std::vector<std::vector<Color3f>> scanline_palettes(height);
    float total_error = 0.0f;

    // Precompute all rows in OKLab for neighbor lookups
    std::vector<std::vector<color_space::OKLab>> all_lab(height);
    for (std::size_t y = 0; y < height; ++y) {
        all_lab[y].resize(width);
        auto row = image.row(y);
        for (std::size_t x = 0; x < width; ++x)
            all_lab[y][x] = color_space::linear_to_oklab(row[x]);
    }

    for (std::size_t y = 0; y < height; ++y) {
        auto row = image.row(y);

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
                current_pal, pal_lab, rows, rows_lab, weights, chipset, swapped);

            if (swap.error_reduction <= 0.0f) break;

            // Apply the swap
            swapped[swap.slot] = true;
            current_pal[swap.slot] = swap.new_color;
            changes.push_back(CopperChange{static_cast<std::uint8_t>(swap.slot),
                                          swap.new_color});
        }

        // Sort changes by register index (bank 0 first) to minimize
        // BPLCON3 switches in the copper list
        std::sort(changes.begin(), changes.end(),
                  [](const CopperChange& a, const CopperChange& b) {
                      return a.reg < b.reg;
                  });

        // Snapshot the effective palette for this scanline
        scanline_palettes[y] = current_pal;
        scanline_changes[y] = std::move(changes);

        // Dither this scanline with the effective palette
        std::vector<color_space::OKLab> pal_lab(num_colors);
        for (std::size_t i = 0; i < num_colors; ++i) {
            pal_lab[i] = color_space::linear_to_oklab(current_pal[i]);
        }

        dither_row(row, pal_lab, y, dither_settings,
                   all_indices, y * width, total_error);
    }

    // Encode to bitplanes
    auto planes = bitplane::encode(all_indices, width, height, depth);
    if (!planes) return std::unexpected{planes.error()};

    // Compute average actual changes per line
    std::size_t total_changes = 0;
    for (auto& ch : scanline_changes) total_changes += ch.size();
    float avg_changes = height > 0
        ? static_cast<float>(total_changes) / static_cast<float>(height)
        : 0.0f;

    return CopperResult{
        *std::move(planes),
        std::move(base_pal),
        std::move(scanline_changes),
        std::move(scanline_palettes),
        num_colors,
        changes_per_line,
        avg_changes,
        total_error,
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
