// CGA composite NTSC simulator + encoder.
//
// Decoder port of Andrew Jenner ("reenigne")'s sampled-chroma-multiplexer
// algorithm — the canonical reference; original Rust at MartyPC
// crates/lib/frontend/marty_videocard_renderer/src/composite_new.rs
// under UNLICENSE. Encoder is our own: a beam-search that picks the
// 4-bit RGBI pixel sequence whose decoder output most closely matches
// the source in OKLab.
//
// Key insight vs the old "16-colour palette dither" approach: the
// output colour at pixel x depends on input pixels in a 10-pixel
// window, so the artifact colours (purple/cyan from alternating-blue
// patterns etc.) are *emergent* — you can't pick them via a per-pixel
// palette lookup, you have to search the pattern space.

#include "cga_composite.hpp"

#include "color_space.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace png2amiga::cga_composite {

namespace {

// CHROMA_MULTIPLEXER[256] — voltage samples for every (left_rgb, right_rgb,
// phase) combination. Indexed as (left<<5)|(right<<2)|phase where left/right
// are 3-bit RGB triples (0..7) and phase is 0..3 (sub-carrier phase mod 4).
// Reenigne derived this empirically from a 1981 IBM 5150 captured signal.
constexpr std::array<std::uint8_t, 256> kChromaMultiplexer = {
      2,   2,   2,   2, 114, 174,   4,   3,   2,   1, 133, 135,   2, 113, 150,   4,
    133,   2,   1,  99, 151, 152,   2,   1,   3,   2,  96, 136, 151, 152, 151, 152,
      2,  56,  62,   4, 111, 250, 118,   4,   0,  51, 207, 137,   1, 171, 209,   5,
    140,  50,  54, 100, 133, 202,  57,   4,   2,  50, 153, 149, 128, 198, 198, 135,
     32,   1,  36,  81, 147, 158,   1,  42,  33,   1, 210, 254,  34, 109, 169,  77,
    177,   2,   0, 165, 189, 154,   3,  44,  33,   0,  91, 197, 178, 142, 144, 192,
      4,   2,  61,  67, 117, 151, 112,  83,   4,   0, 249, 255,   3, 107, 249, 117,
    147,   1,  50, 162, 143, 141,  52,  54,   3,   0, 145, 206, 124, 123, 192, 193,
     72,  78,   2,   0, 159, 208,   4,   0,  53,  58, 164, 159,  37, 159, 171,   1,
    248, 117,   4,  98, 212, 218,   5,   2,  54,  59,  93, 121, 176, 181, 134, 130,
      1,  61,  31,   0, 160, 255,  34,   1,   1,  58, 197, 166,   0, 177, 194,   2,
    162, 111,  34,  96, 205, 253,  32,   1,   1,  57, 123, 125, 119, 188, 150, 112,
     78,   4,   0,  75, 166, 180,  20,  38,  78,   1, 143, 246,  42, 113, 156,  37,
    252,   4,   1, 188, 175, 129,   1,  37, 118,   4,  88, 249, 202, 150, 145, 200,
     61,  59,  60,  60, 228, 252, 117,  77,  60,  58, 248, 251,  81, 212, 254, 107,
    198,  59,  58, 169, 250, 251,  81,  80, 100,  58, 154, 250, 251, 252, 252, 252,
};

// Intensity LUT — luma contribution per RGBI intensity-bit pair.
constexpr std::array<double, 4> kIntensity = {77.175381, 88.654656, 166.564623, 174.228438};

// YIQ→RGB matrix coefficients (NTSC FCC standard).
constexpr double kRI =  0.9563, kRQ =  0.6210;
constexpr double kGI = -0.2721, kGQ = -0.6474;
constexpr double kBI = -1.1069, kBQ =  1.7046;

constexpr double kTau = 6.28318530717958647692;

// new_cga macro from MartyPC, ported literal. Combines chroma sample c +
// intensity i + per-channel intensity samples r/g/b.
inline double new_cga(double c, double i, double r, double g, double b) {
    return (c / 0.72) * 0.29
         + (i / 0.28) * 0.32
         + (r / 0.28) * 0.10
         + (g / 0.28) * 0.22
         + (b / 0.28) * 0.07;
}

inline std::uint8_t byte_clamp(std::int32_t v) {
    auto s = v >> 13;
    return static_cast<std::uint8_t>(std::clamp(s, 0, 255));
}

}  // namespace

