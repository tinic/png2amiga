#pragma once
#include "console_color.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "pipeline.hpp"
#include "ssimulacra2.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

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
// Hardware colour grid: `snap` rounds to the nearest register value and
// `code` names that value (equal codes = equal register contents).
struct Grid {
    Color3f (*snap)(Color3f);
    int (*code)(Color3f);
};
namespace grids {
inline Color3f snap_ocs(Color3f c) { return palette::quantize_to_ocs(c); }
inline int code_ocs(Color3f c) { return palette::linear_to_ocs(c); }
inline Color3f snap_aga(Color3f c) { return palette::aga_to_linear(palette::linear_to_aga(c)); }
inline int code_aga(Color3f c) { return static_cast<int>(palette::linear_to_aga(c)); }
inline Color3f snap_thomson(Color3f c) {
    int code = thomson_code(c);
    return color_space::srgb_hex_to_linear(
        palette::thomson_rgb_hex(code >> 8, (code >> 4) & 15, code & 15));
}
inline Color3f snap_stf(Color3f c) { return palette::quantize_to_stf(c); }
inline int code_stf(Color3f c) { return palette::linear_to_stf(c); }
inline Color3f snap_vga(Color3f c) { return palette::quantize_to_vga(c); }
inline int code_vga(Color3f c) { return static_cast<int>(palette::linear_to_vga(c)); }
inline Color3f snap_ega(Color3f c) { return palette::quantize_to_ega(c); }
inline int code_ega(Color3f c) { return palette::linear_to_ega(c); }
inline Color3f snap_bgr555(Color3f c) { return console_color::bgr555_quantize(c); }
inline int code_bgr555(Color3f c) { return console_color::to_bgr555_word(c); }
}  // namespace grids
inline constexpr Grid kOcs{grids::snap_ocs, grids::code_ocs};
inline constexpr Grid kAga{grids::snap_aga, grids::code_aga};
inline constexpr Grid kThomson{grids::snap_thomson, thomson_code};
inline constexpr Grid kStf{grids::snap_stf, grids::code_stf};
inline constexpr Grid kVga{grids::snap_vga, grids::code_vga};
inline constexpr Grid kEga{grids::snap_ega, grids::code_ega};
inline constexpr Grid kBgr555{grids::snap_bgr555, grids::code_bgr555};
// The grid snap_palette / snap_to_chipset use for a plain-mode palette.
inline Grid grid_for(amiga::Chipset chipset, amiga::Mode mode = amiga::Mode::lores) {
    if (amiga::is_stf(mode)) return kStf;
    if (amiga::is_vga(mode)) return kVga;
    if (amiga::is_ega(mode)) return kEga;
    return chipset == amiga::Chipset::aga ? kAga : kOcs;
}
// Single-palette modes whose plain encode is `dither::apply` on a chosen
// palette: these run joint_refit + refine after the first dither.
inline bool plain_fit_mode(amiga::Mode m) {
    using M = amiga::Mode;
    return m == M::lores || m == M::lores_interlace || m == M::hires || m == M::hires_interlace ||
           m == M::stf_low || m == M::stf_med || m == M::ste_low || m == M::ste_med ||
           m == M::vga_13h || m == M::vga_10h || m == M::vga_12h || m == M::ega_hi;
}
inline Grid grid_of(bool aga, bool to8) { return to8 ? kThomson : aga ? kAga : kOcs; }
inline Color3f snap(Color3f c, const Grid& g) { return g.snap(c); }
inline std::vector<int> key(std::span<const Color3f> colors, const Grid& g) {
    std::vector<int> result;
    for (auto c : colors)
        result.push_back(g.code(c));
    return result;
}
inline Color3f snap(Color3f c, bool aga, bool to8 = false) { return snap(c, grid_of(aga, to8)); }
inline std::vector<int> key(std::span<const Color3f> colors, bool aga, bool to8 = false) {
    return key(colors, grid_of(aga, to8));
}
inline std::vector<Proposal> proposals(const Image& source,
                                       const Frame& f,
                                       const std::vector<bool>& locked,
                                       bool lab,
                                       const Grid& grid,
                                       bool ehb = false,
                                       bool paired = true) {
    const auto w = source.width(), h = source.height(), n = w * h,
               k = ehb ? std::size_t{32} : f.palette.size();
    std::vector<double> gram(k * k, 0);
    std::vector<Color3f> rhs(k), colors;
    for (auto c : f.palette)
        colors.push_back(space(c, lab));
    std::vector<Color3f> src(n);
    for (std::size_t p = 0; p < n; ++p)
        src[p] = space(source.pixels()[p], lab);
    // Zero between pixels: only the active entries are reset.
    std::array<float, 256> weights{};
    // Blurred indicator fields retain how palette entries mix spatially.
    // Off-diagonal terms couple entries that occur together in the filter.
    for (std::size_t p = 0; p < n; ++p) {
        const int x = static_cast<int>(p % w), y = static_cast<int>(p / w);
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
                target += src[q] * a;
                rendered += colors[f.indices[q]] * a;
            }
        auto residual = target - rendered;
        for (auto a : std::span(active).first(active_count)) {
            rhs[a] += residual * weights[a];
            for (auto b : std::span(active).first(active_count))
                gram[a * k + b] += static_cast<double>(weights[a]) *
                                   static_cast<double>(weights[b]);
        }
        for (auto a : std::span(active).first(active_count))
            weights[a] = 0.0f;
    }
    std::vector<Proposal> out;
    const auto original_key = key(std::span(f.palette).first(k), grid);
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
                pal[a] = snap(unspace(colors[a] + da * step, lab), grid);
                if (paired) pal[b] = snap(unspace(colors[b] + db * step, lab), grid);
                if (ehb) {
                    pal[a + 32] = palette::half_brite(pal[a]);
                    pal[b + 32] = palette::half_brite(pal[b]);
                }
                auto ka = key(std::span(pal).first(k), grid);
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

