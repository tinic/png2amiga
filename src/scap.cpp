#include "scap.hpp"

#include "amiga.hpp"
#include "bitplane.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "ham.hpp"
#include "palette.hpp"
#include "pipeline.hpp"
#include "types.hpp"

// scap.cpp's parallel sweep machinery is delegated to
// pipeline::cap_best_sweep / pipeline::parallel_for, so this TU itself
// doesn't need <thread> / <atomic> / <mutex> any more.
#include <algorithm>
#include <array>
#include <optional>
#include <cstdint>
#include <limits>
#include <format>
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
                                       int palette_diversity,
                                       std::function<void(float, std::string_view)>
                                           on_progress,
                                       bool cap_best,
                                       std::string_view cap_best_metric,
                                       int cap_spread_radius,
                                       float cap_spread_decay) {
    // --cap-best: multi-restart with varied palette_diversity + dither
    // strength. The SCAP planner is deterministic for a given input, so
    // varying these knobs is the only way to sample different
    // optimisation landscapes. Each restart is a full encode (~100 ms);
    // user OK'd unbounded compute. Keep the lowest-error result.
    if (cap_best) {
        // DPF: 24 jitter seeds — the 8-colour PF2 palette is highly
        // sensitive to which colours win the median-cut, so heavy jitter
        // sampling buys more here than for wider palettes (EHB stays at
        // 8). Total 5×4×24 + 1 = 481 trials, ~2–3 min on 8 cores.
        auto metric = (cap_best_metric == "msssim")
            ? pipeline::CapBestMetric::msssim
            : pipeline::CapBestMetric::psnr;
        auto best = pipeline::cap_best_sweep<ScapResult>(
            image, dither_settings, palette_diversity, /*jitter_count=*/24,
            [&](const Image& jittered_in,
                const dither::Settings& d, int div) {
                return encode_scap_dpf_ocs(
                    jittered_in, width_arg, height_arg, reserve_color0,
                    d, debug_overlay, copper_changes_override, div,
                    /*on_progress=*/{}, /*cap_best=*/false, "psnr",
                    cap_spread_radius, cap_spread_decay);
            },
            [](const ScapResult& r) -> const Image& { return r.rendered; },
            on_progress,
            /*jitter_amplitude=*/1.0f,
            metric);
        if (best.has_value()) return std::move(*best);
        // Fall through to the single-pass path if every restart failed
        // (shouldn't happen with valid input, but degrade gracefully).
    }

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
    // PF2 index → COLOR-register mapping:
    //   * OCS DPF combiner rule: PF2 index 0 falls through to COLOR00
    //     (the "both PFs zero → background" case). Indices 1..7 use the
    //     implicit +8 offset → COLOR09..15. COLOR08 is unused on OCS.
    //   * AGA DPF with BPLCON3 PF2OF=011: index 0 → COLOR08, indices
    //     1..7 → COLOR09..15.
    // The cpp viewer needs to work on both chipsets, so we write index
    // 0 to BOTH COLOR00 and COLOR08 (one of the two is always the live
    // register depending on chipset). pf2_writes(k) returns the list
    // of registers that must be written for PF2 index k.
    auto pf2_writes = [](std::size_t k) -> std::array<int, 2> {
        return (k == 0) ? std::array<int, 2>{0, 8}
                        : std::array<int, 2>{static_cast<int>(8 + k), -1};
    };
    // Hblank load is fixed at ~9 MOVEs (8 PF2 indices, k=0 dual-writes
    // COLOR00+COLOR08) re-emitted unconditionally every line, so the CAP
    // share is whatever the user asked for — bounded by the 14-MOVE OCS
    // hblank budget. SCAP swaps live in the visible region and don't
    // contend for hblank, so no SCAP share split is needed.
    constexpr std::size_t kMaxCombined = copper::max_changes_per_line(
        /*depth=*/3, false, false, amiga::Chipset::ocs, false);
    std::size_t total_budget = (copper_changes_override > 0)
        ? std::min<std::size_t>(copper_changes_override, kMaxCombined)
        : kMaxCombined;
    std::size_t cap_share = std::min<std::size_t>(total_budget, 2u);
    auto copper_result = copper::encode_copper(
        src, /*depth=*/3, dither_settings,
        amiga::Chipset::ocs,
        cap_share,
        /*user_palette=*/nullptr,
        reserve_color0,
        /*locked=*/{},
        palette_diversity,
        /*skip_initial_swap_rows=*/0,
        /*is_lace=*/false,
        /*is_ehb=*/false,
        /*on_progress=*/{},
        cap_spread_radius >= 0
            ? static_cast<std::size_t>(cap_spread_radius)
            : std::numeric_limits<std::size_t>::max(),
        cap_spread_decay >= 0.0f ? cap_spread_decay : -1.0f);
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

    // Force k_min=1 for DPF SCAP regardless of --reserve-color0. PF2
    // index 0 maps to COLOR00 on OCS and COLOR08 on AGA (per BPLCON3
    // PF2OF=011) — keeping the two in sync mid-line would require
    // emitting two MOVEs per swap, which would shift subsequent slot
    // positions on the bus. Frame-init + per-line CAP MOVEs (both in
    // hblank) handle the dual-write without timing impact, so SCAP
    // simply never picks k=0 and the planner targets k=1..7.
    std::size_t k_min = 1u;

    // Stage-2 error diffusion setup. Honours the user's --dither choice
    // by pulling the diffusion kernel from dither.hpp. The buffer is
    // a whole-image OKLab error grid (matches the EHB+CAP path in
    // main.cpp): residuals diffuse in the perceptual space, get
    // strength-multiplied at scatter time, and per-channel-clamped on
    // read. Linear-RGB diffusion (the previous approach) blew up across
    // strip palette swaps because the residual magnitude isn't
    // perceptually proportional and DPF's tight 8-colour palette has
    // gaps wider than the residuals could absorb.
    // ED scaffolding (kernel, error buf, structure bias, Riemersma) all
    // live inside dither::diffuse_raw_buffer (the post-pass-1 driver
    // call below). The CAP planner only needs to know whether dithering
    // is enabled at all (yliluoma family + ordered + ED kernel) so we
    // keep the policy flags here.

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

    // Captured per-row strip palettes for the post-loop driver call
    // (pass 2). Pass 1 fills strip_palettes[s] for the current row and
    // we snapshot into strip_palettes_per_row[y] before moving on.
    std::vector<std::vector<std::array<Color3f, kBaseColors>>>
        strip_palettes_per_row(height,
            std::vector<std::array<Color3f, kBaseColors>>(num_strips));
    std::vector<std::vector<std::array<color_space::OKLab, kBaseColors>>>
        strip_pal_lab_per_row(height,
            std::vector<std::array<color_space::OKLab, kBaseColors>>(num_strips));

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
    auto report_pass = [&](int pass_idx, float local) {
        if (on_progress) {
            float p = (static_cast<float>(pass_idx) +
                       std::clamp(local, 0.0f, 1.0f)) /
                      static_cast<float>(kPasses);
            on_progress(p, "encoding");
        }
    };
    if (on_progress) on_progress(0.0f, "encoding");
    for (int pass = 0; pass < kPasses; ++pass) {
        if (pass > 0) {
            for (std::size_t i = 0; i < indices.size(); ++i)
                base_index[i] = indices[i];
            for (auto& v : line_moves) v.clear();
            // err_buf is owned by dither::diffuse_raw_buffer (allocated
            // fresh each pass-2 call), so no manual reset is needed.
            total_moves = 0;
            total_error = 0.0;
        }
    for (std::size_t y = 0; y < height; ++y) {
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // Per-line target = CAP plan for this line, OR all-zero in
        // debug mode (hardware enters every line with 0x0000 there).
        std::array<Color3f, kBaseColors> target{};
        for (std::size_t k = 0; k < kBaseColors; ++k)
            target[k] = debug_overlay ? Color3f{0.0f, 0.0f, 0.0f}
                                      : cap_palettes[y][k];

        // 1. Per-line CAP MOVEs: unconditionally re-emit all 8 PF2
        //    base colours every line. DPF only has 8 PF2 indices so a
        //    full reset costs ≤9 hblank MOVEs (k=0 dual-writes
        //    COLOR00+COLOR08, k=1..7 single MOVE each), well below the
        //    14-MOVE OCS hblank capacity. This makes SCAP's mid-line
        //    swaps a non-issue across lines: whatever registers SCAP
        //    polluted on line y-1 get fully overwritten before line y's
        //    visible region starts. No hw_state tracking needed.
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            auto regs = pf2_writes(k);
            for (int reg : regs) {
                if (reg < 0) continue;
                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(reg),
                    palette::linear_to_ocs(target[k]), -1));
            }
        }

        // After per-line CAP MOVEs, hardware state == target. Plug it
        // into the strip-0 palette for the SCAP swap planner.
        for (std::size_t k = 0; k < kBaseColors; ++k) P[k] = target[k];
        recompute_lab();
        strip_palettes[0] = P;
        strip_pal_lab[0] = P_lab;

        // 2. Line-gate WAIT — opens the SCAP chain at HPOS=line_gate_hpos.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));

        // 3. 20 SCAP MOVEs back-to-back. Joint beam-search planner:
        //    explores B parallel sequences of (slot → register, color)
        //    decisions instead of greedy max-reduction per slot. Greedy
        //    locked onto the most-populated register slot-after-slot
        //    because total summed error scales with cluster size — the
        //    planner has no incentive to pick under-utilised registers
        //    even when doing so would unlock far better total-line
        //    coverage. Beam search picks the chain with min total strip
        //    error across all slots, naturally favouring decisions that
        //    don't waste the line on micro-tweaking one register.
        //
        //    Score: per-line per-strip per-register cluster stats are
        //    pre-computed (count, OKLab centroid, spread). Strip error
        //    given palette P = Σ_k count[k]·||centroid[k]-P[k]||² +
        //    spread[k]. O(8) per state-evaluation.

        struct ClusterStat {
            float L = 0, a = 0, b = 0;
            double spread = 0;
            std::uint16_t count = 0;
        };
        struct StripStats {
            std::array<ClusterStat, kBaseColors> clusters{};
            std::vector<Color3f> cands;          // OCS-quantized
            std::vector<color_space::OKLab> cands_lab;
        };

        // Pre-compute per-strip stats. strips[0] = pixels [0..slot0).
        // strips[s+1] = pixels [slot[s] .. slot[s+1]) — the strip slot s
        // controls. Slot s's MOVE affects strips[s+1] (and beyond if no
        // later slot overrides P[k]).
        std::vector<StripStats> strips(num_strips);
        auto strip_x_range = [&](std::size_t s) {
            std::size_t lo = (s == 0) ? std::size_t{0}
                : std::min(width,
                    static_cast<std::size_t>(table.slots[s - 1].pixel_x));
            std::size_t hi = (s < table.slots.size())
                ? std::min(width,
                    static_cast<std::size_t>(table.slots[s].pixel_x))
                : width;
            return std::pair<std::size_t, std::size_t>{lo, hi};
        };
        for (std::size_t s = 0; s < num_strips; ++s) {
            auto [x_lo, x_hi] = strip_x_range(s);
            if (x_lo >= x_hi) continue;
            std::array<double, kBaseColors> sumL{}, suma{}, sumb{};
            std::array<std::uint32_t, kBaseColors> cnt{};
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto k = static_cast<std::size_t>(base_index[y * width + x]);
                auto& lab = img_lab[y * width + x];
                sumL[k] += static_cast<double>(lab.L);
                suma[k] += static_cast<double>(lab.a);
                sumb[k] += static_cast<double>(lab.b);
                ++cnt[k];
            }
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (cnt[k] == 0) continue;
                strips[s].clusters[k].count =
                    static_cast<std::uint16_t>(cnt[k]);
                strips[s].clusters[k].L =
                    static_cast<float>(sumL[k] / cnt[k]);
                strips[s].clusters[k].a =
                    static_cast<float>(suma[k] / cnt[k]);
                strips[s].clusters[k].b =
                    static_cast<float>(sumb[k] / cnt[k]);
            }
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto k = static_cast<std::size_t>(base_index[y * width + x]);
                auto& lab = img_lab[y * width + x];
                float dL = lab.L - strips[s].clusters[k].L;
                float da = lab.a - strips[s].clusters[k].a;
                float db = lab.b - strips[s].clusters[k].b;
                strips[s].clusters[k].spread +=
                    static_cast<double>(dL * dL + da * da + db * db);
            }
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
                if (!seen[key]) {
                    seen[key] = true;
                    strips[s].cands.push_back(cs);
                    strips[s].cands_lab.push_back(
                        color_space::linear_to_oklab(cs));
                }
            };
            for (std::size_t x = x_lo; x < x_hi; ++x) add_cand(src[x, y]);
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (cnt[k] == 0) continue;
                color_space::OKLab cd{strips[s].clusters[k].L,
                                      strips[s].clusters[k].a,
                                      strips[s].clusters[k].b};
                add_cand(color_space::oklab_to_linear(cd).clamped());
            }
        }

        auto strip_err =
            [&](const StripStats& st,
                const std::array<color_space::OKLab, kBaseColors>& P_lab_v) {
                double e = 0;
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    auto& cl = st.clusters[k];
                    if (cl.count == 0) continue;
                    float dL = cl.L - P_lab_v[k].L;
                    float da = cl.a - P_lab_v[k].a;
                    float db = cl.b - P_lab_v[k].b;
                    e += static_cast<double>(cl.count) *
                         static_cast<double>(dL * dL + da * da + db * db);
                    e += cl.spread;
                }
                return e;
            };

        // Beam search params. Tuned by sweep across the test image set
        // (lovers/photo/fromthe/space3/electrichues02). B=64 is the
        // sweet spot for DPF: peak preview-PSNR (33.95 dB) at ~1s per
        // 320×213 image. Wider beams (B=128, 192, 256) keep lowering
        // planner error but PSNR plateaus — the planner's OKLab²
        // metric drifts from blurred-sRGB PSNR past this point.
        // K=16 saturates given 7 modifiable regs × kPerRegCap=4 = 28.
        constexpr std::size_t kBeamWidth = 64;
        constexpr std::size_t kCandsPerSlot = 16;
        struct BeamNode {
            std::array<Color3f, kBaseColors> P;
            std::array<color_space::OKLab, kBaseColors> P_lab;
            std::array<int, 32> dec_reg{};
            std::array<Color3f, 32> dec_color{};
            double cum_err = 0;
        };
        std::vector<BeamNode> beam(1);
        beam[0].P = P;
        beam[0].P_lab = P_lab;
        for (auto& d : beam[0].dec_reg) d = -1;
        beam[0].cum_err = strip_err(strips[0], P_lab);

        std::vector<BeamNode> next;
        next.reserve(kBeamWidth * (kCandsPerSlot + 1));

        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            next.clear();
            auto& st = strips[s + 1];
            bool strip_empty = true;
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (st.clusters[k].count > 0) { strip_empty = false; break; }
            }

            for (auto& state : beam) {
                double filler_err =
                    strip_empty ? 0.0 : strip_err(st, state.P_lab);
                {
                    BeamNode child = state;
                    child.dec_reg[s] = -1;
                    child.cum_err += filler_err;
                    next.push_back(child);
                }
                if (strip_empty) continue;

                struct Move {
                    int reg;
                    std::size_t cand_idx;
                    double err;
                };
                std::vector<Move> moves;
                moves.reserve(kBaseColors * st.cands.size());
                for (std::size_t k = k_min; k < kBaseColors; ++k) {
                    for (std::size_t ci = 0; ci < st.cands.size(); ++ci) {
                        auto& c_lab = st.cands_lab[ci];
                        double e = 0;
                        for (std::size_t k2 = 0; k2 < kBaseColors; ++k2) {
                            auto& cl = st.clusters[k2];
                            if (cl.count == 0) continue;
                            const auto& P2 =
                                (k2 == k) ? c_lab : state.P_lab[k2];
                            float dL = cl.L - P2.L;
                            float da = cl.a - P2.a;
                            float db = cl.b - P2.b;
                            e += static_cast<double>(cl.count) *
                                 static_cast<double>(
                                     dL * dL + da * da + db * db);
                            e += cl.spread;
                        }
                        if (e >= filler_err) continue;
                        moves.push_back({static_cast<int>(k), ci, e});
                    }
                }
                // Per-state per-register cap so beam expansion covers
                // multiple registers — without it, the top-K moves can
                // all target the same dominant register with slight
                // colour variations.
                std::sort(moves.begin(), moves.end(),
                    [](const Move& a, const Move& b) {
                        return a.err < b.err;
                    });
                constexpr std::size_t kPerRegCap = 4;
                std::array<std::size_t, kBaseColors> reg_taken{};
                std::vector<Move> picked;
                picked.reserve(kCandsPerSlot);
                for (auto& m : moves) {
                    auto rk = static_cast<std::size_t>(m.reg);
                    if (reg_taken[rk] >= kPerRegCap) continue;
                    picked.push_back(m);
                    ++reg_taken[rk];
                    if (picked.size() >= kCandsPerSlot) break;
                }
                moves = std::move(picked);

                for (auto& m : moves) {
                    auto reg_idx = static_cast<std::size_t>(m.reg);
                    BeamNode child = state;
                    child.P[reg_idx] = st.cands[m.cand_idx];
                    child.P_lab[reg_idx] = st.cands_lab[m.cand_idx];
                    child.dec_reg[s] = m.reg;
                    child.dec_color[s] = st.cands[m.cand_idx];
                    child.cum_err += m.err;
                    next.push_back(child);
                }
            }

            std::size_t keep_b = std::min(kBeamWidth, next.size());
            if (next.size() > keep_b) {
                std::partial_sort(
                    next.begin(),
                    next.begin() +
                        static_cast<std::ptrdiff_t>(keep_b),
                    next.end(),
                    [](const BeamNode& a, const BeamNode& b) {
                        return a.cum_err < b.cum_err;
                    });
                next.resize(keep_b);
            }
            beam.swap(next);
        }

        auto& best = *std::min_element(
            beam.begin(), beam.end(),
            [](const BeamNode& a, const BeamNode& b) {
                return a.cum_err < b.cum_err;
            });
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            int reg = best.dec_reg[s];
            if (reg < 0) {
                line_moves[y].push_back(make_move(kFillerReg, kFillerVal,
                                                  static_cast<int>(s)));
            } else {
                auto reg_idx = static_cast<std::size_t>(reg);
                Color3f col = best.dec_color[s];
                P[reg_idx] = col;
                P_lab[reg_idx] = color_space::linear_to_oklab(col);
                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(8 + reg_idx),
                    palette::linear_to_ocs(col),
                    static_cast<int>(s)));
                ++total_moves;
            }
            strip_palettes[s + 1] = P;
            strip_pal_lab[s + 1] = P_lab;
        }

        // 4. End-of-line WAIT — release copper to the next line's section.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));

        // Snapshot pass-1 strip state for this row; pass-2 dither runs
        // over the whole image once, after the per-row loop.
        strip_palettes_per_row[y] = strip_palettes;
        strip_pal_lab_per_row[y] = strip_pal_lab;

        if (height > 0 && (y & 0xF) == 0xF) {
            report_pass(pass, static_cast<float>(y + 1) /
                              static_cast<float>(height));
        }
    }

    // ---- Pass 2: whole-image dither against per-row, per-strip
    // palettes. Driver owns ED scaffolding (kernel, serpentine, bias
    // map, Riemersma queue, ostromoukhov scaling, ordered offsets);
    // picker resolves x_strip[x] → row's strip palette.
    total_error = 0.0;
    {
        float te = dither::diffuse_raw_buffer(
            src, dither_settings,
            [&](const color_space::OKLab& target,
                std::size_t x, std::size_t y) -> dither::PickResult {
                auto s = static_cast<std::size_t>(x_strip[x]);
                auto& pal = strip_palettes_per_row[y][s];
                auto& pl_lab = strip_pal_lab_per_row[y][s];
                std::span<const color_space::OKLab> pl_span(
                    pl_lab.data(), kBaseColors);

                std::size_t k = 0;
                color_space::OKLab chosen{};
                float thr = dither::pick_palette_index_with_ostro(
                    dither_settings.method, target, pl_span, x, y,
                    dither_settings.strength, k_min, k, chosen);
                indices[y * width + x] = static_cast<std::uint8_t>(k);
                preview[x, y] = pal[k];
                return {chosen, thr};
            });
        total_error = static_cast<double>(te);
    }

    // DBS post-pass refinement. Per-row, per-strip palettes resolve
    // through the same x_strip[x] lookup as the picker above; DBS
    // sweeps each pixel and tries all 8 candidates in that pixel's
    // effective strip palette, keeping any toggle that lowers the
    // HVS-blurred OKLab cost. After this pass we re-render `preview`
    // from the (possibly-changed) indices so caller-visible buffers
    // stay consistent.
    if (dither_settings.method == dither::Method::dbs) {
        dither::apply_dbs_post_pass(
            src, indices,
            [&](std::size_t x, std::size_t y)
                -> std::span<const color_space::OKLab> {
                auto s = static_cast<std::size_t>(x_strip[x]);
                auto& pl_lab = strip_pal_lab_per_row[y][s];
                return std::span<const color_space::OKLab>(
                    pl_lab.data(), kBaseColors);
            });
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                auto s = static_cast<std::size_t>(x_strip[x]);
                preview[x, y] =
                    strip_palettes_per_row[y][s][indices[y * width + x]];
            }
        }
    }
    report_pass(pass + 1, 0.0f);
    }  // kPasses
    if (on_progress) on_progress(1.0f, "done");

    // ---- 4. 3-plane PF2 encoding, then expand to 6-plane DPF ------------
    auto enc = bitplane::encode(indices, width, height,
                                /*depth=*/3, bitplane::Layout::interleaved);
    if (!enc) return std::unexpected{enc.error()};
    auto expanded = bitplane::expand_to_dpf_pf2(*enc);
    if (!expanded) return std::unexpected{expanded.error()};

    // ---- 5. Output palette: PF2 base entries at OCS DPF addresses -----
    // Per the OCS DPF combiner rule (above), PF2 index 0 displays as
    // COLOR00 (NOT COLOR08), and PF2 indices 1..7 display as
    // COLOR09..15. COLOR08 is unused on OCS DPF. Address the frame-init
    // palette accordingly so the cpp viewer renders correctly on real
    // OCS hardware (and on AGA in OCS-DPF mode without BPLCON3 PF2OF).
    //
    // In debug_overlay mode all entries stay at 0x0000 — together with
    // the forced-zero per-line MOVEs this means the viewer's frame-init
    // writes black to every register and only SCAP MOVEs change colours.
    std::vector<Color3f> output_palette(16, Color3f{0.0f, 0.0f, 0.0f});
    if (!debug_overlay) {
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            auto regs = pf2_writes(k);
            for (int reg : regs) {
                if (reg < 0) continue;
                output_palette[static_cast<std::size_t>(reg)] = base_palette[k];
            }
        }
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
        output_palette[1] = Color3f{1.0f, 0.0f, 0.0f};   // red

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
                preview[x, y] = Color3f{1.0f, 0.0f, 0.0f};
            }
        }
        (void)bpr;
    }

    ScapResult res;
    res.planes = *std::move(expanded);
    res.palette = std::move(output_palette);
    res.slot_table = table;
    res.total_error = static_cast<float>(total_error);
    res.avg_changes_per_line = height > 0
        ? static_cast<float>(total_moves) / static_cast<float>(height)
        : 0.0f;
    {
        std::size_t total_all = 0, row_max = 0;
        std::size_t total_hb = 0, hb_max = 0;
        std::size_t total_vis = 0, vis_max = 0;
        for (auto& row : line_moves) {
            std::size_t rm = 0, hb = 0, vis = 0;
            bool past_line_gate = false;
            for (auto& op : row) {
                if (op.kind == ScapOpKind::kWait) {
                    past_line_gate = true;
                    continue;
                }
                ++rm;
                if (past_line_gate) ++vis; else ++hb;
            }
            total_all += rm;  if (rm  > row_max) row_max = rm;
            total_hb  += hb;  if (hb  > hb_max)  hb_max  = hb;
            total_vis += vis; if (vis > vis_max) vis_max = vis;
        }
        auto h = static_cast<float>(height ? height : 1);
        res.avg_total_moves_per_line   = static_cast<float>(total_all) / h;
        res.max_moves_per_line         = row_max;
        res.avg_hblank_moves_per_line  = static_cast<float>(total_hb)  / h;
        res.max_hblank_moves_per_line  = hb_max;
        res.avg_visible_moves_per_line = static_cast<float>(total_vis) / h;
        res.max_visible_moves_per_line = vis_max;
    }
    res.line_moves = std::move(line_moves);
    // Snap preview to OCS RGB444 — SCAP is OCS-only, and the snap-defer
    // patches in copper/scap/ham left intermediate per-strip palettes at
    // full 8-bit precision. The actual chip displays RGB444; preview must
    // match. Without this, color counts > 4096 leak into the preview.
    for (auto& p : preview.pixels()) p = palette::quantize_to_ocs(p);
    res.rendered = std::move(preview);
    return res;
}

