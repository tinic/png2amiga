#include "ham.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "quantize.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#include <vector>

namespace png2amiga::ham {

// sRGB color as 8-bit components (for HAM channel manipulation).
// Visible to both the anonymous namespace (encoding) and public functions (decoding).
struct SRGBColor {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};

    bool operator==(const SRGBColor&) const = default;
};

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

// Error-diffusion kernels are defined publicly in dither.cpp/dither.hpp;
// use dither::error_diffusion_kernel(method) directly.

// Is the dither method an error-diffusion method (vs ordered or none)?
bool is_error_diffusion(dither::Method m) {
    switch (m) {
    case dither::Method::floyd_steinberg:
    case dither::Method::atkinson:
    case dither::Method::sierra_lite:
    case dither::Method::stucki:
    case dither::Method::jarvis:
        return true;
    default:
        return false;
    }
}

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

struct HamPixelResult {
    std::uint8_t value;     // full encoded value (control + data)
    SRGBColor result_color; // actual color produced by this operation
    float error;            // perceptual error vs target
};

// Encode a HAM value: combine control bits and data bits.
// HAM6 (6 planes): control in bits 5-4 (top 2), data in bits 3-0
// HAM8 (8 planes): control in bits 1-0 (bottom 2), data in bits 7-2
// General HAM-N: for N <= 7, control in top 2 bits; for N = 8, control in bottom 2.
constexpr std::uint8_t make_ham_value(std::uint8_t control, std::uint8_t data,
                                      std::size_t data_bits) noexcept {
    if (data_bits == 6) {
        // HAM8: control in low 2 bits, data in high 6 bits
        return static_cast<std::uint8_t>((data << 2) | control);
    }
    // HAM6 and others: control in high 2 bits, data in low N bits
    return static_cast<std::uint8_t>((control << data_bits) | data);
}

// ---------------------------------------------------------------------------
// Precomputed data for HAM encoding (computed once, reused for all scanlines)
// ---------------------------------------------------------------------------

struct HamPrecomp {
    std::vector<OKLab> palette_lab;         // OKLab for each palette entry
    std::vector<std::uint8_t> expand_lut;   // expand_to_8bit lookup table [0..2^data_bits)
    std::size_t data_bits;
    std::size_t num_data_values;            // 1 << data_bits

    HamPrecomp(std::span<const Color3f> palette, std::size_t db)
        : data_bits(db), num_data_values(std::size_t{1} << db) {
        palette_lab.resize(palette.size());
        for (std::size_t i = 0; i < palette.size(); ++i)
            palette_lab[i] = color_space::linear_to_oklab(palette[i]);

        expand_lut.resize(num_data_values);
        for (std::size_t v = 0; v < num_data_values; ++v)
            expand_lut[v] = expand_to_8bit(static_cast<std::uint8_t>(v), db);
    }
};

// Extract control and data from a HAM value (inverse of make_ham_value)
constexpr std::pair<std::uint8_t, std::uint8_t>
split_ham_value(std::uint8_t value, std::size_t data_bits) noexcept {
    if (data_bits == 6) {
        // HAM8: control in low 2 bits
        return {static_cast<std::uint8_t>(value & 0x03),
                static_cast<std::uint8_t>(value >> 2)};
    }
    // HAM6 and others: control in high 2 bits
    auto data_mask = static_cast<std::uint8_t>((1u << data_bits) - 1u);
    return {static_cast<std::uint8_t>(value >> data_bits),
            static_cast<std::uint8_t>(value & data_mask)};
}

