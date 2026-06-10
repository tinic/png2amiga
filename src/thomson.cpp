#include "thomson.hpp"

#include "palette.hpp"
#include "pipeline.hpp"
#include "quantize.hpp"
#include "ssimulacra2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <numeric>
#include <random>
#include <span>

namespace png2amiga::thomson {

namespace {

using color_space::OKLab;

// Build the linear-RGB + OKLab views of a Thomson palette (list of 4-bit
// channel-index triples).
struct PalView {
    std::vector<Color3f> lin;
    std::vector<OKLab> lab;
};

PalView make_view(const std::vector<PaletteEntry>& pal) {
    PalView v;
    v.lin.reserve(pal.size());
    v.lab.reserve(pal.size());
    for (auto& e : pal) {
        auto lin = color_space::srgb_hex_to_linear(palette::thomson_rgb_hex(e.r, e.g, e.b));
        v.lin.push_back(lin);
        v.lab.push_back(color_space::linear_to_oklab(lin));
    }
    return v;
}

// TO7/70 fixed palette as PaletteEntry list.
std::vector<PaletteEntry> to770_palette() {
    std::vector<PaletteEntry> p;
    p.reserve(16);
    for (auto& e : palette::kThomsonTo770Idx)
        p.push_back({e[0], e[1], e[2]});
    return p;
}

// Snap a linear color to the nearest TO8 palette entry (per-channel 4-bit
// index into the intens[] gamma LUT).
PaletteEntry snap_to8(const Color3f& lin) {
    auto to8 = [](float v) {
        return static_cast<int>(
            std::lround(std::clamp(color_space::linear_to_srgb(v), 0.0f, 1.0f) * 255.0f));
    };
    return {palette::thomson_channel_index(to8(lin.r)),
            palette::thomson_channel_index(to8(lin.g)),
            palette::thomson_channel_index(to8(lin.b))};
}

// Quantize to N colors (continuous), then snap each color's channels to the
// nearest intens[] level → 4-bit (r,g,b) per color. The intens[] grid is
// coarse at the dark end (0 → 96 → 124), so DISTINCT continuous centroids
// can snap onto the same entry — a dark-heavy image (or --native-par pad
// bars) then wastes several of the 16 slots on duplicate blacks/greys.
// Base round: ALL distinct entries from the n-color quantize (the image's
// principal clusters — never displaced). Top-up rounds: the freed slots
// are refilled from progressively finer quantizes (2n, 4n, …) with
// entries not already present. Only the top-up is exposed to median-cut's
// arbitrary tree order — collecting the FIRST n distinct of a fine
// quantize instead loses principal bright/dark clusters wholesale (tried:
// 3macao S2 −20.7 → −62.0).
std::vector<PaletteEntry> quantize_to8(const Image& image, std::size_t n) {
    std::vector<PaletteEntry> out;
    for (std::size_t k = n; k <= 256 && out.size() < n; k *= 2) {
        auto pal = quantize::quantize(image, k, quantize::Algorithm::median_cut, 0.0f);
        if (!pal) break;
        std::size_t before = out.size();
        for (auto& c : pal->colors) {
            auto e = snap_to8(c);
            if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
            if (out.size() == n) break;
        }
        if (out.size() == before) break;  // snapped gamut saturated
    }
    while (out.size() < n)
        out.push_back({0, 0, 0});
    return out;
}

// 3×3 binomial blur on a flat row-major OKLab buffer, image-edge replicate
// padding. Used by the forme-couleur cell scorer so per-cell scoring reads a
// globally-coherent blurred target (same model as c64.cpp's global_blur_3x3).
std::vector<OKLab> global_blur_3x3(std::span<const OKLab> src, std::size_t W, std::size_t H) {
    static constexpr std::array<std::array<float, 3>, 3> k = {{
        {1.0f / 16, 2.0f / 16, 1.0f / 16},
        {2.0f / 16, 4.0f / 16, 2.0f / 16},
        {1.0f / 16, 2.0f / 16, 1.0f / 16},
    }};
    std::vector<OKLab> out(W * H);
    for (std::size_t y = 0; y < H; ++y) {
        for (std::size_t x = 0; x < W; ++x) {
            OKLab acc{0, 0, 0};
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(H) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(W) - 1);
                    float w =
                        k[static_cast<std::size_t>(dy + 1)][static_cast<std::size_t>(dx + 1)];
                    const auto& s =
                        src[static_cast<std::size_t>(ny) * W + static_cast<std::size_t>(nx)];
                    acc.L += w * s.L;
                    acc.a += w * s.a;
                    acc.b += w * s.b;
                }
            }
            out[y * W + x] = acc;
        }
    }
    return out;
}

