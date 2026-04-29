#include "ham.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "pipeline.hpp"
#include "quantize.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace png2amiga::ham {

// SRGBColor / HamPixelResult / HamPrecomp / encode_ham_pixel are
// declared in ham.hpp. Their definitions live in this TU.
SRGBColor linear_to_srgb8(Color3f c) {
    auto srgb = color_space::linear_to_srgb(c).clamped();
    return {
        static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f)),
        static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f)),
        static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f)),
    };
}

Color3f srgb8_to_linear(SRGBColor c) {
    return color_space::srgb_u8_to_linear(c.r, c.g, c.b);
}

// HamPrecomp definition (declared in ham.hpp).
HamPrecomp::HamPrecomp(std::span<const Color3f> palette, std::size_t db)
    : data_bits(db), num_data_values(std::size_t{1} << db) {
    palette_lab.resize(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i)
        palette_lab[i] = color_space::linear_to_oklab(palette[i]);
    expand_lut.resize(num_data_values);
    for (std::size_t v = 0; v < num_data_values; ++v)
        expand_lut[v] = expand_to_8bit(static_cast<std::uint8_t>(v), db);
}

namespace {

// ===========================================================================
// OKLab helpers (mirroring dither.cpp's internal helpers)
// ===========================================================================

using OKLab = color_space::OKLab;

constexpr OKLab oklab_add(OKLab a, OKLab b) noexcept {
    return {a.L + b.L, a.a + b.a, a.b + b.b};
}

constexpr OKLab oklab_sub(OKLab a, OKLab b) noexcept {
    return {a.L - b.L, a.a - b.a, a.b - b.b};
}

constexpr OKLab oklab_scale(OKLab v, float s) noexcept {
    return {v.L * s, v.a * s, v.b * s};
}

constexpr OKLab oklab_clamp(OKLab e, float max_mag) noexcept {
    return {
        std::clamp(e.L, -max_mag, max_mag),
        std::clamp(e.a, -max_mag, max_mag),
        std::clamp(e.b, -max_mag, max_mag),
    };
}

// ===========================================================================
// Error diffusion kernels (same as dither.cpp)
// ===========================================================================

// Error-diffusion kernels live in dither.cpp; HAM uses
// dither::uses_error_diffusion to gate its pre-dither pass.

// ===========================================================================
// HAM encoding core (generic for any data_bits)
//
// For each scanline, we process pixels left to right. At each pixel we
// choose the operation (set palette color, or modify R/G/B) that produces
// the color closest to the target.
//
// The "previous pixel color" state carries across the scanline. At the
// start of each scanline it resets to the background color (palette[0]).
// ===========================================================================

// ---------------------------------------------------------------------------
// Generic HAM pixel encoding (greedy)
//
// Works for any data_bits (2-6). The value space is:
//   - 2^data_bits SET operations (palette index)
//   - 3 * 2^data_bits MODIFY operations (R, G, B channels)
// ---------------------------------------------------------------------------

// Encode a HAM value: combine control bits and data bits.
// HAM6 (6 planes): control in bits 5-4 (top 2), data in bits 3-0
// Standard Amiga HAM convention: control bits live in the TOP 2 bits of
// the encoded value (i.e. the highest-numbered bitplanes), data in the
// remaining low bits. Holds for HAM6 (planes 4-5 control, 0-3 data) and
// HAM8 (planes 6-7 control, 0-5 data) — the chip and every standard
// reader (DPaint, ViewTek, RECOIL) interpret it this way.
constexpr std::uint8_t make_ham_value(std::uint8_t control, std::uint8_t data,
                                      std::size_t data_bits) noexcept {
    return static_cast<std::uint8_t>((control << data_bits) | data);
}

// Extract control and data from a HAM value (inverse of make_ham_value):
// control = top 2 bits, data = low data_bits bits. Same convention for
// HAM6 (data_bits=4) and HAM8 (data_bits=6).
constexpr std::pair<std::uint8_t, std::uint8_t>
split_ham_value(std::uint8_t value, std::size_t data_bits) noexcept {
    auto data_mask = static_cast<std::uint8_t>((1u << data_bits) - 1u);
    return {static_cast<std::uint8_t>(value >> data_bits),
            static_cast<std::uint8_t>(value & data_mask)};
}

// HAM op-selection scorers. Two metrics are compiled in:
// - sRGB-MSE: matches our reported PSNR metric and HAM hardware
//   semantics (per-channel sRGB DAC). Default. +0.5..+1 dB headline.
// - OKLab²: perceptually uniform. Better on banded/HDR content
//   (chuck31 is +3.5 OKLab-dB better here) at the cost of PSNR.
//
// Both score functions return values scaled to OKLab²-magnitude so
// cumulative_error and refine_triple's skip_threshold_per_pixel are
// commensurate across both metrics.
//
// Selection is compile-time per encode_scanline_* call via a HamMetric
// template parameter — `if constexpr` eliminates the branch entirely
// from the hot inner loop. Caller dispatches once per scanline.
inline float score_srgb_mse(SRGBColor a, SRGBColor b) {
    int dr = static_cast<int>(a.r) - static_cast<int>(b.r);
    int dg = static_cast<int>(a.g) - static_cast<int>(b.g);
    int db = static_cast<int>(a.b) - static_cast<int>(b.b);
    return static_cast<float>(dr * dr + dg * dg + db * db) * (1.0f / 65536.0f);
}

inline float score_oklab2(const OKLab& a, const OKLab& b) {
    float dL = a.L - b.L;
    float da = a.a - b.a;
    float db_ = a.b - b.b;
    return dL * dL + da * da + db_ * db_;
}

template <HamMetric M>
HamPixelResult encode_ham_pixel_t(
    SRGBColor prev,
    Color3f target,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb) {

    auto data_bits = pre.data_bits;

    HamPixelResult best;
    best.error = std::numeric_limits<float>::max();

    auto target_srgb = linear_to_srgb8(target);
    [[maybe_unused]] OKLab target_lab;
    if constexpr (M == HamMetric::oklab2)
        target_lab = color_space::linear_to_oklab(target);

    auto score = [&](SRGBColor out, std::size_t pal_idx_for_set,
                     bool is_set) -> float {
        if constexpr (M == HamMetric::srgb_mse) {
            (void)pal_idx_for_set; (void)is_set;
            return score_srgb_mse(target_srgb, out);
        } else {
            if (is_set) return score_oklab2(target_lab, pre.palette_lab[pal_idx_for_set]);
            return score_oklab2(target_lab,
                color_space::linear_to_oklab(srgb8_to_linear(out)));
        }
    };

    // Option 1: SET palette color (control = 00)
    for (std::size_t i = 0; i < base_srgb.size(); ++i) {
        float err = score(base_srgb[i], i, true);
        if (err < best.error) {
            best.error = err;
            best.value = make_ham_value(0b00, static_cast<std::uint8_t>(i), data_bits);
            best.result_color = base_srgb[i];
        }
    }

    // Option 2: MODIFY BLUE (control = 01)
    {
        auto b_data = reduce_to_bits(target_srgb.b, data_bits);
        auto b8 = pre.expand_lut[b_data];
        SRGBColor modified{prev.r, prev.g, b8};
        float err = score(modified, 0, false);
        if (err < best.error) {
            best.error = err;
            best.value = make_ham_value(0b01, b_data, data_bits);
            best.result_color = modified;
        }
    }

    // Option 3: MODIFY RED (control = 10)
    {
        auto r_data = reduce_to_bits(target_srgb.r, data_bits);
        auto r8 = pre.expand_lut[r_data];
        SRGBColor modified{r8, prev.g, prev.b};
        float err = score(modified, 0, false);
        if (err < best.error) {
            best.error = err;
            best.value = make_ham_value(0b10, r_data, data_bits);
            best.result_color = modified;
        }
    }

    // Option 4: MODIFY GREEN (control = 11)
    {
        auto g_data = reduce_to_bits(target_srgb.g, data_bits);
        auto g8 = pre.expand_lut[g_data];
        SRGBColor modified{prev.r, g8, prev.b};
        float err = score(modified, 0, false);
        if (err < best.error) {
            best.error = err;
            best.value = make_ham_value(0b11, g_data, data_bits);
            best.result_color = modified;
        }
    }

    return best;
}

// Backward-compat non-templated wrapper (defaults to sRGB-MSE).
inline HamPixelResult encode_ham_pixel_impl(
    SRGBColor prev,
    Color3f target,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    HamMetric metric = HamMetric::srgb_mse) {
    if (metric == HamMetric::oklab2)
        return encode_ham_pixel_t<HamMetric::oklab2>(prev, target, pre, base_srgb);
    return encode_ham_pixel_t<HamMetric::srgb_mse>(prev, target, pre, base_srgb);
}

// ---------------------------------------------------------------------------
// Choose a base palette for HAM from the image
// ---------------------------------------------------------------------------

Palette choose_ham_palette(const Image& image, std::size_t num_colors,
                           amiga::Chipset chipset,
                           int palette_diversity,
                           std::string_view quantizer) {
    // Quantize N-1 colors, reserve index 0 for black (border/HAM start).
    // Diversity is applied AFTER OCS snap so the SSE objective reflects the
    // actual discrete palette HAM6 will use. (Running it inside median_cut
    // would evaluate against the continuous palette that HAM never sees.)
    auto reserve = (num_colors > 1) ? num_colors - 1 : std::size_t{1};

    // Quantizer selection:
    //   - explicit "pnn": PNN agglomerative in OKLab
    //   - explicit "median-cut" / "fast": median-cut + k-means
    //   - empty / auto: PNN for AGA (HAM8), median-cut otherwise.
    //     Benchmarks showed PNN wins ~10-13% on HAM8 across fantasy/photo/
    //     space3 but regresses on HAM6 OCS, so only opt in for AGA.
    bool use_pnn;
    if (quantizer == "pnn")             use_pnn = true;
    else if (quantizer == "median-cut") use_pnn = false;
    else if (quantizer == "fast")       use_pnn = false;
    else                                use_pnn = (chipset == amiga::Chipset::aga);

    auto pal = use_pnn
        ? quantize::pnn_quantize(image.pixels(), reserve, /*diversity=*/0)
        : quantize::median_cut(image.pixels(), reserve, /*diversity=*/0);

    bool is_ocs = (chipset != amiga::Chipset::aga);
    if (is_ocs) {
        for (auto& color : pal.colors) {
            color = palette::quantize_to_ocs(color);
        }
    }

    if (palette_diversity > 0) {
        quantize::diversify_palette(pal, image.pixels(),
                                    palette_diversity, is_ocs);
    }

    // Prepend black at index 0
    pal.colors.insert(pal.colors.begin(), Color3f{0.0f, 0.0f, 0.0f});

    return pal;
}

// ===========================================================================
// DP beam search encoder (generic)
//
// Instead of greedily picking the locally best operation at each pixel,
// we maintain a "beam" of candidate states. At each pixel position, we
// expand every beam state by all possible HAM operations, compute the
// new cumulative error, and keep only the top beam_width states.
// ===========================================================================

struct BeamState {
    SRGBColor color;            // output sRGB color after this pixel
    float cumulative_error;     // total error from scanline start up to here
    std::uint8_t ham_value;     // the HAM value chosen at the current pixel
    std::uint16_t parent_idx;   // index into previous beam's state array
};

// Shared thread-local OKLab cache for HAM encoding. sRGB (24-bit) → OKLab
// memoization with closed hashing and generation-bump invalidation.
// MODIFY ops repeatedly hit the same output colors (same prev.r/prev.g +
// 64 b values over multiple beam states), so caching skips a large
// fraction of cbrt + matrix multiplies.
constexpr std::size_t kHamOklabCacheSize = 2048;
struct HamOklabCacheEntry { std::uint32_t key; std::uint32_t gen; OKLab lab; };

thread_local std::vector<HamOklabCacheEntry>
    g_ham_oklab_cache(kHamOklabCacheSize, {0, 0, {}});
thread_local std::uint32_t g_ham_oklab_gen = 0;

inline void bump_ham_oklab_cache_gen() {
    if (++g_ham_oklab_gen == 0) {
        for (auto& e : g_ham_oklab_cache) e.gen = 0;
        g_ham_oklab_gen = 1;
    }
}

inline OKLab cached_srgb_to_oklab(SRGBColor c) {
    std::uint32_t key = (static_cast<std::uint32_t>(c.r) << 16) |
                        (static_cast<std::uint32_t>(c.g) << 8) |
                        static_cast<std::uint32_t>(c.b);
    std::uint32_t slot = (key * 2654435761u) & (kHamOklabCacheSize - 1);
    auto& e = g_ham_oklab_cache[slot];
    if (e.gen == g_ham_oklab_gen && e.key == key) return e.lab;
    OKLab lab = color_space::srgb8_to_oklab(c.r, c.g, c.b);
    e.key = key;
    e.gen = g_ham_oklab_gen;
    e.lab = lab;
    return lab;
}


// Expand all reasonable HAM operations from a given previous color state.
// MODIFY ops are restricted to a window of data values around the target's
// bit-projection (±kModifyDpRadius), which is a near-lossless heuristic:
// the optimal MODIFY data for a channel is nearly always within a few
// positions of `reduce_to_bits(target.channel)`, because anything further
// would produce a color farther from target than the bit-projection itself.
// Eliminates ~70% of MODIFY evaluations for HAM8 with no measurable
// quality loss.
constexpr int kModifyDpRadius = 2;

// Compile-time-dispatched expand_ham. The HamMetric template parameter
// selects the score function via `if constexpr`, so the inner loop has
// zero metric-dispatch overhead — both specializations are fully
// inlined hot loops. Caller picks the specialization once per scanline
// (not per pixel, not per candidate).
template <HamMetric M>
void expand_ham_t(
    SRGBColor prev,
    OKLab target_lab,
    SRGBColor target_srgb,
    float prev_error,
    std::uint16_t parent_idx,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::vector<BeamState>& candidates) {

    auto data_bits = pre.data_bits;
    auto num_data_values = pre.num_data_values;
    auto nmax = static_cast<int>(num_data_values);

    // SET palette color (control = 00)
    for (std::size_t i = 0; i < pre.palette_lab.size(); ++i) {
        float err;
        if constexpr (M == HamMetric::srgb_mse) {
            err = score_srgb_mse(target_srgb, base_srgb[i]);
        } else {
            err = score_oklab2(target_lab, pre.palette_lab[i]);
        }
        candidates.push_back({
            base_srgb[i],
            prev_error + err,
            make_ham_value(0b00, static_cast<std::uint8_t>(i), data_bits),
            parent_idx,
        });
    }

    auto tbv = reduce_to_bits(target_srgb.b, data_bits);
    auto trv = reduce_to_bits(target_srgb.r, data_bits);
    auto tgv = reduce_to_bits(target_srgb.g, data_bits);

    // For OKLab² scoring we need linear-LMS partials of `prev` so we can
    // cheaply derive each modified output's OKLab. sRGB-MSE doesn't need
    // them, so guard with `if constexpr` to keep that path zero-cost.
    auto& lms_t = color_space::detail::srgb_lms_lut();
    [[maybe_unused]] color_space::f32x4 lms_prev_r;
    [[maybe_unused]] color_space::f32x4 lms_prev_g;
    [[maybe_unused]] color_space::f32x4 lms_prev_b;
    if constexpr (M == HamMetric::oklab2) {
        lms_prev_r = lms_t[0][prev.r];
        lms_prev_g = lms_t[1][prev.g];
        lms_prev_b = lms_t[2][prev.b];
    }

    // MODIFY BLUE (control = 01) — focused around target
    auto lo_b = std::max(0, static_cast<int>(tbv) - kModifyDpRadius);
    auto hi_b = std::min(nmax, static_cast<int>(tbv) + kModifyDpRadius + 1);
    {
        [[maybe_unused]] color_space::f32x4 rg_lms;
        if constexpr (M == HamMetric::oklab2)
            rg_lms = lms_prev_r + lms_prev_g;
        for (int bv = lo_b; bv < hi_b; ++bv) {
            auto b8 = pre.expand_lut[static_cast<std::size_t>(bv)];
            SRGBColor modified{prev.r, prev.g, b8};
            float err;
            if constexpr (M == HamMetric::srgb_mse) {
                err = score_srgb_mse(target_srgb, modified);
            } else {
                auto lab = color_space::lms_cbrt_to_oklab(
                    color_space::fast_cbrt4(rg_lms + lms_t[2][b8]));
                err = score_oklab2(target_lab, lab);
            }
            candidates.push_back({
                modified, prev_error + err,
                make_ham_value(0b01, static_cast<std::uint8_t>(bv), data_bits),
                parent_idx,
            });
        }
    }

    // MODIFY RED (control = 10)
    auto lo_r = std::max(0, static_cast<int>(trv) - kModifyDpRadius);
    auto hi_r = std::min(nmax, static_cast<int>(trv) + kModifyDpRadius + 1);
    {
        [[maybe_unused]] color_space::f32x4 gb_lms;
        if constexpr (M == HamMetric::oklab2)
            gb_lms = lms_prev_g + lms_prev_b;
        for (int rv = lo_r; rv < hi_r; ++rv) {
            auto r8 = pre.expand_lut[static_cast<std::size_t>(rv)];
            SRGBColor modified{r8, prev.g, prev.b};
            float err;
            if constexpr (M == HamMetric::srgb_mse) {
                err = score_srgb_mse(target_srgb, modified);
            } else {
                auto lab = color_space::lms_cbrt_to_oklab(
                    color_space::fast_cbrt4(lms_t[0][r8] + gb_lms));
                err = score_oklab2(target_lab, lab);
            }
            candidates.push_back({
                modified, prev_error + err,
                make_ham_value(0b10, static_cast<std::uint8_t>(rv), data_bits),
                parent_idx,
            });
        }
    }

    // MODIFY GREEN (control = 11)
    auto lo_g = std::max(0, static_cast<int>(tgv) - kModifyDpRadius);
    auto hi_g = std::min(nmax, static_cast<int>(tgv) + kModifyDpRadius + 1);
    {
        [[maybe_unused]] color_space::f32x4 rb_lms;
        if constexpr (M == HamMetric::oklab2)
            rb_lms = lms_prev_r + lms_prev_b;
        for (int gv = lo_g; gv < hi_g; ++gv) {
            auto g8 = pre.expand_lut[static_cast<std::size_t>(gv)];
            SRGBColor modified{prev.r, g8, prev.b};
            float err;
            if constexpr (M == HamMetric::srgb_mse) {
                err = score_srgb_mse(target_srgb, modified);
            } else {
                auto lab = color_space::lms_cbrt_to_oklab(
                    color_space::fast_cbrt4(rb_lms + lms_t[1][g8]));
                err = score_oklab2(target_lab, lab);
            }
            candidates.push_back({
                modified, prev_error + err,
                make_ham_value(0b11, static_cast<std::uint8_t>(gv), data_bits),
                parent_idx,
            });
        }
    }
}

// Non-templated trampoline preserving the legacy signature (callers that
// don't yet propagate HamMetric — e.g. the inner expand_ham invocation
// inside refine_triple's window beam — funnel through this. Inlines to a
// single template call once the caller's metric is known at compile
// time.) Default keeps the new sRGB-MSE behavior.
inline void expand_ham(
    SRGBColor prev,
    OKLab target_lab,
    SRGBColor target_srgb,
    float prev_error,
    std::uint16_t parent_idx,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::vector<BeamState>& candidates) {
    expand_ham_t<HamMetric::srgb_mse>(prev, target_lab, target_srgb,
        prev_error, parent_idx, pre, base_srgb, candidates);
}

// Prune candidates to the top beam_width by cumulative error.
void prune_beam(std::vector<BeamState>& candidates,
                std::vector<BeamState>& beam,
                std::size_t beam_width) {
    beam.clear();

    if (candidates.size() <= beam_width) {
        beam = std::move(candidates);
        return;
    }

    // partial_sort's heap selection outperforms nth_element for small N
    // (tested: beam candidates are ~300, heap locality wins over
    //  nth_element's quickselect swaps).
    std::partial_sort(
        candidates.begin(),
        candidates.begin() + static_cast<std::ptrdiff_t>(beam_width),
        candidates.end(),
        [](const BeamState& a, const BeamState& b) {
            return a.cumulative_error < b.cumulative_error;
        });

    beam.assign(candidates.begin(),
                candidates.begin() + static_cast<std::ptrdiff_t>(beam_width));
}

// DP beam search for a single scanline. ScanlineResult is declared
// in ham.hpp now so SCAP and other consumers can use it.
// Per-strip variant: takes parallel arrays of HamPrecomp and base_srgb
// (one entry per strip) plus a per-pixel strip-index lookup. The DP
// rolling state crosses strip boundaries unchanged — only the palette
// consulted by expand_ham swaps. Used by SCAP HAM6 to drive row-level
// DP beam search with mid-line palette swaps.
template <HamMetric M>
ScanlineResult encode_scanline_dp_per_strip_t(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_width) {

    auto width = target_row.size();
    if (width == 0) return {{}, 0.0f};
    if (pres.empty() || base_srgbs.empty()) return {{}, 0.0f};

    std::vector<std::vector<BeamState>> beam_history(width);
    std::vector<BeamState> candidates;
    std::vector<BeamState> current_beam;
    auto ops_per_state = pres[0].palette_lab.size() +
                         static_cast<std::size_t>(3 * (2 * kModifyDpRadius + 1));
    candidates.reserve(beam_width * ops_per_state);

    std::vector<OKLab> row_lab(width);
    std::vector<SRGBColor> row_srgb(width);
    for (std::size_t x = 0; x < width; ++x) {
        row_lab[x] = color_space::linear_to_oklab(target_row[x]);
        row_srgb[x] = linear_to_srgb8(target_row[x]);
    }

    std::vector<BeamState> prev_beam;
    prev_beam.push_back({start_color, 0.0f, 0, 0});
    for (std::size_t x = 0; x < width; ++x) {
        candidates.clear();
        auto si = static_cast<std::size_t>(strip_for_x[x]);
        if (si >= pres.size()) si = pres.size() - 1;
        for (std::size_t s = 0; s < prev_beam.size(); ++s) {
            auto parent_idx = static_cast<std::uint16_t>(s);
            expand_ham_t<M>(prev_beam[s].color, row_lab[x], row_srgb[x],
                       prev_beam[s].cumulative_error, parent_idx,
                       pres[si], base_srgbs[si], candidates);
        }
        prune_beam(candidates, current_beam, beam_width);
        beam_history[x] = current_beam;
        prev_beam.swap(current_beam);
    }

    auto& final_beam = beam_history[width - 1];
    auto best_it = std::min_element(final_beam.begin(), final_beam.end(),
        [](const BeamState& a, const BeamState& b) {
            return a.cumulative_error < b.cumulative_error;
        });
    float total_error = best_it->cumulative_error;
    std::vector<std::uint8_t> values(width);
    auto state_idx = static_cast<std::size_t>(
        std::distance(final_beam.begin(), best_it));
    for (auto x = static_cast<std::ptrdiff_t>(width) - 1; x >= 0; --x) {
        auto ux = static_cast<std::size_t>(x);
        auto& state = beam_history[ux][state_idx];
        values[ux] = state.ham_value;
        state_idx = state.parent_idx;
    }
    return {std::move(values), total_error};
}

template <HamMetric M>
ScanlineResult encode_scanline_dp_t(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::size_t beam_width) {

    auto width = target_row.size();
    if (width == 0) return {{}, 0.0f};

    std::vector<std::vector<BeamState>> beam_history(width);
    std::vector<BeamState> candidates;
    std::vector<BeamState> current_beam;

    // Operations per state: num_base + 3 * (2*radius+1), not full 2^data_bits
    auto ops_per_state = pre.palette_lab.size() +
                         static_cast<std::size_t>(3 * (2 * kModifyDpRadius + 1));
    candidates.reserve(beam_width * ops_per_state);

    // Precompute per-pixel target OKLab + sRGB once (previously recomputed
    // inside the beam loop).
    std::vector<OKLab> row_lab(width);
    std::vector<SRGBColor> row_srgb(width);
    for (std::size_t x = 0; x < width; ++x) {
        row_lab[x] = color_space::linear_to_oklab(target_row[x]);
        row_srgb[x] = linear_to_srgb8(target_row[x]);
    }

    std::vector<BeamState> prev_beam;
    prev_beam.push_back({start_color, 0.0f, 0, 0});

    for (std::size_t x = 0; x < width; ++x) {
        candidates.clear();
        for (std::size_t s = 0; s < prev_beam.size(); ++s) {
            auto parent_idx = static_cast<std::uint16_t>(s);
            expand_ham_t<M>(prev_beam[s].color, row_lab[x], row_srgb[x],
                       prev_beam[s].cumulative_error, parent_idx,
                       pre, base_srgb, candidates);
        }

        prune_beam(candidates, current_beam, beam_width);
        beam_history[x] = current_beam;
        prev_beam.swap(current_beam);
    }

    // Find the best final state
    auto& final_beam = beam_history[width - 1];
    auto best_it = std::min_element(final_beam.begin(), final_beam.end(),
        [](const BeamState& a, const BeamState& b) {
            return a.cumulative_error < b.cumulative_error;
        });

    float total_error = best_it->cumulative_error;

    // Trace back the path to reconstruct HAM values
    std::vector<std::uint8_t> values(width);
    auto state_idx = static_cast<std::size_t>(
        std::distance(final_beam.begin(), best_it));

    for (auto x = static_cast<std::ptrdiff_t>(width) - 1; x >= 0; --x) {
        auto ux = static_cast<std::size_t>(x);
        auto& state = beam_history[ux][state_idx];
        values[ux] = state.ham_value;
        state_idx = state.parent_idx;
    }

    return {std::move(values), total_error};
}

// Runtime-dispatched wrappers — pick the metric specialization once
// per call and forward into the templated implementations. The hot
// inner loops have zero metric-related overhead.
ScanlineResult encode_scanline_dp(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::size_t beam_width,
    HamMetric metric = HamMetric::srgb_mse) {
    if (metric == HamMetric::oklab2)
        return encode_scanline_dp_t<HamMetric::oklab2>(
            target_row, start_color, pre, base_srgb, beam_width);
    return encode_scanline_dp_t<HamMetric::srgb_mse>(
        target_row, start_color, pre, base_srgb, beam_width);
}

ScanlineResult encode_scanline_dp_per_strip_impl(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_width,
    HamMetric metric) {
    if (metric == HamMetric::oklab2)
        return encode_scanline_dp_per_strip_t<HamMetric::oklab2>(
            target_row, start_color, pres, base_srgbs, strip_for_x,
            beam_width);
    return encode_scanline_dp_per_strip_t<HamMetric::srgb_mse>(
        target_row, start_color, pres, base_srgbs, strip_for_x,
        beam_width);
}

// ---------------------------------------------------------------------------
// Triple-pixel refinement post-pass.
//
// The main DP uses a 1-pixel lookahead with `beam_width` survivors. This can
// miss a better sequence where the locally-best op at pixel i leads to a
// constrained state that gives a bad choice at pixel i+1 or i+2 — classic
// HAM fringe lag on color transitions.
//
// Post-pass: slide a 3-pixel window over the encoded scanline. Inside each
// window, run a wider beam search (beam_k) over all 3 positions, exploring
// candidates that would have been pruned by the main DP. If the window's
// joint error drops, replace the ops in that segment and continue.
//
// Windows overlap by 1 position: step = 1, so improvements at the trailing
// edge of one window can propagate to the next.
// ---------------------------------------------------------------------------

template <HamMetric M>
void refine_triple_t(
    std::vector<std::uint8_t>& values,
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::size_t beam_k) {

    auto width = values.size();
    if (width < 3) return;

    // Reconstruct the per-pixel output color sequence from `values`.
    std::vector<SRGBColor> states(width + 1);
    states[0] = start_color;
    for (std::size_t x = 0; x < width; ++x) {
        auto [ctrl, data] = split_ham_value(values[x], pre.data_bits);
        auto prev = states[x];
        SRGBColor out = prev;
        switch (ctrl) {
        case 0b00:  out = base_srgb[data]; break;
        case 0b01:  out = {prev.r, prev.g, pre.expand_lut[data]}; break;
        case 0b10:  out = {pre.expand_lut[data], prev.g, prev.b}; break;
        case 0b11:  out = {prev.r, pre.expand_lut[data], prev.b}; break;
        }
        states[x + 1] = out;
    }

    // Precompute target OKLab and sRGB once per scanline — each window
    // previously recomputed these 3 times.
    std::vector<OKLab> row_lab(width);
    std::vector<SRGBColor> row_srgb(width);
    for (std::size_t x = 0; x < width; ++x) {
        row_lab[x] = color_space::linear_to_oklab(target_row[x]);
        row_srgb[x] = linear_to_srgb8(target_row[x]);
    }

    // Use the shared thread-local OKLab cache. Bump the generation for
    // this scanline so we start fresh (entries from previous scanlines
    // are invalidated by the mismatch check; no sweep needed).
    bump_ham_oklab_cache_gen();
    auto cached_oklab = [](SRGBColor c) -> OKLab {
        return cached_srgb_to_oklab(c);
    };
    // Helper to compute the error of a single op against the target.
    // Score matches expand_ham's metric (compile-time M) so this post-pass
    // re-ranks consistently with the beam DP's choice.
    auto op_error = [&](SRGBColor prev, std::uint8_t ham_value,
                        OKLab target_lab, SRGBColor target_srgb)
                        -> std::pair<float, SRGBColor> {
        auto [ctrl, data] = split_ham_value(ham_value, pre.data_bits);
        SRGBColor out = prev;
        switch (ctrl) {
        case 0b00: out = base_srgb[data]; break;
        case 0b01: out = {prev.r, prev.g, pre.expand_lut[data]}; break;
        case 0b10: out = {pre.expand_lut[data], prev.g, prev.b}; break;
        case 0b11: out = {prev.r, pre.expand_lut[data], prev.b}; break;
        }
        if constexpr (M == HamMetric::srgb_mse) {
            return { score_srgb_mse(target_srgb, out), out };
        } else {
            return { score_oklab2(target_lab, cached_oklab(out)), out };
        }
    };

    std::vector<BeamState> window_candidates;
    std::vector<BeamState> window_beam;
    auto ops_per_state = pre.palette_lab.size() + 3 * pre.num_data_values;
    window_candidates.reserve(beam_k * ops_per_state);


    // Cheap early-skip threshold: if every pixel in a window is already
    // within this squared error, the window is "tight" and triple
    // refinement is unlikely to help. Tuned from observation — below ~1e-4
    // the pixel is indistinguishable from its target (sRGB-MSE units after
    // the /65536 scaling are commensurate with OKLab²). Most flat-region
    // windows in real images clear this bar, cutting refinement work
    // roughly in half.
    constexpr float skip_threshold_per_pixel = 5e-5f;

    // Slide window with step=1. Window covers pixels [i, i+1, i+2]. We also
    // track pixel i+3's error (if present) using its existing ham_value —
    // this guards against "improvements" that shift state out of the window
    // in a way that hurts the next pixel.
    for (std::size_t i = 0; i + 3 <= width; ++i) {
        auto prev_state = states[i];

        // Current window error (just the 3 pixels — we assume their states
        // continue correctly because we'll only commit if the final state
        // either matches the original or the continuation pixel's op still
        // makes sense).
        OKLab tgt0 = row_lab[i];
        OKLab tgt1 = row_lab[i + 1];
        OKLab tgt2 = row_lab[i + 2];
        SRGBColor tgts0 = row_srgb[i];
        SRGBColor tgts1 = row_srgb[i + 1];
        SRGBColor tgts2 = row_srgb[i + 2];

        auto [e0_cur, s0_cur] = op_error(prev_state, values[i], tgt0, tgts0);
        auto [e1_cur, s1_cur] = op_error(s0_cur, values[i + 1], tgt1, tgts1);
        auto [e2_cur, s2_cur] = op_error(s1_cur, values[i + 2], tgt2, tgts2);
        float cur_err = e0_cur + e1_cur + e2_cur;

        // Skip windows where all three pixels are already near-perfect.
        if (e0_cur < skip_threshold_per_pixel &&
            e1_cur < skip_threshold_per_pixel &&
            e2_cur < skip_threshold_per_pixel) {
            continue;
        }

        // If there's a pixel i+3, the triple's output state affects its
        // error. Add that delta to the cost (current vs candidate).
        bool has_next = (i + 3) < width;
        float cur_next_err = 0.0f;
        if (has_next) {
            OKLab tgt3 = row_lab[i + 3];
            SRGBColor tgts3 = row_srgb[i + 3];
            auto [e3, _] = op_error(s2_cur, values[i + 3], tgt3, tgts3);
            cur_next_err = e3;
        }
        float cur_total = cur_err + cur_next_err;

        // Beam expansion over the 3-pixel window.
        std::vector<BeamState> prev_beam{{prev_state, 0.0f, 0, 0}};
        std::vector<std::vector<BeamState>> hist(3);

        SRGBColor tgt_srgb[3] = {
            row_srgb[i],
            row_srgb[i + 1],
            row_srgb[i + 2],
        };

        for (std::size_t p = 0; p < 3; ++p) {
            OKLab tgt = (p == 0) ? tgt0 : (p == 1) ? tgt1 : tgt2;
            window_candidates.clear();
            for (std::size_t s = 0; s < prev_beam.size(); ++s) {
                expand_ham_t<M>(prev_beam[s].color, tgt, tgt_srgb[p],
                           prev_beam[s].cumulative_error,
                           static_cast<std::uint16_t>(s),
                           pre, base_srgb, window_candidates);
            }
            prune_beam(window_candidates, window_beam, beam_k);
            hist[p] = window_beam;
            prev_beam.swap(window_beam);
        }

        // Find the best window sequence (with the continuation penalty).
        std::size_t best_idx = 0;
        float best_total = std::numeric_limits<float>::max();
        auto& last = hist[2];
        OKLab tgt3 = has_next ? row_lab[i + 3] : OKLab{};
        SRGBColor tgts3 = has_next ? row_srgb[i + 3] : SRGBColor{};
        for (std::size_t j = 0; j < last.size(); ++j) {
            float total = last[j].cumulative_error;
            if (has_next) {
                auto [e3, _] = op_error(last[j].color, values[i + 3],
                                        tgt3, tgts3);
                total += e3;
            }
            if (total < best_total) {
                best_total = total;
                best_idx = j;
            }
        }

        // Commit only if strictly better.
        if (best_total + 1e-9f < cur_total) {
            // Traceback new ops
            std::size_t idx = best_idx;
            std::uint8_t new_ops[3];
            for (auto p = static_cast<std::ptrdiff_t>(2); p >= 0; --p) {
                new_ops[p] = hist[static_cast<std::size_t>(p)][idx].ham_value;
                idx = hist[static_cast<std::size_t>(p)][idx].parent_idx;
            }
            for (std::size_t p = 0; p < 3; ++p) values[i + p] = new_ops[p];

            // Recompute states inside (and just after) the window.
            auto [_e0, ns0] = op_error(prev_state, values[i],     tgt0, tgts0);
            auto [_e1, ns1] = op_error(ns0,        values[i + 1], tgt1, tgts1);
            auto [_e2, ns2] = op_error(ns1,        values[i + 2], tgt2, tgts2);
            states[i + 1] = ns0;
            states[i + 2] = ns1;
            states[i + 3] = ns2;
        }
    }
}

// Runtime dispatcher for refine_triple — picks the metric template
// once per scanline, forwards into refine_triple_t.
inline void refine_triple(
    std::vector<std::uint8_t>& values,
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::size_t beam_k,
    HamMetric metric = HamMetric::srgb_mse) {
    if (metric == HamMetric::oklab2)
        refine_triple_t<HamMetric::oklab2>(values, target_row, start_color,
                                           pre, base_srgb, beam_k);
    else
        refine_triple_t<HamMetric::srgb_mse>(values, target_row, start_color,
                                             pre, base_srgb, beam_k);
}

// Per-strip variant of refine_triple. Each pixel's HAM op is scored
// against pres[strip_for_x[i]] / base_srgbs[strip_for_x[i]]. Window
// expansion uses the strip palette of the pixel being expanded — so a
// triple straddling a strip boundary picks ops valid for each pixel's
// own palette.
template <HamMetric M>
void refine_triple_per_strip_t(
    std::vector<std::uint8_t>& values,
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_k) {

    auto width = values.size();
    if (width < 3) return;
    if (pres.empty() || base_srgbs.empty()) return;

    auto strip_at = [&](std::size_t x) -> std::size_t {
        auto si = static_cast<std::size_t>(strip_for_x[x]);
        if (si >= pres.size()) si = pres.size() - 1;
        return si;
    };

    std::vector<SRGBColor> states(width + 1);
    states[0] = start_color;
    for (std::size_t x = 0; x < width; ++x) {
        auto si = strip_at(x);
        auto& pre = pres[si];
        auto base_srgb = base_srgbs[si];
        auto [ctrl, data] = split_ham_value(values[x], pre.data_bits);
        auto prev = states[x];
        SRGBColor out = prev;
        switch (ctrl) {
        case 0b00: if (data < base_srgb.size()) out = base_srgb[data]; break;
        case 0b01: out = {prev.r, prev.g, pre.expand_lut[data]}; break;
        case 0b10: out = {pre.expand_lut[data], prev.g, prev.b}; break;
        case 0b11: out = {prev.r, pre.expand_lut[data], prev.b}; break;
        }
        states[x + 1] = out;
    }

    std::vector<OKLab> row_lab(width);
    std::vector<SRGBColor> row_srgb(width);
    for (std::size_t x = 0; x < width; ++x) {
        row_lab[x] = color_space::linear_to_oklab(target_row[x]);
        row_srgb[x] = linear_to_srgb8(target_row[x]);
    }

    bump_ham_oklab_cache_gen();
    auto cached_oklab = [](SRGBColor c) -> OKLab {
        return cached_srgb_to_oklab(c);
    };
    auto op_error = [&](SRGBColor prev, std::uint8_t ham_value,
                        std::size_t pixel_x)
        -> std::pair<float, SRGBColor> {
        auto si = strip_at(pixel_x);
        auto& pre = pres[si];
        auto base_srgb = base_srgbs[si];
        auto [ctrl, data] = split_ham_value(ham_value, pre.data_bits);
        SRGBColor out = prev;
        switch (ctrl) {
        case 0b00: if (data < base_srgb.size()) out = base_srgb[data]; break;
        case 0b01: out = {prev.r, prev.g, pre.expand_lut[data]}; break;
        case 0b10: out = {pre.expand_lut[data], prev.g, prev.b}; break;
        case 0b11: out = {prev.r, pre.expand_lut[data], prev.b}; break;
        }
        if constexpr (M == HamMetric::srgb_mse) {
            return { score_srgb_mse(row_srgb[pixel_x], out), out };
        } else {
            return { score_oklab2(row_lab[pixel_x], cached_oklab(out)), out };
        }
    };

    std::vector<BeamState> window_candidates;
    std::vector<BeamState> window_beam;
    auto ops_per_state = pres[0].palette_lab.size()
        + 3 * pres[0].num_data_values;
    window_candidates.reserve(beam_k * ops_per_state);

    constexpr float skip_threshold_per_pixel = 5e-5f;

    for (std::size_t i = 0; i + 3 <= width; ++i) {
        auto prev_state = states[i];

        OKLab tgt0 = row_lab[i];
        OKLab tgt1 = row_lab[i + 1];
        OKLab tgt2 = row_lab[i + 2];

        auto [e0_cur, s0_cur] = op_error(prev_state, values[i], i);
        auto [e1_cur, s1_cur] = op_error(s0_cur, values[i + 1], i + 1);
        auto [e2_cur, s2_cur] = op_error(s1_cur, values[i + 2], i + 2);
        float cur_err = e0_cur + e1_cur + e2_cur;

        if (e0_cur < skip_threshold_per_pixel &&
            e1_cur < skip_threshold_per_pixel &&
            e2_cur < skip_threshold_per_pixel) {
            continue;
        }

        bool has_next = (i + 3) < width;
        float cur_next_err = 0.0f;
        if (has_next) {
            auto [e3, _] = op_error(s2_cur, values[i + 3], i + 3);
            cur_next_err = e3;
        }
        float cur_total = cur_err + cur_next_err;

        std::vector<BeamState> prev_beam{{prev_state, 0.0f, 0, 0}};
        std::vector<std::vector<BeamState>> hist(3);

        SRGBColor tgt_srgb[3] = {
            row_srgb[i], row_srgb[i + 1], row_srgb[i + 2],
        };

        for (std::size_t p = 0; p < 3; ++p) {
            OKLab tgt = (p == 0) ? tgt0 : (p == 1) ? tgt1 : tgt2;
            auto si = strip_at(i + p);
            window_candidates.clear();
            for (std::size_t s = 0; s < prev_beam.size(); ++s) {
                expand_ham_t<M>(prev_beam[s].color, tgt, tgt_srgb[p],
                           prev_beam[s].cumulative_error,
                           static_cast<std::uint16_t>(s),
                           pres[si], base_srgbs[si], window_candidates);
            }
            prune_beam(window_candidates, window_beam, beam_k);
            hist[p] = window_beam;
            prev_beam.swap(window_beam);
        }

        std::size_t best_idx = 0;
        float best_total = std::numeric_limits<float>::max();
        auto& last = hist[2];
        for (std::size_t j = 0; j < last.size(); ++j) {
            float total = last[j].cumulative_error;
            if (has_next) {
                auto [e3, _] = op_error(last[j].color, values[i + 3],
                                        i + 3);
                total += e3;
            }
            if (total < best_total) {
                best_total = total;
                best_idx = j;
            }
        }

        if (best_total + 1e-9f < cur_total) {
            std::size_t idx = best_idx;
            std::uint8_t new_ops[3];
            for (auto p = static_cast<std::ptrdiff_t>(2); p >= 0; --p) {
                new_ops[p] = hist[static_cast<std::size_t>(p)][idx].ham_value;
                idx = hist[static_cast<std::size_t>(p)][idx].parent_idx;
            }
            for (std::size_t p = 0; p < 3; ++p) values[i + p] = new_ops[p];

            auto [_e0, ns0] = op_error(prev_state, values[i],     i);
            auto [_e1, ns1] = op_error(ns0,        values[i + 1], i + 1);
            auto [_e2, ns2] = op_error(ns1,        values[i + 2], i + 2);
            states[i + 1] = ns0;
            states[i + 2] = ns1;
            states[i + 3] = ns2;
        }
    }
}

inline void refine_triple_per_strip(
    std::vector<std::uint8_t>& values,
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_k,
    HamMetric metric) {
    if (metric == HamMetric::oklab2)
        refine_triple_per_strip_t<HamMetric::oklab2>(
            values, target_row, start_color, pres, base_srgbs,
            strip_for_x, beam_k);
    else
        refine_triple_per_strip_t<HamMetric::srgb_mse>(
            values, target_row, start_color, pres, base_srgbs,
            strip_for_x, beam_k);
}

// ---------------------------------------------------------------------------
// Greedy scanline encoder (generic)
// ---------------------------------------------------------------------------

ScanlineResult encode_scanline_greedy(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb) {

    auto width = target_row.size();
    std::vector<std::uint8_t> values(width);
    float total_error = 0.0f;
    SRGBColor prev = start_color;

    for (std::size_t x = 0; x < width; ++x) {
        HamPixelResult result = encode_ham_pixel_impl(
            prev, target_row[x], pre, base_srgb);

        values[x] = result.value;
        prev = result.result_color;
        total_error += result.error;
    }

    return {std::move(values), total_error};
}

// ---------------------------------------------------------------------------
// Decode a HAM value back to the output sRGB color (used by dithered encoder)
// ---------------------------------------------------------------------------
// Generic HAM encoder (works for any depth 4-8)
// ---------------------------------------------------------------------------

Result<HamResult> encode_ham_generic(
    const Image& image,
    std::size_t num_base_colors,
    std::size_t num_bitplanes,
    amiga::Chipset chipset,
    const HamOptions& opts) {

    auto w = image.width();
    auto h = image.height();

    if (w == 0 || h == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Image dimensions must be non-zero",
        }};
    }

    auto data_bits = num_bitplanes - 2;

    // Select base palette and refine it for HAM encoding.
    // The initial median-cut palette is optimal for nearest-color matching
    // but not for HAM, where only SET operations use the palette directly.
    // Refine: encode with greedy HAM, identify which pixels actually use SET,
    // recompute palette centroids from those pixels' TARGET colors, repeat.
    Palette base_pal;
    if (!opts.external_palette.empty()) {
        // User-supplied palette via --palette: use as-is, no refinement.
        // Trim or pad to exactly num_base_colors entries (pad with black).
        base_pal.name = "user";
        base_pal.colors = opts.external_palette;
        if (base_pal.colors.size() > num_base_colors)
            base_pal.colors.resize(num_base_colors);
        while (base_pal.colors.size() < num_base_colors)
            base_pal.colors.push_back(Color3f{0.0f, 0.0f, 0.0f});
    } else {
        base_pal = choose_ham_palette(image, num_base_colors, chipset,
                                      opts.palette_diversity, opts.quantizer);
    }

    // Refinement only helps HAM6 (many pixels use SET due to 4-bit modify
    // precision).  For HAM8, MODIFY is precise enough that SET usage
    // drops to a small biased sample, and refinement actively hurts quality.
    // Skip refinement entirely when the user supplied an external palette —
    // they explicitly told us what to use; don't second-guess.
    int ham_refine_iters = (!opts.external_palette.empty()) ? 0
                         : (data_bits <= 4) ? 2 : 0;
    auto data_mask = static_cast<std::uint8_t>((1u << data_bits) - 1);
    for (int ri = 0; ri < ham_refine_iters; ++ri) {
        std::vector<SRGBColor> ref_srgb(base_pal.size());
        for (std::size_t i = 0; i < base_pal.size(); ++i)
            ref_srgb[i] = linear_to_srgb8(base_pal.colors[i]);
        HamPrecomp ref_pre{std::span<const Color3f>{base_pal.colors}, data_bits};

        // Per-slot accumulators for SET targets
        struct SetAcc { double L{}, a{}, b{}; double count{}; };
        std::vector<SetAcc> acc(num_base_colors);

        for (std::size_t y = 0; y < h; ++y) {
            SRGBColor start = ref_srgb[0];
            auto row = image.row(y);
            auto scanline = encode_scanline_greedy(
                row, start, ref_pre, std::span<const SRGBColor>{ref_srgb});
            for (std::size_t x = 0; x < w; ++x) {
                auto val = scanline.values[x];
                auto ctrl = val >> data_bits;
                if (ctrl == 0) {  // SET operation
                    auto slot = static_cast<std::size_t>(val & data_mask);
                    auto lab = color_space::linear_to_oklab(row[x]);
                    acc[slot].L += static_cast<double>(lab.L);
                    acc[slot].a += static_cast<double>(lab.a);
                    acc[slot].b += static_cast<double>(lab.b);
                    acc[slot].count += 1.0;
                }
            }
        }

        bool changed = false;
        for (std::size_t k = 1; k < num_base_colors; ++k) {
            if (acc[k].count < 1.0) continue;
            auto new_lab = color_space::OKLab{
                static_cast<float>(acc[k].L / acc[k].count),
                static_cast<float>(acc[k].a / acc[k].count),
                static_cast<float>(acc[k].b / acc[k].count)};
            auto new_color_linear = color_space::oklab_to_linear(new_lab).clamped();
            // Match base palette precision: OCS for HAM6, AGA for HAM8
            auto new_color = (chipset != amiga::Chipset::aga)
                ? palette::quantize_to_ocs(new_color_linear)
                : new_color_linear;
            if (new_color != base_pal.colors[k]) {
                base_pal.colors[k] = new_color;
                changed = true;
            }
        }
        if (!changed) break;
    }

    // Precompute sRGB versions
    std::vector<SRGBColor> base_srgb(base_pal.size());
    for (std::size_t i = 0; i < base_pal.size(); ++i) {
        base_srgb[i] = linear_to_srgb8(base_pal.colors[i]);
    }
    std::span<const SRGBColor> srgb_span{base_srgb};

    // Precompute palette OKLab + expand LUT (used by all scanline encoders)
    HamPrecomp pre{std::span<const Color3f>{base_pal.colors}, data_bits};

    // Encode all pixels
    std::vector<std::uint8_t> ham_values(w * h);
    float total_error = 0.0f;

    // Check if dithering is requested
    bool use_error_diffusion = dither::uses_error_diffusion(opts.dither_method);
    bool use_ordered_dither = (opts.dither_method != dither::Method::none) &&
                              dither::is_ordered(opts.dither_method) &&
                              opts.dither_method != dither::Method::none;

    if (use_ordered_dither) {
        // Ordered dithering: apply threshold bias to target colors before encoding.
        auto strength = opts.dither_strength;
        std::atomic<std::size_t> next_y{0};
        std::atomic<double> atomic_err{0.0};

        auto worker = [&]() {
            std::vector<Color3f> dithered_row(w);
            while (true) {
                auto y = next_y.fetch_add(1);
                if (y >= h) break;
                SRGBColor start = base_srgb[0];
                auto row = image.row(y);

                for (std::size_t x = 0; x < w; ++x) {
                    auto lab = color_space::linear_to_oklab(row[x]);
                    float threshold = dither::ordered_threshold(
                        opts.dither_method, x, y);
                    lab.L += threshold * strength * 0.15f;
                    lab.a += threshold * strength * 0.03f;
                    lab.b += threshold * strength * 0.03f;
                    auto linear = color_space::oklab_to_linear(lab);
                    dithered_row[x] = {
                        std::clamp(linear.r, 0.0f, 1.0f),
                        std::clamp(linear.g, 0.0f, 1.0f),
                        std::clamp(linear.b, 0.0f, 1.0f),
                    };
                }

                std::span<const Color3f> dithered_span{dithered_row};
                auto scanline = encode_scanline_dp(
                    dithered_span, start, pre, srgb_span, opts.beam_width,
                    opts.metric);
                if (opts.triple_beam > 0) {
                    refine_triple(scanline.values, dithered_span, start, pre,
                                  srgb_span, opts.triple_beam, opts.metric);
                }
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                double old = atomic_err.load(std::memory_order_relaxed);
                while (!atomic_err.compare_exchange_weak(
                    old, old + static_cast<double>(scanline.error))) {}
            }
        };

        auto n_threads = std::max<unsigned>(1,
            std::thread::hardware_concurrency());
        if (n_threads > h) n_threads = static_cast<unsigned>(h);
        if (n_threads == 1) {
            worker();
        } else {
            std::vector<std::jthread> threads;
            threads.reserve(n_threads);
            for (unsigned i = 0; i < n_threads; ++i)
                threads.emplace_back(worker);
            threads.clear();
        }
        total_error += static_cast<float>(atomic_err.load());
    } else if (use_error_diffusion) {
        // Error diffusion for HAM: pre-dither the image from full precision
        // to chipset color depth (OCS 12-bit / STF 9-bit), then encode HAM
        // on the pre-dithered image without further dithering.
        // This matches abc's approach: error diffusion smooths the color
        // quantization step, not the HAM encoding itself. Trying to diffuse
        // during HAM encoding causes horizontal banding because HAM's
        // sequential pixel dependency conflicts with error propagation.
        Image dithered_image(w, h);

        dither::Settings d_settings{
            .method = opts.dither_method,
            .strength = opts.dither_strength,
            .error_clamp = opts.error_clamp,
            .serpentine = true,
        };
        dither::diffuse_raw_buffer(
            image, d_settings,
            [&](const OKLab& target,
                std::size_t x, std::size_t y) -> dither::PickResult {
                auto adjusted = color_space::oklab_to_linear(target);
                adjusted.r = std::clamp(adjusted.r, 0.0f, 1.0f);
                adjusted.g = std::clamp(adjusted.g, 0.0f, 1.0f);
                adjusted.b = std::clamp(adjusted.b, 0.0f, 1.0f);

                // Quantize to HAM MODIFY precision (4 bits for HAM6,
                // 6 for HAM8). Snapping to the actual MODIFY grid is
                // what gives the ditherer something to diffuse — the
                // earlier OCS-only quantization no-op'd on AGA.
                auto srgb_adj = linear_to_srgb8(adjusted);
                srgb_adj.r = expand_to_8bit(
                    reduce_to_bits(srgb_adj.r, data_bits), data_bits);
                srgb_adj.g = expand_to_8bit(
                    reduce_to_bits(srgb_adj.g, data_bits), data_bits);
                srgb_adj.b = expand_to_8bit(
                    reduce_to_bits(srgb_adj.b, data_bits), data_bits);
                auto quantized = srgb8_to_linear(srgb_adj);
                dithered_image[x, y] = quantized;
                return {color_space::linear_to_oklab(quantized), 0.5f};
            });

        // Now encode HAM on the pre-dithered image (no further dithering).
        // Parallelized the same way as the non-dithered path.
        std::atomic<std::size_t> next_y_ed{0};
        std::atomic<double> atomic_err_ed{0.0};
        auto worker_ed = [&]() {
            while (true) {
                auto y = next_y_ed.fetch_add(1);
                if (y >= h) break;
                SRGBColor start = base_srgb[0];
                auto row = dithered_image.row(y);
                auto scanline = opts.greedy
                    ? encode_scanline_greedy(row, start, pre, srgb_span)
                    : encode_scanline_dp(row, start, pre,
                                         srgb_span, opts.beam_width,
                                         opts.metric);
                if (!opts.greedy && opts.triple_beam > 0) {
                    refine_triple(scanline.values, row, start, pre,
                                  srgb_span, opts.triple_beam, opts.metric);
                }
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                double old = atomic_err_ed.load(std::memory_order_relaxed);
                while (!atomic_err_ed.compare_exchange_weak(
                    old, old + static_cast<double>(scanline.error))) {}
            }
        };
        {
            auto n = std::max<unsigned>(1, std::thread::hardware_concurrency());
            if (n > h) n = static_cast<unsigned>(h);
            if (n == 1) {
                worker_ed();
            } else {
                std::vector<std::jthread> threads;
                threads.reserve(n);
                for (unsigned i = 0; i < n; ++i)
                    threads.emplace_back(worker_ed);
                threads.clear();
            }
        }
        total_error += static_cast<float>(atomic_err_ed.load());
    } else {
        // Non-dithered encoding path — parallelized across scanlines.
        std::atomic<std::size_t> next_y{0};
        std::atomic<double> atomic_err{0.0};

        auto worker = [&]() {
            while (true) {
                auto y = next_y.fetch_add(1);
                if (y >= h) break;
                SRGBColor start = base_srgb[0];
                auto row = image.row(y);
                auto scanline = opts.greedy
                    ? encode_scanline_greedy(row, start, pre, srgb_span)
                    : encode_scanline_dp(row, start, pre,
                                         srgb_span, opts.beam_width,
                                         opts.metric);
                if (!opts.greedy && opts.triple_beam > 0) {
                    refine_triple(scanline.values, row, start, pre,
                                  srgb_span, opts.triple_beam, opts.metric);
                }
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                double old = atomic_err.load(std::memory_order_relaxed);
                while (!atomic_err.compare_exchange_weak(
                    old, old + static_cast<double>(scanline.error))) {}
            }
        };

        auto n_threads = std::max<unsigned>(1,
            std::thread::hardware_concurrency());
        if (n_threads > h) n_threads = static_cast<unsigned>(h);
        if (n_threads == 1) {
            worker();
        } else {
            std::vector<std::jthread> threads;
            threads.reserve(n_threads);
            for (unsigned i = 0; i < n_threads; ++i)
                threads.emplace_back(worker);
            threads.clear();  // join on destruction
        }
        total_error += static_cast<float>(atomic_err.load());
    }

    // Encode HAM values to bitplanes
    auto planes = bitplane::encode(ham_values, w, h, num_bitplanes);
    if (!planes) return std::unexpected{planes.error()};

    return HamResult{
        *std::move(planes),
        std::move(base_pal.colors),
        total_error,
        {},  // no scanline_palettes
        {},  // no copper_changes
        0,   // changes_per_line
    };
}