// half_brite() lives in palette.hpp; pull it into this TU's lookup.
using palette::half_brite;

// EHB SCAP slot-tuning debug bundle. All bitplane pixels use a
// single shared register (index 2). Frame-init palette puts that
// register at black; SCAP slot s alternates the register between
// white (s even) and black (s odd) at the slot's MOVE position. Net
// visual: 16-px-wide black/white stripes with the transition AT the
// slot's actual hardware MOVE landing — the visible edge IS the
// timing measurement.
//
// PF1-style yellow rulers aren't available on EHB (no PF1 layer),
// so the ruler paints into the bitplane data with index 1 = yellow
// (locked in CAP). Ruler pixels override the stripe content but
// give stable x-coord references at 4/8/16-px hierarchy.
static Result<ScapResult> encode_scap_ehb_debug(std::size_t width,
                                                std::size_t height) {
    auto& table = kScap6bplEhb;
    constexpr std::size_t kStripeReg = 2;       // shared register all pixels use
    constexpr std::uint16_t kBlack = 0x0000;
    constexpr int kVStart = 44;

    // ---- Bitplane data: every pixel = index kStripeReg = 0b00010 -------
    auto enc = bitplane::BitplaneData{};
    auto aligned_w = (width + 15u) & ~std::size_t{15};
    enc.width = width;
    enc.height = height;
    enc.depth = 6;
    enc.bytes_per_row = aligned_w / 8;
    enc.layout = bitplane::Layout::interleaved;
    enc.data.assign(enc.total_bytes(), 0);
    // Set every byte of plane 1 (= bit value 2) to 0xFF → all pixels = idx 2
    for (std::size_t y = 0; y < height; ++y) {
        auto off = enc.plane_row_offset(/*plane=*/1, y);
        std::fill_n(enc.data.data() + off, enc.bytes_per_row,
                    static_cast<std::uint8_t>(0xFF));
    }
    // Yellow rulers: paint ruler pixels with index 1 = 0b00001.
    // Clear plane 1 (drop idx 2), set plane 0 (add idx 1).
    auto h_full    = height;
    auto h_half    = height / 2;
    auto h_quarter = height / 4;
    auto set_bit = [&](std::size_t plane, std::size_t y, std::size_t x, bool on) {
        auto off = enc.plane_row_offset(plane, y);
        auto byte = x / 8;
        auto mask = static_cast<std::uint8_t>(1u << (7 - (x % 8)));
        if (on) enc.data[off + byte] |=  mask;
        else    enc.data[off + byte] &= ~mask;
    };
    for (std::size_t x = 0; x < width; ++x) {
        std::size_t marker_h = 0;
        if      (x % 16 == 0) marker_h = h_full;
        else if (x %  8 == 0) marker_h = h_half;
        else if (x %  4 == 0) marker_h = h_quarter;
        else continue;
        for (std::size_t yy = 0; yy < marker_h; ++yy) {
            set_bit(0, yy, x, true);   // plane 0 ON  → idx |= 1
            set_bit(1, yy, x, false);  // plane 1 OFF → idx &= ~2 (= idx 1)
        }
    }

    // ---- Output palette (32 base entries) ----------------------------
    std::vector<Color3f> palette(32, Color3f{0.0f, 0.0f, 0.0f});
    palette[1] = Color3f{1.0f, 0.0f, 0.0f};  // red ruler
    // palette[kStripeReg] = black (default 0x000); SCAP MOVEs change it.

    // ---- Per-line copper: 1 reset MOVE + line-gate WAIT + 19 swaps ----
    std::vector<std::vector<ScapMove>> line_moves(height);
    for (std::size_t y = 0; y < height; ++y) {
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);
        // Reset the shared register to black at top of each line (the
        // ONE per-line CAP MOVE we need; fits in hblank trivially).
        line_moves[y].push_back(make_move(
            static_cast<std::uint8_t>(kStripeReg), kBlack, -1));
        // Line-gate WAIT.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));
        // 19 SCAP MOVEs: opposing primary/complement RGB pairs on the
        // shared register. Pair N cycles through (R,C), (G,M), (B,Y);
        // pair-mod-3 picks which axis. Each stripe is a single solid
        // saturated colour. Vivid hues make slot positions easy to
        // pick out against the red ruler.
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            auto pair_n = s / 2;
            std::uint16_t color = 0, complement = 0;
            switch (pair_n % 3) {
                case 0: color = 0x0F00; complement = 0x00FF; break;  // R / C
                case 1: color = 0x00F0; complement = 0x0F0F; break;  // G / M
                case 2: color = 0x000F; complement = 0x0FF0; break;  // B / Y
            }
            std::uint16_t v = (s % 2 == 0) ? color : complement;
            line_moves[y].push_back(make_move(
                static_cast<std::uint8_t>(kStripeReg), v,
                static_cast<int>(s)));
        }
        // End-of-line WAIT.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));
    }

    // Build a rendered preview matching what the planner expects: every
    // pixel = palette[kStripeReg] except ruler markers = palette[1].
    Image preview(width, height);
    for (std::size_t yy = 0; yy < height; ++yy) {
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t marker_h = 0;
            if      (x % 16 == 0) marker_h = h_full;
            else if (x %  8 == 0) marker_h = h_half;
            else if (x %  4 == 0) marker_h = h_quarter;
            preview[x, yy] = (yy < marker_h)
                ? Color3f{1.0f, 0.0f, 0.0f}     // ruler red
                // Stripe approximation: white if "MOVE-after" position,
                // black if before. Just paint expected stripe pattern
                // assuming MOVEs land at slots[s].pixel_x.
                : ([&]() {
                    Color3f c{0,0,0};
                    for (std::size_t s = 0; s < table.slots.size(); ++s) {
                        if (static_cast<int>(x) >= table.slots[s].pixel_x &&
                            (s + 1 == table.slots.size() ||
                             static_cast<int>(x) < table.slots[s + 1].pixel_x)) {
                            // Mirror the SCAP MOVE values used above:
                            // pair N gets (0xFFF - N·0x111, N·0x111).
                            std::size_t pair_int = s / 2;
                            // Mirror cpp: cycle (R,C), (G,M), (B,Y).
                            Color3f base, comp;
                            switch (pair_int % 3) {
                                case 0: base = {1, 0, 0}; comp = {0, 1, 1}; break;
                                case 1: base = {0, 1, 0}; comp = {1, 0, 1}; break;
                                default: base = {0, 0, 1}; comp = {1, 1, 0}; break;
                            }
                            c = (s % 2 == 0) ? base : comp;
                            break;
                        }
                    }
                    return c;
                })();
        }
    }

    ScapResult res;
    res.planes = std::move(enc);
    res.palette = std::move(palette);
    res.slot_table = table;
    res.total_error = 0.0f;
    res.avg_changes_per_line = 0.0f;
    res.avg_total_moves_per_line = static_cast<float>(
        line_moves.empty() ? 0 : 1 + table.slots.size());
    res.max_moves_per_line = 1 + table.slots.size();
    res.avg_hblank_moves_per_line = 1.0f;
    res.max_hblank_moves_per_line = 1;
    res.avg_visible_moves_per_line = static_cast<float>(table.slots.size());
    res.max_visible_moves_per_line = table.slots.size();
    res.line_moves = std::move(line_moves);
    for (auto& p : preview.pixels()) p = palette::quantize_to_ocs(p);
    res.rendered = std::move(preview);
    return res;
}