// Accumulate the OKLab² error between two equally-sized images.
float oklab_error(const Image& a, const Image& b) {
    auto pa = a.pixels();
    auto pb = b.pixels();
    std::size_t n = std::min(pa.size(), pb.size());
    float e = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
        e += color_space::perceptual_distance_sq(pa[i], pb[i]);
    return e;
}

// ---------------------------------------------------------------------------
// forme-couleur (320×200, 8×1 attribute cells, 2 colors per cell)
// ---------------------------------------------------------------------------
//
// Cell = 8 pixels wide × 1 tall. Pick the best 2-of-palette pair per cell by
// brute force on the blurred target (dither-aware segment scoring + neighbor
// coherence — see the comment at the pair loop), then run the central
// error-diffusion driver picking 0/1 within each cell's pair.
Result<EncodeResult> encode_formecouleur(const Image& image,
                                         const std::vector<PaletteEntry>& palette_entries,
                                         const dither::Settings& settings,
                                         const FormeCouleurParams& fc) {
    constexpr std::size_t W = 320;
    constexpr std::size_t H = 200;
    constexpr std::size_t kCellW = 8;
    constexpr std::size_t kCols = W / kCellW;  // 40
    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("thomson forme-couleur: expected {}x{}, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    auto view = make_view(palette_entries);
    const std::size_t N = view.lab.size();  // 16

    std::vector<OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);

    EncodeResult res;
    res.rendered = Image(W, H);
    res.page_a.assign(W / 8 * H, 0);  // couleur byte per 8×1 cell: 40*200
    res.page_b.assign(W / 8 * H, 0);  // shape byte per 8×1 cell

    // Per-cell color pair (background c0, foreground c1).
    std::vector<std::array<std::uint8_t, 2>> cell_pair(kCols * H);

    // Brute-force the best pair per cell on the blurred target. With a dither
    // method the pair can MIX: score by distance to the OKLab segment between
    // the two colors plus a mixing-noise penalty λ·t(1−t)·|a−b|² (variance of
    // a Bernoulli mix — keeps far-apart pairs from winning on mean alone and
    // showing up as checkerboard noise). The OKLab segment (not the linear-
    // RGB mix curve) is the right model here because the ED pass diffuses
    // error in OKLab, so realized duty cycles average to the target in OKLab
    // — scoring against linear-RGB mixes was tried and posterizes badly.
    // Segment distance + projection use a chroma-weighted inner product (a,b
    // axes × kPairChromaWeight): every TO7 non-black color is bright (L ≥
    // 0.45), so dark targets must pair with black and all such pairs share a
    // big ΔL — the penalty term then dwarfs unweighted chroma mismatch and
    // hue degenerates to a coin flip (teal water rendered pink). Without
    // dither, score endpoint-min (static pick, no mixing possible). 136
    // pairs × 8 px per cell is cheap. An 8-sample FS-histogram top-2 (the
    // c64 8×8-cell approach) is far too noisy at 8×1 — adjacent cells flip
    // pairs randomly → horizontal tearing.
    //
    // The three weights live in FormeCouleurParams (defaults tuned on
    // Kodak-24); --best sweeps a grid around them per image.
    const bool mixing = settings.method != dither::Method::none;
    const float mix_noise_lambda = fc.mix_noise_lambda;
    const float cw = fc.chroma_weight;
    // Coherence: discount already-decided neighbor cells' pairs so smooth
    // regions keep one pair instead of flipping between near-tied pairs
    // cell to cell (patchy seams).
    const float coherence_bonus = fc.coherence_bonus;
    auto blurred = global_blur_3x3(std::span<const OKLab>(src_lab), W, H);

    // decide_pair scores all 136 pairs for one cell against the blurred
    // cell target shifted by `delta` (the ED feedback at cell entry; zero
    // for the static pre-pass) and commits the winner. Coherence reads
    // whichever of the three potential predecessors (same-row left/right,
    // above) are already decided — in serpentine ED order the in-scan
    // predecessor alternates sides.
    std::vector<std::uint8_t> decided(kCols * H, 0);
    auto decide_pair = [&](std::size_t cx, std::size_t cy, const OKLab& delta) {
        std::array<OKLab, kCellW> cell{};
        for (std::size_t px = 0; px < kCellW; ++px) {
            const auto& s = blurred[cy * W + cx * kCellW + px];
            cell[px] = {s.L + delta.L, s.a + delta.a, s.b + delta.b};
        }
        std::size_t row = cy * kCols;
        const std::array<std::uint8_t, 2>* nb[3] = {
            (cx > 0 && decided[row + cx - 1]) ? &cell_pair[row + cx - 1] : nullptr,
            (cx + 1 < kCols && decided[row + cx + 1]) ? &cell_pair[row + cx + 1] : nullptr,
            (cy > 0 && decided[row - kCols + cx]) ? &cell_pair[row - kCols + cx] : nullptr,
        };
        float best_err = std::numeric_limits<float>::infinity();
        std::array<std::uint8_t, 2> best{0, 0};
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t j = i; j < N; ++j) {
                const auto& a = view.lab[i];
                const auto& b = view.lab[j];
                float dL = b.L - a.L, da = b.a - a.a, db = b.b - a.b;
                float seg_sq = color_space::fma_dist_sq(dL, da, db);
                float seg_sq_w = dL * dL + cw * (da * da + db * db);
                float total = 0.0f;
                for (std::size_t p = 0; p < kCellW; ++p) {
                    const auto& t = cell[p];
                    if (mixing && seg_sq > 0.0f) {
                        float u = ((t.L - a.L) * dL + cw * (t.a - a.a) * da +
                                   cw * (t.b - a.b) * db) /
                                  seg_sq_w;
                        u = std::clamp(u, 0.0f, 1.0f);
                        float eL = t.L - (a.L + u * dL);
                        float ea = t.a - (a.a + u * da);
                        float eb = t.b - (a.b + u * db);
                        total += eL * eL + cw * (ea * ea + eb * eb) +
                                 mix_noise_lambda * u * (1.0f - u) * seg_sq;
                    } else {
                        float ea = color_space::fma_dist_sq(t, a);
                        float eb = color_space::fma_dist_sq(t, b);
                        total += std::min(ea, eb);
                    }
                }
                std::array<std::uint8_t, 2> cand{static_cast<std::uint8_t>(i),
                                                 static_cast<std::uint8_t>(j)};
                for (auto* n : nb)
                    if (n && cand == *n) total -= coherence_bonus;
                if (total < best_err) {
                    best_err = total;
                    best = cand;
                }
            }
        }
        cell_pair[row + cx] = best;
        decided[row + cx] = 1;
    };

    // Static pre-pass (no dither): raster order, no feedback — left + above
    // neighbors are the decided ones, matching the original scan-order
    // coherence.
    if (!mixing) {
        for (std::size_t cy = 0; cy < H; ++cy)
            for (std::size_t cx = 0; cx < kCols; ++cx)
                decide_pair(cx, cy, OKLab{0, 0, 0});
    }

    // Dither pass: pick index 0/1 within each cell's pair via the central
    // error-diffusion driver. With a dither method the pair itself is
    // decided lazily at cell ENTRY (first pixel the serpentine scan visits)
    // from the blurred cell target shifted by the ED feedback delta — the
    // accumulated error the kernel has pushed into this pixel. A static
    // pre-pass can't see that drift, so it picks pairs for a target the
    // dither pass is no longer rendering.
    // The raw entry delta on a ~50% duty cell is up to half a quantization
    // step of dither PHASE error — chasing it makes pair choice oscillate
    // (flat textures explode into random-hue confetti). Damp + clamp so
    // only consistent low-frequency drift steers the pair. The scale is a
    // FormeCouleurParams axis (--best sweeps {0, 0.5}): regular textures
    // like examples/brick.png churn between near-tied pairs under any
    // feedback and want 0.
    static constexpr float kFeedbackClamp = 0.04f;
    const float feedback_scale = fc.feedback_scale;
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cell_idx = y * kCols + (x / kCellW);
        if (!decided[cell_idx]) {
            const auto& s = src_lab[y * W + x];
            auto damp = [&](float v) {
                return std::clamp(v * feedback_scale, -kFeedbackClamp, kFeedbackClamp);
            };
            decide_pair(x / kCellW,
                        y,
                        OKLab{damp(target.L - s.L), damp(target.a - s.a), damp(target.b - s.b)});
        }
        const auto& pair = cell_pair[cell_idx];
        std::array<OKLab, 2> cp{view.lab[pair[0]], view.lab[pair[1]]};
        std::size_t chosen_idx = 0;
        OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target, std::span<const OKLab>(cp), x, y, settings.strength,
            /*k_min=*/0, chosen_idx, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_idx);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    // Pack pages. pageB = shape (bit7 leftmost, set => fg/c1). pageA = color
    // byte in TO-series Decode320x16 format: bits0-2 = bg low3, bits3-5 = fg
    // low3, bit6 = NOT(fg bit3), bit7 = NOT(bg bit3).
    for (std::size_t cy = 0; cy < H; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            std::size_t cell = cy * kCols + cx;
            const auto& pair = cell_pair[cell];
            std::uint8_t c0 = pair[0];  // bg
            std::uint8_t c1 = pair[1];  // fg
            std::uint8_t shape = 0;
            for (std::size_t px = 0; px < kCellW; ++px) {
                std::size_t x = cx * kCellW + px;
                std::uint8_t q = static_cast<std::uint8_t>(indices[cy * W + x] & 0x1);
                shape = static_cast<std::uint8_t>((shape << 1) | q);
                res.rendered[x, cy] = view.lin[q ? c1 : c0];
            }
            res.page_b[cell] = shape;
            std::uint8_t color = static_cast<std::uint8_t>(
                (c0 & 0x07) | ((c1 & 0x07) << 3) |
                (((~c1) & 0x08) << 3) |    // bit6 = NOT(fg bit3)
                (((~c0) & 0x08) << 4));    // bit7 = NOT(bg bit3)
            res.page_a[cell] = color;
        }
    }

    res.total_error = oklab_error(image, res.rendered);
    return res;
}

