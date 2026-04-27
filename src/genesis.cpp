#include "genesis.hpp"
#include "color_space.hpp"
#include "console_color.hpp"
#include "quantize.hpp"

#include <algorithm>
#include <limits>
#include <random>

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

    // 1. Fingerprint each tile.
    std::vector<TileFingerprint> fps(n_tiles);
    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            fps[ty * tiles_x + tx] =
                fingerprint_tile(image, tx * kTileSide, ty * kTileSide);
        }
    }

    // 2. k-means assign tiles → 4 clusters.
    res.tile_palette = kmeans_tiles(fps, 0);

    // 3. For each cluster, gather its pixels and run median-cut for 15
    //    palette colours (index 0 reserved as transparent / backdrop).
    for (std::size_t k = 0; k < kPaletteCount; ++k) {
        std::vector<Color3f> pixels;
        for (std::size_t ty = 0; ty < tiles_y; ++ty) {
            for (std::size_t tx = 0; tx < tiles_x; ++tx) {
                if (res.tile_palette[ty * tiles_x + tx] != k) continue;
                auto x0 = tx * kTileSide;
                auto y0 = ty * kTileSide;
                auto x_end = std::min(x0 + kTileSide, w);
                auto y_end = std::min(y0 + kTileSide, h);
                for (std::size_t y = y0; y < y_end; ++y)
                    for (std::size_t x = x0; x < x_end; ++x)
                        pixels.push_back(image[x, y]);
            }
        }

        // Always 16 entries, with index 0 = backdrop (we use the cluster's
        // mean colour as the backdrop so the palette visually "owns" all
        // 16 slots; transparent pixels in the source still pin to 0).
        res.palette_lines[k].assign(kColorsPerPalette,
                                     Color3f{0.0f, 0.0f, 0.0f});

        if (pixels.empty()) continue;

        // Run median-cut on a 1-row image of these pixels for 15 palette
        // entries, then BGR333-snap each.
        Image cluster_img(pixels.size(), 1, std::move(pixels));
        auto pal = quantize::quantize(
            cluster_img, kColorsPerPalette - 1,
            quantize::Algorithm::median_cut,
            static_cast<int>(palette_diversity));
        if (pal) {
            for (std::size_t i = 0; i < pal->colors.size() &&
                                    i + 1 < kColorsPerPalette; ++i) {
                res.palette_lines[k][i + 1] =
                    console_color::bgr333_quantize(pal->colors[i]);
            }
            // Backdrop = quantised cluster mean for tidiness.
            // (No effect on dithered output: opaque pixels never pick 0.)
            Color3f mean{0, 0, 0};
            std::size_t n = 0;
            for (auto& c : pal->colors) {
                mean.r += c.r; mean.g += c.g; mean.b += c.b; ++n;
            }
            if (n > 0) {
                mean.r /= static_cast<float>(n);
                mean.g /= static_cast<float>(n);
                mean.b /= static_cast<float>(n);
                res.palette_lines[k][0] = console_color::bgr333_quantize(mean);
            }
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