Result<ScapResult> encode_scap_ehb_ocs(const Image& image,
                                       int width_arg,
                                       int height_arg,
                                       bool reserve_color0,
                                       const dither::Settings& dither_settings,
                                       std::size_t copper_changes_override,
                                       int palette_diversity,
                                       bool debug_overlay,
                                       std::function<void(float, std::string_view)>
                                           on_progress,
                                       bool cap_best,
                                       std::string_view cap_best_metric,
                                       int cap_spread_radius,
                                       float cap_spread_decay) {
    // --cap-best: 8 jitter seeds (32-base palette has shallower basins
    // than DPF's 8-base, so heavy jitter sampling buys less here).
    // Total 5×4×8 + 1 = 161 trials, ~30–40 s on 8 cores.
    if (cap_best) {
        auto metric = (cap_best_metric == "msssim")
            ? pipeline::CapBestMetric::msssim
            : pipeline::CapBestMetric::psnr;
        auto best = pipeline::cap_best_sweep<ScapResult>(
            image, dither_settings, palette_diversity, /*jitter_count=*/8,
            [&](const Image& jittered_in,
                const dither::Settings& d, int div) {
                return encode_scap_ehb_ocs(
                    jittered_in, width_arg, height_arg, reserve_color0,
                    d, copper_changes_override, div, debug_overlay,
                    /*on_progress=*/{}, /*cap_best=*/false, "psnr",
                    cap_spread_radius, cap_spread_decay);
            },
            [](const ScapResult& r) -> const Image& { return r.rendered; },
            on_progress,
            /*jitter_amplitude=*/1.0f,
            metric);
        if (best.has_value()) return std::move(*best);
    }
    auto& table = kScap6bplEhb;
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
    if (debug_overlay) return encode_scap_ehb_debug(width, height);
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
    // CAP and SCAP share the OCS hblank's MOVE budget. Adaptive split:
    //   * Each line's hblank fits up to kHblankCeiling MOVEs (=14 for
    //     OCS empirical safe ceiling). Hblank load on line y+1 is
    //     CAP_changes[y+1] + SCAP_swaps[y] (revert) — exceeding 14
    //     causes hardware overflow on busy images (verified per
    //     fantasy.png). So per-line: SCAP_swaps[y] ≤ kHblankCeiling -
    //     CAP_changes[y+1]. CAP_changes per line comes straight from
    //     copper_result->scanline_changes (already planned).
    //   * --copper-changes N caps the COMBINED budget globally.
    //     CAP gets min(N, 2), SCAP gets the per-line adaptive value
    //     bounded by N - CAP_share.
    //   * Auto: same adaptive logic, no global cap beyond hblank.
    constexpr std::size_t kHblankCeiling = 14;
    constexpr std::size_t kMaxCombinedEhb = 20;  // CAP=2 + SCAP=18 visible max
    std::size_t total_budget_ehb = (copper_changes_override > 0)
        ? std::min<std::size_t>(copper_changes_override, kMaxCombinedEhb)
        : kMaxCombinedEhb;
    std::size_t cap_share_ehb = std::min<std::size_t>(total_budget_ehb, 2u);
    std::size_t scap_share_ehb_max = total_budget_ehb - cap_share_ehb;
    auto copper_result = copper::encode_copper(
        src, /*depth=*/5, dither_settings,
        amiga::Chipset::ocs,
        cap_share_ehb,
        /*user_palette=*/nullptr,
        reserve_color0,
        /*locked=*/{},
        palette_diversity,
        /*skip_initial_swap_rows=*/0,
        /*is_lace=*/false,
        /*is_ehb=*/true,
        /*on_progress=*/{},
        cap_spread_radius >= 0
            ? static_cast<std::size_t>(cap_spread_radius)
            : std::numeric_limits<std::size_t>::max(),
        cap_spread_decay >= 0.0f ? cap_spread_decay : -1.0f);
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

    // ED scaffolding (kernel, error buf, structure bias, Riemersma queue,
    // ostromoukhov scaling, ordered offsets) lives inside
    // dither::diffuse_raw_buffer (the post-pass-1 driver call below).

    constexpr int kVStart = 44;
    double total_error = 0.0;
    std::size_t total_moves = 0;

    std::size_t num_strips = table.slots.size() + 1;
    std::vector<std::vector<Color3f>> strip_eff(num_strips,
        std::vector<Color3f>(kEffective));
    std::vector<std::vector<color_space::OKLab>> strip_eff_lab(num_strips,
        std::vector<color_space::OKLab>(kEffective));

    // Per-row snapshot for the post-loop driver call (pass 2). Each row
    // overwrites strip_eff[s]/strip_eff_lab[s] during pass-1 planning;
    // we copy the snapshot into the [y] slot before moving on.
    std::vector<std::vector<std::vector<Color3f>>>
        strip_eff_per_row(height,
            std::vector<std::vector<Color3f>>(num_strips,
                std::vector<Color3f>(kEffective)));
    std::vector<std::vector<std::vector<color_space::OKLab>>>
        strip_eff_lab_per_row(height,
            std::vector<std::vector<color_space::OKLab>>(num_strips,
                std::vector<color_space::OKLab>(kEffective)));

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
    //
    // Actual hardware register state across lines. SCAP swaps leave
    // registers holding swap-colours at end-of-line; the per-line
    // CAP MOVEs need to diff against THIS, not against cap_palettes
    // from the previous line.
    std::vector<Color3f> hw_state(kBaseColors);
    constexpr int kPasses = 6;
    auto report_pass = [&](int pass_idx, float local) {
        if (on_progress) {
            float p = (static_cast<float>(pass_idx) +
                       std::clamp(local, 0.0f, 1.0f)) /
                      static_cast<float>(kPasses);
            on_progress(p, "encoding");
        }
    };
    if (on_progress) on_progress(0.0f, "encoding");
    for (int pass = 0; pass < kPasses; ++pass) {
        // Reset hw_state to the viewer's frame-init at each pass start.
        for (std::size_t k = 0; k < kBaseColors; ++k)
            hw_state[k] = base_palette[k];
        if (pass > 0) {
            for (std::size_t i = 0; i < indices.size(); ++i)
                base_index[i] = indices[i];
            for (auto& v : line_moves) v.clear();
            // err_buf is owned by dither::diffuse_raw_buffer (allocated
            // fresh each pass-2 call), so no manual reset is needed.
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

        // 1. Per-line CAP MOVEs: diff vs the ACTUAL hardware register
        // state at end of the previous line. SCAP's mid-line swaps on
        // line y-1 may have left registers holding swap-colours rather
        // than cap_palettes[y-1], so a diff vs cap_palettes misses
        // them and the registers carry stale state into line y.
        {
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (hw_state[k].r != P[k].r ||
                    hw_state[k].g != P[k].g ||
                    hw_state[k].b != P[k].b) {
                    hw_state[k] = P[k];
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

        // 3. SCAP MOVEs. Joint beam-search planner (matches DPF) with
        //    EHB-specific extras: each base[k] swap implicitly redefines
        //    half-brite[k] = halve(base[k]), so strip pixels can bind
        //    to either index k or index 32+k and both contribute to a
        //    swap's strip error. Hblank ceiling: per-line CAP on line
        //    y+1 emits a MOVE for every register where state.P[k] !=
        //    cap_palettes[y+1][k] — beam expansion forbids candidates
        //    whose application would push that count past kHblankCeiling.
        //    useful_swap_cap = scap_share_ehb_max bounds total swaps
        //    per chain (CAP+SCAP combined budget).
        bool has_next_line = (y + 1 < height &&
                              y + 1 < copper_result->scanline_palettes.size());

        // Per-strip cluster stats: for each register k, separate base
        // and half-brite clusters. Pixel binding[idx] in [0..63] →
        // k = idx & 31, is_half = idx >= 32.
        struct EClust {
            float L = 0, a = 0, b = 0;
            double spread = 0;
            std::uint16_t count = 0;
        };
        struct EStripStats {
            std::array<EClust, kBaseColors> cb{};   // base-bound cluster
            std::array<EClust, kBaseColors> ch{};   // half-bound cluster
            std::vector<Color3f> cands;             // OCS-snapped
            std::vector<color_space::OKLab> cands_lab_b;  // OKLab(c)
            std::vector<color_space::OKLab> cands_lab_h;  // OKLab(halve(c))
        };

        std::vector<EStripStats> strips(num_strips);
        auto strip_x_range = [&](std::size_t s) {
            std::size_t lo = (s == 0) ? std::size_t{0}
                : std::min(width,
                    static_cast<std::size_t>(table.slots[s - 1].pixel_x));
            std::size_t hi = (s < table.slots.size())
                ? std::min(width,
                    static_cast<std::size_t>(table.slots[s].pixel_x))
                : width;
            return std::pair<std::size_t, std::size_t>{lo, hi};
        };
        for (std::size_t s = 0; s < num_strips; ++s) {
            auto [x_lo, x_hi] = strip_x_range(s);
            if (x_lo >= x_hi) continue;
            std::array<double, kBaseColors> sumLb{}, sumab{}, sumbb{};
            std::array<double, kBaseColors> sumLh{}, sumah{}, sumbh{};
            std::array<std::uint32_t, kBaseColors> cntb{}, cnth{};
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto idx = static_cast<std::size_t>(base_index[y * width + x]);
                std::size_t k = idx & (kBaseColors - 1);
                bool is_half = idx >= kBaseColors;
                auto& lab = img_lab[y * width + x];
                if (is_half) {
                    sumLh[k] += static_cast<double>(lab.L);
                    sumah[k] += static_cast<double>(lab.a);
                    sumbh[k] += static_cast<double>(lab.b);
                    ++cnth[k];
                } else {
                    sumLb[k] += static_cast<double>(lab.L);
                    sumab[k] += static_cast<double>(lab.a);
                    sumbb[k] += static_cast<double>(lab.b);
                    ++cntb[k];
                }
            }
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (cntb[k] > 0) {
                    strips[s].cb[k].count = static_cast<std::uint16_t>(cntb[k]);
                    strips[s].cb[k].L = static_cast<float>(sumLb[k] / cntb[k]);
                    strips[s].cb[k].a = static_cast<float>(sumab[k] / cntb[k]);
                    strips[s].cb[k].b = static_cast<float>(sumbb[k] / cntb[k]);
                }
                if (cnth[k] > 0) {
                    strips[s].ch[k].count = static_cast<std::uint16_t>(cnth[k]);
                    strips[s].ch[k].L = static_cast<float>(sumLh[k] / cnth[k]);
                    strips[s].ch[k].a = static_cast<float>(sumah[k] / cnth[k]);
                    strips[s].ch[k].b = static_cast<float>(sumbh[k] / cnth[k]);
                }
            }
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto idx = static_cast<std::size_t>(base_index[y * width + x]);
                std::size_t k = idx & (kBaseColors - 1);
                bool is_half = idx >= kBaseColors;
                auto& lab = img_lab[y * width + x];
                auto& cl = is_half ? strips[s].ch[k] : strips[s].cb[k];
                float dL = lab.L - cl.L;
                float da = lab.a - cl.a;
                float db = lab.b - cl.b;
                cl.spread += static_cast<double>(dL * dL + da * da + db * db);
            }
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
                if (!seen[key]) {
                    seen[key] = true;
                    strips[s].cands.push_back(cs);
                    strips[s].cands_lab_b.push_back(
                        color_space::linear_to_oklab(cs));
                    strips[s].cands_lab_h.push_back(
                        color_space::linear_to_oklab(half_brite(cs)));
                }
            };
            for (std::size_t x = x_lo; x < x_hi; ++x) add_cand(src[x, y]);
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (cntb[k] > 0) {
                    color_space::OKLab cd{strips[s].cb[k].L,
                                          strips[s].cb[k].a,
                                          strips[s].cb[k].b};
                    add_cand(color_space::oklab_to_linear(cd).clamped());
                }
                if (cnth[k] > 0) {
                    // Half-brite-bound pixels want base ≈ 2×pixel.
                    color_space::OKLab dbl{
                        std::min(2.0f * strips[s].ch[k].L, 1.0f),
                        2.0f * strips[s].ch[k].a,
                        2.0f * strips[s].ch[k].b};
                    add_cand(color_space::oklab_to_linear(dbl).clamped());
                }
            }
        }

        // Strip-error helper: given P_lab_b[32] (base) and P_lab_h[32]
        // (halve), summed cluster errors.
        auto e_strip_err =
            [&](const EStripStats& st,
                const std::array<color_space::OKLab, kBaseColors>& Plb,
                const std::array<color_space::OKLab, kBaseColors>& Plh) {
                double e = 0;
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    if (st.cb[k].count > 0) {
                        float dL = st.cb[k].L - Plb[k].L;
                        float da = st.cb[k].a - Plb[k].a;
                        float db = st.cb[k].b - Plb[k].b;
                        e += static_cast<double>(st.cb[k].count) *
                             static_cast<double>(dL * dL + da * da + db * db);
                        e += st.cb[k].spread;
                    }
                    if (st.ch[k].count > 0) {
                        float dL = st.ch[k].L - Plh[k].L;
                        float da = st.ch[k].a - Plh[k].a;
                        float db = st.ch[k].b - Plh[k].b;
                        e += static_cast<double>(st.ch[k].count) *
                             static_cast<double>(dL * dL + da * da + db * db);
                        e += st.ch[k].spread;
                    }
                }
                return e;
            };

        // Beam state. P holds 32 base linear-RGB; P_lab_b and P_lab_h
        // are the cached OKLab of base and halve(base) respectively.
        // B=2 is the sweet spot for EHB SCAP per the same sweep: PSNR
        // peaks at 40.49 dB. Wider beams keep lowering planner error
        // but worsen preview-PSNR because dither residuals scatter
        // into noise the planner doesn't see — the OKLab² metric
        // drifts hard from blurred-sRGB PSNR once the EHB plan is
        // already this tight (default error ~51 vs DPF ~140).
        //   B=1: err=52.13 psnr=40.31 dB  (= greedy)
        //   B=2: err=51.16 psnr=40.49 dB  ← peak
        //   B=3: err=50.74 psnr=40.37 dB
        //   B=4: err=50.57 psnr=40.45 dB
        //   B=16: err=50.03 psnr=40.19 dB (over-fits)
        constexpr std::size_t kBeamWidth = 2;
        constexpr std::size_t kCandsPerSlot = 16;
        constexpr std::size_t kEMaxSlots = 32;
        constexpr std::size_t kHblankCeilingLocal = kHblankCeiling;
        struct ENode {
            std::array<Color3f, kBaseColors> P;
            std::array<color_space::OKLab, kBaseColors> P_lab_b;
            std::array<color_space::OKLab, kBaseColors> P_lab_h;
            std::array<int, kEMaxSlots> dec_reg{};
            std::array<Color3f, kEMaxSlots> dec_color{};
            std::uint16_t projected_hblank = 0;
            std::uint16_t useful_swaps = 0;
            double cum_err = 0;
        };

        constexpr std::size_t kMaxVisibleMoves = 18;
        std::size_t slots_to_run =
            std::min(table.slots.size(), kMaxVisibleMoves);
        std::size_t useful_swap_cap = scap_share_ehb_max;

        ENode init{};
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            init.P[k] = P[k];
            init.P_lab_b[k] = color_space::linear_to_oklab(P[k]);
            init.P_lab_h[k] = color_space::linear_to_oklab(half_brite(P[k]));
        }
        for (auto& d : init.dec_reg) d = -1;
        if (has_next_line) {
            std::uint16_t h0 = 0;
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                auto& a = init.P[k];
                auto& b = cap_palettes[y + 1][k];
                if (a.r != b.r || a.g != b.g || a.b != b.b) ++h0;
            }
            init.projected_hblank = h0;
        }
        init.cum_err = e_strip_err(strips[0], init.P_lab_b, init.P_lab_h);

        std::vector<ENode> beam{init};
        std::vector<ENode> next;
        next.reserve(kBeamWidth * (kCandsPerSlot + 1));

        for (std::size_t s = 0; s < slots_to_run; ++s) {
            next.clear();
            auto& st = strips[s + 1];
            bool strip_empty = true;
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (st.cb[k].count > 0 || st.ch[k].count > 0) {
                    strip_empty = false; break;
                }
            }

            for (auto& state : beam) {
                double filler_err = strip_empty
                    ? 0.0
                    : e_strip_err(st, state.P_lab_b, state.P_lab_h);
                {
                    ENode child = state;
                    child.dec_reg[s] = -1;
                    child.cum_err += filler_err;
                    next.push_back(child);
                }
                if (strip_empty) continue;
                if (state.useful_swaps >= useful_swap_cap) continue;

                struct Move {
                    int reg;
                    std::size_t cand_idx;
                    int hblank_delta;
                    double err;
                };
                std::vector<Move> moves;
                moves.reserve(kBaseColors * st.cands.size());
                for (std::size_t k = k_min; k < kBaseColors; ++k) {
                    // Hblank-budget gate: precompute delta for register
                    // k swap-vs-current. delta = (new_diff_with_next ?
                    // 1 : 0) - (old_diff_with_next ? 1 : 0).
                    bool old_diff = false;
                    if (has_next_line) {
                        auto& a = state.P[k];
                        auto& b = cap_palettes[y + 1][k];
                        old_diff = (a.r != b.r || a.g != b.g || a.b != b.b);
                    }
                    for (std::size_t ci = 0; ci < st.cands.size(); ++ci) {
                        auto& c_lab_b = st.cands_lab_b[ci];
                        auto& c_lab_h = st.cands_lab_h[ci];
                        // Strip error with state.P_lab_*  but k-th
                        // entries replaced by candidate's lab.
                        double e = 0;
                        for (std::size_t k2 = 0; k2 < kBaseColors; ++k2) {
                            const auto& Pb =
                                (k2 == k) ? c_lab_b : state.P_lab_b[k2];
                            const auto& Ph =
                                (k2 == k) ? c_lab_h : state.P_lab_h[k2];
                            if (st.cb[k2].count > 0) {
                                float dL = st.cb[k2].L - Pb.L;
                                float da = st.cb[k2].a - Pb.a;
                                float db = st.cb[k2].b - Pb.b;
                                e += static_cast<double>(st.cb[k2].count) *
                                     static_cast<double>(
                                         dL * dL + da * da + db * db);
                                e += st.cb[k2].spread;
                            }
                            if (st.ch[k2].count > 0) {
                                float dL = st.ch[k2].L - Ph.L;
                                float da = st.ch[k2].a - Ph.a;
                                float db = st.ch[k2].b - Ph.b;
                                e += static_cast<double>(st.ch[k2].count) *
                                     static_cast<double>(
                                         dL * dL + da * da + db * db);
                                e += st.ch[k2].spread;
                            }
                        }
                        if (e >= filler_err) continue;
                        int delta = 0;
                        if (has_next_line) {
                            auto& cs = st.cands[ci];
                            auto& nxt = cap_palettes[y + 1][k];
                            bool new_diff = (cs.r != nxt.r ||
                                             cs.g != nxt.g ||
                                             cs.b != nxt.b);
                            delta = (new_diff ? 1 : 0) -
                                    (old_diff ? 1 : 0);
                            if (state.projected_hblank +
                                static_cast<std::size_t>(std::max(0, delta))
                                > kHblankCeilingLocal) {
                                continue;  // would overflow next-line hblank
                            }
                        }
                        moves.push_back(
                            {static_cast<int>(k), ci, delta, e});
                    }
                }
                std::sort(moves.begin(), moves.end(),
                    [](const Move& a, const Move& b) {
                        return a.err < b.err;
                    });
                constexpr std::size_t kPerRegCap = 1;
                std::array<std::size_t, kBaseColors> reg_taken{};
                std::vector<Move> picked;
                picked.reserve(kCandsPerSlot);
                for (auto& m : moves) {
                    auto rk = static_cast<std::size_t>(m.reg);
                    if (reg_taken[rk] >= kPerRegCap) continue;
                    picked.push_back(m);
                    ++reg_taken[rk];
                    if (picked.size() >= kCandsPerSlot) break;
                }

                for (auto& m : picked) {
                    auto reg_idx = static_cast<std::size_t>(m.reg);
                    ENode child = state;
                    child.P[reg_idx] = st.cands[m.cand_idx];
                    child.P_lab_b[reg_idx] = st.cands_lab_b[m.cand_idx];
                    child.P_lab_h[reg_idx] = st.cands_lab_h[m.cand_idx];
                    child.dec_reg[s] = m.reg;
                    child.dec_color[s] = st.cands[m.cand_idx];
                    child.cum_err += m.err;
                    child.projected_hblank = static_cast<std::uint16_t>(
                        static_cast<int>(child.projected_hblank) +
                        m.hblank_delta);
                    ++child.useful_swaps;
                    next.push_back(child);
                }
            }

            std::size_t keep_b = std::min(kBeamWidth, next.size());
            if (next.size() > keep_b) {
                std::partial_sort(
                    next.begin(),
                    next.begin() +
                        static_cast<std::ptrdiff_t>(keep_b),
                    next.end(),
                    [](const ENode& a, const ENode& b) {
                        return a.cum_err < b.cum_err;
                    });
                next.resize(keep_b);
            }
            beam.swap(next);
        }

        auto& best = *std::min_element(
            beam.begin(), beam.end(),
            [](const ENode& a, const ENode& b) {
                return a.cum_err < b.cum_err;
            });

        // Apply chain: emit per-slot MOVEs, update P/P_eff/P_eff_lab/
        // hw_state, snapshot strip palettes for the render pass.
        for (std::size_t s = 0; s < slots_to_run; ++s) {
            int reg = best.dec_reg[s];
            if (reg < 0) {
                line_moves[y].push_back(make_move(
                    /*reg=*/0, palette::linear_to_ocs(hw_state[0]),
                    static_cast<int>(s)));
            } else {
                auto k = static_cast<std::size_t>(reg);
                Color3f col = best.dec_color[s];
                P[k] = col;
                P_eff[k] = col;
                P_eff[kBaseColors + k] = half_brite(col);
                P_eff_lab[k] = color_space::linear_to_oklab(col);
                P_eff_lab[kBaseColors + k] =
                    color_space::linear_to_oklab(P_eff[kBaseColors + k]);
                hw_state[k] = col;
                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(kRegBase + reg),
                    palette::linear_to_ocs(col),
                    static_cast<int>(s)));
                ++total_moves;
            }
            strip_eff[s + 1] = P_eff;
            strip_eff_lab[s + 1] = P_eff_lab;
        }
        // Skipped slots beyond slots_to_run keep the post-last-slot
        // palette state.
        for (std::size_t s = slots_to_run; s < num_strips - 1; ++s) {
            strip_eff[s + 1] = P_eff;
            strip_eff_lab[s + 1] = P_eff_lab;
        }

        // 4. End-of-line WAIT.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.end_of_line_hpos), vp, -1));

        // Snapshot pass-1 strip state for this row; pass-2 dither runs
        // over the whole image once, after the per-row loop.
        strip_eff_per_row[y] = strip_eff;
        strip_eff_lab_per_row[y] = strip_eff_lab;

        if (height > 0 && (y & 0xF) == 0xF) {
            report_pass(pass, static_cast<float>(y + 1) /
                              static_cast<float>(height));
        }
    }

    // Stage-2 render across all 64 effective entries per pixel against
    // the per-row, per-strip palette. Driver owns ED scaffolding
    // (kernel, serpentine, structure bias, Riemersma, ostromoukhov,
    // ordered offsets); picker resolves x_strip[x] → row's strip
    // palette, then yliluoma family or nearest pair pick.
    total_error = 0.0;
    {
        float te = dither::diffuse_raw_buffer(
            src, dither_settings,
            [&](const color_space::OKLab& target,
                std::size_t x, std::size_t y) -> dither::PickResult {
                auto s = static_cast<std::size_t>(x_strip[x]);
                auto& eff_pal = strip_eff_per_row[y][s];
                auto& eff_lab = strip_eff_lab_per_row[y][s];
                std::span<const color_space::OKLab> eff_span(
                    eff_lab.data(), kEffective);

                std::size_t k = 0;
                color_space::OKLab chosen{};
                float thr = dither::pick_palette_index_with_ostro(
                    dither_settings.method, target, eff_span, x, y,
                    dither_settings.strength, k_min, k, chosen);
                indices[y * width + x] = static_cast<std::uint8_t>(k);
                preview[x, y] = eff_pal[k];
                return {chosen, thr};
            });
        total_error = static_cast<double>(te);
    }

    // DBS post-pass for SCAP+EHB. Same shape as the DPF SCAP path, but
    // the candidate set is the 64-entry effective palette (32 base +
    // 32 half-brites). DBS picks any of the 64 indices; the half-brite
    // bit is just bit 5 of the resulting index.
    if (dither_settings.method == dither::Method::dbs) {
        dither::apply_dbs_post_pass(
            src, indices,
            [&](std::size_t x, std::size_t y)
                -> std::span<const color_space::OKLab> {
                auto s = static_cast<std::size_t>(x_strip[x]);
                auto& eff_lab = strip_eff_lab_per_row[y][s];
                return std::span<const color_space::OKLab>(
                    eff_lab.data(), kEffective);
            });
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                auto s = static_cast<std::size_t>(x_strip[x]);
                preview[x, y] =
                    strip_eff_per_row[y][s][indices[y * width + x]];
            }
        }
    }
    report_pass(pass + 1, 0.0f);
    }  // kPasses
    if (on_progress) on_progress(1.0f, "done");

    // ---- 4. 6-plane bitplane encoding. The 6-bit index already encodes
    // half-brite as bit 5, which is exactly what the EHB hardware reads.
    auto enc = bitplane::encode(indices, width, height,
                                /*depth=*/6, bitplane::Layout::interleaved);
    if (!enc) return std::unexpected{enc.error()};

    ScapResult res;
    res.planes = *std::move(enc);
    res.palette = std::move(base_palette);  // 32 base entries; HW derives 32 half-brites
    res.slot_table = table;
    res.total_error = static_cast<float>(total_error);
    res.avg_changes_per_line = height > 0
        ? static_cast<float>(total_moves) / static_cast<float>(height)
        : 0.0f;
    {
        std::size_t total_all = 0, row_max = 0;
        std::size_t total_hb = 0, hb_max = 0;
        std::size_t total_vis = 0, vis_max = 0;
        for (auto& row : line_moves) {
            std::size_t rm = 0, hb = 0, vis = 0;
            bool past_line_gate = false;
            for (auto& op : row) {
                if (op.kind == ScapOpKind::kWait) {
                    past_line_gate = true;
                    continue;
                }
                ++rm;
                if (past_line_gate) ++vis; else ++hb;
            }
            total_all += rm;  if (rm  > row_max) row_max = rm;
            total_hb  += hb;  if (hb  > hb_max)  hb_max  = hb;
            total_vis += vis; if (vis > vis_max) vis_max = vis;
        }
        auto h = static_cast<float>(height ? height : 1);
        res.avg_total_moves_per_line   = static_cast<float>(total_all) / h;
        res.max_moves_per_line         = row_max;
        res.avg_hblank_moves_per_line  = static_cast<float>(total_hb)  / h;
        res.max_hblank_moves_per_line  = hb_max;
        res.avg_visible_moves_per_line = static_cast<float>(total_vis) / h;
        res.max_visible_moves_per_line = vis_max;
    }
    res.line_moves = std::move(line_moves);
    for (auto& p : preview.pixels()) p = palette::quantize_to_ocs(p);
    res.rendered = std::move(preview);
    return res;
}

