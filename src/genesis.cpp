#include "genesis.hpp"
#include "color_space.hpp"
#include "console_color.hpp"
#include "quantize.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <unordered_map>
#include <utility>

namespace png2amiga::genesis {

namespace {

using OKLab = color_space::OKLab;

// Per-tile descriptor used by the clustering step. We approximate each
// tile as its centroid in OKLab plus a "spread" term so two tiles that
// share a centroid but cover very different colour ranges still want
// distinct palettes. Cheap and good enough for k=4.
struct TileFingerprint {
    OKLab centroid{};
    float spread = 0.0f;     // sum of |pixel - centroid|² in OKLab
    std::size_t pixel_count = 0;
};

TileFingerprint fingerprint_tile(const Image& img,
                                 std::size_t tx0, std::size_t ty0) {
    TileFingerprint fp{};
    auto w = img.width();
    auto h = img.height();
    auto x_end = std::min(tx0 + kTileSide, w);
    auto y_end = std::min(ty0 + kTileSide, h);

    OKLab acc{};
    std::size_t n = 0;
    for (std::size_t y = ty0; y < y_end; ++y) {
        for (std::size_t x = tx0; x < x_end; ++x) {
            auto lab = color_space::linear_to_oklab(img[x, y]);
            acc.L += lab.L;
            acc.a += lab.a;
            acc.b += lab.b;
            ++n;
        }
    }
    if (n == 0) return fp;
    fp.centroid = OKLab{ acc.L / static_cast<float>(n),
                         acc.a / static_cast<float>(n),
                         acc.b / static_cast<float>(n) };
    fp.pixel_count = n;
    float spread = 0.0f;
    for (std::size_t y = ty0; y < y_end; ++y) {
        for (std::size_t x = tx0; x < x_end; ++x) {
            auto lab = color_space::linear_to_oklab(img[x, y]);
            float dL = lab.L - fp.centroid.L;
            float da = lab.a - fp.centroid.a;
            float db = lab.b - fp.centroid.b;
            spread += dL * dL + da * da + db * db;
        }
    }
    fp.spread = spread;
    return fp;
}

// Simple k-means in OKLab with farthest-point seeding. Returns cluster
// assignment per tile (size = n_tiles).
std::vector<std::uint8_t> kmeans_tiles(
    std::span<const TileFingerprint> tiles, std::uint32_t seed) {

    constexpr std::size_t K = kPaletteCount;
    std::vector<std::uint8_t> assign(tiles.size(), 0);
    if (tiles.empty()) return assign;

    // Farthest-point seeding (k-means++ flavour). Start with the first
    // non-empty tile, then pick each next centroid as the tile farthest
    // from any existing centroid.
    std::array<OKLab, K> centroids{};
    std::array<bool, K> centroid_set{};
    centroids[0] = tiles.front().centroid;
    centroid_set[0] = true;

    auto sq_dist = [](const OKLab& a, const OKLab& b) {
        float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
        return dL * dL + da * da + db * db;
    };

    for (std::size_t k = 1; k < K; ++k) {
        float best_d = -1.0f;
        std::size_t best_i = 0;
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (tiles[i].pixel_count == 0) continue;
            float min_d = std::numeric_limits<float>::max();
            for (std::size_t kk = 0; kk < k; ++kk) {
                float d = sq_dist(tiles[i].centroid, centroids[kk]);
                if (d < min_d) min_d = d;
            }
            if (min_d > best_d) { best_d = min_d; best_i = i; }
        }
        centroids[k] = tiles[best_i].centroid;
        centroid_set[k] = true;
    }

    // Tiny mt19937 only used to break exact-tie distances reproducibly.
    std::mt19937 rng(seed ? seed : 0xC0FFEEu);

    constexpr int kMaxIter = 16;
    for (int iter = 0; iter < kMaxIter; ++iter) {
        bool changed = false;
        // Assign step.
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (tiles[i].pixel_count == 0) {
                if (assign[i] != 0) { assign[i] = 0; changed = true; }
                continue;
            }
            float best_d = std::numeric_limits<float>::max();
            std::uint8_t best_k = 0;
            for (std::size_t k = 0; k < K; ++k) {
                float d = sq_dist(tiles[i].centroid, centroids[k]);
                if (d < best_d) { best_d = d; best_k = static_cast<std::uint8_t>(k); }
            }
            if (assign[i] != best_k) { assign[i] = best_k; changed = true; }
        }
        if (!changed && iter > 0) break;