// ---------------------------------------------------------------------------
// Copper HAM encoder: per-scanline base palette
//
// For each scanline, generate an optimal base palette from just that row's
// pixels, then encode that scanline with HAM using its local palette.
// The copper coprocessor changes palette registers each scanline.
// ---------------------------------------------------------------------------

// For copper HAM: find the best K base palette swaps by trial encoding.
// Encode the row with current palette, track which SET operations were
// used. For each swap candidate: replace an unused/low-value slot with
// a color that would reduce the highest per-pixel errors. Verify by
// re-encoding.
struct HamSwap {
    std::size_t slot;
    Color3f new_color;
};

std::vector<HamSwap> find_ham_swaps(
    std::span<const Color3f> row,
    std::vector<Color3f>& current_pal,
    std::size_t num_base_colors,
    std::size_t data_bits,
    std::size_t changes_per_line,
    amiga::Chipset chipset,
    bool best_quality,
    // Optional neighbour rows (prev_row, next_row) and per-row weights.
    // When non-empty, measure_row_error scores each candidate palette by
    // running independent rolling HAM-state encodes on every row and
    // summing weighted errors — same idea as copper.cpp's swap planner.
    // ham_convert's DynamicHires uses 3-line context (last+current+next)
    // since 0.8.1 to reduce per-line palette flicker. Caller passes
    // {prev,curr,next} with weights {decay,1.0,decay}.
    std::span<const std::span<const Color3f>> extra_rows,
    std::span<const float>                    extra_weights) {

    auto w = row.size();
    std::vector<HamSwap> swaps;

    // Helper: encode the row with a given palette and return total HAM error.
    // Used to verify a candidate swap actually reduces error before committing.
    auto measure_row_error = [&](std::span<const Color3f> pal) -> float {
        std::vector<SRGBColor> ps(num_base_colors);
        for (std::size_t i = 0; i < num_base_colors; ++i)
            ps[i] = linear_to_srgb8(pal[i]);
        HamPrecomp pre{pal, data_bits};
        auto encode_one_row = [&](std::span<const Color3f> r) {
            SRGBColor p = ps.empty() ? SRGBColor{0, 0, 0} : ps[0];
            float err = 0.0f;
            for (std::size_t x = 0; x < r.size(); ++x) {
                auto res = encode_ham_pixel_impl(p, r[x], pre,
                                            std::span<const SRGBColor>{ps});
                err += res.error;
                p = res.result_color;
            }
            return err;
        };
        float err = encode_one_row(row);
        // Add weighted neighbour-row contributions when provided. Each
        // neighbour row gets its own rolling HAM state (same algorithm
        // hardware would run on it), and the weighted error sums into
        // the per-trial score. This gives the planner a "what palette
        // works for this row AND its neighbours" signal — the inner-
        // loop equivalent of copper.cpp's neighbour-row weighting.
        for (std::size_t i = 0; i < extra_rows.size(); ++i) {
            auto& nr = extra_rows[i];
            if (nr.empty()) continue;
            float wgt = (i < extra_weights.size()) ? extra_weights[i] : 1.0f;
            err += wgt * encode_one_row(nr);
        }
        return err;
    };

    // Fast (default) path: original single-candidate greedy. ~5-10× cheaper
    // than the best-quality planner; sufficient for interactive web use and
    // most CLI runs. Each iteration: pick the row's worst pixel as the only
    // candidate, replace the least-used SET slot with its OCS-quantized
    // value if and only if the swap reduces total row error. Bail on the
    // first regression (further swaps from this state typically don't help).
    if (!best_quality) {
        for (std::size_t s = 0; s < changes_per_line; ++s) {
            std::vector<SRGBColor> pal_srgb(num_base_colors);
            for (std::size_t i = 0; i < num_base_colors; ++i)
                pal_srgb[i] = linear_to_srgb8(current_pal[i]);

            SRGBColor prev = pal_srgb.empty()
                ? SRGBColor{0, 0, 0} : pal_srgb[0];

            std::vector<std::size_t> set_count(num_base_colors, 0);
            float worst_pixel_error = 0.0f;
            Color3f worst_pixel_target{};
            float base_err = 0.0f;

            HamPrecomp swap_pre{
                std::span<const Color3f>{current_pal.data(), num_base_colors},
                data_bits};

            for (std::size_t x = 0; x < w; ++x) {
                auto result = encode_ham_pixel_impl(
                    prev, row[x], swap_pre,
                    std::span<const SRGBColor>{pal_srgb});
                auto [control, data_idx] = split_ham_value(result.value, data_bits);
                if (control == 0) set_count[data_idx]++;
                if (result.error > worst_pixel_error) {
                    worst_pixel_error = result.error;
                    worst_pixel_target = row[x];
                }
                base_err += result.error;
                prev = result.result_color;
            }

            if (worst_pixel_error < 1e-6f) break;

            std::size_t min_slot = 1;
            std::size_t min_count = std::numeric_limits<std::size_t>::max();
            for (std::size_t k = 1; k < num_base_colors; ++k) {
                if (set_count[k] < min_count) {
                    min_count = set_count[k];
                    min_slot = k;
                }
            }

            auto new_color = (chipset != amiga::Chipset::aga)
                ? palette::quantize_to_ocs(worst_pixel_target)
                : worst_pixel_target;

            auto old_color = current_pal[min_slot];
            current_pal[min_slot] = new_color;
            float trial_err = measure_row_error(
                std::span<const Color3f>{current_pal.data(), num_base_colors});
            if (trial_err > base_err) {
                current_pal[min_slot] = old_color;
                break;
            }
            swaps.push_back({min_slot, new_color});
        }
        return swaps;
    }

    // Best-quality path (--ham-cap-best): per iteration, build a multi-
    // candidate × multi-slot search and pick the (slot, color) pair that
    // drops row error the most. Don't bail on a single failed candidate —
    // only stop when NO candidate-slot pair improves the row. ~5-10×
    // cost of the fast path. Mirrors SCAP's per-strip planner shape.
    for (std::size_t s = 0; s < changes_per_line; ++s) {
        std::vector<SRGBColor> pal_srgb(num_base_colors);
        for (std::size_t i = 0; i < num_base_colors; ++i)
            pal_srgb[i] = linear_to_srgb8(current_pal[i]);

        SRGBColor prev = pal_srgb.empty()
            ? SRGBColor{0, 0, 0} : pal_srgb[0];

        std::vector<std::size_t> set_count(num_base_colors, 0);
        float base_err = 0.0f;

        HamPrecomp swap_pre{
            std::span<const Color3f>{current_pal.data(), num_base_colors},
            data_bits};

        // Track per-pixel error for top-K worst-pixel candidate selection.
        struct PxErr { float err; std::size_t x; };
        std::vector<PxErr> px_errs(w);

        for (std::size_t x = 0; x < w; ++x) {
            auto result = encode_ham_pixel_impl(
                prev, row[x], swap_pre,
                std::span<const SRGBColor>{pal_srgb});
            auto [control, data_idx] = split_ham_value(result.value, data_bits);
            if (control == 0) set_count[data_idx]++;
            base_err += result.error;
            px_errs[x] = {result.error, x};
            prev = result.result_color;
        }

        // Build candidate target colours: top-K highest-error source pixels
        // (OCS-quantised on OCS, dedup by 12-bit key) + OKLab centroid of
        // the K worst pixels. Centroid often beats any single pixel when a
        // smooth high-error region (skin, sky band) needs a fresh anchor.
        constexpr std::size_t kTopK = 16;
        std::partial_sort(
            px_errs.begin(),
            px_errs.begin() + static_cast<std::ptrdiff_t>(std::min(kTopK, w)),
            px_errs.end(),
            [](const PxErr& a, const PxErr& b) { return a.err > b.err; });
        if (px_errs.empty() || px_errs[0].err < 1e-6f) break;

        std::vector<Color3f> cands;
        cands.reserve(kTopK + 1);
        std::array<bool, 4096> seen{};
        auto ocs_key = [](Color3f c) {
            int r = static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 15.0f));
            int g = static_cast<int>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 15.0f));
            int b = static_cast<int>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 15.0f));
            return static_cast<std::size_t>((r << 8) | (g << 4) | b);
        };
        auto add_cand = [&](Color3f c) {
            auto cs = (chipset != amiga::Chipset::aga)
                ? palette::quantize_to_ocs(c) : c;
            auto key = ocs_key(cs);
            if (chipset == amiga::Chipset::aga || !seen[key]) {
                if (chipset != amiga::Chipset::aga) seen[key] = true;
                cands.push_back(cs);
            }
        };
        std::size_t topk_used = std::min(kTopK, w);
        double sumL = 0.0, suma = 0.0, sumb = 0.0;
        for (std::size_t i = 0; i < topk_used; ++i) {
            auto x = px_errs[i].x;
            add_cand(row[x]);
            auto lab = color_space::linear_to_oklab(row[x]);
            sumL += static_cast<double>(lab.L);
            suma += static_cast<double>(lab.a);
            sumb += static_cast<double>(lab.b);
        }
        if (topk_used > 0) {
            OKLab centroid{
                static_cast<float>(sumL / static_cast<double>(topk_used)),
                static_cast<float>(suma / static_cast<double>(topk_used)),
                static_cast<float>(sumb / static_cast<double>(topk_used))};
            add_cand(color_space::oklab_to_linear(centroid).clamped());
        }

        // Candidate slots: every SET slot (k>=1), sorted least-used first.
        // The best-quality planner is already opt-in (--ham-cap-best), so
        // we don't compromise on search width here — searching every slot
        // catches popular-slot swaps that re-route many SET pixels through
        // cheaper MODIFY chains.
        std::vector<std::size_t> slot_order;
        slot_order.reserve(num_base_colors - 1);
        for (std::size_t k = 1; k < num_base_colors; ++k)
            slot_order.push_back(k);
        std::sort(
            slot_order.begin(), slot_order.end(),
            [&](std::size_t a, std::size_t b) {
                return set_count[a] < set_count[b];
            });
        std::size_t slot_trials = slot_order.size();

        // Parallelise the candidate × slot search. Each trial is
        // independent given a thread-local palette copy with the swap
        // applied; measure_row_error reads but doesn't write shared
        // state. Sequential trials are the dominant cost in --cap-best
        // mode (16 cands × 15 slots × 7 swaps × ~213 rows × 5 passes
        // = ~1.8M row encodes), so parallelising here lifts the whole
        // HAM6+CAP+cap-best path from single-thread to N-core.
        struct TrialResult {
            float    err = std::numeric_limits<float>::max();
            Color3f  color{};
            std::size_t slot = 0;
        };
        std::size_t total_trials = cands.size() * slot_trials;
        std::vector<TrialResult> trials(total_trials);
        pipeline::parallel_for(total_trials, [&](std::size_t idx) {
            std::size_t ci = idx / slot_trials;
            std::size_t si = idx % slot_trials;
            auto slot = slot_order[si];
            std::vector<Color3f> local_pal(current_pal.begin(),
                                            current_pal.end());
            local_pal[slot] = cands[ci];
            float trial = measure_row_error(
                std::span<const Color3f>{local_pal.data(),
                                         num_base_colors});
            trials[idx] = {trial, cands[ci], slot};
        });
        float best_trial = base_err;
        Color3f best_color{};
        std::size_t best_slot = 0;
        bool any_improve = false;
        for (auto& t : trials) {
            if (t.err < best_trial) {
                best_trial = t.err;
                best_color = t.color;
                best_slot = t.slot;
                any_improve = true;
            }
        }

        if (!any_improve) break;
        current_pal[best_slot] = best_color;
        swaps.push_back({best_slot, best_color});
    }

    return swaps;
}