// ---------------------------------------------------------------------------
// bitmap modes (160×16 4bpp, 320×4 2bpp, 640×2 1bpp). Quantize → snap → dither
// → pack. Mirrors the GBA mode4 / vga chunky pattern but with intens[] snap.
// ---------------------------------------------------------------------------
Result<EncodeResult> encode_bitmap(const Image& image,
                                   amiga::Mode mode,
                                   const std::vector<PaletteEntry>& palette_entries,
                                   const dither::Settings& settings) {
    auto params = amiga::get_mode_params(mode);
    std::size_t W = params.screen_width;
    std::size_t H = params.screen_height;
    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("thomson bitmap: expected {}x{}, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }
    auto view = make_view(palette_entries);

    auto dith_result = dither::apply(image, view.lin, settings);

    EncodeResult res;
    res.rendered = Image(W, H);
    for (std::size_t i = 0; i < dith_result.indices.size(); ++i)
        res.rendered.pixels()[i] = view.lin[dith_result.indices[i]];

    const auto& idx = dith_result.indices;
    if (mode == amiga::Mode::thomson_to8_160x16) {
        // 16-bit word = pageB<<8 | pageA. 4 px per word, 4bpp:
        //   px0 = pageB[7:4], px1 = pageB[3:0], px2 = pageA[7:4], px3 = pageA[3:0]
        // 160 px / 4 = 40 words per row, 200 rows = 8000 bytes/page.
        res.page_a.assign(8000, 0);
        res.page_b.assign(8000, 0);
        for (std::size_t y = 0; y < H; ++y) {
            for (std::size_t w = 0; w < W / 4; ++w) {
                std::size_t base = y * W + w * 4;
                std::uint8_t p0 = idx[base + 0] & 0x0F;
                std::uint8_t p1 = idx[base + 1] & 0x0F;
                std::uint8_t p2 = idx[base + 2] & 0x0F;
                std::uint8_t p3 = idx[base + 3] & 0x0F;
                std::size_t m = y * (W / 4) + w;
                res.page_b[m] = static_cast<std::uint8_t>((p0 << 4) | p1);
                res.page_a[m] = static_cast<std::uint8_t>((p2 << 4) | p3);
            }
        }
    } else if (mode == amiga::Mode::thomson_to8_320x4) {
        // 2 bitplanes: pageB = plane-hi, pageA = plane-lo. bit7 leftmost.
        // pixel = (pageB bit)<<1 | (pageA bit). 320/8 = 40 bytes/row/plane.
        res.page_a.assign(8000, 0);
        res.page_b.assign(8000, 0);
        for (std::size_t y = 0; y < H; ++y) {
            for (std::size_t bx = 0; bx < W / 8; ++bx) {
                std::uint8_t lo = 0, hi = 0;
                for (std::size_t bit = 0; bit < 8; ++bit) {
                    std::uint8_t c = idx[y * W + bx * 8 + bit] & 0x03;
                    std::uint8_t shift = static_cast<std::uint8_t>(7 - bit);
                    lo = static_cast<std::uint8_t>(lo | ((c & 0x1) << shift));
                    hi = static_cast<std::uint8_t>(hi | (((c >> 1) & 0x1) << shift));
                }
                std::size_t m = y * (W / 8) + bx;
                res.page_a[m] = lo;
                res.page_b[m] = hi;
            }
        }
    } else {  // thomson_to8_640x2
        // 16-bit word = pageB<<8 | pageA: 16 px, bit15..0 left→right.
        // pageB = left 8 px, pageA = right 8 px. 640/16 = 40 words/row.
        res.page_a.assign(8000, 0);
        res.page_b.assign(8000, 0);
        for (std::size_t y = 0; y < H; ++y) {
            for (std::size_t w = 0; w < W / 16; ++w) {
                std::uint8_t hi = 0, lo = 0;  // hi = left 8 px (pageB)
                for (std::size_t bit = 0; bit < 8; ++bit) {
                    std::size_t base = y * W + w * 16;
                    std::uint8_t cl = idx[base + bit] & 0x1;
                    std::uint8_t cr = idx[base + 8 + bit] & 0x1;
                    std::uint8_t shift = static_cast<std::uint8_t>(7 - bit);
                    hi = static_cast<std::uint8_t>(hi | (cl << shift));
                    lo = static_cast<std::uint8_t>(lo | (cr << shift));
                }
                std::size_t m = y * (W / 16) + w;
                res.page_b[m] = hi;
                res.page_a[m] = lo;
            }
        }
    }

    res.palette = palette_entries;
    res.total_error = oklab_error(image, res.rendered);
    return res;
}

}  // namespace

Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings,
                            const FormeCouleurParams& fc,
                            const std::vector<PaletteEntry>* to8_palette) {
    if (amiga::is_thomson_formecouleur(mode)) {
        std::vector<PaletteEntry> pal =
            (mode == amiga::Mode::thomson_to7_320x16)
                ? to770_palette()
                : ((to8_palette && to8_palette->size() == 16) ? *to8_palette
                                                              : quantize_to8(image, 16));
        auto r = encode_formecouleur(image, pal, settings, fc);
        if (!r) return r;
        // TO7/70 has a fixed palette → no .pal emitted; TO8 carries it.
        if (mode == amiga::Mode::thomson_to8_320x16) r->palette = pal;
        return r;
    }
    if (amiga::is_thomson_bitmap(mode)) {
        std::size_t n = amiga::thomson_palette_size(mode);
        auto pal = quantize_to8(image, n);
        return encode_bitmap(image, mode, pal, settings);
    }
    return std::unexpected{Error{ErrorCode::unsupported_mode, "thomson::encode: not a Thomson mode"}};
}

