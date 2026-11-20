#include "snes_io.hpp"
#include "color_space.hpp"
#include "pipeline.hpp"
#include "console_color.hpp"
#include "dither.hpp"
#include "oklab_simd.hpp"
#include "quantize.hpp"
#include "tile_merge.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace png2amiga::snes_io {

// ---------------------------------------------------------------------------
// Mode 7 packer.
//
// Splits the 256×224 input into 32×28 = 896 cells of 64 bytes each;
// dedupes identical patterns; if the unique count exceeds 256, runs the
// merge_to_n algorithm (ported from png2c64/src/charset.cpp:merge_to_256)
// — pairwise distance, sort ascending, greedily collapse closest pairs
// until the budget is met. The "keep larger cluster" rule preserves the
// most-used pattern of each pair so popular tiles aren't displaced by
// rare ones.
// ---------------------------------------------------------------------------

namespace {

using Pattern64 = std::array<std::uint8_t, 64>;

struct PatternHash {
    std::size_t operator()(const Pattern64& a) const noexcept {
        std::uint64_t h = 0xCBF29CE484222325ULL;
        for (auto b : a) {
            h ^= b;
            h *= 0x100000001B3ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

struct Slot {
    Pattern64 pattern{};
    std::vector<std::size_t> cell_indices;  // tilemap cells using this slot
    bool alive = true;
};

}  // namespace

Result<Mode7PackResult> pack_snes_mode7_frame(
    std::span<const std::uint8_t> pixels,
    std::size_t width,
    std::size_t height,
    std::function<float(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b)> distance,
    CachedDistance cached_distance,
    ProgressCb on_progress,
    float progress_lo,
    float progress_hi) {

    auto report = [&](float local, std::string_view stage) {
        if (on_progress)
            on_progress(progress_lo + (progress_hi - progress_lo) * std::clamp(local, 0.0f, 1.0f),
                        stage);
    };

    if (pixels.size() != width * height) {
        return std::unexpected{
            Error{ErrorCode::invalid_png, "pack_snes_mode7_frame: pixels size != width × height"}};
    }
    constexpr std::size_t kTile = 8;
    constexpr std::size_t kMaxTiles = 256;
    constexpr std::size_t kVirtualSide = 128;  // 128×128 tilemap canvas
    auto tiles_x = (width + kTile - 1) / kTile;
    auto tiles_y = (height + kTile - 1) / kTile;
    auto num_cells = tiles_x * tiles_y;

    // 1. Build per-cell 64-byte pattern.
    std::vector<Pattern64> cell_patterns(num_cells);
    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            auto& p = cell_patterns[ty * tiles_x + tx];
            for (std::size_t row = 0; row < kTile; ++row) {
                for (std::size_t col = 0; col < kTile; ++col) {
                    auto px = tx * kTile + col;
                    auto py = ty * kTile + row;
                    p[row * kTile + col] = (px < width && py < height) ? pixels[py * width + px]
                                                                       : 0;
                }
            }
        }
    }

    // 2. Dedup identical patterns.
    std::unordered_map<Pattern64, std::size_t, PatternHash> dict;
    std::vector<Slot> slots;
    std::vector<std::size_t> cell_to_slot(num_cells);
    for (std::size_t i = 0; i < num_cells; ++i) {
        auto& p = cell_patterns[i];
        auto it = dict.find(p);
        if (it == dict.end()) {
            auto idx = slots.size();
            dict.emplace(p, idx);
            slots.push_back({p, {i}, true});
            cell_to_slot[i] = idx;
        } else {
            slots[it->second].cell_indices.push_back(i);
            cell_to_slot[i] = it->second;
        }
    }

    Mode7PackResult res;
    res.unique_after_dedup = slots.size();
    res.merges_done = 0;

    // 3. Merge if over budget (tile_merge::merge_to_budget — closest
    //    pairs first, larger slot survives).
    if (slots.size() > kMaxTiles) {
        // If the caller supplied cached_distance.prepare, build a SlotPre
        // (e.g. OKLab + blurred-OKLab) once per slot and score pairs from
        // the cache — collapses N(N-1) per-pair lab+blur computes to N.
        const bool use_cache = static_cast<bool>(cached_distance.prepare);
        std::vector<SlotPre> slot_pre;
        if (use_cache) {
            slot_pre.resize(slots.size());
            pipeline::parallel_for(slots.size(), [&](std::size_t i) {
                slot_pre[i] = cached_distance.prepare(slots[i].pattern);
            });
        }
        std::vector<std::vector<std::size_t>> slot_cells(slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i)
            slot_cells[i] = std::move(slots[i].cell_indices);
        std::vector<bool> alive(slots.size(), true);
        res.merges_done = tile_merge::merge_to_budget(
            slot_cells,
            alive,
            kMaxTiles,
            [&](std::size_t a, std::size_t b) -> tile_merge::PairScore {
                if (use_cache) return {cached_distance.score(slot_pre[a], slot_pre[b]), 0};
                return {distance(slots[a].pattern, slots[b].pattern), 0};
            },
            {},
            report);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            slots[i].alive = alive[i];
            slots[i].cell_indices = std::move(slot_cells[i]);
            for (auto ci : slots[i].cell_indices)
                cell_to_slot[ci] = i;
        }
    }

    // 4. Compact the alive slots into the output tile array; remap
    //    cell_to_slot through the compaction.
    std::vector<std::size_t> slot_to_tile(slots.size(), static_cast<std::size_t>(-1));
    res.tile_bytes.reserve(kMaxTiles * 64);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].alive) continue;
        slot_to_tile[i] = res.tile_bytes.size() / 64;
        res.tile_bytes.insert(
            res.tile_bytes.end(), slots[i].pattern.begin(), slots[i].pattern.end());
    }
    res.unique_after_merge = res.tile_bytes.size() / 64;

    // 5. Build the 128×128 tilemap. Live cells reference real tiles;
    //    the rest point to tile 0 so the runtime can fill with backdrop.
    res.tilemap.assign(kVirtualSide * kVirtualSide, 0);
    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            auto cell_idx = ty * tiles_x + tx;
            auto tile_idx = slot_to_tile[cell_to_slot[cell_idx]];
            res.tilemap[ty * kVirtualSide + tx] = static_cast<std::uint8_t>(tile_idx);
        }
    }
    return res;
}

// ---------------------------------------------------------------------------
// One-shot Mode 7 encoder. The chunky pixel buffer + the pre-merge
// `rendered` view never escape this function — callers see only the
// post-pack bytes and the post-merge preview. This used to live in
// api.cpp but the chunky intermediate kept leaking into PipelineResult
// (preview, raw_frame, PSNR) before the pack step finished, fooling
// callers into thinking the output was higher-fidelity than the
// hardware would actually display.
// ---------------------------------------------------------------------------

