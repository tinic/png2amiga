#include "preprocess.hpp"
#include "color_space.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace png2amiga::preprocess {

namespace {

// Sharpen/blur in OKLab L channel.
void apply_sharpen(Image& image, float strength) {
    if (std::abs(strength) < 0.01f) return;

    auto w = image.width();
    auto h = image.height();

    std::vector<float> L(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            L[y * w + x] = color_space::linear_to_oklab(image[x, y]).L;

    if (strength > 0.0f) {
        auto L_orig = L;
        for (std::size_t y = 1; y + 1 < h; ++y) {
            for (std::size_t x = 1; x + 1 < w; ++x) {
                float blur = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        blur += L_orig[(y + static_cast<std::size_t>(dy)) * w +
                                       (x + static_cast<std::size_t>(dx))];
                blur /= 9.0f;
                L[y * w + x] += (L_orig[y * w + x] - blur) * strength;
            }
        }
    } else {
        float blur_amount = std::clamp(-strength, 0.0f, 1.0f);
        auto L_orig = L;
        std::vector<float> tmp(w * h);
        for (int p = 0; p < 3; ++p) {
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    float sum = 0.0f;
                    float count = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            auto ny = static_cast<int>(y) + dy;
                            auto nx = static_cast<int>(x) + dx;
                            if (ny >= 0 && static_cast<std::size_t>(ny) < h &&
                                nx >= 0 && static_cast<std::size_t>(nx) < w) {
                                sum += L[static_cast<std::size_t>(ny) * w +
                                         static_cast<std::size_t>(nx)];
                                count += 1.0f;
                            }
                        }
                    }
                    tmp[y * w + x] = sum / count;
                }
            }
            std::swap(L, tmp);
        }
        for (std::size_t i = 0; i < w * h; ++i)
            L[i] = L_orig[i] + (L[i] - L_orig[i]) * blur_amount;
    }

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto lab = color_space::linear_to_oklab(image[x, y]);
            lab.L = std::clamp(L[y * w + x], 0.0f, 1.0f);
            image[x, y] = color_space::oklab_to_linear(lab).clamped();
        }
    }
}

void apply_levels(Image& image, float black_point, float white_point) {
    if (black_point <= 0.0f && white_point <= 0.0f) return;

    float lo = black_point;
    float hi = 1.0f - white_point;
    if (hi <= lo) hi = lo + 0.01f;

    for (auto& pixel : image.pixels()) {
        auto lab = color_space::linear_to_oklab(pixel);
        lab.L = (lab.L - lo) / (hi - lo);
        lab.L = std::clamp(lab.L, 0.0f, 1.0f);
        pixel = color_space::oklab_to_linear(lab).clamped();
    }
}

} // namespace

void apply(Image& image, const Settings& s) {
    // 1. Gamma
    for (auto& pixel : image.pixels()) {
        if (s.gamma != 1.0f) {
            pixel.r = std::pow(std::max(pixel.r, 0.0f), s.gamma);
            pixel.g = std::pow(std::max(pixel.g, 0.0f), s.gamma);
            pixel.b = std::pow(std::max(pixel.b, 0.0f), s.gamma);
        }
    }

    // 2. Sharpen
    apply_sharpen(image, s.sharpen);

    // 3. Black/white point
    apply_levels(image, s.black_point, s.white_point);

    // 4-7. Brightness, contrast, saturation, hue in OKLab space
    float hue_rad = s.hue_shift * (std::numbers::pi_v<float> / 180.0f);
    float cos_h = std::cos(hue_rad);
    float sin_h = std::sin(hue_rad);
    bool do_hue = (std::abs(s.hue_shift) > 0.1f);

    for (auto& pixel : image.pixels()) {
        auto lab = color_space::linear_to_oklab(pixel);

        lab.L += s.brightness;
        lab.L = (lab.L - 0.5f) * s.contrast + 0.5f;
        lab.a *= s.saturation;
        lab.b *= s.saturation;

        if (do_hue) {
            float a2 = lab.a * cos_h - lab.b * sin_h;
            float b2 = lab.a * sin_h + lab.b * cos_h;
            lab.a = a2;
            lab.b = b2;
        }

        pixel = color_space::oklab_to_linear(lab).clamped();
    }
}

