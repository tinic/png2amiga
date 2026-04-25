#include "scap.hpp"

#include "bitplane.hpp"
#include "color_space.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "quantize.hpp"
#include "types.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <vector>

namespace png2amiga::scap {

namespace {

// Build a 6-plane DPF frame where every pixel = PF2 index 1, plus a
// position-marker grid baked into PF1 (foreground) for readability.
//
// PF2 LSB sits at 0-indexed plane 1 (= hardware BPL2). Setting all of
// plane 1 to 0xFF gives PF2 index 1 across the entire frame; PF2 maps
// that to color register 9 (OCS) / 17 (AGA) via PF2OF.
//
// PF1 markers — drawn in front of PF2 (PF2PRI=0). PF1 has 3 planes at
// 0-indexed positions 0, 2, 4 (PF1 LSB / mid / MSB):
//   * minor tick (1 px wide, every 16 px): PF1 plane 0 bit set
//     -> PF1 index 1 -> color register 1
//   * major tick (1 px wide, every 64 px): PF1 planes 0 + 2 bit set
//     (every 64 px is also a multiple of 16, so plane 0 is on too)
//     -> PF1 index 1|2 = 3 -> color register 3
// The probe's frame-start palette assigns reg 1 = bright yellow and
// reg 3 = bright red, so the pixel where the SCAP MOVE fires can be
// read off against the embedded ruler.
Result<bitplane::BitplaneData> make_dpf_pf2_index1_planes(std::size_t width,
                                                          std::size_t height,
                                                          std::size_t total_planes,
                                                          bool add_position_grid) {
    if (total_planes != 6 && total_planes != 8) {
        return std::unexpected{Error{
            ErrorCode::invalid_depth,
            std::format("SCAP probe: expected 6 (OCS) or 8 (AGA) planes, got {}",
                        total_planes),
        }};
    }
    if (width == 0 || height == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "SCAP probe: zero dimensions",
        }};
    }

    auto aligned_width = (width + 15u) & ~std::size_t{15};
    auto bpr = aligned_width / 8;

    bitplane::BitplaneData planes;
    planes.width = width;
    planes.height = height;
    planes.depth = total_planes;
    planes.bytes_per_row = bpr;
    planes.layout = bitplane::Layout::interleaved;
    planes.data.assign(planes.total_bytes(), 0);

    // Set every byte of plane 1 (PF2 LSB) to 0xFF -> PF2 index 1 everywhere.
    for (std::size_t y = 0; y < height; ++y) {
        auto off = planes.plane_row_offset(/*plane=*/1, y);
        std::fill_n(planes.data.data() + off, bpr,
                    static_cast<std::uint8_t>(0xFF));
    }

    if (add_position_grid) {
        // Set bit at column x in plane p, row y: byte = x/8, bit = 7 - (x%8).
        auto set_pixel = [&](std::size_t plane, std::size_t y, std::size_t x) {
            auto off = planes.plane_row_offset(plane, y);
            planes.data[off + x / 8] |=
                static_cast<std::uint8_t>(1u << (7 - (x % 8)));
        };
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; x += 16) {
                set_pixel(/*plane=*/0, y, x);                 // minor tick
                if (x % 64 == 0) set_pixel(/*plane=*/2, y, x); // major tick
            }
        }
    }
    return planes;
}

// HPOS sweep across the per-line copper budget [0x00, 0xE3]. 0xE3 is the
// HPOS used by the existing CAP encoder for interlace per-line WAITs —
// past that there's no DMA-allowed window before the next horizontal
// blank ends.
constexpr int kHpMin = 0x00;
constexpr int kHpMax = 0xE3;

int hp_for_y(int y, int height) {
    if (height <= 1) return kHpMin;
    long num = static_cast<long>(y) * (kHpMax - kHpMin);
    long den = static_cast<long>(height - 1);
    int hp = static_cast<int>(num / den) + kHpMin;
    return std::clamp(hp, kHpMin, kHpMax);
}

ScapMove make_wait(std::uint8_t hpos, std::uint8_t vpos, int slot_index = -1) {
    ScapMove w{};
    w.kind = ScapOpKind::kWait;
    w.hpos = hpos;
    w.vpos = vpos;
    w.slot_index = slot_index;
    return w;
}