// Encodes and scores every palette in `pals`; results in input order. With
// `concurrent` the trials run in parallel, so `encode` must be safe to call
// from several threads at once.
template<class Encode, class Score>
inline void encode_all(const std::vector<const std::vector<Color3f>*>& pals,
                       Encode& encode,
                       const Score& score,
                       bool concurrent,
                       std::vector<dither::DitherResult>& encoded,
                       std::vector<float>& scores) {
    encoded.clear();
    encoded.resize(pals.size());
    scores.assign(pals.size(), 0.0f);
    auto one = [&](std::size_t i) {
        encoded[i] = encode(*pals[i]);
        scores[i] = score(*pals[i], encoded[i].indices);
    };
    if (concurrent && pals.size() > 1) {
        pipeline::parallel_for(pals.size(), one);
    } else {
        for (std::size_t i = 0; i < pals.size(); ++i)
            one(i);
    }
}
inline float score_indexed(const ssimulacra2::PrecomputedSource& reference,
                           const std::vector<Color3f>& pal,
                           const std::vector<std::uint8_t>& idx) {
    std::vector<Color3f> px(idx.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        px[i] = pal[idx[i]];
    return ssimulacra2::compute(reference, px);
}
// Returns the S2 of the final (colors, output): `best` when nothing is
// kept or the search does not run.
template<class Encode>
inline float refine_search(const Image& source,
                           std::vector<Color3f>& colors,
                           dither::DitherResult& output,
                           Encode& encode,
                           const std::vector<bool>& locked,
                           const Grid& grid,
                           bool paired,
                           const ssimulacra2::PrecomputedSource& reference,
                           float best,
                           bool concurrent) {
    if (colors.size() < 2 || colors.size() > 256 || output.indices.size() != source.pixels().size())
        return best;
    if (std::count(locked.begin(), locked.end(), true) >=
        static_cast<std::ptrdiff_t>(colors.size()) - (paired ? 1 : 0))
        return best;
    if (!std::isfinite(best)) return best;
    auto score_fn = [&](const std::vector<Color3f>& pal, const std::vector<std::uint8_t>& idx) {
        return score_indexed(reference, pal, idx);
    };
    for (int pass = 0; pass < 2; ++pass) {
        Frame frame{colors, output.indices};
        std::vector<Proposal> candidates;
        std::set<std::vector<int>> seen;
        std::array<std::vector<Proposal>, 2> by_lab;
        pipeline::parallel_for(2, [&](std::size_t i) {
            by_lab[i] = proposals(source, frame, locked, i == 1, grid, false, paired);
        });
        for (auto& list : by_lab) {
            int added = 0;
            for (auto& p : list) {
                if (!seen.insert(key(p.palette, grid)).second) continue;
                candidates.push_back(std::move(p));
                if (++added == 4) break;
            }
        }
        std::vector<const std::vector<Color3f>*> pals;
        for (const auto& p : candidates)
            pals.push_back(&p.palette);
        std::vector<dither::DitherResult> encoded;
        std::vector<float> trials;
        encode_all(pals, encode, score_fn, concurrent, encoded, trials);
        std::size_t win = candidates.size();
        float score = best;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (std::isfinite(trials[i]) && trials[i] > score + 0.001f) {
                score = trials[i];
                win = i;
            }
        }
        if (score <= best) break;
        best = score;
        colors = candidates[win].palette;
        output = std::move(encoded[win]);
    }
    return best;
}
// Solves A x = b in place for symmetric positive definite A (n x n) and three
// right-hand sides (b is n x 3). Returns false when A is not positive definite.
inline bool cholesky_solve3(std::vector<double>& a, std::vector<double>& b, std::size_t n) {
    for (std::size_t j = 0; j < n; ++j) {
        double s = a[j * n + j];
        for (std::size_t k = 0; k < j; ++k)
            s -= a[j * n + k] * a[j * n + k];
        if (s <= 1e-12) return false;
        a[j * n + j] = std::sqrt(s);
        for (std::size_t i = j + 1; i < n; ++i) {
            double t = a[i * n + j];
            for (std::size_t k = 0; k < j; ++k)
                t -= a[i * n + k] * a[j * n + k];
            a[i * n + j] = t / a[j * n + j];
        }
    }
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t i = 0; i < n; ++i) {
            double t = b[i * 3 + c];
            for (std::size_t k = 0; k < i; ++k)
                t -= a[i * n + k] * b[k * 3 + c];
            b[i * 3 + c] = t / a[i * n + i];
        }
        for (std::size_t i = n; i-- > 0;) {
            double t = b[i * 3 + c];
            for (std::size_t k = i + 1; k < n; ++k)
                t -= a[k * n + i] * b[k * 3 + c];
            b[i * 3 + c] = t / a[i * n + i];
        }
    }
    return true;
}
// Joint refit of every unlocked entry against the blurred dithered result.
// Works in sRGB, the space where error diffusion keeps local means, with a
// 5x5 binomial blur. Solves all entries at once (ridge toward the current
// palette), re-encodes with the caller's exact encoder at steps 0.5 and 1.0,
// and keeps a step only if S2 improves; up to 4 iterations. EHB: `colors`
// holds 64 entries, the half-brite twin enters at gain 0.5 (exact in sRGB)
// and is re-derived from its base. Runs before refine / refine_ehb.
template<class Encode>
inline void joint_refit(const Image& source,
                        std::vector<Color3f>& colors,
                        dither::DitherResult& output,
                        const std::vector<bool>& locked,
                        const Grid& grid,
                        bool ehb,
                        Encode encode,
                        bool concurrent = false) {
    const auto w = source.width(), h = source.height(), n = w * h;
    if (output.indices.size() != n || colors.size() < 2 || colors.size() > 256) return;
    if (ehb && colors.size() != 64) return;
    const auto kb = ehb ? std::size_t{32} : colors.size();
    constexpr int kIterations = 4;
    constexpr int r = 2;
    constexpr std::array<float, 5> taps{1 / 16.0f, 4 / 16.0f, 6 / 16.0f, 4 / 16.0f, 1 / 16.0f};
    auto to_s = [](Color3f lin) { return color_space::linear_to_srgb(lin.clamped()).clamped(); };
    std::vector<Color3f> src_s(n);
    for (std::size_t i = 0; i < n; ++i)
        src_s[i] = to_s(source.pixels()[i]);
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), w, h);
    auto score = [&](const std::vector<Color3f>& pal, const std::vector<std::uint8_t>& idx) {
        return score_indexed(reference, pal, idx);
    };
    float best = score(colors, output.indices);
    if (!std::isfinite(best)) return;
    auto is_locked = [&](std::size_t k) { return k < locked.size() && locked[k]; };
    // The blurred source does not change between iterations.
    std::vector<Color3f> blur_s(n);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) {
            Color3f target{};
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    int xx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1);
                    int yy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1);
                    auto q = static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx);
                    float a = taps[static_cast<std::size_t>(dx + r)] *
                              taps[static_cast<std::size_t>(dy + r)];
                    target += src_s[q] * a;
                }
            blur_s[y * w + x] = target;
        }
    for (int it = 0; it < kIterations; ++it) {
        std::vector<double> gram(kb * kb, 0.0), rhs(kb * 3, 0.0);
        std::vector<float> wts(kb, 0.0f);
        std::vector<std::size_t> active;
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) {
                active.clear();
                const Color3f target = blur_s[y * w + x];
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        int xx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1);
                        int yy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1);
                        auto q = static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx);
                        float a = taps[static_cast<std::size_t>(dx + r)] *
                                  taps[static_cast<std::size_t>(dy + r)];
                        auto idx = output.indices[q];
                        auto k = ehb ? static_cast<std::size_t>(idx % 32) : std::size_t{idx};
                        float g = (ehb && idx >= 32) ? 0.5f : 1.0f;
                        if (wts[k] == 0.0f) active.push_back(k);
                        wts[k] += a * g;
                    }
                for (auto a : active) {
                    rhs[a * 3 + 0] += static_cast<double>(wts[a] * target.r);
                    rhs[a * 3 + 1] += static_cast<double>(wts[a] * target.g);
                    rhs[a * 3 + 2] += static_cast<double>(wts[a] * target.b);
                    for (auto b : active)
                        gram[a * kb + b] +=
                            static_cast<double>(wts[a]) * static_cast<double>(wts[b]);
                }
                for (auto a : active)
                    wts[a] = 0.0f;
            }
        // Locked or unused entries stay fixed: their terms move to the rhs.
        std::vector<Color3f> cur_s(kb);
        for (std::size_t k = 0; k < kb; ++k)
            cur_s[k] = to_s(colors[k]);
        std::vector<std::size_t> free_idx;
        std::vector<bool> is_free(kb, false);
        for (std::size_t k = 0; k < kb; ++k)
            if (!is_locked(k) && gram[k * kb + k] > 1e-3) {
                free_idx.push_back(k);
                is_free[k] = true;
            }
        const auto m = free_idx.size();
        if (m == 0) return;
        std::vector<double> A(m * m), B(m * 3);
        for (std::size_t i = 0; i < m; ++i) {
            const auto a = free_idx[i];
            for (std::size_t c = 0; c < 3; ++c) {
                double v = rhs[a * 3 + c];
                for (std::size_t b = 0; b < kb; ++b)
                    if (!is_free[b]) {
                        const float cb = c == 0 ? cur_s[b].r : c == 1 ? cur_s[b].g : cur_s[b].b;
                        v -= gram[a * kb + b] * static_cast<double>(cb);
                    }
                B[i * 3 + c] = v;
            }
            for (std::size_t j = 0; j < m; ++j)
                A[i * m + j] = gram[a * kb + free_idx[j]];
        }
        // Ridge toward the current palette keeps sparsely used entries stable.
        for (std::size_t i = 0; i < m; ++i) {
            const double lam = 0.05 * A[i * m + i];
            A[i * m + i] += lam;
            const auto c = cur_s[free_idx[i]];
            B[i * 3 + 0] += lam * static_cast<double>(c.r);
            B[i * 3 + 1] += lam * static_cast<double>(c.g);
            B[i * 3 + 2] += lam * static_cast<double>(c.b);
        }
        if (!cholesky_solve3(A, B, m)) return;
        float step_best = best;
        std::vector<Color3f> pal_best;
        dither::DitherResult out_best;
        std::array<std::vector<Color3f>, 2> steps;
        for (std::size_t si = 0; si < 2; ++si) {
            const float t = si == 0 ? 0.5f : 1.0f;
            auto& pal = steps[si];
            pal = colors;
            for (std::size_t i = 0; i < m; ++i) {
                const auto k = free_idx[i];
                const Color3f sol{static_cast<float>(B[i * 3 + 0]),
                                  static_cast<float>(B[i * 3 + 1]),
                                  static_cast<float>(B[i * 3 + 2])};
                const Color3f s = (cur_s[k] + (sol - cur_s[k]) * t).clamped();
                pal[k] = snap(color_space::srgb_to_linear(s), grid);
                if (ehb) pal[k + 32] = palette::half_brite(pal[k]);
            }
        }
        // Trials that cannot be kept are not encoded: a step equal to the
        // first one scores the same and needs a strictly higher score; after
        // a kept iteration `output` is encode(colors), so a step equal to
        // `colors` scores `best`.
        std::vector<const std::vector<Color3f>*> pals;
        for (std::size_t si = 0; si < 2; ++si) {
            if (si == 1 && steps[1] == steps[0]) continue;
            if (it > 0 && steps[si] == colors) continue;
            pals.push_back(&steps[si]);
        }
        std::vector<dither::DitherResult> encoded;
        std::vector<float> trials;
        encode_all(pals, encode, score, concurrent, encoded, trials);
        for (std::size_t i = 0; i < pals.size(); ++i) {
            if (std::isfinite(trials[i]) && trials[i] > step_best + 0.001f) {
                step_best = trials[i];
                pal_best = *pals[i];
                out_best = std::move(encoded[i]);
            }
        }
        if (pal_best.empty()) break;
        best = step_best;
        colors = std::move(pal_best);
        output = std::move(out_best);
    }
}
// Compare both searches from the original encoding. A two-color step can
// escape a local optimum; a single-color step can win in sparse palettes.
// `encode` maps a palette to the caller's exact indexed encoding.
// `concurrent`: see encode_all.
template<class Encode>
inline void refine_with(const Image& source,
                        std::vector<Color3f>& colors,
                        dither::DitherResult& output,
                        const std::vector<bool>& locked,
                        const Grid& grid,
                        Encode encode,
                        bool concurrent = false) {
    if (colors.size() < 2 || colors.size() > 256 || output.indices.size() != source.pixels().size())
        return;
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), source.width(), source.height());
    // One score of the starting point serves both searches; each returns
    // the score of what it ends with.
    const float start = score_indexed(reference, colors, output.indices);
    const auto original_colors = colors;
    const auto original_output = output;
    const float paired_score = refine_search(
        source, colors, output, encode, locked, grid, true, reference, start, concurrent);
    auto single_colors = original_colors;
    auto single_output = original_output;
    const float single_score = refine_search(source,
                                             single_colors,
                                             single_output,
                                             encode,
                                             locked,
                                             grid,
                                             false,
                                             reference,
                                             start,
                                             concurrent);
    if (single_score > paired_score) {
        colors = std::move(single_colors);
        output = std::move(single_output);
    }
}
inline void refine(const Image& source,
                   std::vector<Color3f>& colors,
                   dither::DitherResult& output,
                   const dither::Settings& settings,
                   const std::vector<bool>& locked,
                   const Grid& grid) {
    dither::PrecomputedImage pre;
    pre.build(source);
    refine_with(
        source,
        colors,
        output,
        locked,
        grid,
        [&](const std::vector<Color3f>& p) {
            return dither::apply(source, std::span<const Color3f>(p), settings, &pre);
        },
        /*concurrent=*/true);
}
inline void refine(const Image& source,
                   std::vector<Color3f>& colors,
                   dither::DitherResult& output,
                   const dither::Settings& settings,
                   const std::vector<bool>& locked,
                   amiga::Chipset chipset) {
    refine(source, colors, output, settings, locked, grid_for(chipset));
}
// EHB compares independent single-color and paired searches from the same
// original encoding. The caller supplies its exact EHB quantizer, including
// any dither post-pass; no candidate uses a substitute rendering path.
template<class Encode>
inline void refine_ehb(const Image& source,
                       std::vector<Color3f>& colors,
                       dither::DitherResult& output,
                       const std::vector<bool>& locked,
                       Encode encode,
                       bool concurrent = false) {
    if (colors.size() != 64 || output.indices.size() != source.pixels().size()) return;
    const auto seed_colors = colors;
    const auto seed_output = output;
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), source.width(), source.height());
    auto score = [&](const std::vector<Color3f>& pal, const std::vector<std::uint8_t>& indices) {
        return score_indexed(reference, pal, indices);
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
                for (auto& p : proposals(source, frame, locked, lab, kOcs, true, paired)) {
                    if (!seen.insert(key(p.palette, false)).second) continue;
                    candidates.push_back(std::move(p));
                    if (++added == 4) break;
                }
            }
            std::vector<const std::vector<Color3f>*> pals;
            for (const auto& p : candidates)
                pals.push_back(&p.palette);
            std::vector<dither::DitherResult> encoded;
            std::vector<float> trials;
            encode_all(pals, encode, score, concurrent, encoded, trials);
            std::size_t win = candidates.size();
            float next = best;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (std::isfinite(trials[i]) && trials[i] > next + 0.001f) {
                    next = trials[i];
                    win = i;
                }
            }
            if (next <= best) break;
            best = next;
            current_output = std::move(encoded[win]);
            current_colors = candidates[win].palette;
        }
        if (best > overall) {
            overall = best;
            output = std::move(current_output);
            colors = std::move(current_colors);
        }
    }
}
}  // namespace png2amiga::palette_pairs
