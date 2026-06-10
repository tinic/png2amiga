#include "thomson.hpp"

#include "palette.hpp"
#include "quantize.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
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

// Quantize to N colors (continuous), then snap each color's channels to the
// nearest intens[] level → 4-bit (r,g,b) per color. Mirrors the TO8 gamut
// constraint (4096 colors = intens[] per channel).
std::vector<PaletteEntry> quantize_to8(const Image& image, std::size_t n) {
    auto pal = quantize::quantize(image, n, quantize::Algorithm::median_cut, 0.0f);
    std::vector<PaletteEntry> out;
    if (!pal) {
        out.resize(n, PaletteEntry{0, 0, 0});
        return out;
    }
    out.reserve(n);
    auto to8 = [](float lin) {
        return static_cast<int>(std::lround(std::clamp(color_space::linear_to_srgb(lin), 0.0f, 1.0f) *
                                            255.0f));
    };
    for (auto& c : pal->colors) {
        out.push_back({palette::thomson_channel_index(to8(c.r)),
                       palette::thomson_channel_index(to8(c.g)),
                       palette::thomson_channel_index(to8(c.b))});
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
    // Coherence: discount the left/above neighbor's already-chosen pair so
    // smooth regions keep one pair instead of flipping between near-tied
    // pairs cell to cell (patchy seams). Scan order makes both available.
    const float coherence_bonus = fc.coherence_bonus;
    auto blurred = global_blur_3x3(std::span<const OKLab>(src_lab), W, H);
    for (std::size_t cy = 0; cy < H; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            std::array<OKLab, kCellW> cell{};
            for (std::size_t px = 0; px < kCellW; ++px)
                cell[px] = blurred[cy * W + cx * kCellW + px];
            const std::array<std::uint8_t, 2>* left =
                cx > 0 ? &cell_pair[cy * kCols + cx - 1] : nullptr;
            const std::array<std::uint8_t, 2>* above =
                cy > 0 ? &cell_pair[(cy - 1) * kCols + cx] : nullptr;
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
                    if (left && cand == *left) total -= coherence_bonus;
                    if (above && cand == *above) total -= coherence_bonus;
                    if (total < best_err) {
                        best_err = total;
                        best = cand;
                    }
                }
            }
            cell_pair[cy * kCols + cx] = best;
        }
    }

    // Dither pass: pick index 0/1 within each cell's pair via the central
    // error-diffusion driver.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        const auto& pair = cell_pair[y * kCols + (x / kCellW)];
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
                            const FormeCouleurParams& fc) {
    if (amiga::is_thomson_formecouleur(mode)) {
        std::vector<PaletteEntry> pal = (mode == amiga::Mode::thomson_to7_320x16)
                                            ? to770_palette()
                                            : quantize_to8(image, 16);
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

}  // namespace png2amiga::thomson
