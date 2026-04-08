#include "ham.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "quantize.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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

struct DiffusionEntry {
    int dx;
    int dy;
    float weight;
};

constexpr std::array floyd_steinberg_kernel = {
    DiffusionEntry{ 1, 0, 7.0f / 16.0f},
    DiffusionEntry{-1, 1, 3.0f / 16.0f},
    DiffusionEntry{ 0, 1, 5.0f / 16.0f},
    DiffusionEntry{ 1, 1, 1.0f / 16.0f},
};

constexpr std::array atkinson_kernel = {
    DiffusionEntry{ 1, 0, 1.0f / 8.0f},
    DiffusionEntry{ 2, 0, 1.0f / 8.0f},
    DiffusionEntry{-1, 1, 1.0f / 8.0f},
    DiffusionEntry{ 0, 1, 1.0f / 8.0f},
    DiffusionEntry{ 1, 1, 1.0f / 8.0f},
    DiffusionEntry{ 0, 2, 1.0f / 8.0f},
};

constexpr std::array sierra_lite_kernel = {
    DiffusionEntry{ 1, 0, 2.0f / 4.0f},
    DiffusionEntry{-1, 1, 1.0f / 4.0f},
    DiffusionEntry{ 0, 1, 1.0f / 4.0f},
};

constexpr std::array stucki_kernel = {
    DiffusionEntry{ 1, 0, 8.0f / 42.0f},
    DiffusionEntry{ 2, 0, 4.0f / 42.0f},
    DiffusionEntry{-2, 1, 2.0f / 42.0f},
    DiffusionEntry{-1, 1, 4.0f / 42.0f},
    DiffusionEntry{ 0, 1, 8.0f / 42.0f},
    DiffusionEntry{ 1, 1, 4.0f / 42.0f},
    DiffusionEntry{ 2, 1, 2.0f / 42.0f},
    DiffusionEntry{-2, 2, 1.0f / 42.0f},
    DiffusionEntry{-1, 2, 2.0f / 42.0f},
    DiffusionEntry{ 0, 2, 4.0f / 42.0f},
    DiffusionEntry{ 1, 2, 2.0f / 42.0f},
    DiffusionEntry{ 2, 2, 1.0f / 42.0f},
};

constexpr std::array jarvis_kernel = {
    DiffusionEntry{ 1, 0, 7.0f / 48.0f},
    DiffusionEntry{ 2, 0, 5.0f / 48.0f},
    DiffusionEntry{-2, 1, 3.0f / 48.0f},
    DiffusionEntry{-1, 1, 5.0f / 48.0f},
    DiffusionEntry{ 0, 1, 7.0f / 48.0f},
    DiffusionEntry{ 1, 1, 5.0f / 48.0f},
    DiffusionEntry{ 2, 1, 3.0f / 48.0f},
    DiffusionEntry{-2, 2, 1.0f / 48.0f},
    DiffusionEntry{-1, 2, 3.0f / 48.0f},
    DiffusionEntry{ 0, 2, 5.0f / 48.0f},
    DiffusionEntry{ 1, 2, 3.0f / 48.0f},
    DiffusionEntry{ 2, 2, 1.0f / 48.0f},
};

