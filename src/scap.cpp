#include "scap.hpp"

#include "amiga.hpp"
#include "bitplane.hpp"
#include "color_space.hpp"
#include "copper.hpp"
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
                                       bool debug_overlay,
                                       std::size_t copper_changes_override,
                                       int palette_diversity) {
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

    // ---- 1. CAP first: per-line 8-colour palette evolution.
    // SCAP layers on top of CAP. With only 8 PF2 colours the per-line
    // search space is tiny, but CAP still finds useful palette diffs
    // against neighbour scanlines.
    constexpr int kBaseColors = 8;        // PF2 width = 3 bitplanes
    constexpr int kRegBase = 8;           // PF2 starts at COLOR08 (PF2OF=+8)
    auto copper_result = copper::encode_copper(
        src, /*depth=*/3, dither_settings,
        amiga::Chipset::ocs,
        copper_changes_override,
        /*user_palette=*/nullptr,
        reserve_color0,
        /*locked=*/{},
        palette_diversity,
        /*skip_initial_swap_rows=*/0,
        /*is_lace=*/false);
    if (!copper_result) return std::unexpected{copper_result.error()};
    auto& cap_palettes = copper_result->scanline_palettes;
    auto& base_palette_vec = copper_result->base_palette;
    std::array<Color3f, kBaseColors> base_palette{};
    for (std::size_t k = 0; k < kBaseColors && k < base_palette_vec.size(); ++k)
        base_palette[k] = base_palette_vec[k];

    // ---- 2. Global dither vs FRAME-INIT base palette.
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
    // by pulling the diffusion kernel from dither.hpp. The buffer is
    // a whole-image OKLab error grid (matches the EHB+CAP path in
    // main.cpp): residuals diffuse in the perceptual space, get
    // strength-multiplied at scatter time, and per-channel-clamped on
    // read. Linear-RGB diffusion (the previous approach) blew up across
    // strip palette swaps because the residual magnitude isn't
    // perceptually proportional and DPF's tight 8-colour palette has
    // gaps wider than the residuals could absorb.
    bool ordered = dither::is_ordered(dither_settings.method);
    bool err_diffuse =
        dither_settings.method != dither::Method::none &&
        dither_settings.strength > 0.0f && !ordered;
    auto kernel = err_diffuse
        ? dither::error_diffusion_kernel(dither_settings.method)
        : std::span<const dither::DiffusionEntry>{};
    float clamp_v = dither_settings.error_clamp;

    std::vector<color_space::OKLab> err_buf;
    if (err_diffuse) err_buf.assign(width * height,
                                    color_space::OKLab{0.0f, 0.0f, 0.0f});

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
            std::vector<Color3f> cands;
            cands.reserve(static_cast<std::size_t>(x_hi - x_lo) + kBaseColors);
            std::array<bool, 4096> seen{};
            auto ocs_key = [](const Color3f& c) {
                int r = static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 15.0f));
                int g = static_cast<int>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 15.0f));
                int b = static_cast<int>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 15.0f));
                return static_cast<std::size_t>((r << 8) | (g << 4) | b);
            };
            auto add_cand = [&](Color3f c) {
                auto cs = palette::quantize_to_ocs(c);
                auto key = ocs_key(cs);
                if (!seen[key]) { seen[key] = true; cands.push_back(cs); }
            };
            for (std::size_t x = x_lo; x < x_hi; ++x) add_cand(src[x, y]);
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (count[k] > 0.0) {
                    color_space::OKLab cd{
                        static_cast<float>(sumL[k] / count[k]),
                        static_cast<float>(suma[k] / count[k]),
                        static_cast<float>(sumb[k] / count[k])};
                    add_cand(color_space::oklab_to_linear(cd).clamped());
                }
            }

            out_reg = -1;
            out_reduction = 0.0f;
            for (std::size_t k = k_min; k < kBaseColors; ++k) {
                if (count[k] < 1.0) continue;
                for (auto& cand_lin : cands) {
                    auto cand_lab = color_space::linear_to_oklab(cand_lin);
                    double new_err = 0.0;
                    for (std::size_t x = x_lo; x < x_hi; ++x) {
                        if (base_index[y * width + x] != k) continue;
                        auto& lab = img_lab[y * width + x];
                        float dL = lab.L - cand_lab.L;
                        float da = lab.a - cand_lab.a;
                        float db = lab.b - cand_lab.b;
                        new_err += static_cast<double>(dL * dL + da * da + db * db);
                    }
                    float red = static_cast<float>(cur_err[k] - new_err);
                    if (red > out_reduction) {
                        out_reg = static_cast<int>(k);
                        out_color = cand_lin;
                        out_reduction = red;
                    }
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

    constexpr int kPasses = 6;
    for (int pass = 0; pass < kPasses; ++pass) {
        if (pass > 0) {
            for (std::size_t i = 0; i < indices.size(); ++i)
                base_index[i] = indices[i];
            for (auto& v : line_moves) v.clear();
            if (!err_buf.empty())
                std::fill(err_buf.begin(), err_buf.end(),
                          color_space::OKLab{0.0f, 0.0f, 0.0f});
            total_moves = 0;
            total_error = 0.0;
        }
    for (std::size_t y = 0; y < height; ++y) {
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // Line entry palette = CAP plan for this line. cap_palettes[y]
        // already reflects all per-line palette evolution decisions
        // from CAP — we layer 20 mid-line SCAP swaps on top.
        // In debug_overlay mode, hardware actually enters every line
        // with all PF2 registers forced to 0x0000 (zero frame-init +
        // forced-zero per-line MOVEs), so the planner must too —
        // otherwise the preview leaks CAP colours into strip 0 even
        // though those swaps are never written to hardware.
        for (std::size_t k = 0; k < kBaseColors; ++k)
            P[k] = debug_overlay ? Color3f{0.0f, 0.0f, 0.0f}
                                 : cap_palettes[y][k];
        recompute_lab();
        strip_palettes[0] = P;
        strip_pal_lab[0] = P_lab;

        // 1. Per-line CAP MOVEs (diff vs previous line's palette).
        //    For y=0 the frame-init palette is already loaded by the
        //    viewer before bitplane DMA. In debug_overlay mode every
        //    register is forced to 0x0000 so SCAP MOVEs are the only
        //    visible colour changes.
        if (debug_overlay) {
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(kRegBase + k),
                    std::uint16_t{0x0000}, -1));
            }
        } else {
            // Per-line CAP MOVEs: diff vs the running register state.
            // For y=0 the running state is base_palette (loaded by the
            // viewer's frame-init); CAP can decide to swap on row 0,
            // making cap_palettes[0] differ from base_palette — emit
            // that diff so hardware matches the planner. For y>0 diff
            // is against the previous line's cap palette.
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                Color3f prev = (y == 0) ? base_palette[k]
                                        : cap_palettes[y - 1][k];
                if (prev.r != P[k].r || prev.g != P[k].g ||
                    prev.b != P[k].b) {
                    line_moves[y].push_back(make_move(
                        static_cast<std::uint8_t>(kRegBase + k),
                        palette::linear_to_ocs(P[k]), -1));
                }
            }
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

        // ---- Pass 2: render via OKLab error diffusion against the
        // per-strip palette. Each pixel's nearest-colour lookup uses
        // ITS strip's palette so colour rendition tracks MOVE
        // evolution; the residual is computed in OKLab and scattered
        // by the user's kernel into a whole-image error buffer.
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t s = static_cast<std::size_t>(x_strip[x]);
            auto& pal = strip_palettes[s];
            auto& pl_lab = strip_pal_lab[s];

            auto pixel_lab = color_space::linear_to_oklab(src[x, y]);
            if (err_diffuse) {
                auto& e = err_buf[y * width + x];
                pixel_lab.L += std::clamp(e.L, -clamp_v, clamp_v);
                pixel_lab.a += std::clamp(e.a, -clamp_v, clamp_v);
                pixel_lab.b += std::clamp(e.b, -clamp_v, clamp_v);
            }
            if (ordered &&
                dither_settings.method != dither::Method::none &&
                dither_settings.strength > 0.0f) {
                float th = dither::ordered_threshold(
                    dither_settings.method, x, y);
                pixel_lab.L += th * dither_settings.strength * 0.15f;
                pixel_lab.a += th * dither_settings.strength * 0.03f;
                pixel_lab.b += th * dither_settings.strength * 0.03f;
            }

            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = k_min;
            for (std::size_t k = k_min; k < kBaseColors; ++k) {
                float dL = pixel_lab.L - pl_lab[k].L;
                float da = pixel_lab.a - pl_lab[k].a;
                float db = pixel_lab.b - pl_lab[k].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best_k = k; }
            }
            indices[y * width + x] = static_cast<std::uint8_t>(best_k);
            preview[x, y] = pal[best_k];

            if (err_diffuse) {
                auto& cl = pl_lab[best_k];
                color_space::OKLab qe{
                    (pixel_lab.L - cl.L) * dither_settings.strength,
                    (pixel_lab.a - cl.a) * dither_settings.strength,
                    (pixel_lab.b - cl.b) * dither_settings.strength,
                };
                for (auto& [kdx, kdy, kw] : kernel) {
                    auto nx = static_cast<std::ptrdiff_t>(x) + kdx;
                    auto ny = static_cast<std::ptrdiff_t>(y) + kdy;
                    if (nx >= 0 && static_cast<std::size_t>(nx) < width &&
                        ny >= 0 && static_cast<std::size_t>(ny) < height) {
                        auto& e = err_buf[
                            static_cast<std::size_t>(ny) * width +
                            static_cast<std::size_t>(nx)];
                        e.L += qe.L * kw;
                        e.a += qe.a * kw;
                        e.b += qe.b * kw;
                    }
                }
            }
        }

        for (std::size_t x = 0; x < width; ++x) {
            auto& a = img_lab[y * width + x];
            auto pl = color_space::linear_to_oklab(preview[x, y]);
            float dL = a.L - pl.L, da = a.a - pl.a, db = a.b - pl.b;
            total_error += static_cast<double>(dL * dL + da * da + db * db);
        }
    }
    }  // kPasses

    // ---- 4. 3-plane PF2 encoding, then expand to 6-plane DPF ------------
    auto enc = bitplane::encode(indices, width, height,
                                /*depth=*/3, bitplane::Layout::interleaved);
    if (!enc) return std::unexpected{enc.error()};
    auto expanded = bitplane::expand_to_dpf_pf2(*enc);
    if (!expanded) return std::unexpected{expanded.error()};

    // ---- 5. Output palette: 8 zero PF1 entries + 8 PF2 base entries -----
    // In debug_overlay mode the PF2 entries are also zeroed — together
    // with the forced-zero per-line MOVEs above this means the viewer's
    // frame-init writes black to COLOR08..15 and they STAY black until
    // a SCAP MOVE swaps a register mid-line. Without this the row-0
    // display would briefly flash the auto-quantised base colours
    // before the line-0 zero MOVEs land in hblank.
    std::vector<Color3f> output_palette(16, Color3f{0.0f, 0.0f, 0.0f});
    if (!debug_overlay) {
        for (std::size_t k = 0; k < kBaseColors; ++k)
            output_palette[static_cast<std::size_t>(kRegBase) + k] = base_palette[k];
    }

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
namespace {

// Half-brite a base linear-RGB colour: sRGB-halve then back to linear,
// matching make_ehb_palette() and the Amiga DAC.
Color3f half_brite(const Color3f& c) {
    auto srgb = color_space::linear_to_srgb(c).clamped();
    Color3f half_srgb{srgb.r * 0.5f, srgb.g * 0.5f, srgb.b * 0.5f};
    return color_space::srgb_to_linear(half_srgb);
}

} // namespace