Result<Mode7EncodedFrame> encode_snes_mode7(const Image& source,
                                            bool direct_color,
                                            const dither::Settings& dither_settings,
                                            int palette_diversity,
                                            ProgressCb on_progress) {

    // Stage budgets (sum to 1.0). The Lloyd phase dominates total
    // wall-clock for content with > 256 unique tiles, so it gets the
    // lion's share. Numbers tuned empirically on photo+logo content.
    constexpr float kStageQuant = 0.05f;   // 0% → 5%
    constexpr float kStagePack = 0.20f;    // 5% → 25%
    constexpr float kStageLloyd = 0.70f;   // 25% → 95%
    constexpr float kStageRender = 0.05f;  // 95% → 100%
    auto report = [&](float frac, std::string_view stage) {
        if (on_progress) on_progress(std::clamp(frac, 0.0f, 1.0f), stage);
    };
    report(0.0f, "starting Mode 7 encode");

    auto w = source.width();
    auto h = source.height();
    Mode7EncodedFrame out{};
    out.rendered = Image(w, h);

    // ---- Step 1: chunky byte buffer (palette indices or BBGGGRRR).
    //   This buffer NEVER leaves this function. Quant error here is the
    //   pre-pack quantisation cost; the final PSNR is recomputed by the
    //   caller against the post-merge `rendered`.
    //
    //   ED runs per-8×8-tile on a 24×24 mirror-padded buffer (center =
    //   source tile, 8 surrounding blocks = h/v/hv-flips of the center).
    //   The 24×24 ED's center block is the steady-state response to the
    //   tile's own content, so identical source tiles produce identical
    //   chunky bytes → tile-dedup collapses them cleanly to one 256-tile
    //   slot. Same fix as c64 charset (commit 7236237) and Genesis
    //   (commit a2a8422).
    constexpr std::size_t kTS = 8;
    constexpr std::size_t k3T = 3 * kTS;  // 24
    auto tiles_x = (w + kTS - 1) / kTS;
    auto tiles_y = (h + kTS - 1) / kTS;
    std::vector<std::uint8_t> chunky(w * h, 0);
    Image block(k3T, k3T);
    std::vector<std::uint8_t> block_byte(k3T * k3T, 0);

    auto fill_mirror_block = [&]() {
        for (std::size_t by = 0; by < 3; ++by) {
            for (std::size_t bx = 0; bx < 3; ++bx) {
                for (std::size_t ly = 0; ly < kTS; ++ly) {
                    std::size_t sy_l = (by == 1) ? ly : (kTS - 1 - ly);
                    for (std::size_t lx = 0; lx < kTS; ++lx) {
                        std::size_t sx_l = (bx == 1) ? lx : (kTS - 1 - lx);
                        // Filled per-cell by the loops below — see usage.
                        (void)sy_l;
                        (void)sx_l;
                    }
                }
            }
        }
    };
    (void)fill_mirror_block;  // inlined per-cell below for clarity

    if (!direct_color) {
        // SNES Mode 7 256-color: continuous-RGB quantization, then
        // snap each centroid to BGR555 (5 bits/channel, 32K-color
        // gamut). gpu-restart was tested and did NOT beat median-
        // cut here (mean ΔS2 -1.17 over 20 DIV2K, worst -19 on
        // image 0481) — likely a combination of BGR555 snap and
        // the downstream Lloyd retraining at line ~660 absorbing
        // any base-palette quality differences.
        auto quantized = quantize::quantize(
            source, 256, quantize::Algorithm::median_cut, palette_diversity);
        if (!quantized) return std::unexpected{quantized.error()};
        for (auto& c : quantized->colors)
            c = console_color::bgr555_quantize(c);
        out.palette = quantized->colors;

        // Pre-OKLab the 256 palette for the per-tile picker.
        std::vector<color_space::OKLab> pal_lab_pre(out.palette.size());
        for (std::size_t i = 0; i < out.palette.size(); ++i)
            pal_lab_pre[i] = color_space::linear_to_oklab(out.palette[i]);

        out.quant_error = 0.0f;
        for (std::size_t ty = 0; ty < tiles_y; ++ty) {
            for (std::size_t tx = 0; tx < tiles_x; ++tx) {
                for (std::size_t by = 0; by < 3; ++by) {
                    for (std::size_t bx = 0; bx < 3; ++bx) {
                        for (std::size_t ly = 0; ly < kTS; ++ly) {
                            std::size_t sy_l = (by == 1) ? ly : (kTS - 1 - ly);
                            for (std::size_t lx = 0; lx < kTS; ++lx) {
                                std::size_t sx_l = (bx == 1) ? lx : (kTS - 1 - lx);
                                std::size_t sx = std::min(tx * kTS + sx_l, w - 1);
                                std::size_t sy = std::min(ty * kTS + sy_l, h - 1);
                                block[bx * kTS + lx, by * kTS + ly] = source[sx, sy];
                            }
                        }
                    }
                }
                std::fill(block_byte.begin(), block_byte.end(), std::uint8_t{0});
                out.quant_error += dither::diffuse_raw_buffer(
                    block,
                    dither_settings,
                    [&](const color_space::OKLab& target,
                        std::size_t bxp,
                        std::size_t byp) -> dither::PickResult {
                        float bd = std::numeric_limits<float>::max();
                        std::size_t bi = 0;
                        for (std::size_t i = 0; i < pal_lab_pre.size(); ++i) {
                            const auto& cl = pal_lab_pre[i];
                            float dL = target.L - cl.L;
                            float da = target.a - cl.a;
                            float db = target.b - cl.b;
                            float d = color_space::fma_dist_sq(dL, da, db);
                            if (d < bd) {
                                bd = d;
                                bi = i;
                            }
                        }
                        block_byte[byp * k3T + bxp] = static_cast<std::uint8_t>(bi);
                        return {pal_lab_pre[bi], 0.5f};
                    });
                for (std::size_t ly = 0; ly < kTS; ++ly) {
                    for (std::size_t lx = 0; lx < kTS; ++lx) {
                        auto x = tx * kTS + lx;
                        auto y = ty * kTS + ly;
                        if (x >= w || y >= h) continue;
                        chunky[y * w + x] = block_byte[(kTS + ly) * k3T + (kTS + lx)];
                    }
                }
            }
        }
        report(kStageQuant, "quantising");
    } else {
        // Mode 7 Direct: pixel byte = BBGGGRRR (3+3+2 = 256 colors).
        // The tilemap carries no palette field per cell, so the
        // 2048-color gamut documented for Modes 3/4 isn't reachable.
        // Quantise the dither target to the SAME 3-3-2 grid the byte
        // can actually store — using rgb443_quantize (4-4-3) here was
        // the source of the color artefacts: the picker thought it
        // had finer precision than the byte could hold, so the
        // diffusion residual fought the truncation.
        out.quant_error = 0.0f;
        for (std::size_t ty = 0; ty < tiles_y; ++ty) {
            for (std::size_t tx = 0; tx < tiles_x; ++tx) {
                for (std::size_t by = 0; by < 3; ++by) {
                    for (std::size_t bx = 0; bx < 3; ++bx) {
                        for (std::size_t ly = 0; ly < kTS; ++ly) {
                            std::size_t sy_l = (by == 1) ? ly : (kTS - 1 - ly);
                            for (std::size_t lx = 0; lx < kTS; ++lx) {
                                std::size_t sx_l = (bx == 1) ? lx : (kTS - 1 - lx);
                                std::size_t sx = std::min(tx * kTS + sx_l, w - 1);
                                std::size_t sy = std::min(ty * kTS + sy_l, h - 1);
                                block[bx * kTS + lx, by * kTS + ly] = source[sx, sy];
                            }
                        }
                    }
                }
                std::fill(block_byte.begin(), block_byte.end(), std::uint8_t{0});
                out.quant_error += dither::diffuse_raw_buffer(
                    block,
                    dither_settings,
                    [&](const color_space::OKLab& target,
                        std::size_t bxp,
                        std::size_t byp) -> dither::PickResult {
                        auto byte = console_color::direct_color_byte(
                            color_space::oklab_to_linear(target));
                        block_byte[byp * k3T + bxp] = byte;
                        return {
                            color_space::linear_to_oklab(console_color::direct_color_linear(byte)),
                            0.5f};
                    });
                for (std::size_t ly = 0; ly < kTS; ++ly) {
                    for (std::size_t lx = 0; lx < kTS; ++lx) {
                        auto x = tx * kTS + lx;
                        auto y = ty * kTS + ly;
                        if (x >= w || y >= h) continue;
                        chunky[y * w + x] = block_byte[(kTS + ly) * k3T + (kTS + lx)];
                    }
                }
            }
        }
        out.palette.clear();
        report(kStageQuant, "quantising");
    }

    // ---- Step 2: build palette-aware distance and pack into ≤ 256
    //   unique tiles via the merger.
    std::vector<color_space::OKLab> pal_lab;
    if (!direct_color) {
        pal_lab.resize(out.palette.size());
        for (std::size_t i = 0; i < out.palette.size(); ++i)
            pal_lab[i] = color_space::linear_to_oklab(out.palette[i]);
    }
    // Direct-color byte (BBGGGRRR) → OKLab cache, decoded the way the
    // hardware shows it (console_color::direct_color_linear), the same
    // decode the picker above scores against. Only 256 possible
    // byte values, but unpack_direct gets called per-pixel-per-tile-
    // pair inside distance_fn (called O(N²) by pack_snes_mode7_frame's
    // greedy merger). AMDuProf showed linear_to_oklab at 33 % of CPU
    // for snes-mode7-direct, dominated by these per-call cbrt+pow
    // calls — the LUT collapses that to a 256-entry array load.
    // sRGB level of a direct-colour field value k of `bits` bits: the top
    // bits of a 5-bit channel (console_color::direct_color_linear).
    auto direct_level = [](int k, int bits) {
        return console_color::direct_color_level(k << (5 - bits));
    };
    std::array<color_space::OKLab, 256> direct_lab_lut{};
    for (int b = 0; b < 256; ++b)
        direct_lab_lut[static_cast<std::size_t>(b)] = color_space::linear_to_oklab(
            console_color::direct_color_linear(static_cast<std::uint8_t>(b)));
    auto unpack_direct = [&](std::uint8_t b) -> color_space::OKLab {
        return direct_lab_lut[b];
    };
    // Distance metric: per-pixel OKLab² is correct in the limit but
    // perceptually weak for textured 8×8 patches — two tiles whose
    // pixels differ at high spatial frequency look similar to humans
    // (eye averages over the patch) while scoring far apart pointwise.
    //
    // Mix in a coarse-scale term computed from a 3×3-blurred OKLab
    // representation of each tile. The combined score is
    //   d = (1 − α) · per_pixel_L² + α · coarse_L²
    // with α tuned to ~0.4 — heavy enough to pull the merger toward
    // structural similarity (logos, gradients, skin) without losing
    // edge fidelity. This captures most of what a learned LPIPS-style
    // metric would deliver, at zero binary cost and minimal CPU
    // overhead (the blur is a 3-tap separable kernel applied once).
    auto blur_3x3 = [](std::span<const color_space::OKLab> in, std::span<color_space::OKLab> dst) {
        // 8×8 fixed-size blur with edge-clamp. Separable horizontal +
        // vertical box-3 filter (cheap and good enough for 8×8).
        std::array<color_space::OKLab, 64> tmp{};
        // Horizontal.
        for (std::size_t r = 0; r < 8; ++r) {
            for (std::size_t c = 0; c < 8; ++c) {
                std::size_t cl = (c == 0) ? 0 : c - 1;
                std::size_t cr = (c == 7) ? 7 : c + 1;
                auto& a = in[r * 8 + cl];
                auto& b = in[r * 8 + c];
                auto& d = in[r * 8 + cr];
                tmp[r * 8 + c] = {
                    (a.L + b.L + d.L) / 3.0f, (a.a + b.a + d.a) / 3.0f, (a.b + b.b + d.b) / 3.0f};
            }
        }
        // Vertical.
        for (std::size_t r = 0; r < 8; ++r) {
            for (std::size_t c = 0; c < 8; ++c) {
                std::size_t rt = (r == 0) ? 0 : r - 1;
                std::size_t rb = (r == 7) ? 7 : r + 1;
                auto& a = tmp[rt * 8 + c];
                auto& b = tmp[r * 8 + c];
                auto& d = tmp[rb * 8 + c];
                dst[r * 8 + c] = {
                    (a.L + b.L + d.L) / 3.0f, (a.a + b.a + d.a) / 3.0f, (a.b + b.b + d.b) / 3.0f};
            }
        }
    };
    auto distance_fn = [&](std::span<const std::uint8_t> a,
                           std::span<const std::uint8_t> b) -> float {
        std::array<color_space::OKLab, 64> a_lab, b_lab;
        for (std::size_t i = 0; i < 64; ++i) {
            a_lab[i] = direct_color ? unpack_direct(a[i]) : pal_lab[a[i]];
            b_lab[i] = direct_color ? unpack_direct(b[i]) : pal_lab[b[i]];
        }
        float per_pixel = 0.0f;
        for (std::size_t i = 0; i < 64; ++i) {
            float dL = a_lab[i].L - b_lab[i].L;
            float da = a_lab[i].a - b_lab[i].a;
            float dbb = a_lab[i].b - b_lab[i].b;
            per_pixel += color_space::fma_dist_sq(dL, da, dbb);
        }
        std::array<color_space::OKLab, 64> a_blur, b_blur;
        blur_3x3(a_lab, a_blur);
        blur_3x3(b_lab, b_blur);
        float coarse = 0.0f;
        for (std::size_t i = 0; i < 64; ++i) {
            float dL = a_blur[i].L - b_blur[i].L;
            float da = a_blur[i].a - b_blur[i].a;
            float dbb = a_blur[i].b - b_blur[i].b;
            coarse += color_space::fma_dist_sq(dL, da, dbb);
        }
        // α = 0.4 (60% per-pixel, 40% coarse). Higher α favors
        // structural matches over fine-detail accuracy; lower keeps
        // edges sharp at the cost of texture blending. 0.4 chosen
        // empirically to land roughly midway on photo content.
        return 0.6f * per_pixel + 0.4f * coarse;
    };
    // Build a CachedDistance pair: prepare() does the per-pattern
    // OKLab decode + 3×3 blur once per alive slot; score() consumes
    // those cached arrays and runs only the per-pixel and coarse
    // diff sums (128 fma_dist_sq calls). Without this, the merger's
    // O(N²) loop redoes the lab+blur work per pair.
    CachedDistance cached;
    // Contrast masking: an error in a flat tile shows, the same error in
    // a textured one mostly doesn't. Weight each tile by its flatness,
    // from the variance of its blurred OKLab (dither removed): 1 when
    // flat, falling toward kMaskFloor as the variance passes kMaskVar.
    // This moves the merge budget toward smooth areas.
    constexpr float kMaskVar = 0.001f;
    constexpr float kMaskFloor = 0.25f;
    auto flatness = [](const std::array<color_space::OKLab, 64>& bl) -> float {
        color_space::OKLab m{};
        for (const auto& c : bl) {
            m.L += c.L;
            m.a += c.a;
            m.b += c.b;
        }
        m = {m.L / 64.0f, m.a / 64.0f, m.b / 64.0f};
        float var = 0.0f;
        for (const auto& c : bl)
            var += color_space::fma_dist_sq(c.L - m.L, c.a - m.a, c.b - m.b);
        var /= 64.0f;
        return kMaskFloor + (1.0f - kMaskFloor) * kMaskVar / (kMaskVar + var);
    };
    cached.prepare = [&](std::span<const std::uint8_t> pat) -> SlotPre {
        SlotPre sp;
        for (std::size_t i = 0; i < 64; ++i) {
            sp.lab[i] = direct_color ? unpack_direct(pat[i]) : pal_lab[pat[i]];
        }
        blur_3x3(sp.lab, sp.blur);
        sp.weight = flatness(sp.blur);
        return sp;
    };
    // The merge scores a pair by the flatter of the two tiles.
    CachedDistance merge_cached = cached;
    merge_cached.score = [&](const SlotPre& a, const SlotPre& b) -> float {
        return std::max(a.weight, b.weight) * cached.score(a, b);
    };
    cached.score = [](const SlotPre& a, const SlotPre& b) -> float {
        float per_pixel = 0.0f;
        for (std::size_t i = 0; i < 64; ++i) {
            per_pixel += color_space::fma_dist_sq(
                a.lab[i].L - b.lab[i].L, a.lab[i].a - b.lab[i].a, a.lab[i].b - b.lab[i].b);
        }
        float coarse = 0.0f;
        for (std::size_t i = 0; i < 64; ++i) {
            coarse += color_space::fma_dist_sq(
                a.blur[i].L - b.blur[i].L, a.blur[i].a - b.blur[i].a, a.blur[i].b - b.blur[i].b);
        }
        return 0.6f * per_pixel + 0.4f * coarse;
    };
    auto packed = pack_snes_mode7_frame(chunky,
                                        w,
                                        h,
                                        distance_fn,
                                        merge_cached,
                                        on_progress,
                                        kStageQuant,
                                        kStageQuant + kStagePack);
    if (!packed) return std::unexpected{packed.error()};
    report(kStageQuant + kStagePack, "merged");

    // ---- Step 2b: Lloyd refinement.
    //
    // Greedy merge picks one of the two original tiles as the
    // representative each time it collapses a pair — the surviving 256
    // tiles are *cells the image already contained*, not the perceptual
    // average of the merged groups. Lloyd refinement fixes that:
    //
    //   a) Synthesise each surviving tile as the per-pixel-position
    //      OKLab MEAN of all cells now assigned to it (then Bayer-dither
    //      back to valid bytes: palette indices for 256 mode, BBGGGRRR
    //      for Direct). Clusters of 1 cell, or of identical cells, are
    //      left untouched.
    //   b) Reassign each cell to whichever surviving pattern is now
    //      closest under the same distance metric.
    //   c) Iterate until no cell switches assignment, capped at
    //      kRefineIters. Two iterations capture most of the gain on
    //      photo-content; three is the empirical knee.
    //
    // The two flavours share the same algorithm; only the per-pixel
    // "byte → OKLab" and "OKLab → nearest byte" maps differ.
    // tiles_x / tiles_y reuse the values declared in Step 1 above.
    constexpr std::size_t kTile = 8;
    constexpr std::size_t kVirtualSide = 128;

    auto byte_to_lab = [&](std::uint8_t b) -> color_space::OKLab {
        return direct_color ? unpack_direct(b) : pal_lab[b];
    };
    // Rebuilt tiles: an 8x8 Bayer dither of the mean colour at each tile
    // position. Snapping the mean to the nearest colour wipes out the
    // dither of the merged cells, so smooth areas turn into flat 8x8
    // steps; the Bayer pattern repeats every 8 pixels, so a tile next to
    // itself joins without a seam.
    static constexpr std::array<std::uint8_t, 64> kBayer8 = {
        0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4,  36, 14, 46,
        6,  38, 60, 28, 52, 20, 62, 30, 54, 22, 3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27,
        49, 17, 57, 25, 15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};
    // With dithering off, tiles snap to the nearest colour instead. A
    // pattern dither (ordered, or a Yliluoma-style plan) rebuilds with its
    // own threshold matrix so merged tiles keep the chosen look; the
    // palette-aware plans use the ordered matrix their phase follows.
    using dither::Method;
    const Method dm = dither_settings.method;
    const bool tile_dither = dm != Method::none && dither_settings.strength > 0.0f;
    const bool pattern_dither = dither::is_ordered(dm) || dither::is_yliluoma(dm);
    const Method tile_matrix = [&] {
        switch (dm) {
        case Method::opt_checker:
        case Method::tri_tone:
            return Method::checker;
        case Method::opt_line:
            return Method::line2;
        case Method::opt_line_checker:
            return Method::line_checker;
        case Method::opt_vline:
            return Method::vline2;
        case Method::opt_vline_checker:
            return Method::vline_checker;
        case Method::opt_rowdot:
            return Method::rowdot;
        case Method::opt_rowdot_aligned:
            return Method::rowdot_aligned;
        case Method::opt_rowdot_checker:
            return Method::rowdot_checker;
        case Method::knoll:
            return Method::bayer4x4;
        case Method::yliluoma:
        case Method::yliluoma1:
        case Method::yliluoma2:
            return Method::bayer8x8;
        default:
            return dm;
        }
    }();
    auto tile_threshold = [&](std::size_t pos) {
        if (!tile_dither) return 0.5f;
        if (!pattern_dither) return (static_cast<float>(kBayer8[pos]) + 0.5f) / 64.0f;
        const float t = dither::ordered_threshold(tile_matrix, pos % kTile, pos / kTile);
        return std::clamp(0.5f + t * std::min(dither_settings.strength, 1.0f), 0.0f, 1.0f);
    };
    auto lab_to_byte_bayer = [&](color_space::OKLab lab, std::size_t pos) -> std::uint8_t {
        const float thr = tile_threshold(pos);
        if (direct_color) {
            auto srgb = color_space::linear_to_srgb(color_space::oklab_to_linear(lab));
            // Per channel, between the two direct levels around the value.
            auto q = [&](float v, int bits) {
                const int top = (1 << bits) - 1;
                if (v <= direct_level(0, bits)) return 0;
                for (int k = 0; k < top; ++k) {
                    float lo = direct_level(k, bits), hi = direct_level(k + 1, bits);
                    if (v < hi) return k + ((v - lo) / (hi - lo) > thr ? 1 : 0);
                }
                return top;
            };
            int r3 = q(srgb.r, 3), g3 = q(srgb.g, 3), b2 = q(srgb.b, 2);
            return static_cast<std::uint8_t>((b2 << 6) | (g3 << 3) | r3);
        }
        // Nearest entry, then the partner among the 8 nearest whose segment
        // passes closest to the mean; take it where its share beats thr.
        std::array<std::pair<float, std::size_t>, 256> d{};
        const std::size_t pn = pal_lab.size();
        for (std::size_t i = 0; i < pn; ++i) {
            const auto& c = pal_lab[i];
            d[i] = {color_space::fma_dist_sq(lab.L - c.L, lab.a - c.a, lab.b - c.b), i};
        }
        const std::size_t k = tile_dither ? std::min<std::size_t>(8, pn) : 1;
        std::partial_sort(d.begin(),
                          d.begin() + static_cast<std::ptrdiff_t>(k),
                          d.begin() + static_cast<std::ptrdiff_t>(pn));
        const auto& c1 = pal_lab[d[0].second];
        float best = d[0].first;
        std::size_t partner = d[0].second;
        float share = 0;
        for (std::size_t j = 1; j < k; ++j) {
            const auto& c2 = pal_lab[d[j].second];
            float ex = c2.L - c1.L, ey = c2.a - c1.a, ez = c2.b - c1.b;
            float len = ex * ex + ey * ey + ez * ez;
            if (len <= 0) continue;
            float t = ((lab.L - c1.L) * ex + (lab.a - c1.a) * ey + (lab.b - c1.b) * ez) / len;
            if (t <= 0 || t >= 1) continue;
            float rx = c1.L + t * ex - lab.L, ry = c1.a + t * ey - lab.a,
                  rz = c1.b + t * ez - lab.b;
            float e = rx * rx + ry * ry + rz * rz + 0.1f * t * (1 - t) * len;
            if (e < best) {
                best = e;
                partner = d[j].second;
                share = t;
            }
        }
        return static_cast<std::uint8_t>(share > thr ? partner : d[0].second);
    };

    // Inner Lloyd iterations per outer pass. Empirically converges in
    // 3-5; 16 is the cap.
    constexpr int kRefineIters = 16;
    // Outer alternating optimisation: re-derive the palette from the
    // post-Lloyd tile pixel distribution between Lloyd passes. On
    // BGR555-snapped 256-mode the palette gain is small (< 0.2 dB)
    // because the original median-cut palette is already well-tuned
    // for the BGR555 grid; mainly here to keep the code path warm
    // for future console targets with finer-grained palettes.
    constexpr int kOuterIters = 2;
    auto& tilemap = packed->tilemap;
    auto& tiles = packed->tile_bytes;
    auto num_tiles = tiles.size() / 64;

    int total_lloyd_steps = kOuterIters * kRefineIters;
    int lloyd_step = 0;
    auto lloyd_progress = [&](std::string_view stage) {
        if (!on_progress) return;
        float local = static_cast<float>(lloyd_step) / static_cast<float>(total_lloyd_steps);
        report(kStageQuant + kStagePack + kStageLloyd * local, stage);
    };

    for (int outer = 0; outer < kOuterIters; ++outer) {
        if (outer > 0 && !direct_color) {
            // ---- Palette retraining.
            //
            // The post-Lloyd tiles only display 256 × 64 = 16384
            // pixel "slots" — nothing else from the source image
            // matters at this point. Re-derive the 256-entry palette
            // via median-cut on JUST those displayed colors so the
            // palette anchors land where they're actually used. Then
            // re-snap every tile byte to the closest entry in the
            // new palette and let Lloyd re-converge with the new
            // pal_lab table.
            std::vector<Color3f> displayed;
            displayed.reserve(num_tiles * 64);
            for (auto byte : tiles)
                displayed.push_back(out.palette[byte]);

            const std::size_t disp_count = displayed.size();
            Image disp_img(disp_count, 1, std::move(displayed));
            auto new_pal = quantize::quantize(
                disp_img, 256, quantize::Algorithm::median_cut, palette_diversity);
            if (new_pal) {
                std::vector<Color3f> new_palette = new_pal->colors;
                for (auto& c : new_palette)
                    c = console_color::bgr555_quantize(c);

                // Build new pal_lab.
                std::vector<color_space::OKLab> new_pal_lab(new_palette.size());
                for (std::size_t i = 0; i < new_palette.size(); ++i)
                    new_pal_lab[i] = color_space::linear_to_oklab(new_palette[i]);

                // Remap a byte from old-palette index → new-palette
                // index by finding the new entry nearest the byte's
                // OLD displayed color. SoA SIMD argmin built once,
                // amortised across 256 × 64 = 16 K remap calls.
                oklab_simd::PaletteSoA new_pal_soa{};
                oklab_simd::fill(new_pal_lab, new_pal_soa);
                auto remap = [&](std::uint8_t b) -> std::uint8_t {
                    return static_cast<std::uint8_t>(
                        oklab_simd::argmin(pal_lab[b], new_pal_soa).index);
                };
                // Both the tile bytes AND the chunky source need to
                // be remapped — the next Lloyd round looks up cell
                // patterns in the chunky buffer and compares against
                // tile patterns under the new pal_lab. Without
                // remapping chunky, the distance metric reads stale
                // colors and PSNR collapses.
                for (auto& byte : tiles)
                    byte = remap(byte);
                for (auto& byte : chunky)
                    byte = remap(byte);

                out.palette = std::move(new_palette);
                pal_lab = std::move(new_pal_lab);
            }
        }

        for (int iter = 0; iter < kRefineIters; ++iter) {
            lloyd_progress("Lloyd refinement");
            // ---- 2b.a: synthesise centroids.
            // Per-tile, per-position OKLab accumulators. Indexed
            // [tile_idx*64 + pos].
            std::vector<color_space::OKLab> sums(num_tiles * 64, {0, 0, 0});
            std::vector<std::size_t> counts(num_tiles, 0);
            // First member cell of each tile, and whether any member differs:
            // a tile whose cells are all identical keeps their exact pattern.
            std::vector<std::size_t> first(num_tiles, 0);
            std::vector<std::uint8_t> mixed(num_tiles, 0);
            auto same_cells = [&](std::size_t a, std::size_t b) {
                const std::size_t ax = a % tiles_x * kTile, ay = a / tiles_x * kTile;
                const std::size_t bx = b % tiles_x * kTile, by = b / tiles_x * kTile;
                for (std::size_t row = 0; row < kTile; ++row)
                    for (std::size_t col = 0; col < kTile; ++col) {
                        auto pa = (ax + col < w && ay + row < h) ? chunky[(ay + row) * w + ax + col]
                                                                 : 0;
                        auto pb = (bx + col < w && by + row < h) ? chunky[(by + row) * w + bx + col]
                                                                 : 0;
                        if (pa != pb) return false;
                    }
                return true;
            };

            for (std::size_t ty = 0; ty < tiles_y; ++ty) {
                for (std::size_t tx = 0; tx < tiles_x; ++tx) {
                    auto t = tilemap[ty * kVirtualSide + tx];
                    const std::size_t lin = ty * tiles_x + tx;
                    if (counts[t] == 0)
                        first[t] = lin;
                    else if (!mixed[t] && !same_cells(first[t], lin))
                        mixed[t] = 1;
                    ++counts[t];
                    for (std::size_t row = 0; row < kTile; ++row) {
                        for (std::size_t col = 0; col < kTile; ++col) {
                            auto px = tx * kTile + col;
                            auto py = ty * kTile + row;
                            if (px >= w || py >= h) continue;
                            auto byte = chunky[py * w + px];
                            auto lab = byte_to_lab(byte);
                            auto& acc = sums[t * 64 + row * kTile + col];
                            acc.L += lab.L;
                            acc.a += lab.a;
                            acc.b += lab.b;
                        }
                    }
                }
            }

            // Build new tile bytes from per-position means, Bayer-dithered;
            // clusters of 1 cell, or of identical cells, keep their pattern.
            std::vector<std::uint8_t> new_tiles = tiles;
            pipeline::parallel_for(num_tiles, [&](std::size_t t) {
                if (counts[t] <= 1) return;
                if (!mixed[t]) {
                    const std::size_t cx = first[t] % tiles_x * kTile,
                                      cy = first[t] / tiles_x * kTile;
                    for (std::size_t row = 0; row < kTile; ++row)
                        for (std::size_t col = 0; col < kTile; ++col)
                            new_tiles[t * 64 + row * kTile + col] =
                                (cx + col < w && cy + row < h) ? chunky[(cy + row) * w + cx + col]
                                                               : 0;
                    return;
                }
                float n = static_cast<float>(counts[t]);
                for (std::size_t pos = 0; pos < 64; ++pos) {
                    auto& s = sums[t * 64 + pos];
                    color_space::OKLab mean{s.L / n, s.a / n, s.b / n};
                    new_tiles[t * 64 + pos] = lab_to_byte_bayer(mean, pos);
                }
            });
            tiles = std::move(new_tiles);

            // ---- 2b.b: reassign each cell to nearest centroid.
            // ---- 2b.b: reassign cells to nearest centroid + track per-cell
            //   error so we can split overloaded clusters in step 2b.c.
            // Pre-compute SlotPre for tiles (rebuilt this iter) and for all
            // cells (cells don't change between iterations — could be
            // hoisted outside the iter loop, but doing it per-iter keeps
            // the change minimal). Cell loop is then parallel.
            const std::size_t live_cells = tiles_y * tiles_x;
            std::vector<SlotPre> tile_pre(num_tiles);
            pipeline::parallel_for(num_tiles, [&](std::size_t t) {
                std::span<const std::uint8_t> tile_span(tiles.data() + t * 64, 64);
                tile_pre[t] = cached.prepare(tile_span);
            });
            std::vector<SlotPre> cell_pre(live_cells);
            std::vector<std::array<std::uint8_t, 64>> cell_pats(live_cells);
            pipeline::parallel_for(live_cells, [&](std::size_t lin) {
                const std::size_t ty = lin / tiles_x;
                const std::size_t tx = lin % tiles_x;
                auto& cp = cell_pats[lin];
                for (std::size_t row = 0; row < kTile; ++row) {
                    for (std::size_t col = 0; col < kTile; ++col) {
                        auto px = tx * kTile + col;
                        auto py = ty * kTile + row;
                        cp[row * kTile + col] = (px < w && py < h) ? chunky[py * w + px] : 0;
                    }
                }
                cell_pre[lin] = cached.prepare(std::span<const std::uint8_t>(cp));
            });

            bool changed = false;
            std::vector<float> cell_error(live_cells, 0.0f);
            std::vector<std::size_t> cell_assignment(live_cells, 0);
            std::atomic<bool> changed_atomic{false};
            pipeline::parallel_for(live_cells, [&](std::size_t lin) {
                std::size_t best = 0;
                float best_d = std::numeric_limits<float>::max();
                for (std::size_t t = 0; t < num_tiles; ++t) {
                    float d = cached.score(cell_pre[lin], tile_pre[t]);
                    if (d < best_d) {
                        best_d = d;
                        best = t;
                    }
                }
                cell_error[lin] = best_d * cell_pre[lin].weight;
                cell_assignment[lin] = best;
                const std::size_t ty = lin / tiles_x;
                const std::size_t tx = lin % tiles_x;
                auto& cell = tilemap[ty * kVirtualSide + tx];
                if (cell != best) {
                    cell = static_cast<std::uint8_t>(best);
                    changed_atomic.store(true, std::memory_order_relaxed);
                }
            });
            changed = changed_atomic.load();

            // ---- 2b.c: empty-cluster splitting (PG-k-means / ISODATA).
            //   When reassignment orphans a tile slot (no cells map to it),
            //   re-seed that slot by stealing the WORST-ERROR cell from the
            //   most-overloaded cluster. This recovers a wasted tile slot
            //   and naturally splits clusters that span too much variation
            //   for one centroid to represent.
            std::vector<std::size_t> new_counts(num_tiles, 0);
            std::vector<float> total_err(num_tiles, 0.0f);
            for (std::size_t i = 0; i < cell_assignment.size(); ++i) {
                ++new_counts[cell_assignment[i]];
                total_err[cell_assignment[i]] += cell_error[i];
            }
            // Helper: re-seed tile slot `victim` from the worst-fitting
            // cell of the highest-error cluster (must have ≥ 2 cells).
            // Returns true if a split happened.
            auto split_into = [&](std::size_t victim) -> bool {
                std::size_t worst_t = 0;
                float worst_total = -1.0f;
                for (std::size_t k = 0; k < num_tiles; ++k) {
                    if (k == victim || new_counts[k] < 2) continue;
                    if (total_err[k] > worst_total) {
                        worst_total = total_err[k];
                        worst_t = k;
                    }
                }
                if (worst_total <= 0.0f) return false;
                std::size_t worst_cell = 0;
                float worst_cell_err = -1.0f;
                for (std::size_t i = 0; i < cell_assignment.size(); ++i) {
                    if (cell_assignment[i] != worst_t) continue;
                    if (cell_error[i] > worst_cell_err) {
                        worst_cell_err = cell_error[i];
                        worst_cell = i;
                    }
                }
                std::size_t cy = worst_cell / tiles_x;
                std::size_t cx = worst_cell % tiles_x;
                for (std::size_t row = 0; row < kTile; ++row) {
                    for (std::size_t col = 0; col < kTile; ++col) {
                        auto px = cx * kTile + col;
                        auto py = cy * kTile + row;
                        tiles[victim * 64 + row * kTile + col] = (px < w && py < h)
                                                                     ? chunky[py * w + px]
                                                                     : 0;
                    }
                }
                tilemap[cy * kVirtualSide + cx] = static_cast<std::uint8_t>(victim);
                cell_assignment[worst_cell] = victim;
                new_counts[victim] = 1;
                new_counts[worst_t]--;
                total_err[worst_t] -= cell_error[worst_cell];
                total_err[victim] = 0.0f;
                cell_error[worst_cell] = 0.0f;
                return true;
            };

            // 2b.c.1: re-seed any orphans (count==0).
            for (std::size_t t = 0; t < num_tiles; ++t) {
                if (new_counts[t] != 0) continue;
                if (split_into(t)) changed = true;
            }

            // 2b.c.2: redundancy collapse. If two surviving tiles ended up
            //   with very similar patterns (the tilemap rarely picks one
            //   over the other → wasted slot), collapse the less-used one
            //   and split a high-error cluster into the freed slot. We do
            //   this once per iteration to keep the cost bounded.
            //
            //   Heuristic: find the smallest-cluster tile whose centroid is
            //   < eps from another tile. Steal it. eps tuned at 1.5× the
            //   median cluster's mean per-cell error so we only collapse
            //   genuinely redundant slots.
            if (iter < kRefineIters - 1) {
                std::vector<std::size_t> sorted_by_count(num_tiles);
                for (std::size_t i = 0; i < num_tiles; ++i)
                    sorted_by_count[i] = i;
                std::sort(
                    sorted_by_count.begin(),
                    sorted_by_count.end(),
                    [&](std::size_t a, std::size_t b) { return new_counts[a] < new_counts[b]; });
                // Median cluster's mean per-cell error sets the redundancy
                // threshold — too-low redundancy clusters are noisy edges
                // that we shouldn't collapse.
                float median_total = 0.0f;
                std::size_t median_idx = num_tiles / 2;
                if (median_idx < sorted_by_count.size()) {
                    auto t = sorted_by_count[median_idx];
                    if (new_counts[t] > 0)
                        median_total = total_err[t] / static_cast<float>(new_counts[t]);
                }
                float eps = 1.5f * median_total;
                for (auto victim : sorted_by_count) {
                    if (new_counts[victim] == 0) continue;
                    if (new_counts[victim] > 4) break;  // skip popular tiles
                    // Find another tile with similar centroid.
                    std::span<const std::uint8_t> v_span(tiles.data() + victim * 64, 64);
                    bool redundant = false;
                    for (std::size_t k = 0; k < num_tiles; ++k) {
                        if (k == victim) continue;
                        std::span<const std::uint8_t> k_span(tiles.data() + k * 64, 64);
                        if (distance_fn(v_span, k_span) < eps) {
                            redundant = true;
                            break;
                        }
                    }
                    if (redundant) {
                        if (split_into(victim)) {
                            changed = true;
                            break;
                        }
                    }
                }
            }

            ++lloyd_step;
            if (!changed) {
                // Skip ahead — early-converged inner pass.
                lloyd_step += (kRefineIters - 1 - iter);
                break;
            }
        }
    }  // end outer loop

    // ---- Step 2c: pick each cell's tile with its neighbours in view.
    //   Lloyd scores a cell on its own 8x8 block, so two neighbours can
    //   each be close and still join with a visible step. Re-pick every
    //   cell among its best few tiles on the blurred error over the cell
    //   plus a margin into its neighbours. Cells are swept in 2x2
    //   interleaved classes; same-class cells are 8 px apart, so with a
    //   blur radius of at most 4 they never read each other's pixels.
    //   Each round then runs a descent on the tile bytes themselves,
    //   scored on the same blurred error summed over every cell showing
    //   the tile. A small per-pixel term against the source keeps both
    //   steps from trading accuracy for colour noise the blur hides.
    //   Both steps dither (the pick at tile scale) with an unstructured
    //   look, so neither runs when dithering is off or a pattern dither
    //   is chosen.
    //   Two rounds of (3 pick sweeps, 2 descent sweeps) are the knee:
    //   mean S2 over the test set +4.5 in 256 mode, +14.5 in Direct.
    constexpr float nb_sigma = 1.0f;
    constexpr std::size_t nb_k = 16;
    constexpr int nb_sweeps = 3;
    constexpr int nb_tile_sweeps = 2;
    constexpr int nb_rounds = 2;
    constexpr float nb_fid = 0.05f;
    if (tile_dither && !pattern_dither && num_tiles > 1) {
        const int r = std::min(4, static_cast<int>(std::ceil(2.0f * nb_sigma)));
        std::vector<float> kern(static_cast<std::size_t>(2 * r + 1));
        float ksum = 0;
        for (int i = -r; i <= r; ++i)
            ksum += kern[static_cast<std::size_t>(i + r)] = std::exp(
                -0.5f * static_cast<float>(i * i) / (nb_sigma * nb_sigma));
        for (auto& k : kern)
            k /= ksum;
        auto byte_lin = [&](std::uint8_t b) {
            return direct_color ? console_color::direct_color_linear(b) : out.palette[b];
        };
        const int iw = static_cast<int>(w), ih = static_cast<int>(h);
        auto clampx = [&](int x) {
            return std::clamp(x, 0, iw - 1);
        };
        auto clampy = [&](int y) {
            return std::clamp(y, 0, ih - 1);
        };
        // Blurred source in OKLab, once.
        std::vector<color_space::OKLab> src_blur(w * h);
        {
            std::vector<Color3f> tmp(w * h);
            pipeline::parallel_for(h, [&](std::size_t y) {
                for (int x = 0; x < iw; ++x) {
                    Color3f a{};
                    for (int i = -r; i <= r; ++i)
                        a = a + source[static_cast<std::size_t>(clampx(x + i)), y] *
                                    kern[static_cast<std::size_t>(i + r)];
                    tmp[y * w + static_cast<std::size_t>(x)] = a;
                }
            });
            pipeline::parallel_for(h, [&](std::size_t y) {
                for (std::size_t x = 0; x < w; ++x) {
                    Color3f a{};
                    for (int i = -r; i <= r; ++i)
                        a = a +
                            tmp[static_cast<std::size_t>(clampy(static_cast<int>(y) + i)) * w + x] *
                                kern[static_cast<std::size_t>(i + r)];
                    src_blur[y * w + x] = color_space::linear_to_oklab(a);
                }
            });
        }
        std::vector<color_space::OKLab> src_lab(w * h);
        pipeline::parallel_for(h, [&](std::size_t y) {
            for (std::size_t x = 0; x < w; ++x)
                src_lab[y * w + x] = color_space::linear_to_oklab(source[x, y]);
        });
        std::array<color_space::OKLab, 256> byte_lab{};
        for (std::size_t b = 0; b < 256; ++b)
            byte_lab[b] = color_space::linear_to_oklab(byte_lin(static_cast<std::uint8_t>(b)));
        auto fid = [&](std::uint8_t b, std::size_t q) {
            const auto &l = byte_lab[b], &s = src_lab[q];
            return color_space::fma_dist_sq(l.L - s.L, l.a - s.a, l.b - s.b);
        };
        // Current render, linear.
        std::vector<Color3f> ren(w * h);
        auto paint = [&](std::size_t tx, std::size_t ty, std::size_t t) {
            for (std::size_t row = 0; row < kTile; ++row)
                for (std::size_t col = 0; col < kTile; ++col) {
                    auto px = tx * kTile + col, py = ty * kTile + row;
                    if (px < w && py < h)
                        ren[py * w + px] = byte_lin(tiles[t * 64 + row * kTile + col]);
                }
        };
        for (std::size_t ty = 0; ty < tiles_y; ++ty)
            for (std::size_t tx = 0; tx < tiles_x; ++tx)
                paint(tx, ty, tilemap[ty * kVirtualSide + tx]);

        // Candidates: the nb_k tiles nearest each cell by the Lloyd metric.
        const std::size_t live_cells = tiles_y * tiles_x;
        const std::size_t kc = std::min(nb_k, num_tiles);
        std::vector<SlotPre> tile_pre(num_tiles);
        pipeline::parallel_for(num_tiles, [&](std::size_t t) {
            tile_pre[t] = cached.prepare(std::span<const std::uint8_t>(tiles.data() + t * 64, 64));
        });
        std::vector<std::uint16_t> cand(live_cells * kc);
        pipeline::parallel_for(live_cells, [&](std::size_t lin) {
            const std::size_t ty = lin / tiles_x, tx = lin % tiles_x;
            std::array<std::uint8_t, 64> cp{};
            for (std::size_t row = 0; row < kTile; ++row)
                for (std::size_t col = 0; col < kTile; ++col) {
                    auto px = tx * kTile + col, py = ty * kTile + row;
                    cp[row * kTile + col] = (px < w && py < h) ? chunky[py * w + px] : 0;
                }
            auto pre = cached.prepare(std::span<const std::uint8_t>(cp));
            std::vector<std::pair<float, std::uint16_t>> d(num_tiles);
            for (std::size_t t = 0; t < num_tiles; ++t)
                d[t] = {cached.score(pre, tile_pre[t]), static_cast<std::uint16_t>(t)};
            std::partial_sort(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(kc), d.end());
            for (std::size_t j = 0; j < kc; ++j)
                cand[lin * kc + j] = d[j].second;
        });

        // Cost of the cell at (tx, ty) showing tile t: blurred OKLab error
        // over the cell plus r px around it.
        auto cost = [&](std::size_t tx, std::size_t ty, std::size_t t) {
            const int cx0 = static_cast<int>(tx * kTile), cy0 = static_cast<int>(ty * kTile);
            const int x0 = std::max(0, cx0 - r),
                      x1 = std::min(iw, cx0 + static_cast<int>(kTile) + r);
            const int y0 = std::max(0, cy0 - r),
                      y1 = std::min(ih, cy0 + static_cast<int>(kTile) + r);
            auto at = [&](int x, int y) -> Color3f {
                x = clampx(x);
                y = clampy(y);
                const int lx = x - cx0, ly = y - cy0;
                if (lx >= 0 && lx < static_cast<int>(kTile) && ly >= 0 &&
                    ly < static_cast<int>(kTile))
                    return byte_lin(tiles[t * 64 + static_cast<std::size_t>(ly) * kTile +
                                          static_cast<std::size_t>(lx)]);
                return ren[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
            };
            constexpr int kMax = 8 + 4 * 4;
            std::array<Color3f, kMax * kMax> hb{};  // horizontal blur, rows y0-r..y1+r
            const int rows = (y1 + r) - (y0 - r);
            for (int yy = 0; yy < rows; ++yy)
                for (int x = x0; x < x1; ++x) {
                    Color3f a{};
                    for (int i = -r; i <= r; ++i)
                        a = a + at(x + i, y0 - r + yy) * kern[static_cast<std::size_t>(i + r)];
                    hb[static_cast<std::size_t>(yy * kMax + (x - x0))] = a;
                }
            float e = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    Color3f a{};
                    for (int i = -r; i <= r; ++i)
                        a = a + hb[static_cast<std::size_t>((y - y0 + r + i) * kMax + (x - x0))] *
                                    kern[static_cast<std::size_t>(i + r)];
                    auto l = color_space::linear_to_oklab(a);
                    const auto& s =
                        src_blur[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
                    e += color_space::fma_dist_sq(l.L - s.L, l.a - s.a, l.b - s.b);
                }
            for (int ly = 0; ly < static_cast<int>(kTile) && cy0 + ly < ih; ++ly)
                for (int lx = 0; lx < static_cast<int>(kTile) && cx0 + lx < iw; ++lx)
                    e += nb_fid * fid(tiles[t * 64 + static_cast<std::size_t>(ly) * kTile +
                                            static_cast<std::size_t>(lx)],
                                      static_cast<std::size_t>(cy0 + ly) * w +
                                          static_cast<std::size_t>(cx0 + lx));
            return e;
        };

        // Blurred render, linear; kept exact under tile edits below.
        std::vector<Color3f> ren_blur(w * h);
        auto reblur = [&]() {
            std::vector<Color3f> tmp(w * h);
            pipeline::parallel_for(h, [&](std::size_t y) {
                for (int x = 0; x < iw; ++x) {
                    Color3f a{};
                    for (int i = -r; i <= r; ++i)
                        a = a + ren[y * w + static_cast<std::size_t>(clampx(x + i))] *
                                    kern[static_cast<std::size_t>(i + r)];
                    tmp[y * w + static_cast<std::size_t>(x)] = a;
                }
            });
            pipeline::parallel_for(h, [&](std::size_t y) {
                for (std::size_t x = 0; x < w; ++x) {
                    Color3f a{};
                    for (int i = -r; i <= r; ++i)
                        a = a +
                            tmp[static_cast<std::size_t>(clampy(static_cast<int>(y) + i)) * w + x] *
                                kern[static_cast<std::size_t>(i + r)];
                    ren_blur[y * w + x] = a;
                }
            });
        };
        // Weight of source pixel p in the blur at q along one axis,
        // counting the clamped edge samples.
        auto axis_w = [&](int q, int p, int n) {
            float a = 0;
            for (int i = -r; i <= r; ++i)
                if (std::clamp(q + i, 0, n - 1) == p) a += kern[static_cast<std::size_t>(i + r)];
            return a;
        };
        // Byte alternatives for the tile descent.
        std::vector<std::vector<std::uint8_t>> alts(256);
        for (std::size_t b = 0; b < 256; ++b) {
            if (direct_color) {
                const int rr = static_cast<int>(b & 7), gg = static_cast<int>((b >> 3) & 7),
                          bb = static_cast<int>(b >> 6);
                auto add = [&](int r2, int g2, int b2) {
                    if (r2 < 0 || r2 > 7 || g2 < 0 || g2 > 7 || b2 < 0 || b2 > 3) return;
                    alts[b].push_back(static_cast<std::uint8_t>((b2 << 6) | (g2 << 3) | r2));
                };
                for (int d : {-1, 1}) {
                    add(rr + d, gg, bb);
                    add(rr, gg + d, bb);
                    add(rr, gg, bb + d);
                    add(rr + d, gg + d, bb);
                    add(rr, gg + d, bb + d);
                    add(rr + d, gg + d, bb + d);
                }
            } else if (b < pal_lab.size()) {
                std::vector<std::pair<float, std::uint8_t>> d;
                for (std::size_t i = 0; i < pal_lab.size(); ++i) {
                    if (i == b) continue;
                    const auto &c = pal_lab[i], &o = pal_lab[b];
                    d.push_back({color_space::fma_dist_sq(c.L - o.L, c.a - o.a, c.b - o.b),
                                 static_cast<std::uint8_t>(i)});
                }
                const std::size_t k = std::min<std::size_t>(8, d.size());
                std::partial_sort(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(k), d.end());
                for (std::size_t j = 0; j < k; ++j)
                    alts[b].push_back(d[j].second);
            }
        }
        auto tile_descent = [&]() {
            reblur();
            std::vector<std::vector<std::size_t>> users(num_tiles);
            for (std::size_t ty = 0; ty < tiles_y; ++ty)
                for (std::size_t tx = 0; tx < tiles_x; ++tx)
                    users[tilemap[ty * kVirtualSide + tx]].push_back(ty * tiles_x + tx);
            std::size_t changed = 0;
            for (std::size_t t = 0; t < num_tiles; ++t) {
                if (users[t].empty()) continue;
                for (std::size_t pos = 0; pos < 64; ++pos) {
                    const std::uint8_t cur = tiles[t * 64 + pos];
                    const Color3f cl = byte_lin(cur);
                    std::uint8_t best = cur;
                    float best_d = 0;
                    for (auto nb : alts[cur]) {
                        const Color3f dl = byte_lin(nb) - cl;
                        float dsum = 0;
                        for (auto lin : users[t]) {
                            const int px = static_cast<int>(lin % tiles_x * kTile + pos % kTile);
                            const int py = static_cast<int>(lin / tiles_x * kTile + pos / kTile);
                            if (px >= iw || py >= ih) continue;
                            {
                                const std::size_t pq = static_cast<std::size_t>(py) * w +
                                                       static_cast<std::size_t>(px);
                                dsum += nb_fid * (fid(nb, pq) - fid(cur, pq));
                            }
                            for (int qy = std::max(0, py - r); qy <= std::min(ih - 1, py + r);
                                 ++qy) {
                                const float wy = axis_w(qy, py, ih);
                                for (int qx = std::max(0, px - r); qx <= std::min(iw - 1, px + r);
                                     ++qx) {
                                    const float k = wy * axis_w(qx, px, iw);
                                    const std::size_t q = static_cast<std::size_t>(qy) * w +
                                                          static_cast<std::size_t>(qx);
                                    const auto& sl = src_blur[q];
                                    auto o = color_space::linear_to_oklab(ren_blur[q]);
                                    auto n = color_space::linear_to_oklab(ren_blur[q] + dl * k);
                                    dsum += color_space::fma_dist_sq(
                                                n.L - sl.L, n.a - sl.a, n.b - sl.b) -
                                            color_space::fma_dist_sq(
                                                o.L - sl.L, o.a - sl.a, o.b - sl.b);
                                }
                            }
                        }
                        if (dsum < best_d) {
                            best_d = dsum;
                            best = nb;
                        }
                    }
                    if (best == cur) continue;
                    const Color3f dl = byte_lin(best) - cl;
                    tiles[t * 64 + pos] = best;
                    ++changed;
                    for (auto lin : users[t]) {
                        const int px = static_cast<int>(lin % tiles_x * kTile + pos % kTile);
                        const int py = static_cast<int>(lin / tiles_x * kTile + pos / kTile);
                        if (px >= iw || py >= ih) continue;
                        ren[static_cast<std::size_t>(py) * w + static_cast<std::size_t>(px)] =
                            byte_lin(best);
                        for (int qy = std::max(0, py - r); qy <= std::min(ih - 1, py + r); ++qy) {
                            const float wy = axis_w(qy, py, ih);
                            for (int qx = std::max(0, px - r); qx <= std::min(iw - 1, px + r); ++qx)
                                ren_blur[static_cast<std::size_t>(qy) * w +
                                         static_cast<std::size_t>(qx)] += dl *
                                                                          (wy * axis_w(qx, px, iw));
                        }
                    }
                }
            }
            return changed;
        };

        for (int round = 0; round < nb_rounds; ++round) {
            for (int sweep = 0; sweep < nb_sweeps; ++sweep) {
                std::atomic<std::size_t> moved{0};
                for (std::size_t cls = 0; cls < 4; ++cls) {
                    std::vector<std::size_t> cells;
                    for (std::size_t ty = cls / 2; ty < tiles_y; ty += 2)
                        for (std::size_t tx = cls % 2; tx < tiles_x; tx += 2)
                            cells.push_back(ty * tiles_x + tx);
                    pipeline::parallel_for(cells.size(), [&](std::size_t i) {
                        const std::size_t lin = cells[i], ty = lin / tiles_x, tx = lin % tiles_x;
                        auto& cell = tilemap[ty * kVirtualSide + tx];
                        std::size_t best = cell;
                        float best_e = cost(tx, ty, cell);
                        for (std::size_t j = 0; j < kc; ++j) {
                            const std::size_t t = cand[lin * kc + j];
                            if (t == cell) continue;
                            float e = cost(tx, ty, t);
                            if (e < best_e) {
                                best_e = e;
                                best = t;
                            }
                        }
                        if (best != cell) {
                            cell = static_cast<std::uint8_t>(best);
                            paint(tx, ty, best);
                            moved.fetch_add(1, std::memory_order_relaxed);
                        }
                    });
                }
                if (moved.load() == 0) break;
            }
            for (int ts = 0; ts < nb_tile_sweeps; ++ts)
                if (tile_descent() == 0) break;
        }
    }
    report(kStageQuant + kStagePack + kStageLloyd, "rendering preview");

    // ---- Step 3: build packed bytes (tilemap + tile data) AND re-render
    //   the preview from the post-merge tilemap+tiles+palette so the
    //   user sees what real hardware would actually display.
    out.packed_bytes.reserve(packed->tilemap.size() + packed->tile_bytes.size());
    out.packed_bytes.insert(out.packed_bytes.end(), packed->tilemap.begin(), packed->tilemap.end());
    out.packed_bytes.insert(
        out.packed_bytes.end(), packed->tile_bytes.begin(), packed->tile_bytes.end());

    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            auto tile_idx = packed->tilemap[ty * kVirtualSide + tx];
            auto tile_off = static_cast<std::size_t>(tile_idx) * 64;
            for (std::size_t row = 0; row < kTile; ++row) {
                for (std::size_t col = 0; col < kTile; ++col) {
                    auto px = tx * kTile + col;
                    auto py = ty * kTile + row;
                    if (px >= w || py >= h) continue;
                    auto byte = packed->tile_bytes[tile_off + row * kTile + col];
                    Color3f rgb;
                    if (direct_color) {
                        rgb = console_color::direct_color_linear(byte);
                    } else {
                        rgb = out.palette[byte];
                    }
                    out.rendered[px, py] = rgb;
                }
            }
        }
    }

    report(kStageQuant + kStagePack + kStageLloyd + kStageRender, "done");
    out.unique_after_dedup = packed->unique_after_dedup;
    out.unique_after_merge = packed->unique_after_merge;
    out.merges_done = packed->merges_done;
    return out;
}

}  // namespace png2amiga::snes_io
