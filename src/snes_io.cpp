#include "snes_io.hpp"
#include "color_space.hpp"
#include "console_color.hpp"
#include "dither.hpp"
#include "quantize.hpp"

#include <algorithm>
#include <array>
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
        for (auto b : a) { h ^= b; h *= 0x100000001B3ULL; }
        return static_cast<std::size_t>(h);
    }
};

struct Slot {
    Pattern64 pattern{};
    std::vector<std::size_t> cell_indices;  // tilemap cells using this slot
    bool alive = true;
};

} // namespace

Result<Mode7PackResult> pack_snes_mode7_frame(
    std::span<const std::uint8_t> pixels,
    std::size_t width, std::size_t height,
    std::function<float(std::span<const std::uint8_t> a,
                        std::span<const std::uint8_t> b)> distance,
    ProgressCb on_progress,
    float progress_lo, float progress_hi) {

    auto report = [&](float local, std::string_view stage) {
        if (on_progress)
            on_progress(progress_lo + (progress_hi - progress_lo) *
                        std::clamp(local, 0.0f, 1.0f), stage);
    };

    if (pixels.size() != width * height) {
        return std::unexpected{Error{ErrorCode::invalid_png,
            "pack_snes_mode7_frame: pixels size != width × height"}};
    }
    constexpr std::size_t kTile = 8;
    constexpr std::size_t kMaxTiles = 256;
    constexpr std::size_t kVirtualSide = 128;  // 128×128 tilemap canvas
    auto tiles_x = (width  + kTile - 1) / kTile;
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
                    p[row * kTile + col] =
                        (px < width && py < height)
                            ? pixels[py * width + px] : 0;
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

    // 3. Merge if over budget. Walks all alive pairs in order of
    //    increasing perceptual distance, collapsing closest pairs first.
    if (slots.size() > kMaxTiles) {
        std::vector<std::size_t> alive;
        alive.reserve(slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i) alive.push_back(i);
        auto merges_needed = alive.size() - kMaxTiles;

        struct Pair { std::size_t a, b; float distance; };
        auto num_pairs = alive.size() * (alive.size() - 1) / 2;
        std::vector<Pair> pairs;
        pairs.reserve(num_pairs);
        // Pairwise distance is the dominant cost when content has many
        // unique tiles (O(n² × 64)). Report every ~32 outer rows so the
        // bar moves smoothly without burning syscall overhead.
        report(0.0f, "merging tiles");
        for (std::size_t i = 0; i < alive.size(); ++i) {
            for (std::size_t j = i + 1; j < alive.size(); ++j) {
                pairs.push_back({alive[i], alive[j],
                    distance(slots[alive[i]].pattern,
                              slots[alive[j]].pattern)});
            }
            if (alive.size() > 0 && (i & 31) == 31) {
                // Distance pass progresses ~95% to sort+merge ~5%.
                report(0.95f * static_cast<float>(i + 1) /
                       static_cast<float>(alive.size()),
                       "merging tiles");
            }
        }
        report(0.96f, "sorting pairs");
        std::ranges::sort(pairs, {}, &Pair::distance);
        report(0.98f, "merging tiles");

        std::vector<bool> is_alive(slots.size(), true);
        std::size_t merges_done = 0;
        for (auto& p : pairs) {
            if (merges_done >= merges_needed) break;
            if (!is_alive[p.a] || !is_alive[p.b]) continue;
            // Keep the slot with more cells; reassign discard's cells
            // to keep, then mark discard dead.
            auto keep = p.a, discard = p.b;
            if (slots[keep].cell_indices.size() <
                slots[discard].cell_indices.size())
                std::swap(keep, discard);
            for (auto ci : slots[discard].cell_indices) {
                cell_to_slot[ci] = keep;
                slots[keep].cell_indices.push_back(ci);
            }
            slots[discard].cell_indices.clear();
            slots[discard].alive = false;
            is_alive[discard] = false;
            ++merges_done;
        }
        res.merges_done = merges_done;
    }

    // 4. Compact the alive slots into the output tile array; remap
    //    cell_to_slot through the compaction.
    std::vector<std::size_t> slot_to_tile(slots.size(),
                                            static_cast<std::size_t>(-1));
    res.tile_bytes.reserve(kMaxTiles * 64);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].alive) continue;
        slot_to_tile[i] = res.tile_bytes.size() / 64;
        res.tile_bytes.insert(res.tile_bytes.end(),
                              slots[i].pattern.begin(),
                              slots[i].pattern.end());
    }
    res.unique_after_merge = res.tile_bytes.size() / 64;

    // 5. Build the 128×128 tilemap. Live cells reference real tiles;
    //    the rest point to tile 0 so the runtime can fill with backdrop.
    res.tilemap.assign(kVirtualSide * kVirtualSide, 0);
    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            auto cell_idx = ty * tiles_x + tx;
            auto tile_idx = slot_to_tile[cell_to_slot[cell_idx]];
            res.tilemap[ty * kVirtualSide + tx] =
                static_cast<std::uint8_t>(tile_idx);
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