std::span<const DiffusionEntry> get_diffusion_kernel(dither::Method method) {
    switch (method) {
    case dither::Method::floyd_steinberg:
        return floyd_steinberg_kernel;
    case dither::Method::atkinson:
        return atkinson_kernel;
    case dither::Method::sierra_lite:
        return sierra_lite_kernel;
    case dither::Method::stucki:
        return stucki_kernel;
    case dither::Method::jarvis:
        return jarvis_kernel;
    default:
        return floyd_steinberg_kernel;
    }
}

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
                           amiga::Chipset chipset) {
    // Quantize N-1 colors, reserve index 0 for black (border/HAM start)
    auto reserve = (num_colors > 1) ? num_colors - 1 : std::size_t{1};
    auto pal = quantize::median_cut(image.pixels(), reserve);

    // Always quantize HAM base palette to OCS 12-bit precision.
    (void)chipset;
    for (auto& color : pal.colors) {
        color = palette::quantize_to_ocs(color);
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

// Expand all possible HAM operations from a given previous color state.
// Uses precomputed palette OKLab and expand LUT. Target OKLab is passed in
// to avoid recomputing it for every beam state at the same pixel.
void expand_ham(
    SRGBColor prev,
    OKLab target_lab,
    float prev_error,
    std::uint16_t parent_idx,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::vector<BeamState>& candidates) {

    auto data_bits = pre.data_bits;
    auto num_data_values = pre.num_data_values;

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

    // MODIFY BLUE (control = 01) — uses expand LUT
    for (std::size_t bv = 0; bv < num_data_values; ++bv) {
        SRGBColor modified{prev.r, prev.g, pre.expand_lut[bv]};
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
    for (std::size_t rv = 0; rv < num_data_values; ++rv) {
        SRGBColor modified{pre.expand_lut[rv], prev.g, prev.b};
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
    for (std::size_t gv = 0; gv < num_data_values; ++gv) {
        SRGBColor modified{prev.r, pre.expand_lut[gv], prev.b};
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

    // Operations per state: num_base + 3 * 2^data_bits
    auto ops_per_state = pre.palette_lab.size() + 3 * pre.num_data_values;
    candidates.reserve(beam_width * ops_per_state);

    std::vector<BeamState> prev_beam;
    prev_beam.push_back({start_color, 0.0f, 0, 0});

    for (std::size_t x = 0; x < width; ++x) {
        candidates.clear();
        // Compute target OKLab once per pixel (not per beam state)
        auto target_lab = color_space::linear_to_oklab(target_row[x]);

        for (std::size_t s = 0; s < prev_beam.size(); ++s) {
            auto parent_idx = static_cast<std::uint16_t>(s);
            expand_ham(prev_beam[s].color, target_lab,
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

SRGBColor decode_ham_value(std::uint8_t ham_value, SRGBColor prev,
                           std::span<const SRGBColor> base_srgb,
                           std::size_t data_bits) {
    auto [control, data_val] = split_ham_value(ham_value, data_bits);

    switch (control) {
    case 0b00:  // SET palette color
        if (data_val < base_srgb.size()) {
            return base_srgb[data_val];
        }
        return SRGBColor{0, 0, 0};
    case 0b01:  // MODIFY BLUE
        return SRGBColor{prev.r, prev.g, expand_to_8bit(data_val, data_bits)};
    case 0b10:  // MODIFY RED
        return SRGBColor{expand_to_8bit(data_val, data_bits), prev.g, prev.b};
    case 0b11:  // MODIFY GREEN
        return SRGBColor{prev.r, expand_to_8bit(data_val, data_bits), prev.b};
    }
    return prev;
}

// ===========================================================================
// Dithered greedy encoder
//
// Applies error diffusion during HAM encoding. For each pixel:
// 1. Add accumulated error to the target color (in OKLab space)
// 2. Convert adjusted target back to linear RGB
// 3. Run the HAM pixel encoder against the adjusted target
// 4. Compute quantization error (adjusted_lab - actual_output_lab)
// 5. Distribute error to neighbors using the diffusion kernel
//
// Supports serpentine scanning (alternating direction per scanline).
// ===========================================================================

struct ScanlineResultWithColor {
    std::vector<std::uint8_t> values;
    float error;
    std::vector<SRGBColor> output_colors;   // for error computation
};

ScanlineResultWithColor encode_scanline_greedy_dithered(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::span<const DiffusionEntry> kernel,
    float strength,
    float error_clamp_val,
    std::vector<OKLab>& error_buf,     // width * rows_needed (modified in place)
    std::size_t y,                      // current row index
    std::size_t total_height,
    bool serpentine) {

    auto width = target_row.size();
    std::vector<std::uint8_t> values(width);
    std::vector<SRGBColor> output_colors(width);
    float total_error = 0.0f;
    SRGBColor prev = start_color;

    bool reverse = serpentine && (y % 2 == 1);

    for (std::size_t step = 0; step < width; ++step) {
        std::size_t x = reverse ? (width - 1 - step) : step;
        auto buf_idx = y * width + x;

        // Add accumulated error to the original target
        auto target_lab = color_space::linear_to_oklab(target_row[x]);
        auto clamped_err = oklab_clamp(error_buf[buf_idx], error_clamp_val);
        auto adjusted_lab = oklab_add(target_lab, clamped_err);

        // Convert adjusted target back to linear RGB for the HAM encoder
        auto adjusted_linear = color_space::oklab_to_linear(adjusted_lab);
        // Clamp to valid range
        adjusted_linear.r = std::clamp(adjusted_linear.r, 0.0f, 1.0f);
        adjusted_linear.g = std::clamp(adjusted_linear.g, 0.0f, 1.0f);
        adjusted_linear.b = std::clamp(adjusted_linear.b, 0.0f, 1.0f);

        // Run HAM pixel encoder
        HamPixelResult result = encode_ham_pixel(
            prev, adjusted_linear, pre, base_srgb);

        values[x] = result.value;
        output_colors[x] = result.result_color;
        prev = result.result_color;
        total_error += result.error;

        // Compute quantization error in OKLab space
        auto actual_lab = color_space::linear_to_oklab(
            srgb8_to_linear(result.result_color));
        auto quant_error = oklab_scale(
            oklab_sub(adjusted_lab, actual_lab), strength);

        // Distribute error to neighbors
        for (auto& entry : kernel) {
            auto nx = static_cast<std::ptrdiff_t>(x) +
                      (reverse ? -entry.dx : entry.dx);
            auto ny = static_cast<std::ptrdiff_t>(y) + entry.dy;

            if (nx >= 0 && static_cast<std::size_t>(nx) < width &&
                ny >= 0 && static_cast<std::size_t>(ny) < total_height) {
                auto nidx = static_cast<std::size_t>(ny) * width +
                            static_cast<std::size_t>(nx);
                error_buf[nidx] = oklab_clamp(
                    oklab_add(error_buf[nidx],
                              oklab_scale(quant_error, entry.weight)),
                    error_clamp_val);
            }
        }
    }

    return {std::move(values), total_error, std::move(output_colors)};
}

// ---------------------------------------------------------------------------
// Dithered DP beam search encoder
//
// For the DP beam search, full per-pixel error diffusion is impractical
// because the beam explores multiple paths simultaneously. Instead, we
// apply error diffusion as a pre-pass: errors from previous scanlines
// are added to targets before DP optimization begins within the row.
// This gives inter-scanline diffusion benefit while DP handles intra-row.
//
// After the DP scanline is encoded, we compute the actual output errors
// and propagate them to future scanlines.
// ---------------------------------------------------------------------------

ScanlineResult encode_scanline_dp_dithered(
    std::span<const Color3f> target_row,
    SRGBColor start_color,
    const HamPrecomp& pre,
    std::span<const SRGBColor> base_srgb,
    std::size_t beam_width,
    std::span<const DiffusionEntry> kernel,
    float strength,
    float error_clamp_val,
    std::vector<OKLab>& error_buf,
    std::size_t y,
    std::size_t total_height) {

    auto width = target_row.size();
    if (width == 0) return {{}, 0.0f};

    // Pre-pass: adjust targets with accumulated errors from previous scanlines
    std::vector<Color3f> adjusted_row(width);
    for (std::size_t x = 0; x < width; ++x) {
        auto buf_idx = y * width + x;
        auto target_lab = color_space::linear_to_oklab(target_row[x]);
        auto clamped_err = oklab_clamp(error_buf[buf_idx], error_clamp_val);
        auto adjusted_lab = oklab_add(target_lab, clamped_err);
        auto linear = color_space::oklab_to_linear(adjusted_lab);
        adjusted_row[x] = {
            std::clamp(linear.r, 0.0f, 1.0f),
            std::clamp(linear.g, 0.0f, 1.0f),
            std::clamp(linear.b, 0.0f, 1.0f),
        };
    }

    // Run DP on the adjusted targets
    auto result = encode_scanline_dp(
        adjusted_row, start_color, pre, base_srgb, beam_width);

    // Post-pass: compute actual output colors and propagate errors
    // to future scanlines
    SRGBColor prev = start_color;

    for (std::size_t x = 0; x < width; ++x) {
        auto actual_srgb = decode_ham_value(
            result.values[x], prev, base_srgb, pre.data_bits);
        prev = actual_srgb;

        // Compute error: adjusted target vs actual output (in OKLab)
        auto adjusted_lab = color_space::linear_to_oklab(adjusted_row[x]);
        auto actual_lab = color_space::linear_to_oklab(
            srgb8_to_linear(actual_srgb));
        auto quant_error = oklab_scale(
            oklab_sub(adjusted_lab, actual_lab), strength);

        // Only distribute to future rows (dy > 0) since DP already
        // optimized within this row
        for (auto& entry : kernel) {
            if (entry.dy <= 0) continue;  // skip same-row entries

            auto nx = static_cast<std::ptrdiff_t>(x) + entry.dx;
            auto ny = static_cast<std::ptrdiff_t>(y) + entry.dy;

            if (nx >= 0 && static_cast<std::size_t>(nx) < width &&
                ny >= 0 && static_cast<std::size_t>(ny) < total_height) {
                auto nidx = static_cast<std::size_t>(ny) * width +
                            static_cast<std::size_t>(nx);
                error_buf[nidx] = oklab_clamp(
                    oklab_add(error_buf[nidx],
                              oklab_scale(quant_error, entry.weight)),
                    error_clamp_val);
            }
        }
    }

    return result;
}

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

    // Select base palette
    auto base_pal = choose_ham_palette(image, num_base_colors, chipset);

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
        // Build a pre-dithered copy of the image.
        auto strength = opts.dither_strength;

        for (std::size_t y = 0; y < h; ++y) {
            SRGBColor start = base_srgb[0];
            auto row = image.row(y);

            // Build dithered targets for this row
            std::vector<Color3f> dithered_row(w);
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

            ScanlineResult scanline;
            std::span<const Color3f> dithered_span{dithered_row};

            if (opts.quality == Quality::optimal) {
                scanline = encode_scanline_dp(
                    dithered_span, start, pre, srgb_span,
                    opts.beam_width);
            } else {
                scanline = encode_scanline_greedy(
                    dithered_span, start, pre, srgb_span);
            }

            std::copy(scanline.values.begin(), scanline.values.end(),
                      ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
            total_error += scanline.error;
        }
    } else if (use_error_diffusion) {
        // Error diffusion dithering path
        auto kernel = get_diffusion_kernel(opts.dither_method);

        // Error buffer: needs to cover current row + max kernel reach
        std::vector<OKLab> error_buf(w * h);  // full image error buffer

        for (std::size_t y = 0; y < h; ++y) {
            SRGBColor start = base_srgb[0];
            auto row = image.row(y);

            if (opts.quality == Quality::optimal) {
                auto scanline = encode_scanline_dp_dithered(
                    row, start, pre, srgb_span,
                    opts.beam_width,
                    kernel, opts.dither_strength, opts.error_clamp,
                    error_buf, y, h);

                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                total_error += scanline.error;
            } else {
                auto scanline = encode_scanline_greedy_dithered(
                    row, start, pre, srgb_span,
                    kernel, opts.dither_strength, opts.error_clamp,
                    error_buf, y, h, true /*serpentine*/);

                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                total_error += scanline.error;
            }
        }
    } else {
        // Non-dithered encoding path (original algorithm)
        for (std::size_t y = 0; y < h; ++y) {
            SRGBColor start = base_srgb[0];
            auto row = image.row(y);

            ScanlineResult scanline;

            if (opts.quality == Quality::optimal) {
                scanline = encode_scanline_dp(
                    row, start, pre, srgb_span,
                    opts.beam_width);
            } else {
                scanline = encode_scanline_greedy(
                    row, start, pre, srgb_span);
            }

            std::copy(scanline.values.begin(), scanline.values.end(),
                      ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
            total_error += scanline.error;
        }
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

    for (std::size_t s = 0; s < changes_per_line; ++s) {
        // Encode with current palette to measure error and find weak spots
        std::vector<SRGBColor> pal_srgb(num_base_colors);
        for (std::size_t i = 0; i < num_base_colors; ++i) {
            pal_srgb[i] = linear_to_srgb8(current_pal[i]);
        }

        SRGBColor prev = pal_srgb.empty()
            ? SRGBColor{0, 0, 0} : pal_srgb[0];

        // Track per-slot SET usage count and per-pixel error
        std::vector<std::size_t> set_count(num_base_colors, 0);
        float worst_pixel_error = 0.0f;
        Color3f worst_pixel_target{};

        // Build precomp for current palette state
        HamPrecomp swap_pre{
            std::span<const Color3f>{current_pal.data(), num_base_colors},
            data_bits};

        for (std::size_t x = 0; x < w; ++x) {
            auto result = encode_ham_pixel(
                prev, row[x], swap_pre,
                std::span<const SRGBColor>{pal_srgb});
            auto [control, data_idx] = split_ham_value(result.value, data_bits);
            if (control == 0) {
                auto idx = data_idx;
                set_count[idx]++;
            }
            if (result.error > worst_pixel_error) {
                worst_pixel_error = result.error;
                worst_pixel_target = row[x];
            }
            prev = result.result_color;
        }

        if (worst_pixel_error < 1e-6f) break;  // already perfect

        // Replace the least-used SET slot with the worst pixel's target.
        // Skip slot 0 — it's the background/border color (COLOR00).
        std::size_t min_slot = 1;
        std::size_t min_count = std::numeric_limits<std::size_t>::max();
        for (std::size_t k = 1; k < num_base_colors; ++k) {
            if (set_count[k] < min_count) {
                min_count = set_count[k];
                min_slot = k;
            }
        }

        auto new_color = worst_pixel_target;
        // Always OCS-quantize: copper changes are written as 12-bit in the viewer,
        // so the encoder must use the same precision for pixel-exact match.
        (void)chipset;
        new_color = palette::quantize_to_ocs(new_color);

        current_pal[min_slot] = new_color;
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
    auto base_pal = choose_ham_palette(image, num_base_colors, chipset);
    while (base_pal.colors.size() < num_base_colors) {
        base_pal.colors.push_back(Color3f{0.0f, 0.0f, 0.0f});
    }

    std::vector<Color3f> current_pal = base_pal.colors;
    std::vector<std::uint8_t> ham_values(w * h);
    std::vector<std::vector<Color3f>> scanline_palettes(h);
    std::vector<std::vector<copper::CopperChange>> all_changes(h);
    float total_error = 0.0f;

    // Error diffusion state (persists across scanlines)
    bool use_error_diffusion = (opts.dither_method != dither::Method::none) &&
                               is_error_diffusion(opts.dither_method);
    bool use_ordered = (opts.dither_method != dither::Method::none) &&
                       dither::is_ordered(opts.dither_method);

    std::vector<OKLab> error_buf;
    std::span<const DiffusionEntry> kernel;
    if (use_error_diffusion) {
        error_buf.resize(w * h);
        kernel = get_diffusion_kernel(opts.dither_method);
    }

    for (std::size_t y = 0; y < h; ++y) {
        auto row = image.row(y);

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

        // Encode scanline
        if (use_error_diffusion) {
            // Error diffusion with cross-scanline propagation
            if (opts.quality == Quality::optimal) {
                auto scanline = encode_scanline_dp_dithered(
                    row, start, line_pre, srgb_span,
                    opts.beam_width,
                    kernel, opts.dither_strength, opts.error_clamp,
                    error_buf, y, h);
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                total_error += scanline.error;
            } else {
                auto scanline = encode_scanline_greedy_dithered(
                    row, start, line_pre, srgb_span,
                    kernel, opts.dither_strength, opts.error_clamp,
                    error_buf, y, h, true);
                std::copy(scanline.values.begin(), scanline.values.end(),
                          ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
                total_error += scanline.error;
            }
        } else if (use_ordered) {
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
            ScanlineResult scanline = (opts.quality == Quality::optimal)
                ? encode_scanline_dp(dr, start, line_pre, srgb_span,
                                     opts.beam_width)
                : encode_scanline_greedy(dr, start, line_pre, srgb_span);
            std::copy(scanline.values.begin(), scanline.values.end(),
                      ham_values.begin() + static_cast<std::ptrdiff_t>(y * w));
            total_error += scanline.error;
        } else {
            // No dithering
            ScanlineResult scanline = (opts.quality == Quality::optimal)
                ? encode_scanline_dp(row, start, line_pre, srgb_span,
                                     opts.beam_width)
                : encode_scanline_greedy(row, start, line_pre, srgb_span);
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