Context make_context(const Params& p) {
    Context ctx{};
    ctx.cgamode = p.cgamode;
    ctx.bw = (p.cgamode & 0x04) != 0;

    // MartyPC "stock CGA monitor" defaults — luma=1, contrast=1,
    // saturation=1, hue=0 — folded in as constants. Strip the knobs
    // because the user only ever needs to pick old vs new CGA.
    constexpr double brightness = 0.0;
    constexpr double contrast = 100.0;
    constexpr double saturation = 100.0;
    constexpr double hue_offset = 0.0;

    // min_v / max_v from extremes of CHROMA_MULTIPLEXER + intensity range.
    double min_v, max_v;
    if (!p.new_cga) {
        min_v = static_cast<double>(kChromaMultiplexer[0]) + kIntensity[0];
        max_v = static_cast<double>(kChromaMultiplexer[255]) + kIntensity[3];
    } else {
        double i0 = kIntensity[0], i3 = kIntensity[3];
        min_v = new_cga(kChromaMultiplexer[0], i0, i0, i0, i0);
        max_v = new_cga(kChromaMultiplexer[255], i3, i3, i3, i3);
    }
    double mode_contrast = 256.0 / (max_v - min_v);
    double mode_brightness = -min_v * mode_contrast;

    // Per-mode hue offset: 80-col text mode is 14°, everything else 4°.
    double mode_hue = ((p.cgamode & 3) == 1) ? 14.0 : 4.0;

    mode_contrast *= contrast * (p.new_cga ? 1.2 : 1.0) / 100.0;
    mode_brightness += (p.new_cga ? brightness - 10.0 : brightness) * 5.0;
    double mode_saturation = (p.new_cga ? 4.35 : 2.9) * saturation / 100.0;

    // Build composite_table[1024]: every (left_rgbi, right_rgbi, phase) combo.
    for (int x = 0; x < 1024; ++x) {
        int phase = x & 3;
        int right = (x >> 2) & 15;
        int left  = (x >> 6) & 15;
        int rc = right, lc = left;
        if (ctx.bw) {
            rc = (right & 8) | ((right & 7) != 0 ? 7 : 0);
            lc = (left  & 8) | ((left  & 7) != 0 ? 7 : 0);
        }
        double c = static_cast<double>(
            kChromaMultiplexer[static_cast<std::size_t>(((lc & 7) << 5) | ((rc & 7) << 2) | phase)]);
        double i = kIntensity[static_cast<std::size_t>((left >> 3) | ((right >> 2) & 2))];
        double v;
        if (!p.new_cga) {
            v = c + i;
        } else {
            double r = kIntensity[((left >> 2) & 1) | ((right >> 1) & 2)];
            double g = kIntensity[((left >> 1) & 1) | (right & 2)];
            double b = kIntensity[(left & 1) | ((right << 1) & 2)];
            v = new_cga(c, i, r, g, b);
        }
        ctx.composite_table[static_cast<std::size_t>(x)] =
            static_cast<std::int32_t>(v * mode_contrast + mode_brightness);
    }

    // I/Q rotation reference: sample the table at idx 6*68 which corresponds
    // to a known chroma reference (cyan), and derive the IQ axes from that.
    double i_ref = static_cast<double>(
        ctx.composite_table[6 * 68] - ctx.composite_table[6 * 68 + 2]);
    double q_ref = static_cast<double>(
        ctx.composite_table[6 * 68 + 1] - ctx.composite_table[6 * 68 + 3]);

    double a = kTau * (33.0 + 90.0 + hue_offset + mode_hue) / 360.0;
    double cos_a = std::cos(a), sin_a = std::sin(a);
    double r_norm = 256.0 * mode_saturation / std::sqrt(i_ref * i_ref + q_ref * q_ref);

    double iq_adjust_i = -(i_ref * cos_a + q_ref * sin_a) * r_norm;
    double iq_adjust_q =  (q_ref * cos_a - i_ref * sin_a) * r_norm;

    ctx.video_ri = static_cast<std::int32_t>( kRI * iq_adjust_i + kRQ * iq_adjust_q);
    ctx.video_rq = static_cast<std::int32_t>(-kRI * iq_adjust_q + kRQ * iq_adjust_i);
    ctx.video_gi = static_cast<std::int32_t>( kGI * iq_adjust_i + kGQ * iq_adjust_q);
    ctx.video_gq = static_cast<std::int32_t>(-kGI * iq_adjust_q + kGQ * iq_adjust_i);
    ctx.video_bi = static_cast<std::int32_t>( kBI * iq_adjust_i + kBQ * iq_adjust_q);
    ctx.video_bq = static_cast<std::int32_t>(-kBI * iq_adjust_q + kBQ * iq_adjust_i);
    ctx.video_sharpness = 0;  // MartyPC stock = 0
    return ctx;
}