Result<ScapResult> encode_scap_ehb_ocs(const Image& image,
                                       int width_arg,
                                       int height_arg,
                                       bool reserve_color0,
                                       const dither::Settings& dither_settings,
                                       std::size_t copper_changes_override,
                                       int palette_diversity) {
    auto& table = scap_table_for(6);
    if (table.slots.empty()) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "SCAP EHB planner: kScap6bplOcs slot table is empty",
        }};
    }

    auto width = (width_arg > 0) ? static_cast<std::size_t>(width_arg)
                                  : image.width();
    auto height = (height_arg > 0) ? static_cast<std::size_t>(height_arg)
                                    : image.height();
    if (image.width() != width || image.height() != height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("SCAP EHB planner: image is {}x{} but caller asked "
                        "for {}x{} — resize before calling",
                        image.width(), image.height(), width, height),
        }};
    }

    auto& src = image;
    constexpr std::size_t kBaseColors = 32;
    constexpr std::size_t kEffective  = 64;
    constexpr int kRegBase = 0;

    // ---- 1. CAP first: per-line palette evolution.
    // SCAP is a layer ON TOP OF CAP, not a replacement. The CAP encoder
    // picks a per-line set of register diffs that evolves the 32-base
    // palette across scanlines; SCAP then adds 20 mid-line MOVEs per
    // line on top of that evolving state. Without this layering the
    // image looks single-palette per frame, which is very visible on
    // photographic content.
    auto copper_result = copper::encode_copper(
        src, /*depth=*/5, dither_settings,
        amiga::Chipset::ocs,
        copper_changes_override,
        /*user_palette=*/nullptr,
        reserve_color0,
        /*locked=*/{},
        palette_diversity,
        /*skip_initial_swap_rows=*/0,
        /*is_lace=*/false);
    if (!copper_result) return std::unexpected{copper_result.error()};
    // Copies (not refs) so the joint-refinement pass below can reassign
    // them when it re-runs CAP with a refined base palette.
    auto cap_palettes = copper_result->scanline_palettes;
    auto base_palette = copper_result->base_palette;

    // Build per-line 64-effective palette (32 base + 32 half-brite).
    auto build_effective_64 = [](const std::vector<Color3f>& base) {
        std::vector<Color3f> eff(kEffective);
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            eff[k]             = base[k];
            eff[kBaseColors+k] = half_brite(base[k]);
        }
        return eff;
    };

    // ---- 2. Global dither vs the FRAME-INIT 64 palette.
    // base_index drives the strip-swap planner's centroid math.
    // (Owned by value so the joint-refinement pass can rebuild it.)
    auto effective_init = build_effective_64(base_palette);
    std::span<const Color3f> eff_init_span(effective_init.data(), kEffective);
    auto dith_result = dither::apply(src, eff_init_span, dither_settings);
    auto base_index = std::move(dith_result.indices);

    std::vector<color_space::OKLab> img_lab(width * height);
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            img_lab[y * width + x] = color_space::linear_to_oklab(src[x, y]);

    // ---- 3. Per-line greedy planner -----------------------------------
    std::vector<std::uint8_t> indices(width * height, 0);
    std::vector<std::vector<ScapMove>> line_moves(height);
    Image preview(width, height);

    std::vector<Color3f> P;
    std::vector<Color3f> P_eff;
    std::vector<color_space::OKLab> P_eff_lab(kEffective);
    auto recompute_lab = [&]() {
        for (std::size_t k = 0; k < kEffective; ++k)
            P_eff_lab[k] = color_space::linear_to_oklab(P_eff[k]);
    };

    std::size_t k_min = reserve_color0 ? 1u : 0u;

    bool ordered = dither::is_ordered(dither_settings.method);
    bool err_diffuse =
        dither_settings.method != dither::Method::none &&
        dither_settings.strength > 0.0f && !ordered;
    auto kernel = err_diffuse
        ? dither::error_diffusion_kernel(dither_settings.method)
        : std::span<const dither::DiffusionEntry>{};
    float clamp_v = dither_settings.error_clamp;

    // Whole-image OKLab error buffer, matching the EHB+CAP path in
    // main.cpp. Diffusing the residual in the perceptual space (vs
    // linear RGB) keeps error magnitudes comparable across strip
    // palette swaps and avoids the runaway-mush we saw in the DPF
    // F-S/Stucki/Jarvis runs. Strength is applied at scatter time so
    // --dither-strength actually attenuates the residual.
    std::vector<color_space::OKLab> err_buf;
    if (err_diffuse) err_buf.assign(width * height,
                                    color_space::OKLab{0.0f, 0.0f, 0.0f});

    // Greedy strip swap. We can only MOVE the 32 BASE registers; each
    // base[k] swap also redefines half-brite[k] = halve(base[k]). For
    // a candidate swap on register k, the affected pixels are those
    // currently bound to either index k (base) or index kBaseColors+k
    // (half-brite). New error: nearest-of-(centroid, half-brite-of-
    // centroid) per pixel.
    auto find_best_swap_in_strip =
        [&](std::size_t y, std::size_t x_lo, std::size_t x_hi,
            int& out_reg, Color3f& out_color, float& out_reduction) {
            std::array<double, kBaseColors> cur_err{};
            std::array<bool, kBaseColors> has_pixels_b{}, has_pixels_h{};
            std::array<color_space::OKLab, kBaseColors> centroid_b{}, centroid_h{};
            std::array<int, kBaseColors> count_b{}, count_h{};
            std::array<color_space::OKLab, kBaseColors> sum_b{}, sum_h{};
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto idx = static_cast<std::size_t>(base_index[y * width + x]);
                std::size_t k = idx & (kBaseColors - 1);
                bool is_half = idx >= kBaseColors;
                auto& lab = img_lab[y * width + x];
                auto& sum = is_half ? sum_h[k] : sum_b[k];
                auto& cnt = is_half ? count_h[k] : count_b[k];
                sum.L += lab.L; sum.a += lab.a; sum.b += lab.b; cnt++;
                (is_half ? has_pixels_h[k] : has_pixels_b[k]) = true;
                auto& pl = P_eff_lab[idx];
                float dL = lab.L - pl.L;
                float da = lab.a - pl.a;
                float db = lab.b - pl.b;
                cur_err[k] += static_cast<double>(dL * dL + da * da + db * db);
            }
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (count_b[k] > 0) {
                    float n = static_cast<float>(count_b[k]);
                    centroid_b[k] = {sum_b[k].L / n, sum_b[k].a / n, sum_b[k].b / n};
                }
                if (count_h[k] > 0) {
                    float n = static_cast<float>(count_h[k]);
                    centroid_h[k] = {sum_h[k].L / n, sum_h[k].a / n, sum_h[k].b / n};
                }
            }

            // ---- Build candidate-color pool for the strip.
            // Unique OCS-quantised pixel colours in the strip (~16 max
            // for a 16-px strip), plus the per-register centroids. The
            // candidate pool is shared across registers — for each
            // (k, candidate) pair we compute the swap reduction. This
            // costs ~16-20× the centroid-only approach but lets the
            // planner pick a colour that better serves k's bound pixels
            // even when their centroid lands in a hard-to-quantise spot.
            std::vector<Color3f> cands;
            cands.reserve(static_cast<std::size_t>(x_hi - x_lo) + 16);
            std::array<bool, 4096> seen{};
            auto ocs_key = [](const Color3f& c) -> std::size_t {
                int r = static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 15.0f));
                int g = static_cast<int>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 15.0f));
                int b = static_cast<int>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 15.0f));
                return static_cast<std::size_t>((r << 8) | (g << 4) | b);
            };
            auto add_cand = [&](Color3f c) {
                auto cs = palette::quantize_to_ocs(c);
                auto key = ocs_key(cs);
                if (!seen[key]) { seen[key] = true; cands.push_back(cs); }
            };
            for (std::size_t x = x_lo; x < x_hi; ++x) add_cand(src[x, y]);
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (count_b[k] > 0)
                    add_cand(color_space::oklab_to_linear(centroid_b[k]).clamped());
                if (count_h[k] > 0) {
                    // half-brite-bound pixels want base ≈ 2*pixel in sRGB
                    color_space::OKLab dbl{
                        std::min(2.0f * centroid_h[k].L, 1.0f),
                        2.0f * centroid_h[k].a, 2.0f * centroid_h[k].b};
                    add_cand(color_space::oklab_to_linear(dbl).clamped());
                }
            }

            out_reg = -1;
            out_reduction = 0.0f;
            for (std::size_t k = k_min; k < kBaseColors; ++k) {
                if (!has_pixels_b[k] && !has_pixels_h[k]) continue;
                for (auto& cand_lin : cands) {
                    auto cand_half = half_brite(cand_lin);
                    auto cand_lab  = color_space::linear_to_oklab(cand_lin);
                    auto cand_h_lab = color_space::linear_to_oklab(cand_half);
                    double new_err = 0.0;
                    for (std::size_t x = x_lo; x < x_hi; ++x) {
                        auto idx = static_cast<std::size_t>(base_index[y*width + x]);
                        if ((idx & (kBaseColors - 1)) != k) continue;
                        auto& lab = img_lab[y * width + x];
                        bool is_half = idx >= kBaseColors;
                        auto& cl = is_half ? cand_h_lab : cand_lab;
                        float dL = lab.L - cl.L;
                        float da = lab.a - cl.a;
                        float db = lab.b - cl.b;
                        new_err += static_cast<double>(dL * dL + da * da + db * db);
                    }
                    float red = static_cast<float>(cur_err[k] - new_err);
                    if (red > out_reduction) {
                        out_reg = static_cast<int>(k);
                        out_color = cand_lin;
                        out_reduction = red;
                    }
                }
            }
        };

    constexpr int kVStart = 44;
    double total_error = 0.0;
    std::size_t total_moves = 0;

    std::size_t num_strips = table.slots.size() + 1;
    std::vector<std::vector<Color3f>> strip_eff(num_strips,
        std::vector<Color3f>(kEffective));
    std::vector<std::vector<color_space::OKLab>> strip_eff_lab(num_strips,
        std::vector<color_space::OKLab>(kEffective));

    auto strip_for_x = [&](std::size_t x) -> std::size_t {
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            if (x < static_cast<std::size_t>(table.slots[s].pixel_x))
                return s;
        }
        return table.slots.size();
    };
    std::vector<std::uint16_t> x_strip(width);
    for (std::size_t x = 0; x < width; ++x)
        x_strip[x] = static_cast<std::uint16_t>(strip_for_x(x));

    // Iterative index refinement (#2). Each pass after the first feeds
    // the previous pass's stage-2 indices back as base_index for the
    // swap planner, so the planner optimises against the binding the
    // encoder will actually produce. Empirically gains ~0.05 dB per
    // additional pass through pass 6, then plateaus.
    //
    // We tried full joint base-palette + CAP refinement (#3) — recompute
    // base from final indices, re-run CAP, re-dither stage 1 — but that
    // REGRESSED PSNR by ~0.9 dB on the 10-image sweep. Re-running CAP
    // from a different starting palette breaks the convergence the
    // index iteration was building toward. Pure index refinement wins.
    constexpr int kPasses = 6;
    for (int pass = 0; pass < kPasses; ++pass) {
        if (pass > 0) {
            for (std::size_t i = 0; i < indices.size(); ++i)
                base_index[i] = indices[i];
            for (auto& v : line_moves) v.clear();
            if (!err_buf.empty())
                std::fill(err_buf.begin(), err_buf.end(),
                          color_space::OKLab{0.0f, 0.0f, 0.0f});
            total_moves = 0;
            total_error = 0.0;
        }
    for (std::size_t y = 0; y < height; ++y) {
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // Line entry palette = the CAP plan for this line, not a static
        // base. cap_palettes[y] carries the evolved 32-base state from
        // previous lines (CAP's per-scanline diffs already applied).
        P = cap_palettes[y];
        P_eff = build_effective_64(P);
        recompute_lab();
        strip_eff[0] = P_eff;
        strip_eff_lab[0] = P_eff_lab;

        // 1. Per-line CAP MOVEs: diff vs the running register state.
        // For y=0 the running state is base_palette (loaded by the
        // viewer's frame-init); CAP may decide to swap on row 0,
        // making cap_palettes[0] differ from base_palette — emit that
        // diff so hardware matches the planner. For y>0 diff against
        // the previous line's CAP palette.
        {
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                Color3f prev = (y == 0) ? base_palette[k]
                                        : cap_palettes[y - 1][k];
                if (prev.r != P[k].r || prev.g != P[k].g ||
                    prev.b != P[k].b) {
                    line_moves[y].push_back(make_move(
                        static_cast<std::uint8_t>(kRegBase + k),
                        palette::linear_to_ocs(P[k]),
                        -1));
                }
            }
        }
        // 2. Line-gate WAIT.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));

        // 3. SCAP MOVEs.
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
                auto k = static_cast<std::size_t>(swap_reg);
                P[k] = swap_color;
                P_eff[k] = swap_color;
                P_eff[kBaseColors + k] = half_brite(swap_color);
                P_eff_lab[k] = color_space::linear_to_oklab(P_eff[k]);
                P_eff_lab[kBaseColors + k] =
                    color_space::linear_to_oklab(P_eff[kBaseColors + k]);
                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(kRegBase + swap_reg),
                    palette::linear_to_ocs(swap_color),
                    static_cast<int>(s)));
                ++total_moves;
            }
            strip_eff[s + 1] = P_eff;
            strip_eff_lab[s + 1] = P_eff_lab;
        }

        // 4. End-of-line WAIT.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));

        // Stage-2 render across all 64 effective entries per pixel,
        // with the EHB+CAP-style OKLab error-diffusion / ordered-bias
        // path (see main.cpp's `if (config->mode == ehb && copper)`).
        // Error buffer is whole-image OKLab; clamp on read; residual
        // gets the strength multiplier at scatter.
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t s = static_cast<std::size_t>(x_strip[x]);
            auto& eff_pal = strip_eff[s];
            auto& eff_lab = strip_eff_lab[s];

            auto pixel_lab = color_space::linear_to_oklab(src[x, y]);
            if (err_diffuse) {
                auto& e = err_buf[y * width + x];
                pixel_lab.L += std::clamp(e.L, -clamp_v, clamp_v);
                pixel_lab.a += std::clamp(e.a, -clamp_v, clamp_v);
                pixel_lab.b += std::clamp(e.b, -clamp_v, clamp_v);
            }
            if (ordered &&
                dither_settings.method != dither::Method::none &&
                dither_settings.strength > 0.0f) {
                float th = dither::ordered_threshold(
                    dither_settings.method, x, y);
                pixel_lab.L += th * dither_settings.strength * 0.15f;
                pixel_lab.a += th * dither_settings.strength * 0.03f;
                pixel_lab.b += th * dither_settings.strength * 0.03f;
            }

            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = k_min;
            for (std::size_t k = k_min; k < kEffective; ++k) {
                float dL = pixel_lab.L - eff_lab[k].L;
                float da = pixel_lab.a - eff_lab[k].a;
                float db = pixel_lab.b - eff_lab[k].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best_k = k; }
            }
            indices[y * width + x] = static_cast<std::uint8_t>(best_k);
            preview[x, y] = eff_pal[best_k];

            if (err_diffuse) {
                auto& cl = eff_lab[best_k];
                color_space::OKLab qe{
                    (pixel_lab.L - cl.L) * dither_settings.strength,
                    (pixel_lab.a - cl.a) * dither_settings.strength,
                    (pixel_lab.b - cl.b) * dither_settings.strength,
                };
                for (auto& [kdx, kdy, kw] : kernel) {
                    auto nx = static_cast<std::ptrdiff_t>(x) + kdx;
                    auto ny = static_cast<std::ptrdiff_t>(y) + kdy;
                    if (nx >= 0 && static_cast<std::size_t>(nx) < width &&
                        ny >= 0 && static_cast<std::size_t>(ny) < height) {
                        auto& e = err_buf[
                            static_cast<std::size_t>(ny) * width +
                            static_cast<std::size_t>(nx)];
                        e.L += qe.L * kw;
                        e.a += qe.a * kw;
                        e.b += qe.b * kw;
                    }
                }
            }
        }

        for (std::size_t x = 0; x < width; ++x) {
            auto& a = img_lab[y * width + x];
            auto pl = color_space::linear_to_oklab(preview[x, y]);
            float dL = a.L - pl.L, da = a.a - pl.a, db = a.b - pl.b;
            total_error += static_cast<double>(dL * dL + da * da + db * db);
        }
    }
    }  // kPasses

    // ---- 4. 6-plane bitplane encoding. The 6-bit index already encodes
    // half-brite as bit 5, which is exactly what the EHB hardware reads.
    auto enc = bitplane::encode(indices, width, height,
                                /*depth=*/6, bitplane::Layout::interleaved);
    if (!enc) return std::unexpected{enc.error()};

    ScapResult res;
    res.planes = *std::move(enc);
    res.palette = std::move(base_palette);  // 32 base entries; HW derives 32 half-brites
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