void match_palette_range(Image& image, const Palette& palette,
                         float percentile, float margin) {
    auto pixel_count = image.width() * image.height();
    if (pixel_count == 0 || palette.colors.empty()) return;

    float pal_L_min = 1e9f, pal_L_max = -1e9f;
    float pal_a_min = 1e9f, pal_a_max = -1e9f;
    float pal_b_min = 1e9f, pal_b_max = -1e9f;

    for (auto& c : palette.colors) {
        auto lab = color_space::linear_to_oklab(c);
        pal_L_min = std::min(pal_L_min, lab.L);
        pal_L_max = std::max(pal_L_max, lab.L);
        pal_a_min = std::min(pal_a_min, lab.a);
        pal_a_max = std::max(pal_a_max, lab.a);
        pal_b_min = std::min(pal_b_min, lab.b);
        pal_b_max = std::max(pal_b_max, lab.b);
    }

    auto apply_margin_fn = [margin](float lo, float hi) {
        float span = hi - lo;
        return std::pair{lo + span * margin, hi - span * margin};
    };

    auto [tgt_L_min, tgt_L_max] = apply_margin_fn(pal_L_min, pal_L_max);
    auto [tgt_a_min, tgt_a_max] = apply_margin_fn(pal_a_min, pal_a_max);
    auto [tgt_b_min, tgt_b_max] = apply_margin_fn(pal_b_min, pal_b_max);

    std::vector<color_space::OKLab> image_lab(pixel_count);
    std::vector<float> Ls(pixel_count), As(pixel_count), Bs(pixel_count);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto y = i / image.width();
        auto x = i % image.width();
        image_lab[i] = color_space::linear_to_oklab(image[x, y]);
        Ls[i] = image_lab[i].L;
        As[i] = image_lab[i].a;
        Bs[i] = image_lab[i].b;
    }

    auto percentile_range = [percentile](std::vector<float>& vals) {
        std::ranges::sort(vals);
        auto n = vals.size();
        auto lo_idx = static_cast<std::size_t>(
            static_cast<float>(n) * percentile);
        auto hi_idx = static_cast<std::size_t>(
            static_cast<float>(n) * (1.0f - percentile));
        lo_idx = std::min(lo_idx, n - 1);
        hi_idx = std::min(hi_idx, n - 1);
        return std::pair{vals[lo_idx], vals[hi_idx]};
    };

    auto [src_L_min, src_L_max] = percentile_range(Ls);
    auto [src_a_min, src_a_max] = percentile_range(As);
    auto [src_b_min, src_b_max] = percentile_range(Bs);

    auto remap = [](float val, float src_lo, float src_hi,
                    float dst_lo, float dst_hi) -> float {
        float src_span = src_hi - src_lo;
        if (src_span < 1e-6f) {
            return (dst_lo + dst_hi) * 0.5f;
        }
        float t = (val - src_lo) / src_span;
        return dst_lo + t * (dst_hi - dst_lo);
    };

    auto scale_around_zero = [](float val, float src_lo, float src_hi,
                                float dst_lo, float dst_hi) -> float {
        if (val >= 0.0f) {
            float s = (src_hi > 1e-6f) ? dst_hi / src_hi : 1.0f;
            return val * s;
        }
        float s = (src_lo < -1e-6f) ? dst_lo / src_lo : 1.0f;
        return val * s;
    };

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto& lab = image_lab[i];
        lab.L = remap(lab.L, src_L_min, src_L_max, tgt_L_min, tgt_L_max);
        lab.a = scale_around_zero(lab.a, src_a_min, src_a_max,
                                  tgt_a_min, tgt_a_max);
        lab.b = scale_around_zero(lab.b, src_b_min, src_b_max,
                                  tgt_b_min, tgt_b_max);

        auto y = i / image.width();
        auto x = i % image.width();
        image[x, y] = color_space::oklab_to_linear(lab).clamped();
    }
}