Result<Mode7EncodedFrame> encode_snes_mode7(
    const Image& source, bool direct_color,
    const dither::Settings& dither_settings, int palette_diversity,
    ProgressCb on_progress) {

    // Stage budgets (sum to 1.0). The Lloyd phase dominates total
    // wall-clock for content with > 256 unique tiles, so it gets the
    // lion's share. Numbers tuned empirically on photo+logo content.
    constexpr float kStageQuant   = 0.05f;  // 0% → 5%
    constexpr float kStagePack    = 0.20f;  // 5% → 25%
    constexpr float kStageLloyd   = 0.70f;  // 25% → 95%
    constexpr float kStageRender  = 0.05f;  // 95% → 100%
    auto report = [&](float frac, std::string_view stage) {
        if (on_progress)
            on_progress(std::clamp(frac, 0.0f, 1.0f), stage);
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
    //   ED runs per-8×8-tile on a 24×24 mirror-padded buffer (centre =
    //   source tile, 8 surrounding blocks = h/v/hv-flips of the centre).
    //   The 24×24 ED's centre block is the steady-state response to the
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
                        (void)sy_l; (void)sx_l;
                    }
                }
            }
        }
    };
    (void)fill_mirror_block;  // inlined per-cell below for clarity

    if (!direct_color) {
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
                                std::size_t sx_l =
                                    (bx == 1) ? lx : (kTS - 1 - lx);
                                std::size_t sx = std::min(
                                    tx * kTS + sx_l, w - 1);
                                std::size_t sy = std::min(
                                    ty * kTS + sy_l, h - 1);
                                block[bx * kTS + lx, by * kTS + ly] =
                                    source[sx, sy];
                            }
                        }
                    }
                }
                std::fill(block_byte.begin(), block_byte.end(),
                          std::uint8_t{0});
                out.quant_error += dither::diffuse_raw_buffer(
                    block, dither_settings,
                    [&](const color_space::OKLab& target,
                        std::size_t bxp, std::size_t byp) -> dither::PickResult {
                        float bd = std::numeric_limits<float>::max();
                        std::size_t bi = 0;
                        for (std::size_t i = 0; i < pal_lab_pre.size(); ++i) {
                            const auto& cl = pal_lab_pre[i];
                            float dL = target.L - cl.L;
                            float da = target.a - cl.a;
                            float db = target.b - cl.b;
                            float d = color_space::fma_dist_sq(dL, da, db);
                            if (d < bd) { bd = d; bi = i; }
                        }
                        block_byte[byp * k3T + bxp] =
                            static_cast<std::uint8_t>(bi);
                        return {pal_lab_pre[bi], 0.5f};
                    });
                for (std::size_t ly = 0; ly < kTS; ++ly) {
                    for (std::size_t lx = 0; lx < kTS; ++lx) {
                        auto x = tx * kTS + lx;
                        auto y = ty * kTS + ly;
                        if (x >= w || y >= h) continue;
                        chunky[y * w + x] =
                            block_byte[(kTS + ly) * k3T + (kTS + lx)];
                    }
                }
            }
        }
        report(kStageQuant, "quantising");
    } else {
        // Mode 7 Direct: pixel byte = BBGGGRRR (3+3+2 = 256 colours).
        // The tilemap carries no palette field per cell, so the
        // 2048-colour gamut documented for Modes 3/4 isn't reachable.
        // Quantise the dither target to the SAME 3-3-2 grid the byte
        // can actually store — using rgb443_quantize (4-4-3) here was
        // the source of the colour artefacts: the picker thought it
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
                                std::size_t sx_l =
                                    (bx == 1) ? lx : (kTS - 1 - lx);
                                std::size_t sx = std::min(
                                    tx * kTS + sx_l, w - 1);
                                std::size_t sy = std::min(
                                    ty * kTS + sy_l, h - 1);
                                block[bx * kTS + lx, by * kTS + ly] =
                                    source[sx, sy];
                            }
                        }
                    }
                }
                std::fill(block_byte.begin(), block_byte.end(),
                          std::uint8_t{0});
                out.quant_error += dither::diffuse_raw_buffer(
                    block, dither_settings,
                    [&](const color_space::OKLab& target,
                        std::size_t bxp, std::size_t byp) -> dither::PickResult {
                        auto snapped = console_color::rgb332_quantize(
                            color_space::oklab_to_linear(target));
                        auto srgb = color_space::linear_to_srgb(snapped);
                        int r3 = console_color::quantise_srgb_to_int(srgb.r, 3);
                        int g3 = console_color::quantise_srgb_to_int(srgb.g, 3);
                        int b2 = console_color::quantise_srgb_to_int(srgb.b, 2);
                        block_byte[byp * k3T + bxp] = static_cast<std::uint8_t>(
                            (b2 << 6) | (g3 << 3) | r3);
                        return {color_space::linear_to_oklab(snapped), 0.5f};
                    });
                for (std::size_t ly = 0; ly < kTS; ++ly) {
                    for (std::size_t lx = 0; lx < kTS; ++lx) {
                        auto x = tx * kTS + lx;
                        auto y = ty * kTS + ly;
                        if (x >= w || y >= h) continue;
                        chunky[y * w + x] =
                            block_byte[(kTS + ly) * k3T + (kTS + lx)];
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
    // Decode BBGGGRRR pixel byte to linear RGB using the SAME bit-
    // replication path that rgb332_quantize uses to encode it. Plain
    // division (r3/7, g3/7, b2/3) drifts from the quantiser by ~2 sRGB
    // units per channel and is what made the dither distance metric
    // disagree with the actual displayed colour.
    // Direct-colour byte (BBGGGRRR) → OKLab cache. Only 256 possible
    // byte values, but unpack_direct gets called per-pixel-per-tile-
    // pair inside distance_fn (called O(N²) by pack_snes_mode7_frame's
    // greedy merger). AMDuProf showed linear_to_oklab at 33 % of CPU
    // for snes-mode7-direct, dominated by these per-call cbrt+pow
    // calls — the LUT collapses that to a 256-entry array load.
    std::array<color_space::OKLab, 256> direct_lab_lut{};
    for (int b = 0; b < 256; ++b) {
        int r3 = (b >> 0) & 0x07;
        int g3 = (b >> 3) & 0x07;
        int b2 = (b >> 6) & 0x03;
        Color3f srgb{
            console_color::quantise_srgb_to_bits(
                static_cast<float>(r3) / 7.0f, 3),
            console_color::quantise_srgb_to_bits(
                static_cast<float>(g3) / 7.0f, 3),
            console_color::quantise_srgb_to_bits(
                static_cast<float>(b2) / 3.0f, 2),
        };
        direct_lab_lut[static_cast<std::size_t>(b)] =
            color_space::linear_to_oklab(
                color_space::srgb_to_linear(srgb));
    }
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
    auto blur_3x3 = [](std::span<const color_space::OKLab> in,
                       std::span<color_space::OKLab> dst) {
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
                tmp[r * 8 + c] = {(a.L + b.L + d.L) / 3.0f,
                                  (a.a + b.a + d.a) / 3.0f,
                                  (a.b + b.b + d.b) / 3.0f};
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
                dst[r * 8 + c] = {(a.L + b.L + d.L) / 3.0f,
                                  (a.a + b.a + d.a) / 3.0f,
                                  (a.b + b.b + d.b) / 3.0f};
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
        // α = 0.4 (60% per-pixel, 40% coarse). Higher α favours
        // structural matches over fine-detail accuracy; lower keeps
        // edges sharp at the cost of texture blending. 0.4 chosen
        // empirically to land roughly midway on photo content.
        return 0.6f * per_pixel + 0.4f * coarse;
    };
    auto packed = pack_snes_mode7_frame(chunky, w, h, distance_fn,
        on_progress, kStageQuant, kStageQuant + kStagePack);
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
    //      OKLab MEAN of all cells now assigned to it (then snap back
    //      to a valid byte: palette index for 256 mode, BBGGGRRR for
    //      Direct). Clusters of 1 cell are left untouched (mean of one
    //      sample equals the sample).
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
    auto lab_to_byte = [&](color_space::OKLab lab) -> std::uint8_t {
        if (direct_color) {
            // Re-quantise the OKLab mean to a BBGGGRRR byte (3+3+2).
            auto rgb = color_space::oklab_to_linear(lab);
            auto srgb = color_space::linear_to_srgb(rgb);
            int r3 = console_color::quantise_srgb_to_int(srgb.r, 3);
            int g3 = console_color::quantise_srgb_to_int(srgb.g, 3);
            int b2 = console_color::quantise_srgb_to_int(srgb.b, 2);
            return static_cast<std::uint8_t>((b2 << 6) | (g3 << 3) | r3);
        }
        // 256-palette: nearest entry in OKLab space.
        std::size_t best = 0;
        float best_d = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < pal_lab.size(); ++i) {
            float dL = lab.L - pal_lab[i].L;
            float da = lab.a - pal_lab[i].a;
            float db = lab.b - pal_lab[i].b;
            float d = color_space::fma_dist_sq(dL, da, db);
            if (d < best_d) { best_d = d; best = i; }
        }
        return static_cast<std::uint8_t>(best);
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
        float local = static_cast<float>(lloyd_step) /
                      static_cast<float>(total_lloyd_steps);
        report(kStageQuant + kStagePack + kStageLloyd * local, stage);
    };

    for (int outer = 0; outer < kOuterIters; ++outer) {
        if (outer > 0 && !direct_color) {
            // ---- Palette retraining.
            //
            // The post-Lloyd tiles only display 256 × 64 = 16384
            // pixel "slots" — nothing else from the source image
            // matters at this point. Re-derive the 256-entry palette
            // via median-cut on JUST those displayed colours so the
            // palette anchors land where they're actually used. Then
            // re-snap every tile byte to the closest entry in the
            // new palette and let Lloyd re-converge with the new
            // pal_lab table.
            std::vector<Color3f> displayed;
            displayed.reserve(num_tiles * 64);
            for (auto byte : tiles) displayed.push_back(out.palette[byte]);

            Image disp_img(displayed.size(), 1, std::move(displayed));
            auto new_pal = quantize::quantize(
                disp_img, 256, quantize::Algorithm::median_cut,
                palette_diversity);
            if (new_pal) {
                std::vector<Color3f> new_palette = new_pal->colors;
                for (auto& c : new_palette)
                    c = console_color::bgr555_quantize(c);

                // Build new pal_lab.
                std::vector<color_space::OKLab> new_pal_lab(
                    new_palette.size());
                for (std::size_t i = 0; i < new_palette.size(); ++i)
                    new_pal_lab[i] = color_space::linear_to_oklab(
                        new_palette[i]);

                // Remap a byte from old-palette index → new-palette
                // index by finding the new entry nearest the byte's
                // OLD displayed colour.
                auto remap = [&](std::uint8_t b) -> std::uint8_t {
                    auto cur = pal_lab[b];
                    std::size_t best = 0;
                    float best_d = std::numeric_limits<float>::max();
                    for (std::size_t k = 0; k < new_pal_lab.size(); ++k) {
                        float dL = cur.L - new_pal_lab[k].L;
                        float da = cur.a - new_pal_lab[k].a;
                        float db = cur.b - new_pal_lab[k].b;
                        float d = color_space::fma_dist_sq(dL, da, db);
                        if (d < best_d) { best_d = d; best = k; }
                    }
                    return static_cast<std::uint8_t>(best);
                };
                // Both the tile bytes AND the chunky source need to
                // be remapped — the next Lloyd round looks up cell
                // patterns in the chunky buffer and compares against
                // tile patterns under the new pal_lab. Without
                // remapping chunky, the distance metric reads stale
                // colours and PSNR collapses.
                for (auto& byte : tiles) byte = remap(byte);
                for (auto& byte : chunky) byte = remap(byte);

                out.palette = std::move(new_palette);
                pal_lab = std::move(new_pal_lab);
            }
        }

    for (int iter = 0; iter < kRefineIters; ++iter) {
        lloyd_progress("Lloyd refinement");
        // ---- 2b.a: synthesise centroids.
        // Per-tile, per-position OKLab accumulators. Indexed
        // [tile_idx*64 + pos].
        std::vector<color_space::OKLab> sums(num_tiles * 64, {0,0,0});
        std::vector<std::size_t> counts(num_tiles, 0);

        for (std::size_t ty = 0; ty < tiles_y; ++ty) {
            for (std::size_t tx = 0; tx < tiles_x; ++tx) {
                auto t = tilemap[ty * kVirtualSide + tx];
                ++counts[t];
                for (std::size_t row = 0; row < kTile; ++row) {
                    for (std::size_t col = 0; col < kTile; ++col) {
                        auto px = tx * kTile + col;
                        auto py = ty * kTile + row;
                        if (px >= w || py >= h) continue;
                        auto byte = chunky[py * w + px];
                        auto lab = byte_to_lab(byte);
                        auto& acc = sums[t * 64 + row * kTile + col];
                        acc.L += lab.L; acc.a += lab.a; acc.b += lab.b;
                    }
                }
            }
        }

        // Build new tile bytes from per-position means; clusters of 1
        // cell are left exactly equal to their original (mean of a
        // single sample IS the sample).
        std::vector<std::uint8_t> new_tiles = tiles;
        for (std::size_t t = 0; t < num_tiles; ++t) {
            if (counts[t] <= 1) continue;
            float n = static_cast<float>(counts[t]);
            for (std::size_t pos = 0; pos < 64; ++pos) {
                auto& s = sums[t * 64 + pos];
                color_space::OKLab mean{s.L / n, s.a / n, s.b / n};
                new_tiles[t * 64 + pos] = lab_to_byte(mean);
            }
        }
        tiles = std::move(new_tiles);

        // ---- 2b.b: reassign each cell to nearest centroid.
        // ---- 2b.b: reassign cells to nearest centroid + track per-cell
        //   error so we can split overloaded clusters in step 2b.c.
        bool changed = false;
        std::array<std::uint8_t, 64> cell_pat{};
        // Parallel arrays indexed by (ty * tiles_x + tx) — the live area.
        std::vector<float> cell_error(tiles_y * tiles_x, 0.0f);
        std::vector<std::size_t> cell_assignment(tiles_y * tiles_x, 0);
        for (std::size_t ty = 0; ty < tiles_y; ++ty) {
            for (std::size_t tx = 0; tx < tiles_x; ++tx) {
                for (std::size_t row = 0; row < kTile; ++row) {
                    for (std::size_t col = 0; col < kTile; ++col) {
                        auto px = tx * kTile + col;
                        auto py = ty * kTile + row;
                        cell_pat[row * kTile + col] =
                            (px < w && py < h) ? chunky[py * w + px] : 0;
                    }
                }
                std::size_t best = 0;
                float best_d = std::numeric_limits<float>::max();
                std::span<const std::uint8_t> cell_span(cell_pat);
                for (std::size_t t = 0; t < num_tiles; ++t) {
                    std::span<const std::uint8_t> tile_span(
                        tiles.data() + t * 64, 64);
                    float d = distance_fn(cell_span, tile_span);
                    if (d < best_d) { best_d = d; best = t; }
                }
                auto cell_lin = ty * tiles_x + tx;
                cell_error[cell_lin] = best_d;
                cell_assignment[cell_lin] = best;
                auto& cell = tilemap[ty * kVirtualSide + tx];
                if (cell != best) {
                    cell = static_cast<std::uint8_t>(best);
                    changed = true;
                }
            }
        }

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
                    tiles[victim * 64 + row * kTile + col] =
                        (px < w && py < h) ? chunky[py * w + px] : 0;
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
            for (std::size_t i = 0; i < num_tiles; ++i) sorted_by_count[i] = i;
            std::sort(sorted_by_count.begin(), sorted_by_count.end(),
                [&](std::size_t a, std::size_t b) {
                    return new_counts[a] < new_counts[b];
                });
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
                std::span<const std::uint8_t> v_span(
                    tiles.data() + victim * 64, 64);
                bool redundant = false;
                for (std::size_t k = 0; k < num_tiles; ++k) {
                    if (k == victim) continue;
                    std::span<const std::uint8_t> k_span(
                        tiles.data() + k * 64, 64);
                    if (distance_fn(v_span, k_span) < eps) {
                        redundant = true; break;
                    }
                }
                if (redundant) {
                    if (split_into(victim)) { changed = true; break; }
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
    report(kStageQuant + kStagePack + kStageLloyd, "rendering preview");

    // ---- Step 3: build packed bytes (tilemap + tile data) AND re-render
    //   the preview from the post-merge tilemap+tiles+palette so the
    //   user sees what real hardware would actually display.
    out.packed_bytes.reserve(packed->tilemap.size() + packed->tile_bytes.size());
    out.packed_bytes.insert(out.packed_bytes.end(),
                            packed->tilemap.begin(), packed->tilemap.end());
    out.packed_bytes.insert(out.packed_bytes.end(),
                            packed->tile_bytes.begin(),
                            packed->tile_bytes.end());

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
                        int r3 = (byte >> 0) & 0x07;
                        int g3 = (byte >> 3) & 0x07;
                        int b2 = (byte >> 6) & 0x03;
                        Color3f srgb{
                            console_color::quantise_srgb_to_bits(
                                static_cast<float>(r3) / 7.0f, 3),
                            console_color::quantise_srgb_to_bits(
                                static_cast<float>(g3) / 7.0f, 3),
                            console_color::quantise_srgb_to_bits(
                                static_cast<float>(b2) / 3.0f, 2),
                        };
                        rgb = color_space::srgb_to_linear(srgb);
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

} // namespace png2amiga::snes_io
