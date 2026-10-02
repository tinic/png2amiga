#pragma once

// Constrained coordinate descent on the globally blurred rendered image.
// Variables describe actual color registers; selector groups describe bitmap
// bits, shared writable glyph bits, or complete ROM glyphs. Trials start from
// the same seed in linear RGB and OKLab, with full-image S2 checkpoints after
// each pass. Only the best legal encoding is returned.

#include "ssimulacra2.hpp"
#include "dither.hpp"
#include "color_space.hpp"
#include "types.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::cell_graph_refine {
// A color variable can be local to a cell/row or shared by the whole image.
// Pixel groups share one bitmap selector (single pixels for bitmap modes,
// all occurrences of a glyph bit for writable character sets).
struct Model {
    std::vector<Color3f> palette;
    std::vector<std::uint8_t> values;
    std::vector<std::size_t> limits;
    std::vector<std::array<std::size_t, 4>> slots;
    std::vector<std::uint8_t> labels;
    std::vector<std::vector<std::size_t>> groups;
    std::vector<std::vector<std::uint8_t>> patterns;
    std::vector<std::vector<std::size_t>> pattern_cells;
    std::vector<std::size_t> pattern_ids;
    std::vector<bool> pattern_allowed;
    std::size_t choices = 2;
    std::size_t add(std::uint8_t value, std::size_t limit) {
        values.push_back(value);
        limits.push_back(limit);
        return values.size() - 1;
    }
};
inline float dot(Color3f a, Color3f b) {
    return a.r * b.r + a.g * b.g + a.b * b.b;
}
class Objective {
    std::size_t w, h;
    std::vector<Color3f> residual, delta;
    std::vector<std::size_t> touched;
    std::vector<bool> marked;
    template<class F>
    void halo(std::size_t p, F f) {
        const int x = static_cast<int>(p % w), y = static_cast<int>(p / w);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
                    continue;
                f(static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx),
                  static_cast<float>((dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1)) / 16.0f);
            }
    }
    void clear() {
        for (auto p : touched) {
            delta[p] = {};
            marked[p] = false;
        }
        touched.clear();
    }
    void add(std::size_t p, Color3f d) {
        halo(p, [&](std::size_t q, float a) {
            if (!marked[q]) {
                marked[q] = true;
                touched.push_back(q);
            }
            delta[q] += d * a;
        });
    }

