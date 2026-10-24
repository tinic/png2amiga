// Smooth dither: a per-pixel pair/coverage plan, then coarse-to-fine blur
// refinement with no scan direction.
//
// 1. Plan: per pixel, choose a palette pair (i, j) among the 8 nearest
//    colours and a coverage k/16 of j, minimising OKLab error of the
//    linear mix plus a grain term alpha(1-alpha)|ci-cj|^2 (the variance
//    of the mix).
// 2. Layout: j where the Bayer 4x4 rank is below the coverage, so each
//    level is a fixed regular pattern.
// 3. Refine at sigma 2.0, 1.4, 1.0 on the blurred linear error, visiting
//    pixels in four interleaved classes. Coarse kSteps only swap
//    neighbouring pixels (coverage kept); the last level also lets a
//    pixel take any of its 8 candidates, with a fidelity term.

#include "dither.hpp"
#include "color_space.hpp"
#include "pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace png2amiga::dither {
namespace {

using color_space::OKLab;

constexpr std::size_t kCandidates = 8;
constexpr std::array<float, 3> kSigmas{2.0f, 1.4f, 1.0f};

constexpr std::size_t kSteps = 16;    // coverage steps
constexpr float kGrain = 0.35f;       // weight of the mix variance in the plan
constexpr float kFidelity = 0.05f;    // last level: per-pixel error weight
constexpr int kSweeps = 3;
constexpr std::array<std::uint8_t, 16> kBayer4{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float dot(Color3f a, Color3f b) {
    return a.r * b.r + a.g * b.g + a.b * b.b;
}
float lab_dist(const OKLab& a, const OKLab& b) {
    float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
    return dL * dL + da * da + db * db;
}

std::vector<float> gaussian(float sigma) {
    int r = static_cast<int>(std::ceil(3 * sigma));
    std::vector<float> k(static_cast<std::size_t>(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; ++i)
        sum += k[static_cast<std::size_t>(i + r)] = std::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
    for (auto& v : k)
        v /= sum;
    return k;
}

// Separable convolution with zero outside the image.
std::vector<Color3f> convolve(std::span<const Color3f> in,
                              std::size_t w,
                              std::size_t h,
                              std::span<const float> k) {
    const int r = static_cast<int>(k.size() / 2), iw = static_cast<int>(w),
              ih = static_cast<int>(h);
    std::vector<Color3f> tmp(w * h), out(w * h);
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < iw; ++x) {
            Color3f acc{};
            for (int d = -r; d <= r; ++d)
                if (x + d >= 0 && x + d < iw)
                    acc += in[static_cast<std::size_t>(y * iw + x + d)] *
                           k[static_cast<std::size_t>(d + r)];
            tmp[static_cast<std::size_t>(y * iw + x)] = acc;
        }
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < iw; ++x) {
            Color3f acc{};
            for (int d = -r; d <= r; ++d)
                if (y + d >= 0 && y + d < ih)
                    acc += tmp[static_cast<std::size_t>((y + d) * iw + x)] *
                           k[static_cast<std::size_t>(d + r)];
            out[static_cast<std::size_t>(y * iw + x)] = acc;
        }
    return out;
}

// Blur-space descent at one sigma. cpe = A * (halftone - source) with
// A = g (x) g, so a change d at p costs 2 d.cpe(p) + |d|^2 A(0).
class Level {
    std::size_t w, h;
    int r;
    std::vector<float> a1;  // 1D autocorrelation of the Gaussian
    std::vector<Color3f> cpe;