Result<HamResult> encode_ham_copper_generic(
    const Image& image,
    std::size_t num_base_colors,
    std::size_t num_bitplanes,
    amiga::Chipset chipset,
    bool is_hires,
    const HamOptions& opts,
    std::size_t override_changes) {

    auto w = image.width();
    auto h = image.height();

    if (w == 0 || h == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Image dimensions must be non-zero",
        }};
    }

    auto data_bits = num_bitplanes - 2;
    std::size_t changes_per_line;
    if (override_changes > 0) {
        changes_per_line = override_changes;
    } else {
        changes_per_line = copper::max_changes_per_line(
            num_bitplanes, true, is_hires, chipset,
            opts.skip_initial_swap_rows > 0);
    }

    // Global base palette. User-supplied via --palette takes precedence
    // (trimmed/padded to num_base_colors); otherwise quantize from image.
    Palette base_pal;
    if (!opts.external_palette.empty()) {
        base_pal.name = "user";
        base_pal.colors = opts.external_palette;
        if (base_pal.colors.size() > num_base_colors)
            base_pal.colors.resize(num_base_colors);
    } else {
        base_pal = choose_ham_palette(image, num_base_colors, chipset,
                                      opts.palette_diversity, opts.quantizer);
    }
    while (base_pal.colors.size() < num_base_colors) {
        base_pal.colors.push_back(Color3f{0.0f, 0.0f, 0.0f});
    }

    // For interlace, each field accumulates palette swaps independently so a
    // single per-line K-swap budget covers one row's diff (vs cumulative two
    // rows when fields share state).
    bool is_lace = opts.skip_initial_swap_rows > 0;

    // Error diffusion state
    bool use_error_diffusion = dither::uses_error_diffusion(opts.dither_method);
    bool use_ordered = (opts.dither_method != dither::Method::none) &&
                       dither::is_ordered(opts.dither_method);

    // For error diffusion: pre-dither image to chipset precision (same fix
    // as non-copper path). The dithered image is used for both copper palette
    // selection and HAM encoding.
    Image dithered_image(0, 0);
    if (use_error_diffusion) {
        dithered_image = Image(w, h);
        dither::Settings d_settings{
            .method = opts.dither_method,
            .strength = opts.dither_strength,
            .error_clamp = opts.error_clamp,
            .serpentine = true,
        };
        dither::diffuse_raw_buffer(
            image, d_settings,
            [&](const OKLab& target,
                std::size_t x, std::size_t y) -> dither::PickResult {
                auto adjusted = color_space::oklab_to_linear(target);
                adjusted.r = std::clamp(adjusted.r, 0.0f, 1.0f);
                adjusted.g = std::clamp(adjusted.g, 0.0f, 1.0f);
                adjusted.b = std::clamp(adjusted.b, 0.0f, 1.0f);
                auto quantized = (chipset != amiga::Chipset::aga)
                    ? palette::quantize_to_ocs(adjusted) : adjusted;
                dithered_image[x, y] = quantized;
                return {color_space::linear_to_oklab(quantized), 0.5f};
            });
    }

    // Use pre-dithered image for error diffusion, original for other modes
    const Image& encode_image = use_error_diffusion ? dithered_image : image;

    struct PassResult {
        std::vector<std::uint8_t> ham_values;
        std::vector<std::vector<Color3f>> scanline_palettes;
        std::vector<std::vector<copper::CopperChange>> all_changes;
        std::vector<Color3f> base_used;
        float total_error;
    };

    // Single shared global progress counter so cap_best's many run_passes
    // calls together cover 0..100% in one monotone sweep instead of N
    // visually-distinct sub-bars. Each row processed contributes one
    // weighted "work unit":
    //   - pass 1 (sequential CAP planning) ≈ 5% of run_passes time
    //   - pass 2 (parallel DP beam search)  ≈ 95%
    // so weighting per-row bumps gives a near-linear bar in wall time.
    //
    // cap_best does the initial encode plus kCapBestRefineIters extra
    // refinement passes (no early-stop — predictable progress + every
    // iteration is a chance to find a lower-error palette centroid).
    // User explicitly OK'd more compute for better quality.
    constexpr float kPass1Weight = 0.05f;
    constexpr float kPass2Weight = 0.95f;
    constexpr int kCapBestRefineIters = 4;
    float run_passes_count = opts.cap_best
        ? static_cast<float>(1 + kCapBestRefineIters) : 1.0f;
    float total_units = run_passes_count *
        (kPass1Weight * static_cast<float>(h) +
         kPass2Weight * static_cast<float>(h));
    std::atomic<float> work_done{0.0f};
    auto add_work = [&](float weight) {
        // Lock-free float accumulate: load → add → CAS-with-retry.
        float old = work_done.load(std::memory_order_relaxed);
        while (!work_done.compare_exchange_weak(old, old + weight)) {}
    };
    auto report_global = [&](std::string_view stage) {
        if (!opts.on_progress) return;
        float p = std::clamp(work_done.load() / total_units, 0.0f, 1.0f);
        opts.on_progress(p, stage);
    };

    // Beam width is per-call so refinement passes (cap_best loop below)
    // can ask for a wider DP beam than the initial encode. Wider beam
    // = higher PSNR per row at proportional CPU cost — exactly the
    // tradeoff cap_best trades on.
    auto run_passes = [&](std::vector<Color3f> initial_base,
                          std::string_view stage,
                          std::size_t beam_width) -> PassResult {
        PassResult out;
        out.ham_values.assign(w * h, 0);
        out.scanline_palettes.assign(h, {});
        out.all_changes.assign(h, {});
        out.base_used = initial_base;
        out.total_error = 0.0f;

        std::vector<Color3f> current_pal = initial_base;
        std::vector<Color3f> current_pal_f2 = initial_base;

        // Pass 1 (sequential): per-row CAP planning. Each row mutates
        // current_pal, so this must be serial. Cheap vs the beam search.
        report_global(stage);
        std::size_t pass1_step = std::max<std::size_t>(1, h / 20);
        // 3-line neighbour context experimentally hurt HAM6+CAP across
        // 4 hero images at both weight=0.25 and weight=0.5 (chuck31
        // -0.46/-0.50 dB, electrichues02 -0.09/-0.10, lovers -0.40,
        // fromthe +0.08). HAM op selection is sequential-state-bound
        // within a row, so scoring a palette against neighbour rows
        // (which roll their own HAM state from the same palette) adds
        // noise that doesn't represent real chip behaviour. The
        // ham_convert 3-line trick applies to palette quantisation,
        // not the per-line CAP swap planner. Disabled by default;
        // structure preserved for future experimentation.
        constexpr float kHamNeighbourDecay = 0.0f;
        std::array<std::span<const Color3f>, 2> neighbour_rows;
        std::array<float, 2> neighbour_weights = {kHamNeighbourDecay,
                                                  kHamNeighbourDecay};
        for (std::size_t y = 0; y < h; ++y) {
            auto row = encode_image.row(y);
            auto& pal_for_row = (is_lace && (y & 1))
                ? current_pal_f2 : current_pal;
            auto row_k = (y < opts.skip_initial_swap_rows)
                ? std::size_t{0} : changes_per_line;
            std::size_t nb_count = 0;
            if (y > 0) {
                neighbour_rows[nb_count] = encode_image.row(y - 1);
                neighbour_weights[nb_count] = kHamNeighbourDecay;
                ++nb_count;
            }
            if (y + 1 < h) {
                neighbour_rows[nb_count] = encode_image.row(y + 1);
                neighbour_weights[nb_count] = kHamNeighbourDecay;
                ++nb_count;
            }
            std::span<const std::span<const Color3f>> rows_view(
                neighbour_rows.data(), nb_count);
            std::span<const float> weights_view(
                neighbour_weights.data(), nb_count);
            auto swaps = find_ham_swaps(row, pal_for_row, num_base_colors,
                                        data_bits, row_k, chipset,
                                        opts.cap_best,
                                        rows_view, weights_view);
            std::vector<copper::CopperChange> line_changes;
            for (auto& [slot, color] : swaps) {
                line_changes.push_back({
                    static_cast<std::uint8_t>(slot), color});
            }
            out.all_changes[y] = std::move(line_changes);
            out.scanline_palettes[y] = pal_for_row;
            add_work(kPass1Weight);
            if ((y + 1) % pass1_step == 0) report_global(stage);
        }

        // Pass 2 (parallel): DP beam search per row, independent across rows.
        std::atomic<std::size_t> next_y{0};
        std::atomic<double> atomic_err{0.0};
        std::atomic<std::size_t> rows_done{0};
        auto worker = [&]() {
            while (true) {
                auto y = next_y.fetch_add(1);
                if (y >= h) break;
                auto row = encode_image.row(y);
                auto& pal_for_row = out.scanline_palettes[y];

                std::vector<SRGBColor> pal_srgb(num_base_colors);
                for (std::size_t i = 0; i < num_base_colors; ++i) {
                    pal_srgb[i] = linear_to_srgb8(pal_for_row[i]);
                }
                std::span<const SRGBColor> srgb_span{pal_srgb};
                SRGBColor start = pal_srgb.empty()
                    ? SRGBColor{0, 0, 0} : pal_srgb[0];

                HamPrecomp line_pre{
                    std::span<const Color3f>{pal_for_row.data(),
                                             num_base_colors},
                    data_bits};

                float err;
                if (use_ordered) {
                    std::vector<Color3f> dithered_row(w);
                    for (std::size_t x = 0; x < w; ++x) {
                        auto lab = color_space::linear_to_oklab(row[x]);
                        float threshold = dither::ordered_threshold(
                            opts.dither_method, x, y);
                        lab.L += threshold * opts.dither_strength * 0.15f;
                        lab.a += threshold * opts.dither_strength * 0.03f;
                        lab.b += threshold * opts.dither_strength * 0.03f;
                        auto linear = color_space::oklab_to_linear(lab);
                        dithered_row[x] = {
                            std::clamp(linear.r, 0.0f, 1.0f),
                            std::clamp(linear.g, 0.0f, 1.0f),
                            std::clamp(linear.b, 0.0f, 1.0f),
                        };
                    }
                    std::span<const Color3f> dr{dithered_row};
                    auto scanline = encode_scanline_dp(dr, start, line_pre,
                                                       srgb_span,
                                                       beam_width,
                                                       opts.metric);
                    if (opts.triple_beam > 0) {
                        refine_triple(scanline.values, dr, start, line_pre,
                                      srgb_span, opts.triple_beam,
                                      opts.metric);
                    }
                    std::copy(scanline.values.begin(), scanline.values.end(),
                              out.ham_values.begin() +
                                  static_cast<std::ptrdiff_t>(y * w));
                    err = scanline.error;
                } else {
                    auto scanline = encode_scanline_dp(row, start, line_pre,
                                                       srgb_span,
                                                       beam_width,
                                                       opts.metric);
                    if (opts.triple_beam > 0) {
                        refine_triple(scanline.values, row, start, line_pre,
                                      srgb_span, opts.triple_beam,
                                      opts.metric);
                    }
                    std::copy(scanline.values.begin(), scanline.values.end(),
                              out.ham_values.begin() +
                                  static_cast<std::ptrdiff_t>(y * w));
                    err = scanline.error;
                }
                double old = atomic_err.load(std::memory_order_relaxed);
                while (!atomic_err.compare_exchange_weak(
                    old, old + static_cast<double>(err))) {}
                rows_done.fetch_add(1);
                add_work(kPass2Weight);
                report_global(stage);
            }
        };

        auto n_threads = std::max<unsigned>(1,
            std::thread::hardware_concurrency());
        if (n_threads > h) n_threads = static_cast<unsigned>(h);
        if (n_threads == 1) {
            worker();
        } else {
            std::vector<std::jthread> threads;
            threads.reserve(n_threads);
            for (unsigned i = 0; i < n_threads; ++i)
                threads.emplace_back(worker);
            threads.clear();  // join on destruction
        }
        out.total_error = static_cast<float>(atomic_err.load());
        report_global(stage);
        return out;
    };

    // Single-counter progress: both run_passes calls feed the same
    // global work_done/total_units accumulator above, so the bar walks
    // 0..100 monotonically across both passes (and across pass1+pass2
    // inside each).
    auto best = run_passes(base_pal.colors, "encoding", opts.beam_width);

    // Joint base-palette + CAP refinement (--cap-best only). The
    // initial choose_ham_palette base is fixed without knowing what
    // CAP will do per row; refinement re-picks each slot as the OKLab
    // centroid of the per-row palettes the previous pass settled into,
    // then re-encodes from that better starting point. Each iteration
    // either lowers total_error (kept) or doesn't (kept anyway as the
    // basis for the next centroid — different palettes can break out
    // of local minima even when this one didn't help). Always tracks
    // `best` so we keep the lowest-error result regardless of which
    // iteration produced it.
    //
    // Fixed kCapBestRefineIters iterations rather than convergence-
    // based early stop: predictable progress, and the centroid update
    // is non-monotonic in error (one bad iter can be followed by a
    // good one).
    auto centroid_refine = [&](const PassResult& src) {
        std::vector<Color3f> refined = src.base_used;
        for (std::size_t k = 1; k < num_base_colors; ++k) {
            double sumL = 0.0, suma = 0.0, sumb = 0.0;
            for (std::size_t y = 0; y < h; ++y) {
                auto lab = color_space::linear_to_oklab(
                    src.scanline_palettes[y][k]);
                sumL += static_cast<double>(lab.L);
                suma += static_cast<double>(lab.a);
                sumb += static_cast<double>(lab.b);
            }
            auto inv = 1.0 / static_cast<double>(h);
            OKLab centroid{
                static_cast<float>(sumL * inv),
                static_cast<float>(suma * inv),
                static_cast<float>(sumb * inv)};
            auto lin = color_space::oklab_to_linear(centroid);
            lin.r = std::clamp(lin.r, 0.0f, 1.0f);
            lin.g = std::clamp(lin.g, 0.0f, 1.0f);
            lin.b = std::clamp(lin.b, 0.0f, 1.0f);
            refined[k] = (chipset != amiga::Chipset::aga)
                ? palette::quantize_to_ocs(lin) : lin;
        }
        return refined;
    };
    if (opts.cap_best && changes_per_line > 0 && h > 0) {
        // Pure centroid refinement is a fixed-point iteration — it
        // converges in 1–2 passes and pure looping doesn't gain past
        // that. To keep finding real improvements we alternate centroid
        // refine with JITTERED centroid refine: every odd iter we add a
        // small deterministic OKLab nudge to each slot, giving the next
        // pass a perturbed starting state that can escape the local
        // minimum the pure centroid sat in.
        //
        // Quadruple the beam for refinement passes (default 16 → 64).
        // Wider DP beam typically buys +0.3..1 dB on top of the centroid
        // refine, moving cap_best clearly into "visibly better"
        // territory.
        auto refine_beam = std::max(opts.beam_width, std::size_t{64});
        auto jitter = [&](std::vector<Color3f>& pal, std::uint32_t seed) {
            // Low-discrepancy hash → deterministic per-slot OKLab nudge
            // of ≤0.02 (≈ ΔE 7) on every channel. Small enough to keep
            // each slot near its centroid, big enough to break ties at
            // the per-row swap-pick step.
            for (std::size_t k = 1; k < pal.size(); ++k) {
                auto h32 = seed * 2654435761u +
                           static_cast<std::uint32_t>(k) * 0x9E3779B9u;
                auto unit = [&](unsigned shift) {
                    return (static_cast<float>((h32 >> shift) & 0xFFFFu) /
                            65535.0f - 0.5f) * 0.04f;
                };
                auto lab = color_space::linear_to_oklab(pal[k]);
                lab.L += unit(0);
                lab.a += unit(8);
                lab.b += unit(16);
                auto lin = color_space::oklab_to_linear(lab);
                lin.r = std::clamp(lin.r, 0.0f, 1.0f);
                lin.g = std::clamp(lin.g, 0.0f, 1.0f);
                lin.b = std::clamp(lin.b, 0.0f, 1.0f);
                pal[k] = (chipset != amiga::Chipset::aga)
                    ? palette::quantize_to_ocs(lin) : lin;
            }
        };

        PassResult latest = best;  // copy — must not move-from best
                                    // (retry may never improve and we'd
                                    // be left with empty vectors).
        for (int iter = 0; iter < kCapBestRefineIters; ++iter) {
            auto refined = centroid_refine(latest);
            // Odd iters: jitter (escape fixed point). Even iters: pure
            // centroid (consolidate after the perturbation).
            if (iter & 1)
                jitter(refined, static_cast<std::uint32_t>(iter + 1));
            auto retry = run_passes(std::move(refined), "refining",
                                    refine_beam);
            if (retry.total_error < best.total_error) {
                best = retry;  // copy: still need retry as next `latest`
            }
            latest = std::move(retry);
        }
    }
    if (opts.on_progress) opts.on_progress(1.0f, "done");

    auto planes = bitplane::encode(best.ham_values, w, h, num_bitplanes);
    if (!planes) return std::unexpected{planes.error()};

    return HamResult{
        *std::move(planes),
        std::move(best.base_used),
        best.total_error,
        std::move(best.scanline_palettes),
        std::move(best.all_changes),
        changes_per_line,
    };
}

} // namespace