        // Update step.
        std::array<OKLab, K> sums{};
        std::array<std::size_t, K> counts{};
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (tiles[i].pixel_count == 0) continue;
            auto k = assign[i];
            sums[k].L += tiles[i].centroid.L * static_cast<float>(tiles[i].pixel_count);
            sums[k].a += tiles[i].centroid.a * static_cast<float>(tiles[i].pixel_count);
            sums[k].b += tiles[i].centroid.b * static_cast<float>(tiles[i].pixel_count);
            counts[k] += tiles[i].pixel_count;
        }
        for (std::size_t k = 0; k < K; ++k) {
            if (counts[k] > 0) {
                centroids[k].L = sums[k].L / static_cast<float>(counts[k]);
                centroids[k].a = sums[k].a / static_cast<float>(counts[k]);
                centroids[k].b = sums[k].b / static_cast<float>(counts[k]);
            } else {
                // Empty cluster — re-seed from a random non-empty tile so
                // the next iter has a chance to claim some assignments.
                std::uniform_int_distribution<std::size_t> pick(0, tiles.size() - 1);
                for (int tries = 0; tries < 16; ++tries) {
                    auto idx = pick(rng);
                    if (tiles[idx].pixel_count > 0) {
                        centroids[k] = tiles[idx].centroid;
                        break;
                    }
                }
            }
        }
    }
    return assign;
}

} // namespace

namespace {

// Build palette line `k` from the union of pixels of all tiles assigned
// to it. Median-cut on the union, then BGR333-snap each entry. Backdrop
// (index 0) gets the cluster mean — purely cosmetic since opaque pixels
// never pick 0.
void build_palette_line(
    const Image& image, std::size_t tiles_x, std::size_t tiles_y,
    std::span<const std::uint8_t> tile_palette,
    std::uint8_t k, float palette_diversity,
    std::vector<Color3f>& out) {

    auto w = image.width();
    auto h = image.height();
    out.assign(kColorsPerPalette, Color3f{0.0f, 0.0f, 0.0f});

    std::vector<Color3f> pixels;
    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            if (tile_palette[ty * tiles_x + tx] != k) continue;
            auto x0 = tx * kTileSide, y0 = ty * kTileSide;
            auto x_end = std::min(x0 + kTileSide, w);
            auto y_end = std::min(y0 + kTileSide, h);
            for (std::size_t y = y0; y < y_end; ++y)
                for (std::size_t x = x0; x < x_end; ++x)
                    pixels.push_back(image[x, y]);
        }
    }
    if (pixels.empty()) return;

    Image cluster_img(pixels.size(), 1, std::move(pixels));
    auto pal = quantize::quantize(cluster_img, kColorsPerPalette - 1,
                                   quantize::Algorithm::median_cut,
                                   static_cast<int>(palette_diversity));
    if (!pal) return;
    for (std::size_t i = 0; i < pal->colors.size() &&
                            i + 1 < kColorsPerPalette; ++i) {
        out[i + 1] = console_color::bgr333_quantize(pal->colors[i]);
    }
    Color3f mean{0, 0, 0};
    for (auto& c : pal->colors) {
        mean.r += c.r; mean.g += c.g; mean.b += c.b;
    }
    if (!pal->colors.empty()) {
        float n = static_cast<float>(pal->colors.size());
        mean.r /= n; mean.g /= n; mean.b /= n;
        out[0] = console_color::bgr333_quantize(mean);
    }
}

} // namespace

GenesisResult cluster_tiles_into_palettes(
    const Image& image, float palette_diversity) {

    GenesisResult res;
    auto w = image.width();
    auto h = image.height();
    auto tiles_x = (w + kTileSide - 1) / kTileSide;
    auto tiles_y = (h + kTileSide - 1) / kTileSide;
    auto n_tiles = tiles_x * tiles_y;

    res.tile_palette.assign(n_tiles, 0);
    res.pixel_index.assign(w * h, 0);
    res.preview = Image(w, h);

    // 1. Centroid k-means seeding — fast initial assignment that's "close
    //    enough" for refinement to take over.
    std::vector<TileFingerprint> fps(n_tiles);
    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            fps[ty * tiles_x + tx] =
                fingerprint_tile(image, tx * kTileSide, ty * kTileSide);
        }
    }
    res.tile_palette = kmeans_tiles(fps, 0);

    // 2. Build initial palette lines from the centroid-clustering.
    for (std::size_t k = 0; k < kPaletteCount; ++k) {
        build_palette_line(image, tiles_x, tiles_y, res.tile_palette,
                           static_cast<std::uint8_t>(k), palette_diversity,
                           res.palette_lines[k]);
    }

    // Note: a Lloyd-style refinement step (re-assign each tile to the
    // palette that minimises its nearest-neighbour quantisation error,
    // then rebuild palettes) was tested and produced mixed results: it
    // gained 0.8-1.0 dB on photo.jpg but regressed lovers/fantasy by up
    // to -2.3 dB. The score function uses nearest-only OKLab², while
    // the actual render path is dithered nearest-pair — the metric is
    // overconfident on tiles that benefit from dither's pair-mixing.
    // Pursue dither-aware refinement (or non-greedy 4-palette search)
    // before re-enabling.

    return res;
}