void decode_line(std::span<const std::uint8_t> in,
                 std::span<Color3f> out,
                 const Context& ctx,
                 std::uint8_t border) {
    auto w = in.size();
    if (out.size() < w) return;

    // Padded scratch — needs 5 leading + 5 trailing samples for the
    // filter taps + 2-sample lookahead.
    std::vector<std::int32_t> temp(w + 10, 0);
    std::vector<std::int32_t> atemp(w + 2, 0);
    std::vector<std::int32_t> btemp(w + 2, 0);

    auto& tab = ctx.composite_table;
    const auto* border_tab = &tab[static_cast<std::size_t>(border) * 68];

    // Leading 4-pixel border block (phases 3,0,1,2).
    for (int x = 0; x < 4; ++x) temp[static_cast<std::size_t>(x)] = border_tab[(x + 3) & 3];

    // First real pixel uses border on its left.
    temp[4] = tab[(static_cast<std::size_t>(border & 0x0F) << 6) |
                  (static_cast<std::size_t>(in[0] & 0x0F) << 2) | 3];

    // Interior: temp[5..5+w-1] = tab[(in[i]<<6)|(in[i+1]<<2)|phase]
    for (std::size_t i = 0; i + 1 < w; ++i) {
        temp[5 + i] = tab[(static_cast<std::size_t>(in[i] & 0x0F) << 6) |
                          (static_cast<std::size_t>(in[i + 1] & 0x0F) << 2) | (i & 3)];
    }
    temp[5 + w - 1] = tab[(static_cast<std::size_t>(in[w - 1] & 0x0F) << 6) |
                          (static_cast<std::size_t>(border & 0x0F) << 2) | 3];

    // Trailing 5-pixel border block.
    for (int x = 0; x < 5; ++x) temp[5 + w + static_cast<std::size_t>(x)] = border_tab[x & 3];

    if (ctx.bw) {
        // Greyscale (colour-burst off): just luma.
        for (std::size_t x = 0; x < w; ++x) {
            std::int32_t i_index = static_cast<std::int32_t>(x + 5);
            std::int32_t c = (temp[static_cast<std::size_t>(i_index)] +
                              temp[static_cast<std::size_t>(i_index)]) << 3;
            std::int32_t d = (temp[static_cast<std::size_t>(i_index - 1)] +
                              temp[static_cast<std::size_t>(i_index + 1)]) << 3;
            std::int32_t y = ((c + d) << 8) + ctx.video_sharpness * (c - d);
            auto v = byte_clamp(y);
            out[x] = color_space::srgb_u8_to_linear(v, v, v);
        }
        return;
    }

    // High-pass filters for chroma extraction.
    // MartyPC writes atemp[ap_index + x - 1] with ap_index=1 → atemp[x].
    std::int32_t i_index = 4;
    for (std::size_t x = 0; x < w + 2; ++x) {
        atemp[x] = temp[static_cast<std::size_t>(i_index - 4)]
                 - ((temp[static_cast<std::size_t>(i_index - 2)]
                   - temp[static_cast<std::size_t>(i_index)]
                   + temp[static_cast<std::size_t>(i_index + 2)]) << 1)
                 + temp[static_cast<std::size_t>(i_index + 4)];
        btemp[x] = (temp[static_cast<std::size_t>(i_index - 3)]
                  - temp[static_cast<std::size_t>(i_index - 1)]
                  + temp[static_cast<std::size_t>(i_index + 1)]
                  - temp[static_cast<std::size_t>(i_index + 3)]) << 1;
        ++i_index;
    }

    // Bias temp samples 4 and 5 before the streaming loop. MartyPC's
    // ap_index starts at 1 here, so temp[4] uses atemp[0], temp[5]
    // uses atemp[1].
    temp[4] = (temp[4] << 3) - atemp[0];
    temp[5] = (temp[5] << 3) - atemp[1];

    i_index = 5;
    std::int32_t ap_index = 1;
    std::int32_t bp_index = 1;

    // Block-of-4 decode: each iteration emits 4 output pixels using the
    // 4 phase rotations of (I, Q):
    //   pixel 0: ( a,  b)
    //   pixel 1: (-b,  a)
    //   pixel 2: (-a, -b)
    //   pixel 3: ( b, -a)
    // Matches MartyPC composite_new.rs's composite_process loop body
    // verbatim. Earlier port had pixels 1 and 3 swapped, which rotated
    // the chroma the wrong way and produced sickly green/magenta
    // artifacts where MartyPC shows orange/cyan.
    auto blocks = w / 4;
    auto emit = [&](std::int32_t i_comp, std::int32_t q_comp, std::size_t out_idx) {
        temp[static_cast<std::size_t>(i_index + 1)] =
            (temp[static_cast<std::size_t>(i_index + 1)] << 3)
            - atemp[static_cast<std::size_t>(ap_index + 1)];
        std::int32_t c = temp[static_cast<std::size_t>(i_index)]
                       + temp[static_cast<std::size_t>(i_index)];
        std::int32_t d = temp[static_cast<std::size_t>(i_index - 1)]
                       + temp[static_cast<std::size_t>(i_index + 1)];
        std::int32_t y = ((c + d) << 8) + ctx.video_sharpness * (c - d);
        std::int32_t rr = y + ctx.video_ri * i_comp + ctx.video_rq * q_comp;
        std::int32_t gg = y + ctx.video_gi * i_comp + ctx.video_gq * q_comp;
        std::int32_t bb = y + ctx.video_bi * i_comp + ctx.video_bq * q_comp;
        ++i_index;
        ++ap_index;
        ++bp_index;
        out[out_idx] = color_space::srgb_u8_to_linear(
            byte_clamp(rr), byte_clamp(gg), byte_clamp(bb));
    };
    for (std::size_t blk = 0; blk < blocks; ++blk) {
        std::int32_t a = atemp[static_cast<std::size_t>(ap_index)];
        std::int32_t b = btemp[static_cast<std::size_t>(bp_index)];
        emit( a,  b, blk * 4 + 0);
        // After ++ap_index/++bp_index, re-read a/b for the next phase.
        a = atemp[static_cast<std::size_t>(ap_index)];
        b = btemp[static_cast<std::size_t>(bp_index)];
        emit(-b,  a, blk * 4 + 1);
        a = atemp[static_cast<std::size_t>(ap_index)];
        b = btemp[static_cast<std::size_t>(bp_index)];
        emit(-a, -b, blk * 4 + 2);
        a = atemp[static_cast<std::size_t>(ap_index)];
        b = btemp[static_cast<std::size_t>(bp_index)];
        emit( b, -a, blk * 4 + 3);
    }
    // Tail (w not multiple of 4) — fall back to mean luma. Rare for
    // CGA's 320/640-wide modes, but safe.
    for (std::size_t x = blocks * 4; x < w; ++x) {
        out[x] = color_space::srgb_u8_to_linear(128, 128, 128);
    }
}