// Public forwarders — declared in ham.hpp.
HamPixelResult encode_ham_pixel(SRGBColor prev,
                                 Color3f target,
                                 const HamPrecomp& pre,
                                 std::span<const SRGBColor> base_srgb) {
    return encode_ham_pixel_impl(prev, target, pre, base_srgb);
}

ScanlineResult encode_scanline_dp_per_strip(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_width,
    HamMetric metric) {
    return encode_scanline_dp_per_strip_impl(
        target_row, start_color, pres, base_srgbs,
        strip_for_x, beam_width, metric);
}

void refine_scanline_triple_per_strip(
    std::vector<std::uint8_t>& values,
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    std::span<const HamPrecomp> pres,
    std::span<const std::span<const SRGBColor>> base_srgbs,
    std::span<const std::uint16_t> strip_for_x,
    std::size_t beam_k,
    HamMetric metric) {
    refine_triple_per_strip(values, target_row, start_color,
                            pres, base_srgbs, strip_for_x, beam_k, metric);
}

// ===========================================================================
// Public API: Generic HAM encoding
// ===========================================================================

Result<HamResult> encode_ham(const Image& image, amiga::Mode mode,
                             amiga::Chipset chipset, const HamOptions& opts) {
    auto params = amiga::get_mode_params(mode);
    if (!params.is_ham) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "Mode is not a HAM mode",
        }};
    }
    auto num_base_colors = params.max_colors;
    auto num_bitplanes = params.bitplane_depth;
    return encode_ham_generic(image, num_base_colors, num_bitplanes,
                              chipset, opts);
}