// Hue-preserving chroma compression onto a palette's 3D gamut hull.
// Computes c_max(L, h) — max chroma reachable by the palette's convex
// hull at lightness L and hue h — into a 64×64 LUT, then per pixel:
//   • decompose to (L, c, h)
//   • if c > c_max(L, h): scale (a, b) by c_max/c (preserves hue and L)
//   • else: pass through.
//
// LUT build: for each L bin, intersect every palette segment (i, j)
// with the L-isosurface to collect (a, b) candidate points at that L.
// For each h bin, the max chroma in direction (cos h, sin h) is the
// max projection of any candidate onto that direction. This is a max
// of a linear function over a convex set, so the convex hull of
// candidates is implicit — taking max over all candidates (whether or
// not they're hull vertices) yields the same value.
void gamut_map(Image& image, const Palette& palette) {
    if (palette.colors.size() < 2) return;
    auto pixel_count = image.width() * image.height();
    if (pixel_count == 0) return;

    std::vector<color_space::OKLab> pal_lab(palette.colors.size());
    for (std::size_t i = 0; i < palette.colors.size(); ++i)
        pal_lab[i] = color_space::linear_to_oklab(palette.colors[i]);

    constexpr int kL = 64;
    constexpr int kH = 64;
    constexpr float kPi = std::numbers::pi_v<float>;
    std::array<std::array<float, kH>, kL> c_max{};

    for (int Lb = 0; Lb < kL; ++Lb) {
        const float Lt = static_cast<float>(Lb)
                       / static_cast<float>(kL - 1);
        std::vector<std::pair<float, float>> cand;
        cand.reserve(pal_lab.size() * pal_lab.size() / 2 + pal_lab.size());
        // Palette points whose L is in this bin's bucket count
        // directly — without this, a palette point at L=0.5 and a bin
        // at L=0.5 would otherwise need a degenerate i==j segment.
        for (auto& p : pal_lab) {
            if (std::abs(p.L - Lt) < 1.0f / static_cast<float>(kL)) {
                cand.emplace_back(p.a, p.b);
            }
        }
        for (std::size_t i = 0; i < pal_lab.size(); ++i) {
            for (std::size_t j = i + 1; j < pal_lab.size(); ++j) {
                const float Li = pal_lab[i].L;
                const float Lj = pal_lab[j].L;
                if ((Li - Lt) * (Lj - Lt) > 0) continue;  // both same side
                const float dL = Lj - Li;
                if (std::abs(dL) < 1e-6f) continue;
                const float t = (Lt - Li) / dL;
                const float a = pal_lab[i].a
                              + t * (pal_lab[j].a - pal_lab[i].a);
                const float b = pal_lab[i].b
                              + t * (pal_lab[j].b - pal_lab[i].b);
                cand.emplace_back(a, b);
            }
        }
        // Origin always reachable (gray scaled to L), so c_max ≥ 0.
        cand.emplace_back(0.0f, 0.0f);

        for (int Hb = 0; Hb < kH; ++Hb) {
            const float h = (static_cast<float>(Hb)
                          / static_cast<float>(kH)) * 2.0f * kPi;
            const float ch = std::cos(h);
            const float sh = std::sin(h);
            float best = 0.0f;
            for (auto& [a, b] : cand) {
                const float p = a * ch + b * sh;
                if (p > best) best = p;
            }
            c_max[static_cast<std::size_t>(Lb)]
                 [static_cast<std::size_t>(Hb)] = best;
        }
    }

    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            auto lab = color_space::linear_to_oklab(image[x, y]);
            const float c = std::sqrt(lab.a * lab.a + lab.b * lab.b);
            if (c < 1e-6f) continue;
            float h = std::atan2(lab.b, lab.a);
            if (h < 0) h += 2.0f * kPi;

            const float Lf = std::clamp(lab.L, 0.0f, 1.0f)
                           * static_cast<float>(kL - 1);
            const float Hf = (h / (2.0f * kPi))
                           * static_cast<float>(kH);
            int L0 = std::clamp(static_cast<int>(Lf), 0, kL - 2);
            int L1 = L0 + 1;
            float Lt = Lf - static_cast<float>(L0);
            int H0 = static_cast<int>(Hf) % kH;
            int H1 = (H0 + 1) % kH;
            float Ht = Hf - std::floor(Hf);
            const float c00 = c_max[static_cast<std::size_t>(L0)]
                                   [static_cast<std::size_t>(H0)];
            const float c01 = c_max[static_cast<std::size_t>(L0)]
                                   [static_cast<std::size_t>(H1)];
            const float c10 = c_max[static_cast<std::size_t>(L1)]
                                   [static_cast<std::size_t>(H0)];
            const float c11 = c_max[static_cast<std::size_t>(L1)]
                                   [static_cast<std::size_t>(H1)];
            const float c_lim =
                (1.0f - Lt) * ((1.0f - Ht) * c00 + Ht * c01)
              +         Lt  * ((1.0f - Ht) * c10 + Ht * c11);

            if (c > c_lim) {
                const float scale = c_lim / c;
                lab.a *= scale;
                lab.b *= scale;
                image[x, y] = color_space::oklab_to_linear(lab).clamped();
            }
        }
    }
}

} // namespace png2amiga::preprocess