public:
    std::vector<Color3f> pixels;
    Objective(const Image& source, const Model& m)
        : w(source.width()),
          h(source.height()),
          residual(w * h),
          delta(w * h),
          marked(w * h),
          pixels(w * h) {
        for (std::size_t p = 0; p < pixels.size(); ++p)
            pixels[p] = m.palette[m.values[m.slots[p][m.labels[p]]]];
        for (std::size_t p = 0; p < pixels.size(); ++p)
            halo(p, [&](std::size_t q, float a) {
                residual[q] += (pixels[p] - source.pixels()[p]) * a;
            });
    }
    float score(std::span<const std::size_t> positions,
                std::span<const Color3f> colors,
                bool commit = false) {
        clear();
        for (std::size_t i = 0; i < positions.size(); ++i)
            add(positions[i], colors[i] - pixels[positions[i]]);
        float e = 0;
        for (auto p : touched)
            e += 2 * dot(residual[p], delta[p]) + dot(delta[p], delta[p]);
        if (commit) {
            for (auto p : touched)
                residual[p] += delta[p];
            for (std::size_t i = 0; i < positions.size(); ++i)
                pixels[positions[i]] = colors[i];
        }
        return e;
    }
    // An ordered candidate changes a register, then re-runs the selected
    // quantizer for every pixel whose palette contains that register. Never
    // optimize individual selectors independently of the dither phase.
    void recolor_ordered(Model& m,
                         std::size_t v,
                         std::span<const std::size_t> users,
                         std::span<const color_space::OKLab> targets,
                         std::span<const color_space::OKLab> palette_lab,
                         const dither::Settings& settings) {
        if (users.empty() || m.limits[v] <= 1) return;
        const auto old = m.values[v];
        auto best = old;
        float cost = 0;
        std::vector<Color3f> colors(users.size()), best_colors;
        std::vector<std::uint8_t> labels(users.size()), best_labels;
        for (std::size_t c = 0; c < m.limits[v]; ++c) {
            m.values[v] = static_cast<std::uint8_t>(c);
            for (std::size_t i = 0; i < users.size(); ++i) {
                auto p = users[i];
                std::array<color_space::OKLab, 4> local{};
                for (std::size_t q = 0; q < m.choices; ++q)
                    local[q] = palette_lab[m.values[m.slots[p][q]]];
                std::size_t q = 0;
                color_space::OKLab chosen{};
                dither::pick_palette_index_with_ostro(settings.method,
                                                      targets[p],
                                                      {local.data(), m.choices},
                                                      p % w,
                                                      p / w,
                                                      settings.strength,
                                                      0,
                                                      q,
                                                      chosen);
                labels[i] = static_cast<std::uint8_t>(q);
                colors[i] = m.palette[m.values[m.slots[p][q]]];
            }
            float e = score(users, colors);
            if (e < cost - 1e-7f) {
                cost = e;
                best = static_cast<std::uint8_t>(c);
                best_colors = colors;
                best_labels = labels;
            }
        }
        m.values[v] = best;
        if (!best_labels.empty()) {
            for (std::size_t i = 0; i < users.size(); ++i)
                m.labels[users[i]] = best_labels[i];
            score(users, best_colors, true);
        }
    }
    void recolor(Model& m, std::size_t v, std::span<const std::size_t> users) {
        std::vector<std::size_t> active;
        clear();
        for (auto p : users)
            if (m.slots[p][m.labels[p]] == v) {
                active.push_back(p);
                add(p, {1, 0, 0});
            }
        if (active.empty() || m.limits[v] <= 1) return;
        float aa = 0;
        Color3f b{};
        for (auto p : touched) {
            float a = delta[p].r;
            aa += a * a;
            b += residual[p] * a;
        }
        const auto old = m.palette[m.values[v]];
        auto best = m.values[v];
        float cost = 0;
        for (std::size_t c = 0; c < m.limits[v]; ++c) {
            auto d = m.palette[c] - old;
            float e = 2 * dot(b, d) + aa * dot(d, d);
            if (e < cost - 1e-7f) {
                cost = e;
                best = static_cast<std::uint8_t>(c);
            }
        }
        if (best != m.values[v]) {
            m.values[v] = best;
            std::vector<Color3f> colors(active.size(), m.palette[best]);
            score(active, colors, true);
        }
    }
};
inline bool refine_passes(const Image& source,
                          Model& m,
                          Image& rendered,
                          bool lab,
                          const dither::Settings* ordered,
                          bool fixed_patterns) {
    const auto linear_palette = m.palette;
    std::vector<Color3f> before(rendered.pixels().begin(), rendered.pixels().end());
    std::vector<color_space::OKLab> targets, palette_lab;
    if (ordered && !fixed_patterns) {
        for (auto c : linear_palette)
            palette_lab.push_back(color_space::linear_to_oklab(c));
        targets.resize(source.width() * source.height());
        // Reuse the encoder's threshold offsets, clipping and phase exactly.
        dither::diffuse_raw_buffer(
            source, *ordered, [&](const color_space::OKLab& target, std::size_t x, std::size_t y) {
                targets[y * source.width() + x] = target;
                return dither::PickResult{target, 0.5f};
            });
    }
    Image metric_source = source;
    if (lab) {
        auto cv = [](Color3f c) {
            auto l = color_space::linear_to_oklab(c);
            return Color3f{l.L, l.a, l.b};
        };
        for (auto& c : m.palette)
            c = cv(c);
        for (auto& c : metric_source.pixels())
            c = cv(c);
    }
    Objective obj(metric_source, m);
    std::vector<std::vector<std::size_t>> users(m.values.size());
    for (std::size_t p = 0; p < m.slots.size(); ++p)
        for (std::size_t q = 0; q < m.choices; ++q) {
            auto v = m.slots[p][q];
            bool seen = false;
            for (std::size_t k = 0; k < q; ++k)
                if (m.slots[p][k] == v) seen = true;
            if (!seen) users[v].push_back(p);
        }
    constexpr int passes = 4;
    ssimulacra2::PrecomputedSource ref;
    ref.prepare(source.pixels(), source.width(), source.height());
    float best_score = ssimulacra2::compute(ref, before);
    auto best_values = m.values;
    auto best_labels = m.labels;
    auto best_ids = m.pattern_ids;
    auto best_pixels = before;
    bool improved = false;
    for (int pass = 0; pass < passes; ++pass) {
        for (std::size_t k = 0; k < m.values.size(); ++k) {
            auto v = pass ? m.values.size() - 1 - k : k;
            if (ordered && !fixed_patterns)
                obj.recolor_ordered(m, v, users[v], targets, palette_lab, *ordered);
            else
                obj.recolor(m, v, users[v]);
        }
        for (std::size_t k = 0; !ordered && k < m.groups.size(); ++k) {
            const auto& group = m.groups[pass ? m.groups.size() - 1 - k : k];
            if (group.empty()) continue;
            auto best = m.labels[group[0]];
            float cost = 0;
            std::vector<Color3f> colors(group.size());
            for (std::size_t q = 0; q < m.choices; ++q) {
                for (std::size_t i = 0; i < group.size(); ++i)
                    colors[i] = m.palette[m.values[m.slots[group[i]][q]]];
                float e = obj.score(group, colors);
                if (e < cost - 1e-7f) {
                    cost = e;
                    best = static_cast<std::uint8_t>(q);
                }
            }
            if (best != m.labels[group[0]]) {
                for (std::size_t i = 0; i < group.size(); ++i) {
                    auto p = group[i];
                    m.labels[p] = best;
                    colors[i] = m.palette[m.values[m.slots[p][best]]];
                }
                obj.score(group, colors, true);
            }
        }
        // Fixed ROM glyphs: replace a whole cell with another legal glyph.
        for (std::size_t k = 0; k < m.pattern_cells.size(); ++k) {
            auto c = pass ? m.pattern_cells.size() - 1 - k : k;
            const auto& positions = m.pattern_cells[c];
            auto best = m.pattern_ids[c];
            float cost = 0;
            std::vector<Color3f> colors(positions.size());
            for (std::size_t g = 0; g < m.patterns.size(); ++g) {
                if (!m.pattern_allowed.empty() && !m.pattern_allowed[g]) continue;
                for (std::size_t i = 0; i < positions.size(); ++i)
                    colors[i] = m.palette[m.values[m.slots[positions[i]][m.patterns[g][i]]]];
                float e = obj.score(positions, colors);
                if (e < cost - 1e-7f) {
                    cost = e;
                    best = g;
                }
            }
            if (best != m.pattern_ids[c]) {
                m.pattern_ids[c] = best;
                for (std::size_t i = 0; i < positions.size(); ++i) {
                    auto p = positions[i];
                    m.labels[p] = m.patterns[best][i];
                    colors[i] = m.palette[m.values[m.slots[p][m.labels[p]]]];
                }
                obj.score(positions, colors, true);
            }
        }
        std::vector<Color3f> candidate(obj.pixels.size());
        for (std::size_t p = 0; p < candidate.size(); ++p)
            candidate[p] = linear_palette[m.values[m.slots[p][m.labels[p]]]];
        float score = ssimulacra2::compute(ref, candidate);
        if (std::isfinite(score) && score > best_score) {
            best_score = score;
            best_values = m.values;
            best_labels = m.labels;
            best_ids = m.pattern_ids;
            best_pixels = std::move(candidate);
            improved = true;
        }
    }
    m.palette = linear_palette;
    m.values = std::move(best_values);
    m.labels = std::move(best_labels);
    m.pattern_ids = std::move(best_ids);
    if (improved) std::copy(best_pixels.begin(), best_pixels.end(), rendered.pixels().begin());
    return improved;
}
inline bool refine(const Image& source,
                   Model& m,
                   Image& rendered,
                   const dither::Settings* ordered = nullptr,
                   bool fixed_patterns = false) {
    auto alternative = m;
    Image alternate_render = rendered;
    bool improved = refine_passes(source, m, rendered, false, ordered, fixed_patterns);
    bool other = refine_passes(
        source, alternative, alternate_render, true, ordered, fixed_patterns);
    if (other) {
        ssimulacra2::PrecomputedSource ref;
        ref.prepare(source.pixels(), source.width(), source.height());
        if (!improved || ssimulacra2::compute(ref, alternate_render.pixels()) >
                             ssimulacra2::compute(ref, rendered.pixels())) {
            m = std::move(alternative);
            rendered = std::move(alternate_render);
            return true;
        }
    }
    return improved;
}
}  // namespace png2amiga::cell_graph_refine
