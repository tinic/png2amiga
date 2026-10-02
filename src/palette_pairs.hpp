#pragma once
#include "dither.hpp"
#include "palette.hpp"
#include "ssimulacra2.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>

// Joint palette updates estimated from spatial color mixing. Candidate pixels
// always come from the caller's unchanged dither, never from free pixel edits.
namespace png2amiga::palette_pairs {
struct Frame {
    std::vector<Color3f> palette;
    std::vector<std::uint8_t> indices;
};
struct Proposal {
    double predicted{};
    std::vector<Color3f> palette;
};
inline Color3f space(Color3f c, bool lab) {
    if (!lab) return c;
    auto l = color_space::linear_to_oklab(c);
    return {l.L, l.a, l.b};
}
inline Color3f unspace(Color3f c, bool lab) {
    return lab ? color_space::oklab_to_linear({c.r, c.g, c.b}) : c;
}
inline double dot(Color3f a, Color3f b) {
    return static_cast<double>(a.r) * static_cast<double>(b.r) +
           static_cast<double>(a.g) * static_cast<double>(b.g) +
           static_cast<double>(a.b) * static_cast<double>(b.b);
}
inline int thomson_code(Color3f c) {
    auto channel = [](float v) {
        return palette::thomson_channel_index(static_cast<int>(
            std::lround(std::clamp(color_space::linear_to_srgb(v), 0.0f, 1.0f) * 255.0f)));
    };
    return (channel(c.r) << 8) | (channel(c.g) << 4) | channel(c.b);
}
inline Color3f snap(Color3f c, bool aga, bool to8 = false) {
    if (to8) {
        int code = thomson_code(c);
        return color_space::srgb_hex_to_linear(
            palette::thomson_rgb_hex(code >> 8, (code >> 4) & 15, code & 15));
    }
    return aga ? palette::aga_to_linear(palette::linear_to_aga(c)) : palette::quantize_to_ocs(c);
}
inline std::vector<int> key(std::span<const Color3f> colors, bool aga, bool to8 = false) {
    std::vector<int> result;
    for (auto c : colors)
        result.push_back(to8   ? thomson_code(c)
                         : aga ? static_cast<int>(palette::linear_to_aga(c))
                               : palette::linear_to_ocs(c));
    return result;
}
inline std::vector<Proposal> proposals(const Image& source,
                                       const Frame& f,
                                       const std::vector<bool>& locked,
                                       bool lab,
                                       bool aga,
                                       bool ehb = false,
                                       bool paired = true,
                                       bool to8 = false) {
    const auto w = source.width(), h = source.height(), n = w * h,
               k = ehb ? std::size_t{32} : f.palette.size();
    std::vector<double> gram(k * k, 0);
    std::vector<Color3f> rhs(k), colors;
    for (auto c : f.palette)
        colors.push_back(space(c, lab));
    // Blurred indicator fields retain how palette entries mix spatially.
    // Off-diagonal terms couple entries that occur together in the filter.
    for (std::size_t p = 0; p < n; ++p) {
        const int x = static_cast<int>(p % w), y = static_cast<int>(p / w);
        std::array<float, 256> weights{};
        std::array<std::size_t, 9> active{};
        std::size_t active_count = 0;
        Color3f target{}, rendered{};
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
                    continue;
                auto q = static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx);
                float a = static_cast<float>((dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1)) / 16.0f;
                const auto index = ehb ? static_cast<std::size_t>(f.indices[q] % 32)
                                       : static_cast<std::size_t>(f.indices[q]);
                if (weights[index] == 0) active[active_count++] = index;
                // Approximate half-bright response only to propose updates.
                // Candidates are snapped and their actual half-bright colors
                // are re-derived before the full re-encode and S2 comparison.
                const float response = ehb && f.indices[q] >= 32 ? (lab ? 0.63f : 0.25f) : 1.0f;
                weights[index] += a * response;
                target += space(source.pixels()[q], lab) * a;
                rendered += colors[f.indices[q]] * a;
            }
        auto residual = target - rendered;
        for (auto a : std::span(active).first(active_count)) {
            rhs[a] += residual * weights[a];
            for (auto b : std::span(active).first(active_count))
                gram[a * k + b] += static_cast<double>(weights[a]) *
                                   static_cast<double>(weights[b]);
        }
    }
    std::vector<Proposal> out;
    const auto original_key = key(std::span(f.palette).first(k), aga, to8);
    const auto unique_before = std::set<int>(original_key.begin(), original_key.end()).size();
    std::set<std::vector<int>> seen;
    for (std::size_t a = 0; a < k; ++a) {
        if (a < locked.size() && locked[a]) continue;
        for (std::size_t b = paired ? a + 1 : a; b < (paired ? k : a + 1); ++b) {
            if (b < locked.size() && locked[b]) continue;
            double aa = gram[a * k + a], bb = gram[b * k + b], ab = paired ? gram[a * k + b] : 0;
            if (aa < 1e-5 || bb < 1e-5 || (paired && std::abs(ab) < 0.01 * std::sqrt(aa * bb)))
                continue;
            double ridge = 0.001 * (aa + bb);
            double det = (aa + ridge) * (bb + ridge) - ab * ab;
            if (det < 1e-8) continue;
            auto da = (rhs[a] * static_cast<float>(bb + ridge) - rhs[b] * static_cast<float>(ab)) *
                      static_cast<float>(1 / det);
            auto db = (rhs[b] * static_cast<float>(aa + ridge) - rhs[a] * static_cast<float>(ab)) *
                      static_cast<float>(1 / det);
            for (float step : {0.5f, 1.0f, 1.5f}) {
                auto pal = f.palette;
                pal[a] = snap(unspace(colors[a] + da * step, lab), aga, to8);
                if (paired) pal[b] = snap(unspace(colors[b] + db * step, lab), aga, to8);
                if (ehb) {
                    pal[a + 32] = palette::half_brite(pal[a]);
                    pal[b + 32] = palette::half_brite(pal[b]);
                }
                auto ka = key(std::span(pal).first(k), aga, to8);
                if (ka == original_key || !seen.insert(ka).second ||
                    std::set<int>(ka.begin(), ka.end()).size() < unique_before)
                    continue;
                if (paired && (ka[a] == original_key[a] || ka[b] == original_key[b])) continue;
                auto d1 = space(pal[a], lab) - colors[a];
                auto d2 = paired ? space(pal[b], lab) - colors[b] : Color3f{};
                double e = aa * dot(d1, d1) + bb * dot(d2, d2) + 2 * ab * dot(d1, d2) -
                           2 * dot(rhs[a], d1) - 2 * dot(rhs[b], d2);
                if (e < 0) out.push_back({e, std::move(pal)});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Proposal& a, const Proposal& b) {
        return a.predicted < b.predicted;
    });
    return out;
}