ScapMove make_move(std::uint8_t reg, std::uint16_t rgb_ocs, int slot_index = -1) {
    ScapMove m{};
    m.kind = ScapOpKind::kMove;
    m.reg = reg;
    m.rgb_ocs = rgb_ocs;
    m.slot_index = slot_index;
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// Planner — OCS DPF, 6-plane lores 320 px.
//
// Two-stage approach to keep F-S texture flowing across the whole frame
// while still letting each strip pick a palette tuned to its content:
//
//   Stage 1 (planning) — global F-S vs the 8-colour base palette
//   produces a per-pixel base_index whose distribution per strip drives
//   the per-line MOVE planner. Each strip's swap is the OKLab centroid
//   of the strip pixels currently assigned to one of its registers,
//   chosen for biggest error reduction.
//
//   Stage 2 (rendering) — runs F-S a second time, but this time against
//   the EVOLVING per-strip palette: for each pixel the nearest-colour
//   lookup uses strip_palettes[strip(x)], and residuals propagate
//   through the standard F-S kernel across strip boundaries. Errors
//   are in linear RGB so they discharge in whichever palette is active
//   downstream — F-S texture is uniform; only the colour rendition
//   shifts at strip boundaries (and only by the amount the palette
//   actually changed).
//
//   This combines:
//     * uniform dither texture across the frame (no per-strip "blocks")
//     * per-strip palette specialisation (good colour fidelity)
// ---------------------------------------------------------------------------
Result<ScapResult> encode_scap_dpf_ocs(const Image& image,
                                       int width_arg,
                                       int height_arg,
                                       bool reserve_color0,
                                       const dither::Settings& dither_settings,
                                       bool debug_overlay) {
    auto& table = scap_table_for(6);
    if (table.slots.empty()) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "SCAP planner: kScap6bplOcs slot table is empty",
        }};
    }

    auto width = (width_arg > 0) ? static_cast<std::size_t>(width_arg)
                                  : image.width();
    auto height = (height_arg > 0) ? static_cast<std::size_t>(height_arg)
                                    : image.height();
    if (image.width() != width || image.height() != height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("SCAP planner: image is {}x{} but caller asked for "
                        "{}x{} — resize before calling",
                        image.width(), image.height(), width, height),
        }};
    }

    // ---- 0. Optional debug source: synthetic 4-ramp test pattern --------
    // When debug_overlay is on, replace the input image with the same
    // 4-ramps-per-line test pattern as examples/ramps.png (black->green,
    // black->red, black->blue, black->white; 16 steps × 5 lores px each).
    // The whole debug bundle (this ramp + black base palette + yellow
    // PF1 rulers) is the canonical visual test case for slot-tuning.
    Image ramps_holder;
    const Image* src_image = &image;
    if (debug_overlay) {
        ramps_holder = Image(width, height);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                std::size_t band = (x * 4) / std::max<std::size_t>(width, 1);
                std::size_t band_w = std::max<std::size_t>(width / 4, 1);
                std::size_t band_x = x - band * band_w;
                std::size_t step = (band_x * 16) / std::max<std::size_t>(band_w, 1);
                if (step > 15) step = 15;
                float v = static_cast<float>(step) / 15.0f;  // 0..1 sRGB
                // Match examples/ramps.png: bands are sRGB ramps, so we
                // need to convert to linear before storing in the Image.
                float lin = color_space::srgb_to_linear(v);
                Color3f c{0.0f, 0.0f, 0.0f};
                if      (band == 0) c.g = lin;
                else if (band == 1) c.r = lin;
                else if (band == 2) c.b = lin;
                else                c = Color3f{lin, lin, lin};
                ramps_holder[x, y] = c;
            }
        }
        src_image = &ramps_holder;
    }
    auto& src = *src_image;

    // ---- 1. Quantise to 8 PF2 colours (reserve idx 0 if requested) -------
    constexpr int kBaseColors = 8;        // PF2 width = 3 bitplanes
    constexpr int kRegBase = 8;           // PF2 starts at COLOR08 (PF2OF=+8)
    int qcount = reserve_color0 ? kBaseColors - 1 : kBaseColors;
    auto quantised = quantize::quantize(src,
                                        static_cast<std::size_t>(qcount),
                                        quantize::Algorithm::ocs_bruteforce,
                                        /*diversity=*/0);
    if (!quantised) return std::unexpected{quantised.error()};

    std::array<Color3f, kBaseColors> base_palette{};
    for (auto& c : base_palette) c = Color3f{0.0f, 0.0f, 0.0f};
    std::size_t off = reserve_color0 ? 1u : 0u;
    for (std::size_t i = 0; i < quantised->colors.size() &&
                            (off + i) < static_cast<std::size_t>(kBaseColors); ++i) {
        base_palette[off + i] = palette::quantize_to_ocs(quantised->colors[i]);
    }

    // ---- 2. Global dither vs base palette --------------------------------
    // dither::apply F-S's the whole image once against the 8-colour base
    // palette. base_index[y*w+x] is the resulting palette index per pixel
    // — these are the bitplane bits the hardware will read. The dither
    // texture is uniform across the entire frame (no strip boundaries),
    // which is what avoids the blocking artifacts a per-strip dither
    // produces.
    std::span<const Color3f> base_pal_span(base_palette.data(), kBaseColors);
    auto dith_result = dither::apply(src, base_pal_span, dither_settings);
    auto& base_index = dith_result.indices;

    // OKLab cache of the SOURCE pixels (used by per-strip swap planning).
    std::vector<color_space::OKLab> img_lab(width * height);
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            img_lab[y * width + x] = color_space::linear_to_oklab(src[x, y]);

    // ---- 3. Per-line greedy planner -------------------------------------
    std::vector<std::uint8_t> indices(width * height, 0);
    std::vector<std::vector<ScapMove>> line_moves(height);
    Image preview(width, height);

    auto P = base_palette;
    std::array<color_space::OKLab, kBaseColors> P_lab{};
    auto recompute_lab = [&]() {
        for (std::size_t k = 0; k < kBaseColors; ++k)
            P_lab[k] = color_space::linear_to_oklab(P[k]);
    };
    recompute_lab();

    std::size_t k_min = reserve_color0 ? 1u : 0u;

    // Stage-2 error diffusion setup. Honours the user's --dither choice
    // by pulling the diffusion kernel (F-S, Atkinson, Sierra-Lite,
    // Stucki, Jarvis) from dither.hpp. Wider kernels (Stucki, Jarvis,
    // 5×3) spread residuals further than F-S's 4-neighbour cluster, so
    // errors can flow across multiple 16-px strips before fully
    // discharging — the original "localised dither" symptom was a
    // hardcoded F-S kernel here that ignored the CLI choice.
    bool ordered = dither::is_ordered(dither_settings.method);
    bool err_diffuse =
        dither_settings.method != dither::Method::none &&
        dither_settings.strength > 0.0f && !ordered;
    auto kernel = err_diffuse
        ? dither::error_diffusion_kernel(dither_settings.method)
        : std::span<const dither::DiffusionEntry>{};
    int kernel_max_dy = 0;
    for (auto& e : kernel) kernel_max_dy = std::max(kernel_max_dy, e.dy);
    float clamp_v = dither_settings.error_clamp;

    // Three row buffers — Stucki/Jarvis reach dy=2, F-S/Sierra/Atkinson
    // cover dy=0..2 too. +4 horizontal padding so kernel writes at
    // x+{-2..+2} can stay bounds-check-free.
    constexpr std::size_t kPad = 4;
    std::array<std::vector<Color3f>, 3> err_rows{
        std::vector<Color3f>(width + 2 * kPad, Color3f{0.0f, 0.0f, 0.0f}),
        std::vector<Color3f>(width + 2 * kPad, Color3f{0.0f, 0.0f, 0.0f}),
        std::vector<Color3f>(width + 2 * kPad, Color3f{0.0f, 0.0f, 0.0f}),
    };

    auto clamp_err = [&](Color3f c) {
        if (clamp_v <= 0.0f) return c;
        c.r = std::clamp(c.r, -clamp_v, clamp_v);
        c.g = std::clamp(c.g, -clamp_v, clamp_v);
        c.b = std::clamp(c.b, -clamp_v, clamp_v);
        return c;
    };

    // For each strip pick the swap that maximises error reduction.
    // base_index[pixel] is FIXED (set by the global F-S pass), so the
    // per-strip "cluster" of a register k is exactly the strip pixels
    // whose base_index equals k. We replace P[k] with the OKLab centroid
    // of those source pixels, snapped to the OCS palette.
    auto find_best_swap_in_strip =
        [&](std::size_t y, std::size_t x_lo, std::size_t x_hi,
            int& out_reg, Color3f& out_color, float& out_reduction) {
            std::array<double, kBaseColors> sumL{}, suma{}, sumb{}, count{}, cur_err{};
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto k = static_cast<std::size_t>(base_index[y * width + x]);
                auto& lab = img_lab[y * width + x];
                sumL[k] += static_cast<double>(lab.L);
                suma[k] += static_cast<double>(lab.a);
                sumb[k] += static_cast<double>(lab.b);
                count[k] += 1.0;
                float dL = lab.L - P_lab[k].L;
                float da = lab.a - P_lab[k].a;
                float db = lab.b - P_lab[k].b;
                cur_err[k] += static_cast<double>(dL * dL + da * da + db * db);
            }
            out_reg = -1;
            out_reduction = 0.0f;
            for (std::size_t k = k_min; k < kBaseColors; ++k) {
                if (count[k] < 1.0) continue;
                color_space::OKLab centroid{
                    static_cast<float>(sumL[k] / count[k]),
                    static_cast<float>(suma[k] / count[k]),
                    static_cast<float>(sumb[k] / count[k]),
                };
                double new_err = 0.0;
                for (std::size_t x = x_lo; x < x_hi; ++x) {
                    if (base_index[y * width + x] != k) continue;
                    auto& lab = img_lab[y * width + x];
                    float dL = lab.L - centroid.L;
                    float da = lab.a - centroid.a;
                    float db = lab.b - centroid.b;
                    new_err += static_cast<double>(dL * dL + da * da + db * db);
                }
                float red = static_cast<float>(cur_err[k] - new_err);
                if (red > out_reduction) {
                    auto linear = color_space::oklab_to_linear(centroid).clamped();
                    linear = palette::quantize_to_ocs(linear);
                    out_reg = static_cast<int>(k);
                    out_color = linear;
                    out_reduction = red;
                }
            }
        };

    constexpr int kVStart = 44;
    constexpr std::uint8_t kFillerReg = 31;       // COLOR31 — unread in OCS DPF 3+3
    constexpr std::uint16_t kFillerVal = 0x0000;
    double total_error = 0.0;
    std::size_t total_moves = 0;

    // strip_palettes[s] is the palette state during pixels in strip s.
    // strip 0 = entry palette (P at start of line); strip s+1 = palette
    // after slot s's MOVEs are applied. There are slots.size()+1 strips.
    std::size_t num_strips = table.slots.size() + 1;
    std::vector<std::array<Color3f, kBaseColors>> strip_palettes(num_strips);
    std::vector<std::array<color_space::OKLab, kBaseColors>> strip_pal_lab(num_strips);

    // slots[s].pixel_x is the LEFT edge of strip s+1 (i.e. strip s+1 covers
    // pixels [slots[s].pixel_x .. slots[s+1].pixel_x), and strip s+1 uses
    // the palette state AFTER slot s's MOVE has fired). Strip 0 is the
    // entry palette and covers [0 .. slots[0].pixel_x).
    auto strip_for_x = [&](std::size_t x) -> std::size_t {
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            if (x < static_cast<std::size_t>(table.slots[s].pixel_x))
                return s;
        }
        return table.slots.size();
    };

    // Precompute pixel x → strip-index for the F-S boundary check below.
    std::vector<std::uint16_t> x_strip(width);
    for (std::size_t x = 0; x < width; ++x)
        x_strip[x] = static_cast<std::uint16_t>(strip_for_x(x));

    for (std::size_t y = 0; y < height; ++y) {
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // ---- Per-line CAP-style reset: palette starts each line at base ----
        P = base_palette;
        recompute_lab();
        strip_palettes[0] = P;
        strip_pal_lab[0] = P_lab;

        // 1. 8 base-palette MOVEs (write COLOR08..15). These fire in the
        //    horizontal blank just before this line's display area. In
        //    debug_overlay mode they all write 0x0000 so the line opens
        //    with PF2 regs black and every visible colour change is
        //    attributable to a SCAP MOVE.
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            line_moves[y].push_back(make_move(
                static_cast<std::uint8_t>(kRegBase + k),
                debug_overlay ? std::uint16_t{0x0000}
                              : palette::linear_to_ocs(base_palette[k]),
                -1));
        }

        // 2. Line-gate WAIT — opens the SCAP chain at HPOS=line_gate_hpos.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));

        // 3. 20 SCAP MOVEs back-to-back. Greedy per-slot swap selection;
        //    fall back to a harmless filler MOVE if no useful swap.
        //    Slot s's MOVE fires AT slots[s].pixel_x, so the strip it
        //    affects is [slots[s].pixel_x .. slots[s+1].pixel_x) — that's
        //    what the swap planner targets.
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            std::size_t x_lo = std::min(width,
                static_cast<std::size_t>(table.slots[s].pixel_x));
            std::size_t x_hi = (s + 1 < table.slots.size())
                ? std::min(width,
                    static_cast<std::size_t>(table.slots[s + 1].pixel_x))
                : width;

            int swap_reg = -1;
            Color3f swap_color{};
            float reduction = 0.0f;
            if (x_lo < width && x_hi > x_lo) {
                find_best_swap_in_strip(y, x_lo, x_hi,
                                        swap_reg, swap_color, reduction);
            }

            if (swap_reg >= 0 && reduction > 0.0f) {
                auto swap_idx = static_cast<std::size_t>(swap_reg);
                P[swap_idx] = swap_color;
                P_lab[swap_idx] = color_space::linear_to_oklab(swap_color);

                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(kRegBase + swap_reg),
                    palette::linear_to_ocs(swap_color),
                    static_cast<int>(s)));
                ++total_moves;
            } else {
                line_moves[y].push_back(make_move(kFillerReg, kFillerVal,
                                                  static_cast<int>(s)));
            }
            strip_palettes[s + 1] = P;
            strip_pal_lab[s + 1] = P_lab;
        }

        // 4. End-of-line WAIT — release copper to the next line's section.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));

        // ---- Pass 2: render via error diffusion against the per-strip
        // palette, using the user-selected kernel. Residuals propagate
        // freely across strip boundaries (linear-RGB, palette-agnostic),
        // so wide kernels (Stucki, Jarvis) actually spread error across
        // many strips. Each pixel's nearest-colour lookup uses ITS
        // strip's palette so colour rendition tracks the MOVE evolution.
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t s = static_cast<std::size_t>(x_strip[x]);
            auto& pal = strip_palettes[s];
            auto& pl_lab = strip_pal_lab[s];

            Color3f target = src[x, y];
            if (err_diffuse) {
                auto& e = err_rows[0][x + kPad];
                target.r += e.r;
                target.g += e.g;
                target.b += e.b;
            }
            color_space::OKLab tgt_lab = color_space::linear_to_oklab(target);
            // Ordered dithering: apply the per-pixel threshold bias to
            // OKLab. Same scaling as dither.cpp (0.15 for L, 0.03 for a/b).
            // Stage 1 only fed it to the global vs-base pass; stage 2 needs
            // its own threshold so the per-strip nearest-colour lookup
            // actually picks dithered indices.
            if (ordered &&
                dither_settings.method != dither::Method::none &&
                dither_settings.strength > 0.0f) {
                float th = dither::ordered_threshold(
                    dither_settings.method, x, y);
                tgt_lab.L += th * dither_settings.strength * 0.15f;
                tgt_lab.a += th * dither_settings.strength * 0.03f;
                tgt_lab.b += th * dither_settings.strength * 0.03f;
            }

            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = k_min;
            for (std::size_t k = k_min; k < kBaseColors; ++k) {
                float dL = tgt_lab.L - pl_lab[k].L;
                float da = tgt_lab.a - pl_lab[k].a;
                float db = tgt_lab.b - pl_lab[k].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best_k = k; }
            }
            indices[y * width + x] = static_cast<std::uint8_t>(best_k);
            preview[x, y] = pal[best_k];

            if (err_diffuse) {
                Color3f residual = clamp_err(Color3f{
                    target.r - pal[best_k].r,
                    target.g - pal[best_k].g,
                    target.b - pal[best_k].b,
                });
                for (auto& ent : kernel) {
                    auto target_x = static_cast<std::ptrdiff_t>(x)
                                  + ent.dx;
                    if (target_x < 0
                        || target_x >= static_cast<std::ptrdiff_t>(width))
                        continue;
                    auto idx = static_cast<std::size_t>(target_x) + kPad;
                    auto& buf = err_rows[static_cast<std::size_t>(ent.dy)];
                    buf[idx].r += residual.r * ent.weight;
                    buf[idx].g += residual.g * ent.weight;
                    buf[idx].b += residual.b * ent.weight;
                }
            }
        }
        // Rotate row buffers: row[0] becomes row[1] for next y, etc.
        // err_rows[0] (just consumed) becomes err_rows[2] (cleared).
        std::ranges::fill(err_rows[0], Color3f{0.0f, 0.0f, 0.0f});
        std::rotate(err_rows.begin(), err_rows.begin() + 1, err_rows.end());

        for (std::size_t x = 0; x < width; ++x) {
            auto& a = img_lab[y * width + x];
            auto pl = color_space::linear_to_oklab(preview[x, y]);
            float dL = a.L - pl.L, da = a.a - pl.a, db = a.b - pl.b;
            total_error += static_cast<double>(dL * dL + da * da + db * db);
        }
    }

    // ---- 4. 3-plane PF2 encoding, then expand to 6-plane DPF ------------
    auto enc = bitplane::encode(indices, width, height,
                                /*depth=*/3, bitplane::Layout::interleaved);
    if (!enc) return std::unexpected{enc.error()};
    auto expanded = bitplane::expand_to_dpf_pf2(*enc);
    if (!expanded) return std::unexpected{expanded.error()};

    // ---- 5. Output palette: 8 zero PF1 entries + 8 PF2 base entries -----
    std::vector<Color3f> output_palette(16, Color3f{0.0f, 0.0f, 0.0f});
    for (std::size_t k = 0; k < kBaseColors; ++k)
        output_palette[static_cast<std::size_t>(kRegBase) + k] = base_palette[k];

    // ---- 5b. Optional PF1 ruler markers (slot-tuning aid) ---------------
    // Yellow vertical guides at 4 / 8 / 16 px, with hierarchy by height:
    //   * x % 16 == 0           → full height
    //   * x % 8  == 0  (not 16) → top half
    //   * x % 4  == 0  (not 8)  → top quarter
    // Markers paint into PF1 LSB (= dst plane 0 of the 6-plane DPF
    // output) at column x. PF1 in front of PF2, so non-zero PF1 pixels
    // override the image. palette[1] is recoloured to yellow so PF1
    // index 1 displays the ruler colour. Also recolour the same in the
    // per-pixel preview so PNG / stats reflect the markers.
    if (debug_overlay) {
        output_palette[1] = Color3f{1.0f, 1.0f, 0.0f};   // yellow

        auto& dst = *expanded;
        auto bpr = dst.bytes_per_row;
        auto h_full    = height;
        auto h_half    = height / 2;
        auto h_quarter = height / 4;

        auto set_pf1_lsb = [&](std::size_t x, std::size_t y) {
            auto row_off = dst.plane_row_offset(/*plane=*/0, y);
            dst.data[row_off + x / 8] |=
                static_cast<std::uint8_t>(1u << (7 - (x % 8)));
        };
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t marker_h = 0;
            if (x % 16 == 0)      marker_h = h_full;
            else if (x % 8  == 0) marker_h = h_half;
            else if (x % 4  == 0) marker_h = h_quarter;
            else continue;
            for (std::size_t y = 0; y < marker_h; ++y) {
                set_pf1_lsb(x, y);
                preview[x, y] = Color3f{1.0f, 1.0f, 0.0f};
            }
        }
        (void)bpr;
    }

    ScapResult res;
    res.planes = *std::move(expanded);
    res.palette = std::move(output_palette);
    res.line_moves = std::move(line_moves);
    res.slot_table = table;
    res.total_error = static_cast<float>(total_error);
    res.avg_changes_per_line = height > 0
        ? static_cast<float>(total_moves) / static_cast<float>(height)
        : 0.0f;
    res.rendered = std::move(preview);
    return res;
}

