#include "sms.hpp"
#include "color_space.hpp"
#include "console_color.hpp"
#include "genesis.hpp"
#include "palette.hpp"
#include "pipeline.hpp"
#include "quantize.hpp"
#include "tile_merge.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace png2amiga::sms {

namespace {

using OKLab = color_space::OKLab;
using Pattern = std::array<std::uint8_t, 64>;

constexpr std::size_t kTS = kTileSide;

// Canonical pattern pixel (kx, ky) shown in a cell with flips (h, v).
constexpr std::size_t flipped_pos(std::size_t k, bool h, bool v) noexcept {
    std::size_t kx = k % kTS, ky = k / kTS;
    return (v ? kTS - 1 - ky : ky) * kTS + (h ? kTS - 1 - kx : kx);
}

// 16-color palette for one cluster of pixels, snapped to the hardware
// gamut. RGB222 is exactly the EGA rrggbb gamut, so the EGA histogram
// picker applies verbatim; RGB444 is the OCS gamut.
std::vector<Color3f> build_palette(const std::vector<Color3f>& pixels, bool gg) {
    std::vector<Color3f> out;
    if (!pixels.empty()) {
        Image img(pixels.size(), 1, std::vector<Color3f>(pixels));
        if (gg) {
            auto pal = quantize::quantize(
                img, kColorsPerPalette, quantize::Algorithm::ocs_bruteforce);
            if (pal) out = pal->colors;
            for (auto& c : out)
                c = palette::quantize_to_ocs(c);
        } else {
            out = quantize::ega_histogram(img, kColorsPerPalette).colors;
        }
    }
    if (out.size() > kColorsPerPalette) out.resize(kColorsPerPalette);
    while (out.size() < kColorsPerPalette)
        out.push_back(out.empty() ? Color3f{0, 0, 0} : out.front());
    return out;
}

std::uint8_t sms_cram_byte(Color3f c) noexcept {
    auto e = palette::linear_to_ega(c);  // rrggbb
    auto r = (e >> 4) & 3, g = (e >> 2) & 3, b = e & 3;
    return static_cast<std::uint8_t>(r | (g << 2) | (b << 4));
}

std::uint16_t gg_cram_word(Color3f c) noexcept {
    auto rgb = palette::linear_to_ocs(c);  // 0x0RGB
    auto r = (rgb >> 8) & 0xF, g = (rgb >> 4) & 0xF, b = rgb & 0xF;
    return static_cast<std::uint16_t>(r | (g << 4) | (b << 8));
}

std::array<std::uint8_t, kTileBytes> pack_tile(const Pattern& p) noexcept {
    std::array<std::uint8_t, kTileBytes> out{};
    for (std::size_t row = 0; row < kTS; ++row)
        for (std::size_t plane = 0; plane < 4; ++plane) {
            std::uint8_t byte = 0;
            for (std::size_t x = 0; x < kTS; ++x)
                byte = static_cast<std::uint8_t>(byte |
                                                 (((p[row * kTS + x] >> plane) & 1) << (7 - x)));
            out[row * 4 + plane] = byte;
        }
    return out;
}

}  // namespace

std::vector<Color3f> decode_palette(amiga::Mode mode, std::span<const std::uint8_t> cram) {
    std::vector<Color3f> out(kPalettes * kColorsPerPalette, Color3f{0, 0, 0});
    const bool gg = amiga::is_game_gear(mode);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (gg) {
            if (i * 2 + 1 >= cram.size()) break;
            unsigned w = cram[i * 2] | (static_cast<unsigned>(cram[i * 2 + 1]) << 8);
            unsigned r = w & 0xF, g = (w >> 4) & 0xF, b = (w >> 8) & 0xF;
            out[i] = palette::ocs_to_linear(static_cast<std::uint16_t>((r << 8) | (g << 4) | b));
        } else {
            if (i >= cram.size()) break;
            unsigned v = cram[i];
            unsigned r = v & 3, g = (v >> 2) & 3, b = (v >> 4) & 3;
            out[i] = palette::ega_to_linear(static_cast<std::uint8_t>((r << 4) | (g << 2) | b));
        }
    }
    return out;
}