inline void refine_search(const Image& source,
                          std::vector<Color3f>& colors,
                          dither::DitherResult& output,
                          const dither::Settings& settings,
                          const std::vector<bool>& locked,
                          amiga::Chipset chipset,
                          bool paired) {
    const bool aga = chipset == amiga::Chipset::aga;
    if (colors.size() < 2 || colors.size() > 256 || output.indices.size() != source.pixels().size())
        return;
    if (std::count(locked.begin(), locked.end(), true) >=
        static_cast<std::ptrdiff_t>(colors.size()) - (paired ? 1 : 0))
        return;
    auto render = [&](const std::vector<Color3f>& pal, const std::vector<std::uint8_t>& idx) {
        Image result(source.width(), source.height());
        for (std::size_t p = 0; p < idx.size(); ++p)
            result.pixels()[p] = pal[idx[p]];
        return result;
    };
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), source.width(), source.height());
    float best = ssimulacra2::compute(reference, render(colors, output.indices).pixels());
    if (!std::isfinite(best)) return;
    for (int pass = 0; pass < 2; ++pass) {
        Frame frame{colors, output.indices};
        std::vector<Proposal> candidates;
        std::set<std::vector<int>> seen;
        for (bool lab : {false, true}) {
            int added = 0;
            for (auto& p : proposals(source, frame, locked, lab, aga, false, paired)) {
                if (!seen.insert(key(p.palette, aga)).second) continue;
                candidates.push_back(std::move(p));
                if (++added == 4) break;
            }
        }
        auto winner = output;
        auto palette_winner = colors;
        float score = best;
        for (const auto& p : candidates) {
            auto encoded = dither::apply(source, p.palette, settings);
            float trial = ssimulacra2::compute(reference,
                                               render(p.palette, encoded.indices).pixels());
            if (std::isfinite(trial) && trial > score + 0.001f) {
                score = trial;
                winner = std::move(encoded);
                palette_winner = p.palette;
            }
        }
        if (score <= best) break;
        best = score;
        colors = std::move(palette_winner);
        output = std::move(winner);
    }
}
// Compare both searches from the original encoding. A two-color step can
// escape a local optimum; a single-color step can win in sparse palettes.
inline void refine(const Image& source,
                   std::vector<Color3f>& colors,
                   dither::DitherResult& output,
                   const dither::Settings& settings,
                   const std::vector<bool>& locked,
                   amiga::Chipset chipset) {
    if (colors.size() < 2 || colors.size() > 256 || output.indices.size() != source.pixels().size())
        return;
    const auto original_colors = colors;
    const auto original_output = output;
    refine_search(source, colors, output, settings, locked, chipset, true);
    auto single_colors = original_colors;
    auto single_output = original_output;
    refine_search(source, single_colors, single_output, settings, locked, chipset, false);
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), source.width(), source.height());
    auto score = [&](const auto& palette, const auto& indices) {
        std::vector<Color3f> pixels(indices.size());
        for (std::size_t p = 0; p < indices.size(); ++p)
            pixels[p] = palette[indices[p]];
        return ssimulacra2::compute(reference, pixels);
    };
    if (score(single_colors, single_output.indices) > score(colors, output.indices)) {
        colors = std::move(single_colors);
        output = std::move(single_output);
    }
}
// EHB compares independent single-color and paired searches from the same
// original encoding. The caller supplies its exact EHB quantizer, including
// any dither post-pass; no candidate uses a substitute rendering path.
template<class Encode>
inline void refine_ehb(const Image& source,
                       std::vector<Color3f>& colors,
                       dither::DitherResult& output,
                       const std::vector<bool>& locked,
                       Encode encode) {
    if (colors.size() != 64 || output.indices.size() != source.pixels().size()) return;
    const auto seed_colors = colors;
    const auto seed_output = output;
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), source.width(), source.height());
    auto score = [&](const auto& pal, const auto& indices) {
        std::vector<Color3f> pixels(indices.size());
        for (std::size_t i = 0; i < indices.size(); ++i)
            pixels[i] = pal[indices[i]];
        return ssimulacra2::compute(reference, pixels);
    };
    const float baseline = score(colors, output.indices);
    if (!std::isfinite(baseline)) return;
    float overall = baseline;
    for (bool paired : {false, true}) {
        auto current_colors = seed_colors;
        auto current_output = seed_output;
        float best = baseline;
        for (int pass = 0; pass < 2; ++pass) {
            Frame frame{current_colors, current_output.indices};
            std::vector<Proposal> candidates;
            std::set<std::vector<int>> seen;
            for (bool lab : {false, true}) {
                int added = 0;
                for (auto& p : proposals(source, frame, locked, lab, false, true, paired)) {
                    if (!seen.insert(key(p.palette, false)).second) continue;
                    candidates.push_back(std::move(p));
                    if (++added == 4) break;
                }
            }
            auto winner = current_output;
            auto winner_colors = current_colors;
            float next = best;
            for (const auto& p : candidates) {
                auto encoded = encode(p.palette);
                const float trial = score(p.palette, encoded.indices);
                if (std::isfinite(trial) && trial > next + 0.001f) {
                    next = trial;
                    winner = std::move(encoded);
                    winner_colors = p.palette;
                }
            }
            if (next <= best) break;
            best = next;
            current_output = std::move(winner);
            current_colors = std::move(winner_colors);
        }
        if (best > overall) {
            overall = best;
            output = std::move(current_output);
            colors = std::move(current_colors);
        }
    }
}
}  // namespace png2amiga::palette_pairs
