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
                            if (ny >= 0 && static_cast<std::size_t>(ny) < h && nx >= 0 &&
                                static_cast<std::size_t>(nx) < w) {
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

}  // namespace

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

// Hue-preserving chroma stretch onto a palette's 3D gamut hull.
//
// Two LUTs over (L_bin, h_bin), 64 × 64:
//   c_pal(L, h)  — max chroma reachable by the palette's convex hull
//                  at that lightness and hue.
//   c_src(L, h)  — 95th-percentile chroma seen in the source image at
//                  that (L, h) bin, then box-diffused so under-sampled
//                  bins still get a sensible scale.
//
// Per pixel: scale (a, b) by c_pal/c_src so the source's chroma range
// at every (L, h) slice fills the palette's reachable extent. Clip
// out-of-gamut pixels to c_pal afterwards. Hue and L are preserved.
//
// Replaces the old axis-aligned OKLab-bounding-box stretch (which
// over-stretched diagonals and rotated hues away from palette
// reachable colors). The `percentile` and `margin` params are kept
// in the signature for ABI compatibility but are unused — both
// concepts are subsumed by the per-(L, h) 95th-percentile binning
// and the palette-hull intersection respectively.
void match_palette_range(Image& image,
                         const Palette& palette,
                         float /*percentile*/,
                         float /*margin*/) {
    if (palette.colors.size() < 2) return;
    auto pixel_count = image.width() * image.height();
    if (pixel_count == 0) return;

    std::vector<color_space::OKLab> pal_lab(palette.colors.size());
    for (std::size_t i = 0; i < palette.colors.size(); ++i)
        pal_lab[i] = color_space::linear_to_oklab(palette.colors[i]);

    constexpr int kL = 64;
    constexpr int kH = 64;
    constexpr float kPi = std::numbers::pi_v<float>;
    using Grid = std::array<std::array<float, kH>, kL>;
    Grid c_pal{};

    // Palette LUT.
    for (int Lb = 0; Lb < kL; ++Lb) {
        const float Lt = static_cast<float>(Lb) / static_cast<float>(kL - 1);
        std::vector<std::pair<float, float>> cand;
        cand.reserve(pal_lab.size() * pal_lab.size() / 2 + pal_lab.size());
        for (auto& p : pal_lab) {
            if (std::abs(p.L - Lt) < 1.0f / static_cast<float>(kL)) {
                cand.emplace_back(p.a, p.b);
            }
        }
        for (std::size_t i = 0; i < pal_lab.size(); ++i) {
            for (std::size_t j = i + 1; j < pal_lab.size(); ++j) {
                const float Li = pal_lab[i].L;
                const float Lj = pal_lab[j].L;
                if ((Li - Lt) * (Lj - Lt) > 0) continue;
                const float dL = Lj - Li;
                if (std::abs(dL) < 1e-6f) continue;
                const float t = (Lt - Li) / dL;
                const float a = pal_lab[i].a + t * (pal_lab[j].a - pal_lab[i].a);
                const float b = pal_lab[i].b + t * (pal_lab[j].b - pal_lab[i].b);
                cand.emplace_back(a, b);
            }
        }
        cand.emplace_back(0.0f, 0.0f);
        for (int Hb = 0; Hb < kH; ++Hb) {
            const float h = (static_cast<float>(Hb) / static_cast<float>(kH)) * 2.0f * kPi;
            const float ch = std::cos(h);
            const float sh = std::sin(h);
            float best = 0.0f;
            for (auto& [a, b] : cand) {
                const float p = a * ch + b * sh;
                if (p > best) best = p;
            }
            c_pal[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)] = best;
        }
    }

    // Source per-(L, h) chroma bins. Collect chromas, then take the
    // 95th percentile per bin so single-pixel outliers don't blow the
    // scale factor up.
    std::vector<std::vector<std::vector<float>>> bins(kL, std::vector<std::vector<float>>(kH));
    std::vector<color_space::OKLab> img_lab(pixel_count);
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            auto lab = color_space::linear_to_oklab(image[x, y]);
            img_lab[y * image.width() + x] = lab;
            const float c = std::sqrt(lab.a * lab.a + lab.b * lab.b);
            if (c < 1e-6f) continue;
            float h = std::atan2(lab.b, lab.a);
            if (h < 0) h += 2.0f * kPi;
            int Lb = std::clamp(
                static_cast<int>(std::clamp(lab.L, 0.0f, 1.0f) * static_cast<float>(kL - 1)),
                0,
                kL - 1);
            int Hb = static_cast<int>((h / (2.0f * kPi)) * static_cast<float>(kH)) % kH;
            bins[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)].push_back(c);
        }
    }
    Grid c_src{};
    for (int Lb = 0; Lb < kL; ++Lb) {
        for (int Hb = 0; Hb < kH; ++Hb) {
            auto& v = bins[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)];
            if (v.empty()) continue;
            auto idx = static_cast<std::size_t>(static_cast<float>(v.size()) * 0.95f);
            if (idx >= v.size()) idx = v.size() - 1;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
            c_src[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)] = v[idx];
        }
    }
    // Diffuse the c_src grid — bins the source under-sampled (e.g. a
    // hue+lightness combination present in only a few pixels) would
    // otherwise have c_src = 0 and stretch by infinity. A 3-tap box
    // blur on each axis spreads neighboring info into empty cells.
    auto diffuse = [&]() {
        Grid tmp{};
        for (int Lb = 0; Lb < kL; ++Lb) {
            for (int Hb = 0; Hb < kH; ++Hb) {
                float s = 0;
                int n = 0;
                for (int dh = -1; dh <= 1; ++dh) {
                    int Hn = (Hb + dh + kH) % kH;
                    float v = c_src[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hn)];
                    if (v > 0) {
                        s += v;
                        ++n;
                    }
                }
                tmp[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)] =
                    n > 0 ? s / static_cast<float>(n)
                          : c_src[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)];
            }
        }
        c_src = tmp;
        Grid tmp2{};
        for (int Lb = 0; Lb < kL; ++Lb) {
            for (int Hb = 0; Hb < kH; ++Hb) {
                float s = 0;
                int n = 0;
                for (int dl = -1; dl <= 1; ++dl) {
                    int Ln = std::clamp(Lb + dl, 0, kL - 1);
                    float v = c_src[static_cast<std::size_t>(Ln)][static_cast<std::size_t>(Hb)];
                    if (v > 0) {
                        s += v;
                        ++n;
                    }
                }
                tmp2[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)] =
                    n > 0 ? s / static_cast<float>(n)
                          : c_src[static_cast<std::size_t>(Lb)][static_cast<std::size_t>(Hb)];
            }
        }
        c_src = tmp2;
    };
    diffuse();
    diffuse();

    // Apply the per-(L, h) stretch.
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            auto lab = img_lab[y * image.width() + x];
            const float c = std::sqrt(lab.a * lab.a + lab.b * lab.b);
            if (c < 1e-6f) continue;
            float h = std::atan2(lab.b, lab.a);
            if (h < 0) h += 2.0f * kPi;

            const float Lf = std::clamp(lab.L, 0.0f, 1.0f) * static_cast<float>(kL - 1);
            const float Hf = (h / (2.0f * kPi)) * static_cast<float>(kH);
            int L0 = std::clamp(static_cast<int>(Lf), 0, kL - 2);
            int L1 = L0 + 1;
            float Lt = Lf - static_cast<float>(L0);
            int H0 = static_cast<int>(Hf) % kH;
            int H1 = (H0 + 1) % kH;
            float Ht = Hf - std::floor(Hf);

            auto interp = [&](const Grid& g) {
                const float v00 = g[static_cast<std::size_t>(L0)][static_cast<std::size_t>(H0)];
                const float v01 = g[static_cast<std::size_t>(L0)][static_cast<std::size_t>(H1)];
                const float v10 = g[static_cast<std::size_t>(L1)][static_cast<std::size_t>(H0)];
                const float v11 = g[static_cast<std::size_t>(L1)][static_cast<std::size_t>(H1)];
                return (1.0f - Lt) * ((1.0f - Ht) * v00 + Ht * v01) +
                       Lt * ((1.0f - Ht) * v10 + Ht * v11);
            };
            const float c_p = interp(c_pal);
            const float c_s = interp(c_src);
            if (c_s < 1e-4f || c_p < 1e-6f) continue;
            const float scale = c_p / c_s;
            float new_c = c * scale;
            if (new_c > c_p) new_c = c_p;
            const float k = new_c / c;
            lab.a *= k;
            lab.b *= k;
            image[x, y] = color_space::oklab_to_linear(lab).clamped();
        }
    }
}

}  // namespace png2amiga::preprocess