HamPixelResult encode_ham_pixel(
    SRGBColor prev,
    Color3f target,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb) {

    auto data_bits = pre.data_bits;

    HamPixelResult best;
    best.error = std::numeric_limits<float>::max();

    // Compute target OKLab once for all candidates
    auto target_lab = color_space::linear_to_oklab(target);

    // Option 1: SET palette color (control = 00) — uses precomputed palette OKLab
    for (std::size_t i = 0; i < pre.palette_lab.size(); ++i) {
        float dL = target_lab.L - pre.palette_lab[i].L;
        float da = target_lab.a - pre.palette_lab[i].a;
        float db = target_lab.b - pre.palette_lab[i].b;
        float err = dL * dL + da * da + db * db;
        if (err < best.error) {
            best.error = err;
            best.value = make_ham_value(0b00, static_cast<std::uint8_t>(i), data_bits);
            best.result_color = base_srgb[i];
        }
    }

    // For modify operations, we work in sRGB space because HAM modifies
    // individual sRGB channel values at the hardware's bit precision.
    auto target_srgb = linear_to_srgb8(target);

    // Option 2: MODIFY BLUE (control = 01)
    {
        auto b_data = reduce_to_bits(target_srgb.b, data_bits);
        auto b8 = pre.expand_lut[b_data];
        SRGBColor modified{prev.r, prev.g, b8};
        auto lab = color_space::linear_to_oklab(srgb8_to_linear(modified));
        float dL = target_lab.L - lab.L;
        float da = target_lab.a - lab.a;
        float db = target_lab.b - lab.b;
        float err = dL * dL + da * da + db * db;
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
        auto lab = color_space::linear_to_oklab(srgb8_to_linear(modified));
        float dL = target_lab.L - lab.L;
        float da = target_lab.a - lab.a;
        float db = target_lab.b - lab.b;
        float err = dL * dL + da * da + db * db;
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
        auto lab = color_space::linear_to_oklab(srgb8_to_linear(modified));
        float dL = target_lab.L - lab.L;
        float da = target_lab.a - lab.a;
        float db = target_lab.b - lab.b;
        float err = dL * dL + da * da + db * db;
        if (err < best.error) {
            best.error = err;
            best.value = make_ham_value(0b11, g_data, data_bits);
            best.result_color = modified;
        }
    }

    return best;
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
    OKLab lab = color_space::linear_to_oklab(srgb8_to_linear(c));
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

void expand_ham(
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

    // SET palette color (control = 00) — uses precomputed palette OKLab
    for (std::size_t i = 0; i < pre.palette_lab.size(); ++i) {
        float dL = target_lab.L - pre.palette_lab[i].L;
        float da = target_lab.a - pre.palette_lab[i].a;
        float db = target_lab.b - pre.palette_lab[i].b;
        float err = dL * dL + da * da + db * db;
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

    // MODIFY BLUE (control = 01) — focused around target
    auto lo_b = std::max(0, static_cast<int>(tbv) - kModifyDpRadius);
    auto hi_b = std::min(nmax, static_cast<int>(tbv) + kModifyDpRadius + 1);
    for (int bv = lo_b; bv < hi_b; ++bv) {
        SRGBColor modified{prev.r, prev.g,
            pre.expand_lut[static_cast<std::size_t>(bv)]};
        auto lab = color_space::linear_to_oklab(srgb8_to_linear(modified));
        float dL = target_lab.L - lab.L;
        float da = target_lab.a - lab.a;
        float db = target_lab.b - lab.b;
        float err = dL * dL + da * da + db * db;
        candidates.push_back({
            modified, prev_error + err,
            make_ham_value(0b01, static_cast<std::uint8_t>(bv), data_bits),
            parent_idx,
        });
    }

    // MODIFY RED (control = 10)
    auto lo_r = std::max(0, static_cast<int>(trv) - kModifyDpRadius);
    auto hi_r = std::min(nmax, static_cast<int>(trv) + kModifyDpRadius + 1);
    for (int rv = lo_r; rv < hi_r; ++rv) {
        SRGBColor modified{
            pre.expand_lut[static_cast<std::size_t>(rv)], prev.g, prev.b};
        auto lab = color_space::linear_to_oklab(srgb8_to_linear(modified));
        float dL = target_lab.L - lab.L;
        float da = target_lab.a - lab.a;
        float db = target_lab.b - lab.b;
        float err = dL * dL + da * da + db * db;
        candidates.push_back({
            modified, prev_error + err,
            make_ham_value(0b10, static_cast<std::uint8_t>(rv), data_bits),
            parent_idx,
        });
    }

    // MODIFY GREEN (control = 11)
    auto lo_g = std::max(0, static_cast<int>(tgv) - kModifyDpRadius);
    auto hi_g = std::min(nmax, static_cast<int>(tgv) + kModifyDpRadius + 1);
    for (int gv = lo_g; gv < hi_g; ++gv) {
        SRGBColor modified{prev.r,
            pre.expand_lut[static_cast<std::size_t>(gv)], prev.b};
        auto lab = color_space::linear_to_oklab(srgb8_to_linear(modified));
        float dL = target_lab.L - lab.L;
        float da = target_lab.a - lab.a;
        float db = target_lab.b - lab.b;
        float err = dL * dL + da * da + db * db;
        candidates.push_back({
            modified, prev_error + err,
            make_ham_value(0b11, static_cast<std::uint8_t>(gv), data_bits),
            parent_idx,
        });
    }
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

// DP beam search for a single scanline (generic).
struct ScanlineResult {
    std::vector<std::uint8_t> values;
    float error;
};

ScanlineResult encode_scanline_dp(
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
            expand_ham(prev_beam[s].color, row_lab[x], row_srgb[x],
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

void refine_triple(
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
    auto op_error = [&](SRGBColor prev, std::uint8_t ham_value,
                        OKLab target_lab) -> std::pair<float, SRGBColor> {
        auto [ctrl, data] = split_ham_value(ham_value, pre.data_bits);
        SRGBColor out = prev;
        switch (ctrl) {
        case 0b00: out = base_srgb[data]; break;
        case 0b01: out = {prev.r, prev.g, pre.expand_lut[data]}; break;
        case 0b10: out = {pre.expand_lut[data], prev.g, prev.b}; break;
        case 0b11: out = {prev.r, pre.expand_lut[data], prev.b}; break;
        }
        auto out_lab = cached_oklab(out);
        float dL = target_lab.L - out_lab.L;
        float da = target_lab.a - out_lab.a;
        float db = target_lab.b - out_lab.b;
        return { dL * dL + da * da + db * db, out };
    };

    std::vector<BeamState> window_candidates;
    std::vector<BeamState> window_beam;
    auto ops_per_state = pre.palette_lab.size() + 3 * pre.num_data_values;
    window_candidates.reserve(beam_k * ops_per_state);


    // Cheap early-skip threshold: if every pixel in a window is already
    // within this squared-OKLab error, the window is "tight" and triple
    // refinement is unlikely to help. Tuned from observation — below ~1e-4
    // the pixel is indistinguishable from its target. Most flat-region
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

        auto [e0_cur, s0_cur] = op_error(prev_state, values[i], tgt0);
        auto [e1_cur, s1_cur] = op_error(s0_cur, values[i + 1], tgt1);
        auto [e2_cur, s2_cur] = op_error(s1_cur, values[i + 2], tgt2);
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
            auto [e3, _] = op_error(s2_cur, values[i + 3], tgt3);
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
                expand_ham(prev_beam[s].color, tgt, tgt_srgb[p],
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
        for (std::size_t j = 0; j < last.size(); ++j) {
            float total = last[j].cumulative_error;
            if (has_next) {
                auto [e3, _] = op_error(last[j].color, values[i + 3], tgt3);
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
            auto [_e0, ns0] = op_error(prev_state,    values[i],     tgt0);
            auto [_e1, ns1] = op_error(ns0,           values[i + 1], tgt1);
            auto [_e2, ns2] = op_error(ns1,           values[i + 2], tgt2);
            states[i + 1] = ns0;
            states[i + 2] = ns1;
            states[i + 3] = ns2;
        }
    }
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
        HamPixelResult result = encode_ham_pixel(
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
    auto base_pal = choose_ham_palette(image, num_base_colors, chipset, opts.palette_diversity, opts.quantizer);

    // Refinement only helps HAM6 (many pixels use SET due to 4-bit modify
    // precision).  For HAM8, MODIFY is precise enough that SET usage
    // drops to a small biased sample, and refinement actively hurts quality.
    int ham_refine_iters = (data_bits <= 4) ? 2 : 0;
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
    bool use_error_diffusion = (opts.dither_method != dither::Method::none) &&
                               is_error_diffusion(opts.dither_method);
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
                    dithered_span, start, pre, srgb_span, opts.beam_width);
                if (opts.triple_beam > 0) {
                    refine_triple(scanline.values, dithered_span, start, pre,
                                  srgb_span, opts.triple_beam);
                }
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                double old = atomic_err.load(std::memory_order_relaxed);
                while (!atomic_err.compare_exchange_weak(
                    old, old + static_cast<double>(scanline.error))) {}
            }
        };

#ifndef __EMSCRIPTEN__
        auto n_threads = std::max<unsigned>(1,
            std::thread::hardware_concurrency());
        if (n_threads > h) n_threads = static_cast<unsigned>(h);
        std::vector<std::jthread> threads;
        threads.reserve(n_threads);
        for (unsigned i = 0; i < n_threads; ++i) threads.emplace_back(worker);
        threads.clear();
#else
        worker();
#endif
        total_error += static_cast<float>(atomic_err.load());
    } else if (use_error_diffusion) {
        // Error diffusion for HAM: pre-dither the image from full precision
        // to chipset color depth (OCS 12-bit / STF 9-bit), then encode HAM
        // on the pre-dithered image without further dithering.
        // This matches abc's approach: error diffusion smooths the color
        // quantization step, not the HAM encoding itself. Trying to diffuse
        // during HAM encoding causes horizontal banding because HAM's
        // sequential pixel dependency conflicts with error propagation.
        auto kernel = dither::error_diffusion_kernel(opts.dither_method);
        auto strength = opts.dither_strength;
        auto error_clamp_val = opts.error_clamp;

        // Pre-dither: quantize each pixel to chipset precision with error diffusion
        Image dithered_image(w, h);
        std::vector<OKLab> error_buf(w * h, OKLab{0, 0, 0});

        for (std::size_t y = 0; y < h; ++y) {
            bool reverse = (y % 2 == 1);  // serpentine
            auto row = image.row(y);
            for (std::size_t step = 0; step < w; ++step) {
                std::size_t x = reverse ? (w - 1 - step) : step;
                auto buf_idx = y * w + x;

                // Add accumulated error to original color
                auto target_lab = color_space::linear_to_oklab(row[x]);
                auto clamped_err = oklab_clamp(error_buf[buf_idx], error_clamp_val);
                auto adjusted_lab = oklab_add(target_lab, clamped_err);
                auto adjusted = color_space::oklab_to_linear(adjusted_lab);
                adjusted.r = std::clamp(adjusted.r, 0.0f, 1.0f);
                adjusted.g = std::clamp(adjusted.g, 0.0f, 1.0f);
                adjusted.b = std::clamp(adjusted.b, 0.0f, 1.0f);

                // Quantize to match HAM MODIFY precision. HAM uses
                // `data_bits` per channel for MODIFY ops (4 for HAM6,
                // 6 for HAM8). Quantizing to exactly that grid gives
                // the ditherer something meaningful to diffuse — the
                // previous OCS-only quantization was a no-op on AGA,
                // which is why HAM8 was showing banding regardless of
                // --dither.
                auto srgb_adj = linear_to_srgb8(adjusted);
                srgb_adj.r = expand_to_8bit(
                    reduce_to_bits(srgb_adj.r, data_bits), data_bits);
                srgb_adj.g = expand_to_8bit(
                    reduce_to_bits(srgb_adj.g, data_bits), data_bits);
                srgb_adj.b = expand_to_8bit(
                    reduce_to_bits(srgb_adj.b, data_bits), data_bits);
                auto quantized = srgb8_to_linear(srgb_adj);
                dithered_image[x, y] = quantized;

                // Compute and distribute error
                auto actual_lab = color_space::linear_to_oklab(quantized);
                auto quant_error = oklab_scale(
                    oklab_sub(adjusted_lab, actual_lab), strength);

                for (auto& entry : kernel) {
                    auto nx = static_cast<std::ptrdiff_t>(x) +
                              (reverse ? -entry.dx : entry.dx);
                    auto ny = static_cast<std::ptrdiff_t>(y) + entry.dy;
                    if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                        ny >= 0 && static_cast<std::size_t>(ny) < h) {
                        auto nidx = static_cast<std::size_t>(ny) * w +
                                    static_cast<std::size_t>(nx);
                        error_buf[nidx] = oklab_clamp(
                            oklab_add(error_buf[nidx],
                                      oklab_scale(quant_error, entry.weight)),
                            error_clamp_val);
                    }
                }
            }
        }

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
                                         srgb_span, opts.beam_width);
                if (!opts.greedy && opts.triple_beam > 0) {
                    refine_triple(scanline.values, row, start, pre,
                                  srgb_span, opts.triple_beam);
                }
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                double old = atomic_err_ed.load(std::memory_order_relaxed);
                while (!atomic_err_ed.compare_exchange_weak(
                    old, old + static_cast<double>(scanline.error))) {}
            }
        };
#ifndef __EMSCRIPTEN__
        {
            auto n = std::max<unsigned>(1, std::thread::hardware_concurrency());
            if (n > h) n = static_cast<unsigned>(h);
            std::vector<std::jthread> threads;
            threads.reserve(n);
            for (unsigned i = 0; i < n; ++i) threads.emplace_back(worker_ed);
            threads.clear();
        }
#else
        worker_ed();
#endif
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
                                         srgb_span, opts.beam_width);
                if (!opts.greedy && opts.triple_beam > 0) {
                    refine_triple(scanline.values, row, start, pre,
                                  srgb_span, opts.triple_beam);
                }
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                double old = atomic_err.load(std::memory_order_relaxed);
                while (!atomic_err.compare_exchange_weak(
                    old, old + static_cast<double>(scanline.error))) {}
            }
        };

