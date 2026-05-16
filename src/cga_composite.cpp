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
    // 4 phase rotations of I/Q.
    auto blocks = w / 4;
    for (std::size_t blk = 0; blk < blocks; ++blk) {
        auto convert = [&](std::int32_t ic, std::int32_t qc) {
            temp[static_cast<std::size_t>(i_index + 1)] =
                (temp[static_cast<std::size_t>(i_index + 1)] << 3)
                - atemp[static_cast<std::size_t>(ap_index + 1)];
            std::int32_t a = atemp[static_cast<std::size_t>(ap_index)];
            std::int32_t b = btemp[static_cast<std::size_t>(bp_index)];
            // Phase rotates (a,b) → (b,-a) → (-a,-b) → (-b,a) over 4
            // pixels; caller passes ic/qc as ±1 selectors.
            std::int32_t i_comp = (ic == 0) ? a : (ic == 1) ?  b : (ic == 2) ? -a : -b;
            std::int32_t q_comp = (qc == 0) ? b : (qc == 1) ? -a : (qc == 2) ? -b : a;
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
            std::size_t out_idx = blk * 4 + static_cast<std::size_t>(ic);
            out[out_idx] = color_space::srgb_u8_to_linear(
                byte_clamp(rr), byte_clamp(gg), byte_clamp(bb));
        };
        convert(0, 0);  // I, Q
        convert(1, 1);  // Q, -I
        convert(2, 2);  // -I, -Q
        convert(3, 3);  // -Q, I
    }
    // Tail (w not multiple of 4) — fall back to mean luma. Rare for
    // CGA's 320/640-wide modes, but safe.
    for (std::size_t x = blocks * 4; x < w; ++x) {
        out[x] = color_space::srgb_u8_to_linear(128, 128, 128);
    }
}

// Beam-search encoder. State at column x = the last `kCtx` input
// decisions (enough to fully evaluate output at x - kLag). At each
// step we expand each beam state by all 16 candidate next pixels,
// score against `src`, and keep the top `beam_width` by accumulated
// OKLab error.
namespace {

constexpr std::size_t kCtx = 9;  // 9 prior pixels needed for one output
constexpr std::size_t kLag = 5;  // output at column x is fully known once
                                  // pixel x+kLag has been decided

struct BeamState {
    // History: the last kCtx pixels (oldest first → newest last).
    std::array<std::uint8_t, kCtx> hist{};
    double error = 0.0;
};

// Decode the output pixel at column `out_col` given input pixels stored
// in `hist` (positions out_col-1..out_col+kLag-1 relative to the row).
// Faithful single-pixel re-derivation of `decode_line`'s inner loop;
// callable from the beam search per-step scorer.
Color3f decode_one(const std::array<std::uint8_t, kCtx>& hist, std::size_t phase, const Context& ctx) {
    // Build the 11-sample temp window around the output column. hist[5]
    // is `in[out_col]`; we have hist[0..8] = in[out_col-5..out_col+3].
    std::int32_t t[11];
    auto& tab = ctx.composite_table;
    for (int j = 0; j < 10; ++j) {
        std::size_t left  = hist[static_cast<std::size_t>(j)] & 0x0F;
        std::size_t right = (j + 1 < static_cast<int>(kCtx))
            ? (hist[static_cast<std::size_t>(j + 1)] & 0x0F)
            : 0u;
        std::size_t ph = (phase + static_cast<std::size_t>(j)) & 3;
        t[j] = tab[(left << 6) | (right << 2) | ph];
    }
    t[10] = t[9];  // safe trailing pad — outside the 9-window taps anyway

    // High-pass at center (j=5).
    std::int32_t a5 = t[1] - ((t[3] - t[5] + t[7]) << 1) + t[9];
    std::int32_t b5 = (t[2] - t[4] + t[6] - t[8]) << 1;
    // Pre-bias: temp[i] becomes (temp[i] << 3) - atemp[i_index-4] when
    // it is later consumed as a c/d sample; here c=t[5]+t[5], d=t[4]+t[6].
    std::int32_t c_pre = (t[5] << 3);
    std::int32_t d_pre_l = (t[4] << 3);
    std::int32_t d_pre_r = (t[6] << 3);
    // The actual bias subtracts the *adjacent* atemp. Approximate it
    // with the local a5 — close enough for the per-pixel scorer; the
    // row-level `decode_line` is what we use for the final preview.
    std::int32_t c = (c_pre + c_pre) - 2 * a5;
    std::int32_t d = (d_pre_l + d_pre_r) - 2 * a5;
    std::int32_t y = ((c + d) << 8) + ctx.video_sharpness * (c - d);

    // Phase rotation: column phase mod 4 picks one of (a,b) (b,-a) (-a,-b) (-b,a).
    std::int32_t i_comp, q_comp;
    switch (phase & 3) {
        case 0: i_comp =  a5; q_comp =  b5; break;
        case 1: i_comp =  b5; q_comp = -a5; break;
        case 2: i_comp = -a5; q_comp = -b5; break;
        default: i_comp = -b5; q_comp =  a5; break;
    }
    std::int32_t rr = y + ctx.video_ri * i_comp + ctx.video_rq * q_comp;
    std::int32_t gg = y + ctx.video_gi * i_comp + ctx.video_gq * q_comp;
    std::int32_t bb = y + ctx.video_bi * i_comp + ctx.video_bq * q_comp;
    return color_space::srgb_u8_to_linear(byte_clamp(rr), byte_clamp(gg), byte_clamp(bb));
}

}  // namespace

