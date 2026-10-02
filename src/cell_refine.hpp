#pragma once

// Optional joint color/pattern refinement for fixed-palette 8-pixel cells.
// Coordinate descent minimizes linear-RGB error after a global 3x3 binomial
// blur, including the one-pixel halo around each cell. Each step searches
// every supplied legal pattern and palette pair; two opposite-order passes
// reduce dependence on the first sweep. An S2 check retains the original
// whole image when this surrogate objective fails to improve it.
// Internal callers supply 1..16 colors, cell heights 1..8, and a complete
// nonempty rectangular grid of cells. Pattern bit y*8+x is foreground.

#include "types.hpp"
#include "ssimulacra2.hpp"
#include <array>
#include <vector>
#include <span>
#include <cmath>
#include <limits>
namespace png2amiga::cell_refine {
struct Cell {
    std::uint64_t mask;
    std::uint8_t fg, bg, ch;
};
struct Pattern {
    std::uint64_t mask;
    std::uint8_t ch;
};
inline float dot(Color3f a, Color3f b) {
    return a.r * b.r + a.g * b.g + a.b * b.b;
}
inline float refine(const Image& source,
                    std::span<const Color3f> pal,
                    std::size_t cell_h,
                    std::span<const Pattern> patterns,
                    std::vector<Cell>& cells, std::size_t locked_left_cells = 0) {
    // Keep the original legal encoding if the full-image S2 score regresses.
    const auto original_cells = cells;
    const auto W = source.width(), H = source.height(), cols = W / 8;
    const int hh = static_cast<int>(cell_h), ww = static_cast<int>(W), ih = static_cast<int>(H);
    const int pw = 10, ph = hh + 2;
    const auto area = static_cast<std::size_t>(pw * ph);
    constexpr int passes = 2;
    std::vector<Color3f> pixels(W * H), residual(W * H);
    for (std::size_t cy = 0; cy < H / cell_h; ++cy)
        for (std::size_t cx = 0; cx < cols; ++cx) {
            const auto& c = cells[cy * cols + cx];
            for (std::size_t y = 0; y < cell_h; ++y)
                for (std::size_t x = 0; x < 8; ++x)
                    pixels[(cy * cell_h + y) * W + cx * 8 + x] =
                        pal[(c.mask & (std::uint64_t{1} << (y * 8 + x))) ? c.fg : c.bg];
        }
    const auto original_pixels = pixels;
    auto tap = [](int x, int y) {
        return static_cast<float>((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) / 16.0f;
    };
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < ww; ++x) {
            Color3f r{};
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= ww || sy >= ih) continue;
                    auto i = static_cast<std::size_t>(sy) * W + static_cast<std::size_t>(sx);
                    r += (pixels[i] - source.pixels()[i]) * tap(dx, dy);
                }
            residual[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)] = r;
        }
    float initial_error = 0;
    for (auto r : residual)
        initial_error += dot(r, r);
    struct Shape {
        Pattern p;
        std::vector<float> a, b;
    };
    std::vector<Shape> shapes;
    for (auto p : patterns) {
        Shape sh{p, std::vector<float>(area), std::vector<float>(area)};
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < 8; ++x) {
                bool fg = (p.mask & (std::uint64_t{1} << static_cast<unsigned>(y * 8 + x))) != 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto q = static_cast<std::size_t>((y + dy + 1) * pw + x + dx + 1);
                        (fg ? sh.a[q] : sh.b[q]) += tap(dx, dy);
                    }
            }
        shapes.push_back(std::move(sh));
    }
    std::array<float, 16> norm{};
    std::array<std::array<float, 16>, 16> pd{};
    for (std::size_t a = 0; a < pal.size(); ++a) {
        norm[a] = dot(pal[a], pal[a]);
        for (std::size_t b = 0; b < pal.size(); ++b)
            pd[a][b] = dot(pal[a], pal[b]);
    }
    for (int pass = 0; pass < passes; ++pass)
        for (std::size_t step = 0; step < cells.size(); ++step) {
            const auto ci = (pass % 2) ? cells.size() - 1 - step : step;
            if (ci % cols < locked_left_cells) continue;
            const int ox = static_cast<int>(ci % cols) * 8, oy = static_cast<int>(ci / cols) * hh;
            std::vector<Color3f> target(area), old(area);
            std::vector<std::size_t> valid;
            for (int y = 0; y < hh; ++y)
                for (int x = 0; x < 8; ++x) {
                    const auto v = pixels[static_cast<std::size_t>(oy + y) * W +
                                          static_cast<std::size_t>(ox + x)];
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            old[static_cast<std::size_t>((y + dy + 1) * pw + x + dx + 1)] +=
                                v * tap(dx, dy);
                }
            float oldcost = 0;
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x) {
                    int xx = ox + x - 1, yy = oy + y - 1;
                    if (xx < 0 || yy < 0 || xx >= ww || yy >= ih) continue;
                    auto q = static_cast<std::size_t>(y * pw + x),
                         i = static_cast<std::size_t>(yy) * W + static_cast<std::size_t>(xx);
                    valid.push_back(q);
                    target[q] = old[q] - residual[i];
                    oldcost += dot(residual[i], residual[i]);
                }
            Cell best = cells[ci];
            float bestcost = oldcost;
            const Shape* chosen = nullptr;
            for (const auto& sh : shapes) {
                Color3f ta{}, tb{};
                float aa = 0, ab = 0, bb = 0, tt = 0;
                for (auto q : valid) {
                    float a = sh.a[q], b = sh.b[q];
                    ta += target[q] * a;
                    tb += target[q] * b;
                    aa += a * a;
                    ab += a * b;
                    bb += b * b;
                    tt += dot(target[q], target[q]);
                }
                std::array<float, 16> fa{}, fb{};
                for (std::size_t a = 0; a < pal.size(); ++a) {
                    fa[a] = aa * norm[a] - 2 * dot(ta, pal[a]);
                    fb[a] = bb * norm[a] - 2 * dot(tb, pal[a]);
                }
                for (std::size_t a = 0; a < pal.size(); ++a)
                    for (std::size_t b = 0; b < pal.size(); ++b) {
                        float e = tt + fa[a] + fb[b] + 2 * ab * pd[a][b];
                        if (e < bestcost - 0.000001f) {
                            bestcost = e;
                            best = {sh.p.mask,
                                    static_cast<std::uint8_t>(a),
                                    static_cast<std::uint8_t>(b),
                                    sh.p.ch};
                            chosen = &sh;
                        }
                    }
            }
            if (!chosen) continue;
            for (auto q : valid) {
                int xx = ox + static_cast<int>(q % 10) - 1, yy = oy + static_cast<int>(q / 10) - 1;
                residual[static_cast<std::size_t>(yy) * W + static_cast<std::size_t>(xx)] +=
                    pal[best.fg] * chosen->a[q] + pal[best.bg] * chosen->b[q] - old[q];
            }
            cells[ci] = best;
            for (std::size_t y = 0; y < cell_h; ++y)
                for (std::size_t x = 0; x < 8; ++x)
                    pixels[(static_cast<std::size_t>(oy) + y) * W + static_cast<std::size_t>(ox) +
                           x] =
                        pal[(best.mask & (std::uint64_t{1} << (y * 8 + x))) ? best.fg : best.bg];
        }
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), W, H);
    const float before = ssimulacra2::compute(reference, original_pixels);
    const float after = ssimulacra2::compute(reference, pixels);
    if (!std::isfinite(after) || after <= before) {
        cells = original_cells;
        return initial_error;
    }
    float final_error = 0;
    for (auto r : residual)
        final_error += dot(r, r);
    return final_error;
}
}  // namespace png2amiga::cell_refine