// ---------------------------------------------------------------------------
// HAM6 + SCAP — v0 implementation
//
// HAM6 has the same 6-plane DMA pattern as EHB and DPF, so the
// kScap6bplEhb slot table (19 mid-line MOVE positions) transfers
// directly. We mid-line-swap the 16 BASE palette registers; HAM SET ops
// resolve against whichever strip palette is currently active, while
// MODIFY ops continue to mutate the rolling output colour irrespective
// of palette state.
//
// v0 simplifications (deliberate, see commit msg):
//   * Greedy single-pass strip swap planner: per-strip pixel histogram,
//     swap the K least-used base slots with the strip's most-frequent
//     RGB444-bucketed colours.
//   * No multi-pass joint refinement (EHB SCAP runs 6 passes).
//   * No cap-best wiring.
//   * Inline HAM op selector — keeps scap.cpp self-contained without
//     needing to expose ham.cpp's anonymous-namespace helpers.
// ---------------------------------------------------------------------------
namespace {

// [v0 inline HAM picker — superseded by ham::encode_ham_pixel which is
//  the same DP beam search used by the HAM6+CAP path. Kept commented
//  for reference; was 0.6 dB weaker than the production picker.]
#if 0
struct HamPickResult {
    std::uint8_t value;
    Color3f      out_lin;
    float        error;
};
HamPickResult pick_ham6_op(
    Color3f prev_lin,
    const color_space::OKLab& target_lab,
    std::span<const Color3f> base_pal,
    std::span<const color_space::OKLab> base_pal_lab) {
    HamPickResult best{0, prev_lin, std::numeric_limits<float>::max()};
    auto score = [&](const color_space::OKLab& lab) {
        float dL = target_lab.L - lab.L;
        float da = target_lab.a - lab.a;
        float db = target_lab.b - lab.b;
        return dL * dL + da * da + db * db;
    };
    // SET ops (control=00).
    for (std::size_t k = 0; k < base_pal.size() && k < 16; ++k) {
        float e = score(base_pal_lab[k]);
        if (e < best.error) {
            best = {static_cast<std::uint8_t>((0u << 4) | k),
                    base_pal[k], e};
        }
    }
    // MODIFY ops (control=01 blue, 10 red, 11 green).
    auto modify = [&](std::uint8_t ctrl, int channel) {
        for (int n = 0; n < 16; ++n) {
            float v = static_cast<float>(n * 17) / 255.0f;  // nibble→sRGB
            // Linearise the new channel; keep the other two from prev_lin.
            // Prev is already linear RGB; we need to re-quantise the
            // modified channel through sRGB → linear so the value
            // matches what the chip emits.
            Color3f new_lin = prev_lin;
            float v_lin = color_space::srgb_to_linear(v);
            if (channel == 0) new_lin.r = v_lin;
            else if (channel == 1) new_lin.g = v_lin;
            else                   new_lin.b = v_lin;
            auto new_lab = color_space::linear_to_oklab(new_lin);
            float e = score(new_lab);
            if (e < best.error) {
                best = {static_cast<std::uint8_t>((ctrl << 4) | n),
                        new_lin, e};
            }
        }
    };
    modify(0b01, 2);  // 01 = MODIFY blue
    modify(0b10, 0);  // 10 = MODIFY red
    modify(0b11, 1);  // 11 = MODIFY green
    return best;
}

#endif

}  // namespace