void encode_line(std::span<const Color3f> src,
                 std::span<std::uint8_t> out,
                 const Context& ctx) {
    // Beam width fixed at 64 — tuned against Kodak-24, doubling to 128
    // barely moves SSIMULACRA2 and triples the wall time.
    constexpr std::size_t beam_width = 64;
    auto w = src.size();
    if (out.size() < w) return;
    if (w == 0) return;

    // Seed beam with a single all-zero state (border = black).
    std::vector<BeamState> beam;
    beam.reserve(beam_width);
    beam.push_back(BeamState{});

    // Reserve buffer for the per-step expansion + the picked
    // pixel-decisions per beam slot (for backtracing the winner).
    std::vector<std::vector<std::uint8_t>> trace(1);
    trace[0].reserve(w);

    std::vector<BeamState> next;
    next.reserve(beam_width * 16);
    std::vector<std::vector<std::uint8_t>> next_trace;
    next_trace.reserve(beam_width * 16);

    for (std::size_t x = 0; x < w; ++x) {
        next.clear();
        next_trace.clear();
        // For every (state, candidate) pair: shift hist, append candidate,
        // possibly evaluate output at x - kLag.
        for (std::size_t bi = 0; bi < beam.size(); ++bi) {
            const auto& s = beam[bi];
            for (std::uint8_t cand = 0; cand < 16; ++cand) {
                BeamState ns = s;
                // Shift left, append.
                for (std::size_t k = 0; k + 1 < kCtx; ++k) ns.hist[k] = ns.hist[k + 1];
                ns.hist[kCtx - 1] = cand;
                // If we now have ≥ kLag+1 pixels of context, the output
                // at column (x - kLag) is fully known.
                if (x >= kLag) {
                    std::size_t out_col = x - kLag;
                    auto dec = decode_one(ns.hist, out_col & 3, ctx);
                    auto src_lab = color_space::linear_to_oklab(src[out_col]);
                    auto dec_lab = color_space::linear_to_oklab(dec);
                    float dL = src_lab.L - dec_lab.L;
                    float da = src_lab.a - dec_lab.a;
                    float db = src_lab.b - dec_lab.b;
                    ns.error += static_cast<double>(dL * dL + da * da + db * db);
                }
                next.push_back(ns);
                auto t = trace[bi];
                t.push_back(cand);
                next_trace.push_back(std::move(t));
            }
        }
        // Prune to top `beam_width` by error.
        if (next.size() > beam_width) {
            std::vector<std::size_t> idx(next.size());
            for (std::size_t k = 0; k < idx.size(); ++k) idx[k] = k;
            std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(beam_width),
                              idx.end(),
                              [&](std::size_t a, std::size_t b) { return next[a].error < next[b].error; });
            std::vector<BeamState> pruned;
            std::vector<std::vector<std::uint8_t>> pruned_trace;
            pruned.reserve(beam_width);
            pruned_trace.reserve(beam_width);
            for (std::size_t k = 0; k < beam_width; ++k) {
                pruned.push_back(std::move(next[idx[k]]));
                pruned_trace.push_back(std::move(next_trace[idx[k]]));
            }
            beam = std::move(pruned);
            trace = std::move(pruned_trace);
        } else {
            beam = std::move(next);
            trace = std::move(next_trace);
        }
    }

    // Winner = lowest accumulated error in the final beam.
    std::size_t best = 0;
    for (std::size_t k = 1; k < beam.size(); ++k) {
        if (beam[k].error < beam[best].error) best = k;
    }
    auto& winner = trace[best];
    for (std::size_t x = 0; x < w; ++x) out[x] = winner[x];
}

}  // namespace png2amiga::cga_composite