// Per-cell pattern-dither encoder.
//
// CGA composite produces artifact colours from sub-pixel structure
// inside each 4-pixel cell. The cycle's frequency content drives
// what the NTSC chroma demodulator sees:
//
//   aaaa : DC               — solid RGBI primary (16 colours)
//   aabb : 3.58 MHz (chroma) — strong artifact colour (~144 distinct)
//   abab : 7.16 MHz          — gets aliased / attenuated, near null
//
// The "1024-colour CGA" demos work by picking the right (a, b) AABB
// pair per cell so the demodulated artifact colour matches the target
// picture. So we enumerate all 16×16 (a,b) AABB candidates, decode
// each through the real NTSC filter, and pick the pair whose decoded
// cell colour is closest to the source's 4-pixel mean in OKLab.
namespace {

// Pre-decoded artifact-cell entry. (a, b) generate an aabb 4-pixel cell.
// `mean_linear` is the linear-RGB mean of the four decoded output pixels;
// `mean_lab` is the OKLab of that mean. We match on the mean because a
// real composite monitor's bandwidth + viewer's eye low-pass the chroma
// stripes inside each cell — the visible cell colour IS the mean — while
// per-pixel matching biases the encoder toward neutral solids and loses
// the entire artifact-colour gamut.
struct CellEntry {
    Color3f mean_linear;
    color_space::OKLab mean_lab;
    std::uint8_t a, b;
};

std::vector<CellEntry> build_cell_palette(const Context& ctx) {
    constexpr std::size_t kProbeW = 64;
    constexpr std::size_t kCentre = 28;  // start of a centred phase-0 cell
    constexpr std::size_t kCells = 2;    // average across two cells for stability
    std::vector<std::uint8_t> row(kProbeW);
    std::vector<Color3f>      dec(kProbeW);
    std::vector<CellEntry>    pal;
    pal.reserve(256);
    for (int a = 0; a < 16; ++a) {
        for (int b = 0; b < 16; ++b) {
            for (std::size_t i = 0; i < kProbeW; ++i)
                row[i] = static_cast<std::uint8_t>(((i >> 1) & 1) ? b : a);
            decode_line(row, std::span<Color3f>(dec), ctx);
            float r = 0, g = 0, bb = 0;
            constexpr std::size_t kAvg = kCells * 4;
            for (std::size_t i = kCentre; i < kCentre + kAvg; ++i) {
                r  += dec[i].r;
                g  += dec[i].g;
                bb += dec[i].b;
            }
            float inv = 1.0f / static_cast<float>(kAvg);
            Color3f mean{r * inv, g * inv, bb * inv};
            pal.push_back({mean, color_space::linear_to_oklab(mean),
                           static_cast<std::uint8_t>(a),
                           static_cast<std::uint8_t>(b)});
        }
    }
    return pal;
}

}  // namespace