// ===========================================================================
// Public API: HAM6 encoding (convenience wrapper)
// ===========================================================================

Result<HamResult> encode_ham6(const Image& image, amiga::Chipset chipset,
                              const HamOptions& opts) {
    return encode_ham(image, amiga::Mode::ham6, chipset, opts);
}

// ===========================================================================
// Public API: HAM8 encoding (convenience wrapper)
// ===========================================================================

Result<HamResult> encode_ham8(const Image& image, const HamOptions& opts) {
    return encode_ham(image, amiga::Mode::ham8, amiga::Chipset::aga, opts);
}

// ===========================================================================
// Public API: Copper HAM encoding (per-scanline base palettes)
// ===========================================================================

Result<HamResult> encode_ham_copper(const Image& image, amiga::Mode mode,
                                    amiga::Chipset chipset,
                                    const HamOptions& opts,
                                    bool is_hires,
                                    std::size_t override_changes) {
    auto params = amiga::get_mode_params(mode);
    if (!params.is_ham) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "Mode is not a HAM mode",
        }};
    }
    return encode_ham_copper_generic(image, params.max_colors,
                                     params.bitplane_depth, chipset, is_hires,
                                     opts, override_changes);
}

// ===========================================================================
// Public API: Render HAM bitplane data back to an Image
// ===========================================================================