    float A(int dx, int dy) const {
        if (std::abs(dx) > r || std::abs(dy) > r) return 0;
        return a1[static_cast<std::size_t>(dx + r)] * a1[static_cast<std::size_t>(dy + r)];
    }
    void apply(std::size_t p, Color3f d) {
        const int px = static_cast<int>(p % w), py = static_cast<int>(p / w);
        for (int dy = -r; dy <= r; ++dy) {
            int y = py + dy;
            if (y < 0 || y >= static_cast<int>(h)) continue;
            float ay = a1[static_cast<std::size_t>(dy + r)];
            for (int dx = -r; dx <= r; ++dx) {
                int x = px + dx;
                if (x < 0 || x >= static_cast<int>(w)) continue;
                cpe[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] +=
                    d * (ay * a1[static_cast<std::size_t>(dx + r)]);
            }
        }
    }

public:
    Level(float sigma,
          std::size_t w_,
          std::size_t h_,
          std::span<const Color3f> source,
          std::span<const Color3f> halftone)
        : w(w_), h(h_) {
        auto g = gaussian(sigma);
        const int gr = static_cast<int>(g.size() / 2);
        r = 2 * gr;
        a1.assign(static_cast<std::size_t>(2 * r + 1), 0.0f);
        for (int i = -gr; i <= gr; ++i)
            for (int j = -gr; j <= gr; ++j)
                a1[static_cast<std::size_t>(i - j + r)] +=
                    g[static_cast<std::size_t>(i + gr)] * g[static_cast<std::size_t>(j + gr)];
        std::vector<Color3f> err(w * h);
        for (std::size_t i = 0; i < err.size(); ++i)
            err[i] = halftone[i] - source[i];
        cpe = convolve(err, w, h, a1);
    }

    // Returns the number of accepted changes.
    std::size_t sweep(std::vector<std::uint8_t>& idx,
                      std::span<const Color3f> pal,
                      std::span<const Color3f> source,
                      std::span<const std::array<std::uint8_t, kCandidates>> cand,
                      std::span<const float> weight,
                      bool toggles,
                      bool reverse) {
        const float a0 = A(0, 0);
        std::size_t changes = 0;
        // Four interleaved classes (x&1, y&1): no scan direction dominates.
        std::vector<std::size_t> order;
        order.reserve(w * h);
        for (std::size_t cls = 0; cls < 4; ++cls)
            for (std::size_t y = cls >> 1; y < h; y += 2)
                for (std::size_t x = cls & 1; x < w; x += 2)
                    order.push_back(y * w + x);
        if (reverse) std::reverse(order.begin(), order.end());
        for (const std::size_t p : order) {
            const int px = static_cast<int>(p % w), py = static_cast<int>(p / w);
            const Color3f cp = pal[idx[p]];
            float best = -1e-9f;
            int best_q = -1;
            std::uint8_t best_c = idx[p];
            // Swap with a neighbour: keeps the local coverage.
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    int qx = px + dx, qy = py + dy;
                    if (qx < 0 || qy < 0 || qx >= static_cast<int>(w) || qy >= static_cast<int>(h))
                        continue;
                    std::size_t q = static_cast<std::size_t>(qy) * w + static_cast<std::size_t>(qx);
                    if (idx[q] == idx[p]) continue;
                    Color3f dp = pal[idx[q]] - cp, dq = cp - pal[idx[q]];
                    float e = 2 * dot(dp, cpe[p]) + 2 * dot(dq, cpe[q]) +
                              a0 * (dot(dp, dp) + dot(dq, dq)) + 2 * A(dx, dy) * dot(dp, dq);
                    e *= 0.5f * (weight[p] + weight[q]);
                    if (toggles) {
                        auto fid = [&](std::size_t at, std::uint8_t from, std::uint8_t to) {
                            Color3f a = pal[to] - source[at], b = pal[from] - source[at];
                            return weight[at] * kFidelity * (dot(a, a) - dot(b, b));
                        };
                        e += fid(p, idx[p], idx[q]) + fid(q, idx[q], idx[p]);
                    }
                    if (e < best) {
                        best = e;
                        best_q = static_cast<int>(q);
                    }
                }
            if (toggles)
                for (auto c : cand[p]) {
                    if (c == idx[p]) continue;
                    Color3f d = pal[c] - cp;
                    Color3f a = pal[c] - source[p], b = cp - source[p];
                    float e = weight[p] * (2 * dot(d, cpe[p]) + a0 * dot(d, d) +
                                           kFidelity * (dot(a, a) - dot(b, b)));
                    if (e < best) {
                        best = e;
                        best_q = -1;
                        best_c = c;
                    }
                }
            if (best_q >= 0) {
                auto q = static_cast<std::size_t>(best_q);
                Color3f d = pal[idx[q]] - cp;
                apply(p, d);
                apply(q, d * -1.0f);
                std::swap(idx[p], idx[q]);
                ++changes;
            } else if (best_c != idx[p]) {
                apply(p, pal[best_c] - cp);
                idx[p] = best_c;
                ++changes;
            }
        }
        return changes;
    }
};

}  // namespace