std::vector<Color3f> shadow_palette_line(std::span<const Color3f> base) {
    std::vector<Color3f> out(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        // Hardware shadow: halve each 3-bit DAC value with truncation.
        // We approximate via bgr333_quantize(linear × 0.5 in sRGB), then
        // re-snap to BGR333 to land on a valid CRAM-display value.
        auto srgb = color_space::linear_to_srgb(base[i]);
        Color3f half_srgb{srgb.r * 0.5f, srgb.g * 0.5f, srgb.b * 0.5f};
        auto half_lin = color_space::srgb_to_linear(half_srgb);
        out[i] = console_color::bgr333_quantize(half_lin);
    }
    return out;
}

GenesisResult cluster_tiles_into_palettes_sh(
    const Image& image, float palette_diversity) {

    // 1. Same base clustering as the non-S/H path.
    auto res = cluster_tiles_into_palettes(image, palette_diversity);
    auto w = image.width();
    auto h = image.height();
    auto tiles_x = (w + kTileSide - 1) / kTileSide;
    auto tiles_y = (h + kTileSide - 1) / kTileSide;
    res.tile_shadow.assign(tiles_x * tiles_y, 0);

    // 2. Pre-compute the shadowed view of each palette line + Lab variants.
    std::array<std::vector<Color3f>, kPaletteCount> shadow_lines;
    std::array<std::array<color_space::OKLab, kColorsPerPalette>,
               kPaletteCount> base_lab, shadow_lab;
    for (std::size_t k = 0; k < kPaletteCount; ++k) {
        shadow_lines[k] = shadow_palette_line(res.palette_lines[k]);
        for (std::size_t i = 0; i < kColorsPerPalette; ++i) {
            base_lab[k][i]   = color_space::linear_to_oklab(res.palette_lines[k][i]);
            shadow_lab[k][i] = color_space::linear_to_oklab(shadow_lines[k][i]);
        }
    }

    // 3. Per-tile decision: score normal vs shadow for the tile's
    //    assigned palette; pick whichever has lower nearest² error.
    //    The same caveat as before applies (nearest-only, not dither-
    //    aware) but since the choice is BINARY per tile (not which of 4
    //    palettes), it's much less prone to the oscillation that bit
    //    the Lloyd-style refinement attempt.
    auto score_against = [&](std::span<const color_space::OKLab> pl_lab,
                             std::size_t tx, std::size_t ty) -> float {
        auto x0 = tx * kTileSide, y0 = ty * kTileSide;
        auto x_end = std::min(x0 + kTileSide, w);
        auto y_end = std::min(y0 + kTileSide, h);
        float sum = 0.0f;
        for (std::size_t y = y0; y < y_end; ++y) {
            for (std::size_t x = x0; x < x_end; ++x) {
                auto lab = color_space::linear_to_oklab(image[x, y]);
                float best = std::numeric_limits<float>::max();
                for (std::size_t i = 1; i < kColorsPerPalette; ++i) {
                    float dL = lab.L - pl_lab[i].L;
                    float da = lab.a - pl_lab[i].a;
                    float db = lab.b - pl_lab[i].b;
                    float d = dL * dL + da * da + db * db;
                    if (d < best) best = d;
                }
                sum += best;
            }
        }
        return sum;
    };

    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            std::uint8_t k = res.tile_palette[ty * tiles_x + tx];
            std::span<const color_space::OKLab> base{base_lab[k]};
            std::span<const color_space::OKLab> shad{shadow_lab[k]};
            float base_err = score_against(base, tx, ty);
            float shad_err = score_against(shad, tx, ty);
            res.tile_shadow[ty * tiles_x + tx] =
                (shad_err < base_err) ? 1 : 0;
        }
    }
    return res;
}

namespace {

// Apply an H or V flip (or both) to a 64-entry 8×8 nibble pattern.
std::array<std::uint8_t, 64> flip_tile(
    const std::array<std::uint8_t, 64>& src, bool h, bool v) noexcept {
    std::array<std::uint8_t, 64> out{};
    for (std::size_t y = 0; y < kTileSide; ++y) {
        std::size_t sy = v ? (kTileSide - 1 - y) : y;
        for (std::size_t x = 0; x < kTileSide; ++x) {
            std::size_t sx = h ? (kTileSide - 1 - x) : x;
            out[y * kTileSide + x] = src[sy * kTileSide + sx];
        }
    }
    return out;
}

// Encode a 64-entry nibble pattern straight to 32 VRAM bytes — used by
// dedup before the dictionary lookup.
std::array<std::uint8_t, 32> pack_pattern(
    const std::array<std::uint8_t, 64>& p) noexcept {
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t row = 0; row < kTileSide; ++row) {
        for (std::size_t pair = 0; pair < 4; ++pair) {
            std::uint8_t left  = p[row * kTileSide + pair * 2]     & 0x0F;
            std::uint8_t right = p[row * kTileSide + pair * 2 + 1] & 0x0F;
            bytes[row * 4 + pair] =
                static_cast<std::uint8_t>((left << 4) | right);
        }
    }
    return bytes;
}

} // namespace