Result<Image> render_ham(const bitplane::BitplaneData& planes,
                         std::span<const Color3f> base_palette,
                         std::size_t data_bits) {
    // Decode bitplanes to raw index values
    auto decoded = bitplane::decode(planes);
    if (!decoded) return std::unexpected{decoded.error()};

    auto w = planes.width;
    auto h = planes.height;
    // Pre-convert base palette to sRGB8
    std::vector<SRGBColor> base_srgb(base_palette.size());
    for (std::size_t i = 0; i < base_palette.size(); ++i) {
        base_srgb[i] = linear_to_srgb8(base_palette[i]);
    }

    Image image(w, h);

    for (std::size_t y = 0; y < h; ++y) {
        // Each scanline starts with palette[0]
        SRGBColor prev = base_srgb.empty()
            ? SRGBColor{0, 0, 0} : base_srgb[0];

        for (std::size_t x = 0; x < w; ++x) {
            auto raw = (*decoded)[y * w + x];
            auto [control, data_val] = split_ham_value(raw, data_bits);

            SRGBColor pixel;
            switch (control) {
            case 0b00:  // SET palette color
                if (data_val < base_srgb.size()) {
                    pixel = base_srgb[data_val];
                } else {
                    pixel = SRGBColor{0, 0, 0};
                }
                break;
            case 0b01:  // MODIFY BLUE
                pixel = SRGBColor{prev.r, prev.g,
                    expand_to_8bit(data_val, data_bits)};
                break;
            case 0b10:  // MODIFY RED
                pixel = SRGBColor{expand_to_8bit(data_val, data_bits),
                    prev.g, prev.b};
                break;
            case 0b11:  // MODIFY GREEN
                pixel = SRGBColor{prev.r,
                    expand_to_8bit(data_val, data_bits), prev.b};
                break;
            }

            prev = pixel;
            image[x, y] = srgb8_to_linear(pixel);
        }
    }

    return image;
}