void encode_line(std::span<const Color3f> src,
                 std::span<std::uint8_t> out,
                 const Context& ctx) {
    auto w = src.size();
    if (out.size() < w) return;
    if (w == 0) return;

    static thread_local std::vector<CellEntry> cell_pal;
    static thread_local const Context* cell_ctx = nullptr;
    if (cell_ctx != &ctx || cell_pal.empty()) {
        cell_pal = build_cell_palette(ctx);
        cell_ctx = &ctx;
    }

    // Floyd-Steinberg between cells in linear-RGB: pick the closest mean,
    // propagate the cell's residual error to the next cell on the same
    // row and the cell directly below on the next row. The error is one
    // per cell (each cell's 4 pixels share the same artifact mean), so a
    // 2-weight (7/16 right, 9/16 down) split is plenty.
    //
    // Per-row state is local to this row; cross-row diffusion would
    // require a wider buffer than encode_line owns. Inter-row ED, if
    // wanted, can be added by the caller buffering residuals between
    // rows — but in practice the within-row spread already breaks up
    // banding on smooth gradients.
    auto cells = w / 4;
    std::vector<Color3f> next_err(cells + 1, Color3f{0, 0, 0});
    Color3f carry{0, 0, 0};

    for (std::size_t c = 0; c < cells; ++c) {
        std::size_t x = c * 4;
        // Source cell mean (4 pixels) + diffused error.
        float r = 0, g = 0, b = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            r += src[x + i].r;
            g += src[x + i].g;
            b += src[x + i].b;
        }
        Color3f target{r * 0.25f + carry.r,
                       g * 0.25f + carry.g,
                       b * 0.25f + carry.b};
        auto src_lab = color_space::linear_to_oklab(target);

        // Chroma-weighted OKLab: bump a/b vs L so the closest match for a
        // saturated source actually picks an artifact cell rather than a
        // luma-matched grey solid. 2× a/b is gentle — keeps grey sources
        // landing on greys but breaks the tie toward chroma when the
        // source has any saturation at all.
        constexpr float kAbWeight = 2.0f;
        std::size_t best = 0;
        float best_d = std::numeric_limits<float>::max();
        for (std::size_t k = 0; k < cell_pal.size(); ++k) {
            float dL = src_lab.L - cell_pal[k].mean_lab.L;
            float da = (src_lab.a - cell_pal[k].mean_lab.a) * kAbWeight;
            float db = (src_lab.b - cell_pal[k].mean_lab.b) * kAbWeight;
            float d = dL * dL + da * da + db * db;
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        std::uint8_t a = cell_pal[best].a;
        std::uint8_t bv = cell_pal[best].b;
        for (std::size_t i = 0; i < 4; ++i)
            out[x + i] = ((i >> 1) & 1) ? bv : a;

        // Residual in linear RGB. 7/16 to right cell, 9/16 carried by
        // next_err (used as left-of-next-row would be in 2D FS, but we
        // only fold this row's residual forward — it still serves as a
        // damped propagation against banding).
        const Color3f& m = cell_pal[best].mean_linear;
        Color3f err{target.r - m.r, target.g - m.g, target.b - m.b};
        constexpr float kRight = 0.7f;
        carry = {err.r * kRight, err.g * kRight, err.b * kRight};
    }

    // Tail (w not multiple of 4): emit solid black, rare for 160/320-wide.
    for (std::size_t x = cells * 4; x < w; ++x) out[x] = 0;
}