#ifndef __EMSCRIPTEN__
        auto n_threads = std::max<unsigned>(1,
            std::thread::hardware_concurrency());
        if (n_threads > h) n_threads = static_cast<unsigned>(h);
        std::vector<std::jthread> threads;
        threads.reserve(n_threads);
        for (unsigned i = 0; i < n_threads; ++i) threads.emplace_back(worker);
        threads.clear(); // join on destruction
#else
        worker();
#endif
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
    amiga::Chipset chipset) {

    auto w = row.size();
    std::vector<HamSwap> swaps;

    // Helper: encode the row with a given palette and return total HAM error.
    // Used to verify a candidate swap actually reduces error before committing.
    auto measure_row_error = [&](std::span<const Color3f> pal) -> float {
        std::vector<SRGBColor> ps(num_base_colors);
        for (std::size_t i = 0; i < num_base_colors; ++i)
            ps[i] = linear_to_srgb8(pal[i]);
        HamPrecomp pre{pal, data_bits};
        SRGBColor p = ps.empty() ? SRGBColor{0, 0, 0} : ps[0];
        float err = 0.0f;
        for (std::size_t x = 0; x < w; ++x) {
            auto r = encode_ham_pixel(p, row[x], pre,
                                      std::span<const SRGBColor>{ps});
            err += r.error;
            p = r.result_color;
        }
        return err;
    };

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
            auto result = encode_ham_pixel(
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

        // Candidate: replace the least-used SET slot with the worst pixel's target.
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

        // Trial the swap: commit only if it reduces total row error.
        // Prevents the regression where a greedy swap happens to hurt quality.
        auto old_color = current_pal[min_slot];
        current_pal[min_slot] = new_color;
        float trial_err = measure_row_error(
            std::span<const Color3f>{current_pal.data(), num_base_colors});
        // Allow ties (trial_err == base_err) — sometimes a swap is neutral
        // on this line but helps visual coherence across lines.
        if (trial_err > base_err) {
            current_pal[min_slot] = old_color;
            break;  // net regression — further swaps unlikely to help
        }
        swaps.push_back({min_slot, new_color});
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
            num_bitplanes, true, is_hires, chipset);
    }

    // Global base palette
    auto base_pal = choose_ham_palette(image, num_base_colors, chipset, opts.palette_diversity, opts.quantizer);
    while (base_pal.colors.size() < num_base_colors) {
        base_pal.colors.push_back(Color3f{0.0f, 0.0f, 0.0f});
    }

    std::vector<Color3f> current_pal = base_pal.colors;
    std::vector<std::uint8_t> ham_values(w * h);
    std::vector<std::vector<Color3f>> scanline_palettes(h);
    std::vector<std::vector<copper::CopperChange>> all_changes(h);
    float total_error = 0.0f;

    // Error diffusion state
    bool use_error_diffusion = (opts.dither_method != dither::Method::none) &&
                               is_error_diffusion(opts.dither_method);
    bool use_ordered = (opts.dither_method != dither::Method::none) &&
                       dither::is_ordered(opts.dither_method);

    // For error diffusion: pre-dither image to chipset precision (same fix
    // as non-copper path). The dithered image is used for both copper palette
    // selection and HAM encoding.
    Image dithered_image(0, 0);
    if (use_error_diffusion) {
        dithered_image = Image(w, h);
        auto kernel = dither::error_diffusion_kernel(opts.dither_method);
        auto strength = opts.dither_strength;
        auto error_clamp_val = opts.error_clamp;
        std::vector<OKLab> error_buf(w * h, OKLab{0, 0, 0});

        for (std::size_t y = 0; y < h; ++y) {
            bool reverse = (y % 2 == 1);
            auto row = image.row(y);
            for (std::size_t step = 0; step < w; ++step) {
                std::size_t x = reverse ? (w - 1 - step) : step;
                auto buf_idx = y * w + x;
                auto target_lab = color_space::linear_to_oklab(row[x]);
                auto clamped_err = oklab_clamp(error_buf[buf_idx], error_clamp_val);
                auto adjusted_lab = oklab_add(target_lab, clamped_err);
                auto adjusted = color_space::oklab_to_linear(adjusted_lab);
                adjusted.r = std::clamp(adjusted.r, 0.0f, 1.0f);
                adjusted.g = std::clamp(adjusted.g, 0.0f, 1.0f);
                adjusted.b = std::clamp(adjusted.b, 0.0f, 1.0f);
                auto quantized = (chipset != amiga::Chipset::aga)
                    ? palette::quantize_to_ocs(adjusted) : adjusted;
                dithered_image[x, y] = quantized;
                auto actual_lab = color_space::linear_to_oklab(quantized);
                auto quant_error = oklab_scale(
                    oklab_sub(adjusted_lab, actual_lab), strength);
                for (auto& entry : kernel) {
                    auto nx = static_cast<std::ptrdiff_t>(x) +
                              (reverse ? -entry.dx : entry.dx);
                    auto ny = static_cast<std::ptrdiff_t>(y) + entry.dy;
                    if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                        ny >= 0 && static_cast<std::size_t>(ny) < h) {
                        auto nidx = static_cast<std::size_t>(ny) * w +
                                    static_cast<std::size_t>(nx);
                        error_buf[nidx] = oklab_clamp(
                            oklab_add(error_buf[nidx],
                                      oklab_scale(quant_error, entry.weight)),
                            error_clamp_val);
                    }
                }
            }
        }
    }

    // Use pre-dithered image for error diffusion, original for other modes
    const Image& encode_image = use_error_diffusion ? dithered_image : image;

    for (std::size_t y = 0; y < h; ++y) {
        auto row = encode_image.row(y);

        // Find best K swaps (modifies current_pal in-place)
        auto swaps = find_ham_swaps(row, current_pal, num_base_colors,
                                    data_bits, changes_per_line, chipset);

        // Record as CopperChange
        std::vector<copper::CopperChange> line_changes;
        for (auto& [slot, color] : swaps) {
            line_changes.push_back({
                static_cast<std::uint8_t>(slot), color});
        }
        all_changes[y] = std::move(line_changes);
        scanline_palettes[y] = current_pal;

        // Precompute sRGB + OKLab for this scanline's palette
        std::vector<SRGBColor> pal_srgb(num_base_colors);
        for (std::size_t i = 0; i < num_base_colors; ++i) {
            pal_srgb[i] = linear_to_srgb8(current_pal[i]);
        }
        std::span<const SRGBColor> srgb_span{pal_srgb};
        SRGBColor start = pal_srgb.empty()
            ? SRGBColor{0, 0, 0} : pal_srgb[0];

        // Rebuild precomp for this scanline's (potentially modified) palette
        HamPrecomp line_pre{
            std::span<const Color3f>{current_pal.data(), num_base_colors},
            data_bits};

        // Encode scanline (error diffusion already handled by pre-dither)
        if (use_ordered) {
            // Ordered dithering with correct Y coordinate
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
                                               srgb_span, opts.beam_width);
            if (opts.triple_beam > 0) {
                refine_triple(scanline.values, dr, start, line_pre,
                              srgb_span, opts.triple_beam);
            }
            std::copy(scanline.values.begin(), scanline.values.end(),
                      ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
            total_error += scanline.error;
        } else {
            // No dithering
            auto scanline = encode_scanline_dp(row, start, line_pre,
                                               srgb_span, opts.beam_width);
            if (opts.triple_beam > 0) {
                refine_triple(scanline.values, row, start, line_pre,
                              srgb_span, opts.triple_beam);
            }
            std::copy(scanline.values.begin(), scanline.values.end(),
                      ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
            total_error += scanline.error;
        }
    }

    auto planes = bitplane::encode(ham_values, w, h, num_bitplanes);
    if (!planes) return std::unexpected{planes.error()};

    return HamResult{
        *std::move(planes),
        std::move(base_pal.colors),
        total_error,
        std::move(scanline_palettes),
        std::move(all_changes),
        changes_per_line,
    };
}

} // namespace

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