DitherResult apply_smooth(const Image& image, std::span<const OKLab> palette_lab) {
    const std::size_t w = image.width(), h = image.height(), n = w * h;
    const std::size_t pn = palette_lab.size();
    DitherResult res;
    res.indices.assign(n, 0);
    if (pn == 0 || n == 0) return res;
    std::vector<Color3f> pal(pn);
    for (std::size_t i = 0; i < pn; ++i)
        pal[i] = color_space::oklab_to_linear(palette_lab[i]);
    std::span<const Color3f> source = image.pixels();

    // 1. Plan per pixel.
    const std::size_t nc = std::min(kCandidates, pn);
    std::vector<std::array<std::uint8_t, kCandidates>> cand(n);
    std::vector<std::array<std::uint8_t, 2>> pair(n);
    std::vector<std::uint8_t> cover(n);
    pipeline::parallel_for(h, [&](std::size_t y) {
        std::vector<std::pair<float, std::uint8_t>> near(pn);
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t p = y * w + x;
            const OKLab t = color_space::linear_to_oklab(source[p]);
            for (std::size_t i = 0; i < pn; ++i)
                near[i] = {lab_dist(t, palette_lab[i]), static_cast<std::uint8_t>(i)};
            std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(nc),
                              near.end());
            auto& cs = cand[p];
            for (std::size_t i = 0; i < kCandidates; ++i)
                cs[i] = near[std::min(i, nc - 1)].second;
            float best = std::numeric_limits<float>::infinity();
            for (std::size_t a = 0; a < nc; ++a)
                for (std::size_t b = a; b < nc; ++b) {
                    const auto ia = cs[a], ib = cs[b];
                    const float spread = lab_dist(palette_lab[ia], palette_lab[ib]);
                    for (std::size_t k = 0; k <= (ia == ib ? 0 : kSteps); ++k) {
                        const float al = static_cast<float>(k) / static_cast<float>(kSteps);
                        OKLab m = color_space::linear_to_oklab(pal[ia] * (1 - al) + pal[ib] * al);
                        float e = lab_dist(t, m) + kGrain * al * (1 - al) * spread;
                        if (e < best) {
                            best = e;
                            pair[p] = {ia, ib};
                            cover[p] = static_cast<std::uint8_t>(k);
                        }
                    }
                }
        }
    });

    // 2. Layout from the Bayer rank.
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t p = y * w + x;
            const unsigned rank = kBayer4[(y & 3) * 4 + (x & 3)];
            res.indices[p] = rank * kSteps < static_cast<unsigned>(cover[p]) * 16u ? pair[p][1]
                                                                                  : pair[p][0];
        }

    // 3. Coarse-to-fine refinement. Linear error under-counts dark areas:
    // weight each pixel by the squared slope of a cube-root lightness at
    // the pixel's luminance, (Y^(-2/3))^2.
    std::vector<float> weight(n);
    for (std::size_t i = 0; i < n; ++i) {
        float Y = std::max(0.2126f * source[i].r + 0.7152f * source[i].g + 0.0722f * source[i].b,
                           0.002f);
        weight[i] = std::pow(Y, -4.0f / 3.0f);
    }
    std::vector<Color3f> halftone(n);
    for (std::size_t li = 0; li < kSigmas.size(); ++li) {
        for (std::size_t i = 0; i < n; ++i)
            halftone[i] = pal[res.indices[i]];
        Level level(kSigmas[li], w, h, source, halftone);
        const bool last = li + 1 == kSigmas.size();
        for (int s = 0; s < kSweeps; ++s)
            if (level.sweep(res.indices, pal, source, cand, weight, last, s & 1) < n / 1000) break;
    }

    float total = 0;
    for (std::size_t i = 0; i < n; ++i)
        total += lab_dist(color_space::linear_to_oklab(source[i]), palette_lab[res.indices[i]]);
    res.total_error = total;
    return res;
}

}  // namespace png2amiga::dither