void apply_monitor_lp(std::span<Color3f> row) {
    auto w = row.size();
    if (w < 2) return;
    std::vector<Color3f> tmp(w);
    auto get = [&](std::ptrdiff_t i) -> Color3f {
        if (i < 0) i = 0;
        if (static_cast<std::size_t>(i) >= w) i = static_cast<std::ptrdiff_t>(w - 1);
        return row[static_cast<std::size_t>(i)];
    };
    for (std::size_t x = 0; x < w; ++x) {
        auto p0 = get(static_cast<std::ptrdiff_t>(x) - 2);
        auto p1 = get(static_cast<std::ptrdiff_t>(x) - 1);
        auto p2 = row[x];
        auto p3 = get(static_cast<std::ptrdiff_t>(x) + 1);
        auto p4 = get(static_cast<std::ptrdiff_t>(x) + 2);
        tmp[x] = {
            (p0.r + 4 * p1.r + 6 * p2.r + 4 * p3.r + p4.r) * (1.0f / 16.0f),
            (p0.g + 4 * p1.g + 6 * p2.g + 4 * p3.g + p4.g) * (1.0f / 16.0f),
            (p0.b + 4 * p1.b + 6 * p2.b + 4 * p3.b + p4.b) * (1.0f / 16.0f),
        };
    }
    std::copy(tmp.begin(), tmp.end(), row.begin());
}

}  // namespace png2amiga::cga_composite