DedupResult dedup_tiles(std::span<const std::uint8_t> pixel_index,
                        std::span<const std::uint8_t> tile_palette,
                        std::size_t width, std::size_t height) {

    DedupResult res;
    auto tiles_x = (width  + kTileSide - 1) / kTileSide;
    auto tiles_y = (height + kTileSide - 1) / kTileSide;
    res.tilemap.assign(tiles_x * tiles_y, TilemapCell{});

    // Dictionary: 32-byte canonical (no-flip) tile bytes → VRAM tile index.
    // unordered_map keys on the array directly via a small custom hash.
    struct ArrayHash {
        std::size_t operator()(const std::array<std::uint8_t, 32>& a) const noexcept {
            // FNV-1a 64-bit, plenty good for ≤2k tile populations.
            std::uint64_t h = 0xCBF29CE484222325ULL;
            for (auto b : a) {
                h ^= b;
                h *= 0x100000001B3ULL;
            }
            return static_cast<std::size_t>(h);
        }
    };
    std::unordered_map<std::array<std::uint8_t, 32>, std::uint16_t,
                       ArrayHash> dict;

    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            std::array<std::uint8_t, 64> raw{};
            auto x0 = tx * kTileSide;
            auto y0 = ty * kTileSide;
            for (std::size_t row = 0; row < kTileSide; ++row) {
                for (std::size_t col = 0; col < kTileSide; ++col) {
                    auto px = x0 + col;
                    auto py = y0 + row;
                    raw[row * kTileSide + col] =
                        (px < width && py < height)
                            ? (pixel_index[py * width + px] & 0x0F)
                            : 0;
                }
            }

            // Probe four orientations against the dictionary in this order:
            //   identity → H → V → H+V
            // First hit wins. The canonical key in the dict is always the
            // identity-orientation tile bytes — when we add a NEW tile we
            // store its identity form.
            TilemapCell cell{};
            cell.palette_line = tile_palette[ty * tiles_x + tx];
            bool found = false;
            std::array<std::pair<bool, bool>, 4> orients{{
                {false, false}, {true, false}, {false, true}, {true, true}}};
            for (auto [h, v] : orients) {
                auto flipped = (h || v) ? flip_tile(raw, h, v) : raw;
                auto bytes = pack_pattern(flipped);
                auto it = dict.find(bytes);
                if (it != dict.end()) {
                    cell.tile_index = it->second;
                    cell.h_flip = h;
                    cell.v_flip = v;
                    if (h || v) ++res.flipped_dedupes;
                    else        ++res.identical_dedupes;
                    found = true;
                    break;
                }
            }
            if (!found) {
                auto bytes = pack_pattern(raw);
                auto idx = static_cast<std::uint16_t>(res.tiles.size());
                res.tiles.push_back(bytes);
                dict.emplace(bytes, idx);
                cell.tile_index = idx;
            }
            res.tilemap[ty * tiles_x + tx] = cell;
        }
    }
    return res;
}

std::array<std::uint8_t, 32> encode_4bpp_tile(
    std::span<const std::uint8_t> pixel_indices) {

    std::array<std::uint8_t, 32> tile{};
    // Genesis VRAM tile: 8 rows × 4 bytes; each byte = 2 pixels packed
    // [hi nibble = left pixel, lo nibble = right pixel].
    for (std::size_t row = 0; row < 8; ++row) {
        for (std::size_t pair = 0; pair < 4; ++pair) {
            std::size_t left_x = pair * 2;
            std::size_t right_x = left_x + 1;
            std::uint8_t left =
                (row * 8 + left_x  < pixel_indices.size())
                    ? (pixel_indices[row * 8 + left_x]  & 0x0F) : 0;
            std::uint8_t right =
                (row * 8 + right_x < pixel_indices.size())
                    ? (pixel_indices[row * 8 + right_x] & 0x0F) : 0;
            tile[row * 4 + pair] =
                static_cast<std::uint8_t>((left << 4) | right);
        }
    }
    return tile;
}

} // namespace png2amiga::genesis