namespace {

// OKLab nudge on one slot, step sizes mirrored from palette_search.cpp's
// lores GA (±0.08 L, ±0.04 a/b), snapped back to the intens[] grid.
void nudge_slot(std::vector<PaletteEntry>& pal, std::size_t k, std::mt19937& rng) {
    std::uniform_real_distribution<float> dL(-0.08f, 0.08f);
    std::uniform_real_distribution<float> dab(-0.04f, 0.04f);
    auto lin =
        color_space::srgb_hex_to_linear(palette::thomson_rgb_hex(pal[k].r, pal[k].g, pal[k].b));
    auto lab = color_space::linear_to_oklab(lin);
    lab.L = std::clamp(lab.L + dL(rng), 0.0f, 1.0f);
    lab.a = std::clamp(lab.a + dab(rng), -0.4f, 0.4f);
    lab.b = std::clamp(lab.b + dab(rng), -0.4f, 0.4f);
    pal[k] = snap_to8(color_space::oklab_to_linear(lab));
}

void mutate_pal(std::vector<PaletteEntry>& pal, std::mt19937& rng) {
    std::uniform_int_distribution<int> count_dist(1, 2);
    std::uniform_int_distribution<std::size_t> slot_dist(0, pal.size() - 1);
    int n = count_dist(rng);
    for (int i = 0; i < n; ++i)
        nudge_slot(pal, slot_dist(rng), rng);
}

// Re-roll any slot that duplicates an earlier one. A duplicate entry can
// never help (the pair scorer just sees the same color twice) and the
// snapped intens[] grid makes collisions common after crossover /
// mutation — without repair the GA happily carries 2-3 wasted slots.
void repair_duplicates(std::vector<PaletteEntry>& pal, std::mt19937& rng) {
    for (std::size_t k = 1; k < pal.size(); ++k) {
        for (int tries = 0; tries < 8; ++tries) {
            bool dup = false;
            for (std::size_t j = 0; j < k; ++j) {
                if (pal[j] == pal[k]) {
                    dup = true;
                    break;
                }
            }
            if (!dup) break;
            nudge_slot(pal, k, rng);
        }
    }
}

std::vector<PaletteEntry> crossover_pal(const std::vector<PaletteEntry>& a,
                                        const std::vector<PaletteEntry>& b,
                                        std::mt19937& rng) {
    std::vector<PaletteEntry> child(a.size());
    std::uniform_int_distribution<int> coin(0, 1);
    for (std::size_t k = 0; k < a.size(); ++k)
        child[k] = (coin(rng) != 0) ? a[k] : b[k];
    return child;
}

}  // namespace