Result<ScapResult> encode_scap_ham6_ocs(const Image& image,
                                        int width_arg,
                                        int height_arg,
                                        bool reserve_color0,
                                        const dither::Settings& dither_settings,
                                        std::size_t copper_changes_override,
                                        int palette_diversity,
                                        std::function<void(float, std::string_view)>
                                            on_progress,
                                        int cap_spread_radius,
                                        float cap_spread_decay,
                                        bool cap_best,
                                        std::string_view cap_best_metric) {
    // --cap-best: 8 jitter seeds × 5 strengths × 4 diversities + 1
    // baseline = 161 trials. Same shape as EHB SCAP since HAM6's 16
    // base palette has comparable basin depth.
    if (cap_best) {
        auto metric = (cap_best_metric == "msssim")
            ? pipeline::CapBestMetric::msssim
            : pipeline::CapBestMetric::psnr;
        auto best = pipeline::cap_best_sweep<ScapResult>(
            image, dither_settings, palette_diversity, /*jitter_count=*/8,
            [&](const Image& jittered_in,
                const dither::Settings& d, int div) {
                return encode_scap_ham6_ocs(
                    jittered_in, width_arg, height_arg, reserve_color0,
                    d, copper_changes_override, div,
                    /*on_progress=*/{},
                    cap_spread_radius, cap_spread_decay,
                    /*cap_best=*/false, "psnr");
            },
            [](const ScapResult& r) -> const Image& { return r.rendered; },
            on_progress,
            /*jitter_amplitude=*/1.0f,
            metric);
        if (best.has_value()) return std::move(*best);
    }
    auto& table = kScap6bplEhb;  // HAM6 shares 6-plane DMA with EHB
    if (table.slots.empty()) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "SCAP HAM6 planner: kScap6bplEhb slot table is empty",
        }};
    }
    auto width = (width_arg > 0) ? static_cast<std::size_t>(width_arg)
                                  : image.width();
    auto height = (height_arg > 0) ? static_cast<std::size_t>(height_arg)
                                    : image.height();
    if (image.width() != width || image.height() != height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("SCAP HAM6 planner: image is {}x{} but caller asked "
                        "for {}x{} — resize before calling",
                        image.width(), image.height(), width, height),
        }};
    }

    constexpr std::size_t kBaseColors = 16;
    constexpr std::size_t kHblankCeiling = 14;   // MOVEs CAP can land
                                                  // in hblank.
    constexpr std::size_t kVisibleBudget = 18;   // MOVEs SCAP can land
                                                  // in the visible
                                                  // raster (kScap6bplEhb
                                                  // has 19 slots; cap
                                                  // at 18 mirrors EHB
                                                  // SCAP's design).
    // Hblank and visible-line are SEPARATE DMA windows — they don't
    // compete. CAP MOVEs (slot_index = -1) all land in hblank; SCAP
    // MOVEs (slot_index >= 0) land mid-line during the visible
    // raster. The combined per-row MOVE count is hblank_used +
    // visible_used, but the BUDGET is per-window. copper_changes_
    // override applies ONLY to the visible (SCAP) budget — caller
    // can throttle SCAP without crippling CAP.
    std::size_t cap_share = kHblankCeiling;
    std::size_t scap_share = (copper_changes_override > 0)
        ? std::min<std::size_t>(copper_changes_override, kVisibleBudget)
        : kVisibleBudget;

    // ---- 0. Pre-dither for HAM encoding -------------------------------
    // ham::encode_ham_copper internally pre-dithers when given an ED
    // dither_method, then runs DP on that dithered image. SCAP needs
    // to drive its OWN per-strip beam search on the SAME dithered
    // input — otherwise the CAP-planned palettes (derived from the
    // dithered image inside ham::encode_ham_copper) are mismatched
    // against the SCAP-encoded pixels (running on the raw image).
    // Solution: pre-dither once here, hand the dithered image to
    // ham::encode_ham_copper with dither=none so it doesn't dither
    // again, and feed the same dithered image to our per-strip DP.
    Image scap_input(width, height);
    if (dither::uses_error_diffusion(dither_settings.method)) {
        dither::Settings d{
            .method = dither_settings.method,
            .strength = dither_settings.strength,
            .error_clamp = dither_settings.error_clamp,
            .serpentine = true,
        };
        constexpr std::size_t kHam6DataBits = 4;
        dither::diffuse_raw_buffer(image, d,
            [&](const color_space::OKLab& target,
                std::size_t x, std::size_t y) -> dither::PickResult {
                auto adjusted = color_space::oklab_to_linear(target);
                adjusted.r = std::clamp(adjusted.r, 0.0f, 1.0f);
                adjusted.g = std::clamp(adjusted.g, 0.0f, 1.0f);
                adjusted.b = std::clamp(adjusted.b, 0.0f, 1.0f);
                auto srgb_adj = ham::linear_to_srgb8(adjusted);
                // HAM6 MODIFY precision = 4 bits → nibble replication.
                srgb_adj.r = static_cast<std::uint8_t>(
                    (srgb_adj.r >> (8 - kHam6DataBits)) * 17u);
                srgb_adj.g = static_cast<std::uint8_t>(
                    (srgb_adj.g >> (8 - kHam6DataBits)) * 17u);
                srgb_adj.b = static_cast<std::uint8_t>(
                    (srgb_adj.b >> (8 - kHam6DataBits)) * 17u);
                auto quantized = color_space::srgb_u8_to_linear(
                    srgb_adj.r, srgb_adj.g, srgb_adj.b);
                scap_input[x, y] = quantized;
                return {color_space::linear_to_oklab(quantized), 0.5f};
            });
    } else {
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
                scap_input[x, y] = image[x, y];
    }

    // ---- 1. Per-line CAP base palette (16 colours, evolving across rows).
    // Use ham::encode_ham_copper for the per-line CAP plan. We pre-
    // dithered above (when applicable), so disable dither here to
    // avoid double-application — the CAP planner sees the same image
    // as our per-strip DP.
    ham::HamOptions ham_opts;
    ham_opts.dither_method = dither::Method::none;  // pre-dithered above
    ham_opts.palette_diversity = palette_diversity;
    auto ham_cap_result = ham::encode_ham_copper(
        scap_input, amiga::Mode::ham6, amiga::Chipset::ocs, ham_opts,
        /*is_hires=*/false, cap_share);
    if (!ham_cap_result) return std::unexpected{ham_cap_result.error()};
    auto& cap_palettes = ham_cap_result->scanline_palettes;
    auto base_palette = ham_cap_result->base_palette;
    (void)cap_spread_radius;
    (void)cap_spread_decay;

    // Strip layout helpers (same shape as EHB SCAP).
    std::size_t num_strips = table.slots.size() + 1;
    auto strip_for_x = [&](std::size_t x) -> std::size_t {
        for (std::size_t s = 0; s < table.slots.size(); ++s) {
            if (x < static_cast<std::size_t>(table.slots[s].pixel_x))
                return s;
        }
        return table.slots.size();
    };
    auto strip_x_range = [&](std::size_t s)
        -> std::pair<std::size_t, std::size_t> {
        std::size_t lo = (s == 0) ? std::size_t{0}
            : std::min(width,
                static_cast<std::size_t>(table.slots[s - 1].pixel_x));
        std::size_t hi = (s < table.slots.size())
            ? std::min(width,
                static_cast<std::size_t>(table.slots[s].pixel_x))
            : width;
        return {lo, hi};
    };

    // Pre-compute pixel OKLab.
    std::vector<color_space::OKLab> img_lab(width * height);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x)
            img_lab[y * width + x] = color_space::linear_to_oklab(
                image[x, y]);
    }

    // ---- 2. Per-line HAM6+SCAP encoding -------------------------------
    constexpr int kVStart = 44;
    std::vector<std::uint8_t> ham_values(width * height, 0);
    std::vector<std::vector<ScapMove>> line_moves(height);
    std::vector<std::vector<Color3f>> scanline_palettes_full(height);
    Image preview(width, height);
    double total_error = 0.0;
    std::size_t total_moves = 0;

    if (on_progress) on_progress(0.0f, "encoding");
    for (std::size_t y = 0; y < height; ++y) {
        if (on_progress && (y & 0xF) == 0) {
            on_progress(static_cast<float>(y) / static_cast<float>(height),
                        "encoding");
        }
        int abs_vpos = static_cast<int>(y) + kVStart;
        auto vp = static_cast<std::uint8_t>(abs_vpos & 0xFF);

        // Strip working palette: starts as cap_palettes[y] (16 colours),
        // mutates as SCAP MOVEs land. The HAM op picker uses whichever
        // strip palette is active at the current pixel.
        std::vector<Color3f> strip_pal = cap_palettes[y];
        if (strip_pal.size() < kBaseColors) strip_pal.resize(kBaseColors);
        std::vector<color_space::OKLab> strip_pal_lab(kBaseColors);
        auto refresh_lab = [&]() {
            for (std::size_t k = 0; k < kBaseColors; ++k)
                strip_pal_lab[k] = color_space::linear_to_oklab(
                    strip_pal[k]);
        };
        refresh_lab();

        // Per-line CAP MOVEs (line entry palette). v0: assume the previous
        // line's hardware state is cap_palettes[y-1] (we don't track SCAP
        // mid-line swaps' carryover for v0 — every CAP transition emits a
        // MOVE, even ones that match prior swap state).
        std::vector<Color3f> prev_line_pal = (y == 0)
            ? base_palette : cap_palettes[y - 1];
        if (prev_line_pal.size() < kBaseColors)
            prev_line_pal.resize(kBaseColors);
        for (std::size_t k = 0; k < kBaseColors; ++k) {
            if (strip_pal[k].r != prev_line_pal[k].r ||
                strip_pal[k].g != prev_line_pal[k].g ||
                strip_pal[k].b != prev_line_pal[k].b) {
                line_moves[y].push_back(make_move(
                    static_cast<std::uint8_t>(k),
                    palette::linear_to_ocs(strip_pal[k]), -1));
            }
        }
        // Line gate WAIT.
        line_moves[y].push_back(make_wait(
            static_cast<std::uint8_t>(table.line_gate_hpos), vp, -1));

        // ---- 3. Per-strip cluster-math swap planner ---------------------
        // EHB-style scoring (no halfbrite): per strip, build per-slot
        // cluster stats over the strip's pixels (count + OKLab mean +
        // residual spread). Generate swap candidates from (a) every
        // pixel in the strip OCS-quantised + deduped on RGB444, plus
        // (b) the OKLab centroid of each non-empty cluster. For each
        // (slot, candidate) pair, score the swap by cluster-error
        // reduction and pick the largest reduction. Repeat per strip
        // until the SCAP budget is exhausted.
        std::size_t scap_budget = scap_share;
        std::size_t scap_used = 0;
        for (std::size_t s = 1; s < num_strips; ++s) {
            if (scap_used >= scap_budget) break;
            auto [x_lo, x_hi] = strip_x_range(s);
            if (x_lo >= x_hi) continue;

            // Per-slot cluster: pixel assignments + cumulative LAB sum
            // for centroid computation, plus per-pixel residual squared
            // distance for the spread term.
            struct Clust {
                double L = 0, a = 0, b = 0, spread = 0;
                std::uint32_t count = 0;
            };
            std::array<Clust, kBaseColors> clust{};
            // First pass: nearest-slot assignment + per-cluster sum.
            std::vector<std::uint8_t> assign(x_hi - x_lo, 0);
            for (std::size_t x = x_lo; x < x_hi; ++x) {
                auto& lab = img_lab[y * width + x];
                float best_d = std::numeric_limits<float>::max();
                std::size_t best_k = 0;
                for (std::size_t k = 0; k < kBaseColors; ++k) {
                    float dL = lab.L - strip_pal_lab[k].L;
                    float da = lab.a - strip_pal_lab[k].a;
                    float db = lab.b - strip_pal_lab[k].b;
                    float d = dL * dL + da * da + db * db;
                    if (d < best_d) { best_d = d; best_k = k; }
                }
                assign[x - x_lo] = static_cast<std::uint8_t>(best_k);
                auto& c = clust[best_k];
                c.L += static_cast<double>(lab.L);
                c.a += static_cast<double>(lab.a);
                c.b += static_cast<double>(lab.b);
                c.spread += static_cast<double>(best_d);
                ++c.count;
            }

            // Compute centroid means for non-empty clusters.
            std::array<color_space::OKLab, kBaseColors> centroid{};
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (clust[k].count > 0) {
                    centroid[k] = color_space::OKLab{
                        static_cast<float>(clust[k].L /
                            static_cast<double>(clust[k].count)),
                        static_cast<float>(clust[k].a /
                            static_cast<double>(clust[k].count)),
                        static_cast<float>(clust[k].b /
                            static_cast<double>(clust[k].count)),
                    };
                }
            }

            // Build candidate set: cluster centroids + every distinct
            // strip pixel (OCS-bucketed, dedup via 12-bit key).
            std::vector<Color3f> cands;
            std::vector<color_space::OKLab> cands_lab;
            std::array<bool, 4096> seen{};
            auto add_cand = [&](Color3f c) {
                auto cs = palette::quantize_to_ocs(c);
                auto key = static_cast<std::size_t>(
                    palette::linear_to_ocs(cs) & 0xFFFu);
                if (!seen[key]) {
                    seen[key] = true;
                    cands.push_back(cs);
                    cands_lab.push_back(color_space::linear_to_oklab(cs));
                }
            };
            for (std::size_t x = x_lo; x < x_hi; ++x) add_cand(image[x, y]);
            for (std::size_t k = 0; k < kBaseColors; ++k) {
                if (clust[k].count > 0) {
                    add_cand(color_space::oklab_to_linear(centroid[k])
                                 .clamped());
                }
            }

            // Score every (slot, candidate) pair: cluster error change
            // when slot k → cand. The OTHER slots' clusters are
            // unaffected; only cluster k's distance-from-palette term
            // changes. spread term is per-pixel residual which doesn't
            // change under a swap.
            std::size_t best_slot = 0;
            std::size_t best_cand = 0;
            double best_reduction = 0.0;
            std::size_t k_min = reserve_color0 ? 1u : 0u;
            for (std::size_t k = k_min; k < kBaseColors; ++k) {
                if (clust[k].count == 0) continue;
                // Old error (slot k holding strip_pal[k]):
                float dL_old = centroid[k].L - strip_pal_lab[k].L;
                float da_old = centroid[k].a - strip_pal_lab[k].a;
                float db_old = centroid[k].b - strip_pal_lab[k].b;
                double old_e = static_cast<double>(clust[k].count) *
                    static_cast<double>(
                        dL_old * dL_old + da_old * da_old + db_old * db_old);
                for (std::size_t ci = 0; ci < cands.size(); ++ci) {
                    auto& cl = cands_lab[ci];
                    float dL = centroid[k].L - cl.L;
                    float da = centroid[k].a - cl.a;
                    float db = centroid[k].b - cl.b;
                    double new_e = static_cast<double>(clust[k].count) *
                        static_cast<double>(
                            dL * dL + da * da + db * db);
                    double red = old_e - new_e;
                    if (red > best_reduction) {
                        best_reduction = red;
                        best_slot = k;
                        best_cand = ci;
                    }
                }
            }
            if (best_reduction <= 0.0) continue;

            // Apply swap.
            strip_pal[best_slot] = cands[best_cand];
            strip_pal_lab[best_slot] = cands_lab[best_cand];
            auto rgb_ocs = palette::linear_to_ocs(cands[best_cand]);
            line_moves[y].push_back(make_move(
                static_cast<std::uint8_t>(best_slot),
                rgb_ocs,
                static_cast<int>(s - 1)));
            ++scap_used;
            (void)kHblankCeiling;
        }
        scanline_palettes_full[y] = strip_pal;  // end-of-line state
        total_moves += line_moves[y].size();

        // ---- 4. Per-strip row-level DP beam-search HAM encoding -------
        // Build per-strip palette state by replaying the SCAP MOVEs we
        // just emitted for this line. strip 0 starts from the line-
        // entry palette (cap_palettes[y]); each subsequent strip
        // applies the MOVEs whose slot_index matches s-1.
        constexpr std::size_t kBeamWidth = 48;
        constexpr std::size_t kTripleBeam = 16;

        std::vector<std::uint16_t> strip_idx(width);
        for (std::size_t x = 0; x < width; ++x)
            strip_idx[x] = static_cast<std::uint16_t>(strip_for_x(x));

        std::vector<Color3f> row_pixels(width);
        for (std::size_t x = 0; x < width; ++x)
            row_pixels[x] = scap_input[x, y];
        auto row_span = std::span<const Color3f>(row_pixels.data(), width);
        auto idx_span = std::span<const std::uint16_t>(strip_idx.data(),
                                                        width);

        // Build strip palette state from cap_palettes[y] + replaying any
        // line_moves whose slot_index matches s-1.
        auto build_strips = [&](bool apply_swaps,
                                std::vector<std::vector<Color3f>>& strip_pals,
                                std::vector<std::vector<ham::SRGBColor>>& strip_srgbs,
                                std::vector<ham::HamPrecomp>& strip_pres,
                                std::vector<std::span<const ham::SRGBColor>>&
                                    strip_srgb_spans) {
            strip_pals.assign(num_strips, {});
            strip_srgbs.assign(num_strips, {});
            strip_pres.clear();
            strip_pres.reserve(num_strips);
            strip_srgb_spans.clear();
            strip_srgb_spans.reserve(num_strips);
            std::vector<Color3f> running_pal = cap_palettes[y];
            if (running_pal.size() < kBaseColors)
                running_pal.resize(kBaseColors);
            for (std::size_t s = 0; s < num_strips; ++s) {
                if (apply_swaps && s > 0) {
                    for (auto& m : line_moves[y]) {
                        if (m.kind == ScapOpKind::kMove &&
                            m.slot_index == static_cast<int>(s - 1) &&
                            m.reg < kBaseColors) {
                            auto rgb12 = m.rgb_ocs & 0xFFFu;
                            float r = static_cast<float>((rgb12 >> 8) & 0xF) / 15.0f;
                            float g = static_cast<float>((rgb12 >> 4) & 0xF) / 15.0f;
                            float bv = static_cast<float>(rgb12 & 0xF) / 15.0f;
                            running_pal[m.reg] = color_space::srgb_to_linear(
                                Color3f{r, g, bv});
                        }
                    }
                }
                strip_pals[s] = running_pal;
                strip_srgbs[s].resize(kBaseColors);
                for (std::size_t k = 0; k < kBaseColors; ++k)
                    strip_srgbs[s][k] = ham::linear_to_srgb8(running_pal[k]);
                strip_pres.emplace_back(
                    std::span<const Color3f>(strip_pals[s].data(),
                                             kBaseColors),
                    /*data_bits=*/4);
                strip_srgb_spans.emplace_back(strip_srgbs[s].data(),
                                              kBaseColors);
            }
        };

        // Encode given a built strip layout. Returns the encoded value
        // sequence + cumulative OKLab² error.
        auto encode_with = [&](
            const std::vector<std::vector<ham::SRGBColor>>& strip_srgbs,
            std::span<const ham::HamPrecomp> pres_span,
            std::span<const std::span<const ham::SRGBColor>> srgbs_span)
            -> ham::ScanlineResult {
            ham::SRGBColor start = strip_srgbs[0].empty()
                ? ham::SRGBColor{0, 0, 0} : strip_srgbs[0][0];
            auto sl = ham::encode_scanline_dp_per_strip(
                row_span, start, pres_span, srgbs_span,
                idx_span, kBeamWidth);
            ham::refine_scanline_triple_per_strip(
                sl.values, row_span, start, pres_span, srgbs_span,
                idx_span, kTripleBeam);
            return sl;
        };

        // Build with swaps + encode.
        std::vector<std::vector<Color3f>> strip_pals;
        std::vector<std::vector<ham::SRGBColor>> strip_srgbs;
        std::vector<ham::HamPrecomp> strip_pres;
        std::vector<std::span<const ham::SRGBColor>> strip_srgb_spans;
        build_strips(/*apply_swaps=*/true, strip_pals, strip_srgbs,
                     strip_pres, strip_srgb_spans);
        auto pres_span = std::span<const ham::HamPrecomp>(
            strip_pres.data(), strip_pres.size());
        auto srgbs_span = std::span<const std::span<const ham::SRGBColor>>(
            strip_srgb_spans.data(), strip_srgb_spans.size());
        auto sl = encode_with(strip_srgbs, pres_span, srgbs_span);

        // Quality gate: if the cluster planner emitted any SCAP swaps,
        // also encode WITHOUT them (every strip uses cap_palettes[y]
        // unchanged) and keep whichever beats the other. The cluster
        // planner uses nearest-color (SET-only) scoring which sometimes
        // recommends swaps that hurt HAM-MODIFY-aware encoding (most
        // dramatic on row-distinct synthetic images: ocs_4096.png went
        // 91 → 62 dB before this gate).
        bool has_scap_swaps = false;
        for (auto& m : line_moves[y])
            if (m.kind == ScapOpKind::kMove && m.slot_index >= 0) {
                has_scap_swaps = true; break;
            }
        if (has_scap_swaps) {
            std::vector<std::vector<Color3f>> ns_pals;
            std::vector<std::vector<ham::SRGBColor>> ns_srgbs;
            std::vector<ham::HamPrecomp> ns_pres;
            std::vector<std::span<const ham::SRGBColor>> ns_srgb_spans;
            build_strips(/*apply_swaps=*/false, ns_pals, ns_srgbs,
                         ns_pres, ns_srgb_spans);
            auto ns_pres_span = std::span<const ham::HamPrecomp>(
                ns_pres.data(), ns_pres.size());
            auto ns_srgbs_span =
                std::span<const std::span<const ham::SRGBColor>>(
                    ns_srgb_spans.data(), ns_srgb_spans.size());
            auto ns_sl = encode_with(ns_srgbs, ns_pres_span, ns_srgbs_span);
            if (ns_sl.error < sl.error) {
                sl = std::move(ns_sl);
                strip_pals = std::move(ns_pals);
                strip_srgbs = std::move(ns_srgbs);
                strip_pres = std::move(ns_pres);
                strip_srgb_spans = std::move(ns_srgb_spans);
                pres_span = std::span<const ham::HamPrecomp>(
                    strip_pres.data(), strip_pres.size());
                srgbs_span =
                    std::span<const std::span<const ham::SRGBColor>>(
                        strip_srgb_spans.data(),
                        strip_srgb_spans.size());
                // Drop SCAP swaps from line_moves; keep CAP entries
                // (slot_index = -1) and the line-gate WAIT.
                auto& lm = line_moves[y];
                lm.erase(std::remove_if(lm.begin(), lm.end(),
                    [](const ScapMove& m) {
                        return m.kind == ScapOpKind::kMove &&
                               m.slot_index >= 0;
                    }), lm.end());
                scanline_palettes_full[y] = strip_pals.back();
            }
        }
        ham::SRGBColor start = strip_srgbs[0].empty()
            ? ham::SRGBColor{0, 0, 0} : strip_srgbs[0][0];
        // Render the per-pixel preview by replaying the encoded values
        // through the strip's palette + HAM rolling state.
        ham::SRGBColor prev = start;
        for (std::size_t x = 0; x < width; ++x) {
            ham_values[y * width + x] = sl.values[x];
            std::size_t s = strip_idx[x];
            std::uint8_t v = sl.values[x];
            std::uint8_t ctrl = static_cast<std::uint8_t>(v >> 4);
            std::uint8_t data = static_cast<std::uint8_t>(v & 0xF);
            ham::SRGBColor out;
            if (ctrl == 0u) {
                out = strip_srgbs[s][data];
            } else {
                std::uint8_t expanded =
                    static_cast<std::uint8_t>(data * 17u);
                out = prev;
                if (ctrl == 0b01) out.b = expanded;
                else if (ctrl == 0b10) out.r = expanded;
                else if (ctrl == 0b11) out.g = expanded;
            }
            preview[x, y] = color_space::srgb_u8_to_linear(
                out.r, out.g, out.b);
            prev = out;
        }
        total_error += static_cast<double>(sl.error);
    }

    // ---- 5. Pack 6-plane bitplane data --------------------------------
    auto planes = bitplane::encode(ham_values, width, height, 6);
    if (!planes) return std::unexpected{planes.error()};

    // ---- 6. Assemble result ------------------------------------------
    ScapResult res;
    res.planes = *std::move(planes);
    res.palette = base_palette;
    res.line_moves = std::move(line_moves);
    res.rendered = std::move(preview);
    res.total_error = static_cast<float>(total_error);
    res.avg_changes_per_line = static_cast<float>(
        ham_cap_result->changes_per_line);
    auto h_div = static_cast<float>(height ? height : 1);
    res.avg_total_moves_per_line = static_cast<float>(total_moves) / h_div;
    res.max_moves_per_line = 1 + table.slots.size();
    res.avg_hblank_moves_per_line = 1.0f;
    res.max_hblank_moves_per_line = 1;
    res.avg_visible_moves_per_line = static_cast<float>(table.slots.size());
    res.max_visible_moves_per_line = table.slots.size();
    if (on_progress) on_progress(1.0f, "done");
    for (auto& p : res.rendered.pixels()) p = palette::quantize_to_ocs(p);
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