Image decode(amiga::Mode mode,
             std::span<const std::uint8_t> tiles,
             std::span<const std::uint16_t> tilemap,
             std::span<const std::uint8_t> cram,
             std::size_t cols,
             std::size_t rows) {
    auto pal = decode_palette(mode, cram);
    Image out(cols * kTS, rows * kTS);
    for (std::size_t cy = 0; cy < rows; ++cy) {
        for (std::size_t cx = 0; cx < cols; ++cx) {
            auto e = tilemap[cy * cols + cx];
            std::size_t tile = e & 0x1FF;
            bool h = (e >> 9) & 1, v = (e >> 10) & 1;
            std::size_t pbase = ((e >> 11) & 1) * kColorsPerPalette;
            for (std::size_t y = 0; y < kTS; ++y) {
                std::size_t row = v ? kTS - 1 - y : y;
                for (std::size_t x = 0; x < kTS; ++x) {
                    std::size_t col = h ? kTS - 1 - x : x;
                    std::size_t base = tile * kTileBytes + row * 4;
                    std::size_t idx = 0;
                    if (base + 3 < tiles.size())
                        for (std::size_t plane = 0; plane < 4; ++plane)
                            idx |= static_cast<std::size_t>((tiles[base + plane] >> (7 - col)) & 1)
                                   << plane;
                    out[cx * kTS + x, cy * kTS + y] = pal[pbase + idx];
                }
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> raw_bytes(const EncodeResult& r) {
    std::vector<std::uint8_t> out(r.tiles);
    for (auto e : r.tilemap) {
        out.push_back(static_cast<std::uint8_t>(e & 0xFF));
        out.push_back(static_cast<std::uint8_t>(e >> 8));
    }
    out.insert(out.end(), r.palette.begin(), r.palette.end());
    return out;
}

Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings,
                            const ProgressCb& on_progress) {
    if (!amiga::is_sms(mode))
        return std::unexpected{Error{ErrorCode::unsupported_mode, "sms::encode: not an SMS mode"}};
    const auto params = amiga::get_mode_params(mode);
    const std::size_t W = params.screen_width, H = params.screen_height;
    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("sms: expected {}x{}, got {}x{}", W, H, image.width(), image.height())}};
    }
    const bool gg = amiga::is_game_gear(mode);
    auto report = [&](float f, std::string_view s) {
        if (on_progress) on_progress(f, s);
    };
    const std::size_t cols = W / kTS, rows = H / kTS, ncells = cols * rows;

    // 1. Cells → 2 palettes (Genesis tile k-means), 16 colors each.
    report(0.0f, "palettes");
    auto cell_pal = genesis::cluster_tiles(image, kPalettes);
    std::array<std::vector<Color3f>, kPalettes> pals;
    for (std::size_t k = 0; k < kPalettes; ++k) {
        std::vector<Color3f> px;
        for (std::size_t c = 0; c < ncells; ++c) {
            if (cell_pal[c] != k) continue;
            for (std::size_t y = 0; y < kTS; ++y)
                for (std::size_t x = 0; x < kTS; ++x)
                    px.push_back(image[(c % cols) * kTS + x, (c / cols) * kTS + y]);
        }
        pals[k] = build_palette(px, gg);
    }
    std::array<std::vector<OKLab>, kPalettes> pal_lab;
    for (std::size_t k = 0; k < kPalettes; ++k)
        for (auto& c : pals[k])
            pal_lab[k].push_back(color_space::linear_to_oklab(c));

    // 2. Per-cell mirrored ED against each cell's palette.
    report(0.2f, "dithering");
    std::vector<std::uint8_t> idx;
    dither::diffuse_cells_mirrored(
        image,
        settings,
        kTS,
        kTS,
        /*k_min=*/0,
        [&](std::size_t cell) -> std::span<const OKLab> { return pal_lab[cell_pal[cell]]; },
        idx);

    // 3. Exact dedup incl. H/V flips (palette lives in the tilemap).
    report(0.4f, "dedup");
    auto dd = genesis::dedup_tiles(idx, cell_pal, W, H);
    std::vector<Pattern> patterns(dd.tiles.size());
    for (std::size_t t = 0; t < dd.tiles.size(); ++t)
        for (std::size_t i = 0; i < 32; ++i) {
            patterns[t][i * 2] = dd.tiles[t][i] >> 4;
            patterns[t][i * 2 + 1] = dd.tiles[t][i] & 0x0F;
        }
    std::vector<genesis::TilemapCell> map = dd.tilemap;

    EncodeResult res;
    res.unique_after_dedup = patterns.size();

    // 4. Merge down to the VRAM budget.
    if (patterns.size() > kMaxTiles) {
        const std::size_t n = patterns.size();
        std::vector<std::vector<std::size_t>> slot_cells(n);
        std::vector<std::uint8_t> slot_mask(n, 0);
        for (std::size_t c = 0; c < ncells; ++c) {
            slot_cells[map[c].tile_index].push_back(c);
            slot_mask[map[c].tile_index] |= static_cast<std::uint8_t>(1u << map[c].palette_line);
        }
        // Per-palette index-pair distance tables.
        std::array<std::array<std::array<float, 16>, 16>, kPalettes> dist{};
        for (std::size_t k = 0; k < kPalettes; ++k)
            for (std::size_t i = 0; i < 16; ++i)
                for (std::size_t j = 0; j < 16; ++j) {
                    auto& a = pal_lab[k][i];
                    auto& b = pal_lab[k][j];
                    dist[k][i][j] = color_space::fma_dist_sq(a.L - b.L, a.a - b.a, a.b - b.b);
                }
        // Rendering-aware score: both slots are shown in every palette
        // either of them is used with. Per-pixel term (weight 1/4) plus a
        // 2×2-block mean term (weight 4) so dither-phase differences cost
        // less than a real change of local color. Best of the 4 flips.
        auto score = [&](std::size_t a, std::size_t b) -> tile_merge::PairScore {
            std::uint8_t mask = slot_mask[a] | slot_mask[b];
            // Cross-palette pairs go last: the refinement below keeps every
            // slot on a single palette.
            const float cross = (slot_mask[a] != slot_mask[b]) ? 1.0e6f : 0.0f;
            float best = std::numeric_limits<float>::max();
            std::uint8_t best_o = 0;
            for (std::uint8_t o = 0; o < 4; ++o) {
                bool fh = o & 1, fv = o & 2;
                float e = 0.0f;
                for (std::size_t k = 0; k < kPalettes; ++k) {
                    if (!(mask & (1u << k))) continue;
                    float px = 0.0f;
                    std::array<OKLab, 16> blk{};
                    for (std::size_t q = 0; q < 64; ++q) {
                        auto ia = patterns[a][flipped_pos(q, fh, fv)];
                        auto ib = patterns[b][q];
                        px += dist[k][ia][ib];
                        auto& la = pal_lab[k][ia];
                        auto& lb = pal_lab[k][ib];
                        auto& acc = blk[(q / kTS / 2) * 4 + (q % kTS) / 2];
                        acc.L += la.L - lb.L;
                        acc.a += la.a - lb.a;
                        acc.b += la.b - lb.b;
                    }
                    float lf = 0.0f;
                    for (auto& d : blk)
                        lf += color_space::fma_dist_sq(d.L * 0.25f, d.a * 0.25f, d.b * 0.25f);
                    e += 0.25f * px + 4.0f * lf;
                }
                if (e < best) {
                    best = e;
                    best_o = o;
                }
            }
            return {best + cross, best_o};
        };
        std::vector<bool> alive(n, true);
        tile_merge::merge_to_budget(
            slot_cells,
            alive,
            kMaxTiles,
            score,
            [&](std::size_t keep, std::size_t discard, std::uint8_t o) {
                for (auto c : slot_cells[discard]) {
                    map[c].tile_index = static_cast<std::uint16_t>(keep);
                    map[c].h_flip = map[c].h_flip != static_cast<bool>(o & 1);
                    map[c].v_flip = map[c].v_flip != static_cast<bool>(o & 2);
                }
            },
            [&](float f, std::string_view s) { report(0.4f + 0.4f * f, s); });

        // Lloyd refinement (same shape as the SNES Mode 7 packer): the
        // greedy pass keeps the survivor's own pattern, which stops being
        // representative once a slot absorbs many cells. Alternate
        //   centroid — dither the cells' mean source colors (canonical
        //              orientation) with the slot's majority palette, and
        //   assign   — move every cell to the slot + flip whose display
        //              best matches its source (same pixel + 2×2 metric),
        // reseeding emptied slots from the worst-fitting cells.
        report(0.82f, "refining tiles");
        std::vector<std::array<OKLab, 64>> src_lab(ncells);
        for (std::size_t c = 0; c < ncells; ++c)
            for (std::size_t q = 0; q < 64; ++q)
                src_lab[c][q] = color_space::linear_to_oklab(
                    image[(c % cols) * kTS + q % kTS, (c / cols) * kTS + q / kTS]);
        std::vector<std::size_t> live;
        for (std::size_t q = 0; q < n; ++q)
            if (alive[q]) live.push_back(q);
        auto cell_cost = [&](std::size_t c,
                             const std::array<OKLab, 64>& rend,
                             bool fh,
                             bool fv,
                             float cutoff) -> float {
            float px = 0.0f;
            std::array<OKLab, 16> blk{};
            for (std::size_t q = 0; q < 64; ++q) {
                const auto& r = rend[flipped_pos(q, fh, fv)];
                const auto& t = src_lab[c][q];
                float dL = r.L - t.L, da = r.a - t.a, db = r.b - t.b;
                px += color_space::fma_dist_sq(dL, da, db);
                auto& acc = blk[(q / kTS / 2) * 4 + (q % kTS) / 2];
                acc.L += dL;
                acc.a += da;
                acc.b += db;
            }
            float e = 0.25f * px;
            if (e >= cutoff) return e;
            for (auto& d : blk)
                e += 4.0f * color_space::fma_dist_sq(d.L * 0.25f, d.a * 0.25f, d.b * 0.25f);
            return e;
        };
        // Each slot serves one palette (majority of its cells).
        std::vector<std::uint8_t> slot_pal(n, 0);
        for (auto s : live) {
            std::array<std::size_t, kPalettes> votes{};
            for (auto c : slot_cells[s])
                ++votes[map[c].palette_line];
            slot_pal[s] = votes[1] > votes[0] ? 1 : 0;
        }
        constexpr int kLloydIters = 6;
        for (int it = 0; it < kLloydIters; ++it) {
            // Centroid step.
            pipeline::parallel_for(live.size(), [&](std::size_t li) {
                auto s = live[li];
                const auto& cs = slot_cells[s];
                const std::size_t pmaj = slot_pal[s];
                bool mixed = false;
                for (auto c : cs)
                    mixed |= map[c].palette_line != pmaj;
                if (cs.size() < 2 && !mixed) return;
                Image mean(kTS, kTS);
                for (std::size_t q = 0; q < 64; ++q) {
                    OKLab acc{0, 0, 0};
                    for (auto c : cs) {
                        const auto& t = src_lab[c][flipped_pos(q, map[c].h_flip, map[c].v_flip)];
                        acc.L += t.L;
                        acc.a += t.a;
                        acc.b += t.b;
                    }
                    float inv = 1.0f / static_cast<float>(cs.size());
                    mean[q % kTS, q / kTS] = color_space::oklab_to_linear(
                        OKLab{acc.L * inv, acc.a * inv, acc.b * inv});
                }
                std::vector<std::uint8_t> tidx;
                dither::diffuse_cells_mirrored(
                    mean,
                    settings,
                    kTS,
                    kTS,
                    0,
                    [&](std::size_t) -> std::span<const OKLab> { return pal_lab[pmaj]; },
                    tidx);
                std::copy(tidx.begin(), tidx.end(), patterns[s].begin());
            });
            if (it + 1 == kLloydIters) break;
            // Assignment step.
            std::array<std::vector<std::array<OKLab, 64>>, kPalettes> rend;
            for (std::size_t k = 0; k < kPalettes; ++k) {
                rend[k].resize(live.size());
                for (std::size_t li = 0; li < live.size(); ++li)
                    for (std::size_t q = 0; q < 64; ++q)
                        rend[k][li][q] = pal_lab[k][patterns[live[li]][q]];
            }
            std::vector<std::size_t> best_slot(ncells);
            std::vector<std::uint8_t> best_o(ncells);
            std::vector<float> best_e(ncells);
            pipeline::parallel_for(ncells, [&](std::size_t c) {
                auto k = map[c].palette_line;
                float be = std::numeric_limits<float>::max();
                std::size_t bs = 0;
                std::uint8_t bo = 0;
                for (std::size_t li = 0; li < live.size(); ++li) {
                    if (slot_pal[live[li]] != k) continue;
                    for (std::uint8_t o = 0; o < 4; ++o) {
                        float e = cell_cost(c, rend[k][li], o & 1, o & 2, be);
                        if (e < be) {
                            be = e;
                            bs = li;
                            bo = o;
                        }
                    }
                }
                best_slot[c] = bs;
                best_o[c] = bo;
                best_e[c] = be;
            });
            bool changed = false;
            for (auto s : live)
                slot_cells[s].clear();
            for (std::size_t c = 0; c < ncells; ++c) {
                auto s = live[best_slot[c]];
                bool h = best_o[c] & 1, v = best_o[c] & 2;
                if (map[c].tile_index != s || map[c].h_flip != h || map[c].v_flip != v)
                    changed = true;
                map[c].tile_index = static_cast<std::uint16_t>(s);
                map[c].h_flip = h;
                map[c].v_flip = v;
                slot_cells[s].push_back(c);
            }
            // Reseed empty slots with the worst-fitting cells (taken from
            // slots that keep at least one cell).
            std::vector<std::size_t> order(ncells);
            for (std::size_t c = 0; c < ncells; ++c)
                order[c] = c;
            std::ranges::sort(order,
                              [&](std::size_t x, std::size_t y) { return best_e[x] > best_e[y]; });
            std::size_t next = 0;
            for (auto s : live) {
                if (!slot_cells[s].empty()) continue;
                while (next < order.size() && slot_cells[map[order[next]].tile_index].size() < 2)
                    ++next;
                if (next >= order.size()) break;
                auto c = order[next++];
                auto& from = slot_cells[map[c].tile_index];
                from.erase(std::ranges::find(from, c));
                map[c] = {static_cast<std::uint16_t>(s), map[c].palette_line, false, false};
                slot_pal[s] = map[c].palette_line;
                slot_cells[s].push_back(c);
                // Exact pre-merge pattern for the reseeded cell.
                for (std::size_t q = 0; q < 64; ++q)
                    patterns[s][q] =
                        idx[((c / cols) * kTS + q / kTS) * W + (c % cols) * kTS + q % kTS];
                changed = true;
            }
            if (!changed) break;
        }

        // Compact.
        std::vector<std::size_t> remap(n, 0);
        std::vector<Pattern> kept;
        for (std::size_t s = 0; s < n; ++s) {
            if (!alive[s]) continue;
            remap[s] = kept.size();
            kept.push_back(patterns[s]);
        }
        for (auto& cell : map)
            cell.tile_index = static_cast<std::uint16_t>(remap[cell.tile_index]);
        patterns = std::move(kept);
    }

    // 5. Pack.
    report(0.95f, "packing");
    res.cols = cols;
    res.rows = rows;
    res.unique_tiles = patterns.size();
    for (auto& p : patterns) {
        auto b = pack_tile(p);
        res.tiles.insert(res.tiles.end(), b.begin(), b.end());
    }
    for (auto& cell : map) {
        res.tilemap.push_back(static_cast<std::uint16_t>(
            (cell.tile_index & 0x1FF) | (cell.h_flip ? 0x200 : 0) | (cell.v_flip ? 0x400 : 0) |
            (cell.palette_line ? 0x800 : 0)));
    }
    for (std::size_t k = 0; k < kPalettes; ++k)
        for (auto& c : pals[k]) {
            if (gg) {
                auto w16 = gg_cram_word(c);
                res.palette.push_back(static_cast<std::uint8_t>(w16 & 0xFF));
                res.palette.push_back(static_cast<std::uint8_t>(w16 >> 8));
            } else {
                res.palette.push_back(sms_cram_byte(c));
            }
        }
    res.palette_rgb = decode_palette(mode, res.palette);
    res.rendered = decode(mode, res.tiles, res.tilemap, res.palette, cols, rows);
    float err = 0.0f;
    for (std::size_t i = 0; i < W * H; ++i)
        err += color_space::perceptual_distance_sq(image.pixels()[i], res.rendered.pixels()[i]);
    res.total_error = err;
    report(1.0f, "done");
    return res;
}

}  // namespace png2amiga::sms