// ===========================================================================
// Public API: Render copper HAM (per-scanline base palettes)
// ===========================================================================

Result<Image> render_ham_copper(
    const bitplane::BitplaneData& planes,
    const std::vector<std::vector<Color3f>>& scanline_palettes,
    std::size_t data_bits) {

    auto decoded = bitplane::decode(planes);
    if (!decoded) return std::unexpected{decoded.error()};

    auto w = planes.width;
    auto h = planes.height;
    Image image(w, h);

    for (std::size_t y = 0; y < h; ++y) {
        // Get this scanline's base palette
        auto& pal = (y < scanline_palettes.size())
            ? scanline_palettes[y] : scanline_palettes[0];
        std::vector<SRGBColor> pal_srgb(pal.size());
        for (std::size_t i = 0; i < pal.size(); ++i) {
            pal_srgb[i] = linear_to_srgb8(pal[i]);
        }

        SRGBColor prev = pal_srgb.empty()
            ? SRGBColor{0, 0, 0} : pal_srgb[0];

        for (std::size_t x = 0; x < w; ++x) {
            auto raw = (*decoded)[y * w + x];
            auto [control, data_val] = split_ham_value(raw, data_bits);

            SRGBColor pixel;
            switch (control) {
            case 0b00:
                pixel = (data_val < pal_srgb.size())
                    ? pal_srgb[data_val] : SRGBColor{0, 0, 0};
                break;
            case 0b01:
                pixel = {prev.r, prev.g, expand_to_8bit(data_val, data_bits)};
                break;
            case 0b10:
                pixel = {expand_to_8bit(data_val, data_bits), prev.g, prev.b};
                break;
            case 0b11:
                pixel = {prev.r, expand_to_8bit(data_val, data_bits), prev.b};
                break;
            }

            prev = pixel;
            image[x, y] = srgb8_to_linear(pixel);
        }
    }

    return image;
}

} // namespace png2amiga::ham