Result<std::vector<PaletteEntry>> formecouleur_palette_search(const Image& image,
                                                              const dither::Settings& settings,
                                                              const PopSearchOptions& opts) {
    if (image.width() != 320 || image.height() != 200) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                     "formecouleur_palette_search: expected 320x200 input"}};
    }

    ssimulacra2::PrecomputedSource src_pre;
    src_pre.prepare(image.pixels(), image.width(), image.height());

    auto fitness = [&](const std::vector<PaletteEntry>& pal) -> float {
        auto r = encode_formecouleur(image, pal, settings, FormeCouleurParams{});
        if (!r) return -std::numeric_limits<float>::infinity();
        return ssimulacra2::compute(src_pre, r->rendered.pixels());
    };

    // Seed: candidate 0 = verbatim median-cut palette; remainder are
    // progressively heavier mutations of it. Deterministic rng for
    // reproducible output (same convention as palette_search.cpp).
    auto seed0 = quantize_to8(image, 16);
    std::mt19937 rng(0xa5a5);  // NOLINT(bugprone-random-generator-seed)
    std::vector<std::vector<PaletteEntry>> population;
    population.reserve(static_cast<std::size_t>(opts.pop_size));
    population.push_back(seed0);
    for (int i = 1; i < opts.pop_size; ++i) {
        auto p = seed0;
        for (int j = 0; j <= i / 8; ++j)
            mutate_pal(p, rng);
        repair_duplicates(p, rng);
        population.push_back(std::move(p));
    }

    std::vector<float> scores(population.size(), 0.0f);
    std::vector<std::uint8_t> needs_score(population.size(), 1);
    auto score_all = [&] {
        std::vector<std::size_t> todo;
        for (std::size_t i = 0; i < population.size(); ++i)
            if (needs_score[i]) todo.push_back(i);
        pipeline::parallel_for(todo.size(), [&](std::size_t k) {
            scores[todo[k]] = fitness(population[todo[k]]);
        });
        std::fill(needs_score.begin(), needs_score.end(), std::uint8_t{0});
    };

    auto report = [&](int gen, float best) {
        if (!opts.on_progress) return;
        char label[64];
        std::snprintf(label,
                      sizeof(label),
                      "pop search gen %d/%d  best=%.2f",
                      gen,
                      opts.generations,
                      static_cast<double>(best));
        opts.on_progress(static_cast<float>(gen) / static_cast<float>(opts.generations), label);
    };

    score_all();
    float prev_best = *std::max_element(scores.begin(), scores.end());
    int stale_gens = 0;
    report(0, prev_best);

    for (int gen = 1; gen <= opts.generations; ++gen) {
        std::vector<std::size_t> idx(population.size());
        std::iota(idx.begin(), idx.end(), std::size_t{0});
        std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
            return scores[a] > scores[b];
        });
        int n_keep = std::max(2, opts.pop_size / 4);
        std::vector<std::vector<PaletteEntry>> new_pop;
        std::vector<float> new_scores;
        new_pop.reserve(static_cast<std::size_t>(opts.pop_size));
        new_scores.reserve(static_cast<std::size_t>(opts.pop_size));
        for (int i = 0; i < n_keep; ++i) {
            std::size_t k = idx[static_cast<std::size_t>(i)];
            new_pop.push_back(population[k]);
            new_scores.push_back(scores[k]);
        }
        std::uniform_int_distribution<int> parent_pick(0, n_keep - 1);
        while (new_pop.size() < static_cast<std::size_t>(opts.pop_size)) {
            int role = static_cast<int>(new_pop.size()) % 4;
            if (role < 2) {
                auto& a = new_pop[static_cast<std::size_t>(parent_pick(rng))];
                auto& b = new_pop[static_cast<std::size_t>(parent_pick(rng))];
                auto child = crossover_pal(a, b, rng);
                mutate_pal(child, rng);
                repair_duplicates(child, rng);
                new_pop.push_back(std::move(child));
            } else {
                auto child = new_pop[static_cast<std::size_t>(parent_pick(rng))];
                for (int m = 0; m < 3; ++m)
                    mutate_pal(child, rng);
                repair_duplicates(child, rng);
                new_pop.push_back(std::move(child));
            }
            new_scores.push_back(0.0f);
        }
        population = std::move(new_pop);
        scores = std::move(new_scores);
        needs_score.assign(population.size(), 1);
        for (int i = 0; i < n_keep; ++i)
            needs_score[static_cast<std::size_t>(i)] = 0;
        score_all();

        float cur_best = *std::max_element(scores.begin(), scores.end());
        report(gen, cur_best);
        if (cur_best - prev_best < 0.001f) {
            if (++stale_gens >= opts.stale_limit) break;
        } else {
            stale_gens = 0;
            prev_best = cur_best;
        }
    }

    auto winner_it = std::max_element(scores.begin(), scores.end());
    return population[static_cast<std::size_t>(winner_it - scores.begin())];
}

}  // namespace png2amiga::thomson