// ---------------------------------------------------------------------------
// Probe A — sweep one COLOR09 write across HPOS 0x00..0xE3, one per line,
// on a 6-plane OCS DPF frame.
// ---------------------------------------------------------------------------
Result<ScapResult> make_scap_probe_a_dpf_ocs(int width, int height) {
    if (width <= 0) width = 320;
    if (height <= 0) height = 256;

    auto planes = make_dpf_pf2_index1_planes(static_cast<std::size_t>(width),
                                             static_cast<std::size_t>(height),
                                             /*total_planes=*/6,
                                             /*add_position_grid=*/true);
    if (!planes) return std::unexpected{planes.error()};

    ScapResult res;
    res.planes = *std::move(planes);
    res.slot_table = scap_table_for(6);
    res.probe_label = "probe_a_dpf_ocs";

    // Frame-start palette: 16 entries.
    //   reg 0  = black (bg, also PF1 idx 0 = transparent)
    //   reg 1  = yellow (PF1 minor tick every 16 px,  plane 0 only)
    //   reg 3  = red    (PF1 major tick every 64 px,  plane 0|2 -> idx 3)
    //   reg 9  = SCAP-controlled (PF2 colour, swept by per-line copper)
    //   others = black
    res.palette.assign(16, Color3f{0.0f, 0.0f, 0.0f});
    res.palette[1] = Color3f{1.0f, 1.0f, 0.0f};   // yellow
    res.palette[3] = Color3f{1.0f, 0.0f, 0.0f};   // red

    res.line_moves.resize(static_cast<std::size_t>(height));

    // Image row 0 is rendered at PAL VSTART=44, so absolute VPOS = y + 44.
    constexpr int kVStart = 44;

    for (int y = 0; y < height; ++y) {
        auto& ops = res.line_moves[static_cast<std::size_t>(y)];
        int abs_vpos = y + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // 1. Reset COLOR09 to black at top-of-line.
        ops.push_back(make_wait(0x00, vp));
        ops.push_back(make_move(/*reg=*/9, /*rgb=*/0x0000));

        // 2. Anchor WAIT — deterministic phase reference.
        ops.push_back(make_wait(
            static_cast<std::uint8_t>(res.slot_table.line_gate_hpos), vp));

        // 3. Probe WAIT at swept HPOS, then MOVE white into COLOR09.
        ops.push_back(make_wait(static_cast<std::uint8_t>(hp_for_y(y, height)),
                                vp, /*slot_index=*/0));
        ops.push_back(make_move(/*reg=*/9, /*rgb=*/0x0FFF, /*slot_index=*/0));
    }

    return res;
}

// Probes B/C/D — placeholders until Probe A data is in.
Result<ScapResult> make_scap_probe_b_dpf_ocs(int /*w*/, int /*h*/) {
    return std::unexpected{Error{
        ErrorCode::unsupported_mode,
        "SCAP Probe B not implemented yet (needs Probe A slot data first)",
    }};
}

Result<ScapResult> make_scap_probe_c_dpf_aga(int /*w*/, int /*h*/) {
    return std::unexpected{Error{
        ErrorCode::unsupported_mode,
        "SCAP Probe C not implemented yet (AGA bandwidth, deferred)",
    }};
}

Result<ScapResult> make_scap_probe_d_dpf_ocs(int /*w*/, int /*h*/) {
    return std::unexpected{Error{
        ErrorCode::unsupported_mode,
        "SCAP Probe D not implemented yet (at-x vs after-x pixel mapping)",
    }};
}

} // namespace png2amiga::scap
