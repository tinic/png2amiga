#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "cheader.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "scap.hpp"
#include "types.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace png2amiga::api { struct Options; }

namespace png2amiga::pipeline {

// Filename stem -> C identifier (lowercased, non-alphanumeric -> '_',
// leading digit prefixed with '_'). Empty result becomes "image".
std::string derive_symbol_name(std::string_view path);

// Canonical chipset resolution. Modes that need >6 bitplanes force AGA;
// otherwise the user's request wins (explicit Chipset::ocs is preserved),
// and an empty/unrecognised request defaults to OCS. Used by both CLI
// (Config::chipset is std::optional<Chipset>) and WASM (Options::chipset
// is a string parsed from JS).
amiga::Chipset resolve_chipset(std::optional<amiga::Chipset> requested,
                               amiga::Mode mode);
amiga::Chipset resolve_chipset(std::string_view requested,
                               amiga::Mode mode);

// Source-of-truth for the "core" CHeaderOptions fields. Each output site
// fills this from its current context (Config / api::Options / pipeline
// state) and calls make_ch_opts() to produce a populated options struct.
// Per-feature attachments (CAP scanline data, SCAP line moves, batch
// frames) are still set on the returned struct by the caller — those
// vary too much per-site to live here.
struct ChOptsBase {
    std::string_view output_path = {};     // for symbol derivation when override empty
    std::string_view symbol_override = {}; // empty => derive from output_path
    bool hires = false;
    bool interlace = false;
    bool aga = false;
    bool fade_in = false;
    bool dpf = false;
    bool interleaved = false;
};

cheader::CHeaderOptions make_ch_opts(const ChOptsBase& base);

// Canonical pipeline result. Filled in by run_pipeline() (currently
// defined inside src/api.cpp; will migrate to pipeline.cpp once its
// internal helpers are un-anon-namespaced — see REFACTOR_PLAN.md). Both
// the WASM converters and the CLI output dispatchers consume this:
// bitplane data, derived palette, mode-specific raw hardware bytes,
// copper / SCAP per-line state, and a rendered preview.
struct PipelineResult {
    // Canonical preview image — what the chip would display, post-encoder
    // and post-CAP-cascade. Always populated by run_pipeline via
    // pipeline::render_preview(); downstream consumers (convert_*, web
    // frontend, future main.cpp branches) read this rather than
    // re-rendering. Centralising the render here means preview
    // correctness fixes (e.g. the deferred OCS preview-vs-chip gradient
    // bug — REFACTOR_PLAN.md target #3 follow-up) only need to land in
    // one place.
    Image rendered;
    bitplane::BitplaneData planes;
    std::vector<Color3f> palette;
    amiga::Mode mode{};
    bool hires = false;
    bool interlace = false;

    // Per-pixel palette indices, populated only for modes with a single
    // global palette (lores/hires/EHB without copper). Empty for HAM and
    // copper modes where the palette varies. Used by the PNG encoder to
    // emit a palettized PNG-8 instead of full RGB.
    std::vector<std::uint8_t> indices;

    // Copper / palette modes
    bool copper = false;
    bool aga = false;
    bool dpf = false;
    bool scap = false;
    std::vector<std::vector<Color3f>> scanline_palettes;
    std::vector<std::vector<copper::CopperChange>> scanline_changes;
    // Populated by the SCAP planner. Each inner vector is the raw
    // WAIT/MOVE op stream for one image scanline — fed verbatim to
    // cheader::CHeaderOptions::scap_line_moves.
    std::vector<std::vector<scap::ScapMove>> scap_line_moves;
    std::size_t copper_num_colors{};
    std::size_t changes_per_line{};
    std::size_t max_moves_per_line{};   // worst-case copper MOVEs/line for chip-RAM sizing

    // SCAP-only stats. Populated when scap=true; zero otherwise. Match
    // the same fields on scap::ScapResult so the CLI / web UI can show
    // the planner's per-line move budget breakdown without re-running
    // the encoder.
    float scap_avg_total_moves_per_line{};
    float scap_avg_hblank_moves_per_line{};
    std::size_t scap_max_hblank_moves_per_line{};
    float scap_avg_visible_moves_per_line{};
    std::size_t scap_max_visible_moves_per_line{};
    std::size_t scap_slot_count{};

    // Set after construction.
    bool has_transparency = false;
    std::vector<bool> transparency_mask;
    float copper_changes{};
    float quant_error{};
    float psnr{};

    // Mode-specific raw hardware bytes — used by DOS modes that don't flow
    // through the bitplane encoder (chunky VGA indices, CGA-banked planar
    // frame, composite pair-packed frame, text-mode char+attr pairs).
    std::vector<std::uint8_t> raw_frame;

    // Text-mode-graphics only (ega_text / cga_text). Needed by the DJGPP
    // viewer generator to build the shifted custom font and program the
    // CRTC max-scan-line register; zero for all other modes.
    std::uint8_t text_scanline_offset = 0;
    std::uint8_t text_cell_height = 0;

    // CGA 320x200 (mode 4): byte the DJGPP viewer must write to port 0x3D9
    // so the hardware matches the auto-picked palette+bg variant. 0xFF
    // means "not a CGA-320 run" (viewer falls back to its default 0x30).
    std::uint8_t cga_mode_ctrl2 = 0xFF;

    // Tile-dedup stats — set by Genesis (4bpp 8×8 tiles, 32 B each) and
    // SNES Mode 7 (8bpp 8×8 tiles, 64 B each). 0 = not a tiled run.
    std::size_t genesis_unique_tiles = 0;
    std::size_t genesis_total_cells = 0;
    std::size_t tile_data_bytes = 0;  // unique_tiles × bytes-per-tile
    // Genesis split byte streams for SGDK header generation. raw_frame
    // remains the single concatenated stream for .bin output.
    std::vector<std::uint8_t>  genesis_tile_bytes;     // unique_tiles × 32
    std::vector<std::uint16_t> genesis_tilemap_cells;  // total_cells
    std::vector<std::uint16_t> genesis_palette_words;  // 64 BGR333 words

    // Fill quant_error + psnr from the source image and the rendered
    // preview. Replaces the same 4 lines repeated at every mode branch.
    void finalize_psnr(const Image& src, float total_error);
};

// Single per-mode preview-render dispatcher. Picks the right back-end:
//   - HAM (no scanline palettes)        → ham::render_ham
//   - HAM + CAP (scanline_palettes set) → ham::render_ham_copper
//   - indexed + CAP (scanline_palettes) → copper::render_copper_capped
//                                         (top-K diff cascade — matches
//                                         the cheader-side lace_rebuild
//                                         so the preview tracks what
//                                         hardware actually displays)
//   - indexed plain                     → bitplane::render
// Used by main.cpp's CLI dispatchers and api.cpp's run_pipeline so all
// preview-correctness fixes land in one place.
//
// Known per-back-end behavioural divergences preserved by this
// dispatcher (don't unify silently — each was an explicit choice):
//   - chipset / OCS quantization: only render_copper_capped snaps
//     output pixels to the 12-bit OCS gamut. bitplane::render and the
//     two HAM renderers don't — their callers rely on the supplied
//     palette already being mode-quantized (or, for HAM, on the modify
//     ops being intrinsically lossy).
//   - is_lace: only render_copper_capped is lace-aware (each field
//     replays its own diff cascade via cheader's lace_rebuild). The
//     other renderers ignore the flag — non-CAP outputs are
//     field-agnostic.
//   - data_bits: HAM-only; computed from planes.depth - 2 internally.
// Callers must still pass is_lace and chipset; they're forwarded only
// where each back-end honours them. The deferred OCS preview-vs-chip
// gradient bug (REFACTOR_PLAN.md target #3 step 5) lives entirely in
// the render_copper_capped branch.
Result<Image> render_preview(
    const bitplane::BitplaneData& planes,
    std::span<const Color3f> base_palette,
    bool is_ham,
    bool is_lace,
    amiga::Chipset chipset,
    const std::vector<std::vector<Color3f>>* scanline_palettes = nullptr,
    std::size_t cap_changes_per_line = 0);

// Build a deterministically jittered copy of `source` (per-pixel
// hash-based perturbation, ±0.5*amplitude/255 per channel). Used by
// cap_best_sweep to give an encoder a different median-cut basin per
// trial. amplitude=1.0 = ±1/255 nudge (default; right for OCS where
// the discrete 12-bit gamut means small input nudges produce
// meaningful palette divergence). amplitude=0.4 = ±0.4/255 (right for
// AGA where continuous-RGB median-cut already gives natural variation
// and big nudges can drift the picked palette far enough to introduce
// per-line swap shimmer the rendered-preview PSNR doesn't capture).
Image jitter_image(const Image& source, std::uint32_t seed,
                   float amplitude = 1.0f);

// Run body(i) for i in [0, n) — parallel-dispatched across
// hardware_concurrency() jthreads on native, sequential under WASM.
// Used by cap_best_sweep but generic; any caller with N independent
// units of work can use it.
void parallel_for(std::size_t n,
                  std::function<void(std::size_t)> body);

// MS-SSIM (multi-scale Structural Similarity Index) between two
// linear-RGB images, computed on Rec. 709 luminance Y. Returns a value
// in (0, 1] — higher = more similar; 1.0 = identical inputs. Five
// scales with standard Wang et al. 2003 weights {0.0448, 0.2856,
// 0.3001, 0.2363, 0.1333}; per-scale SSIM uses an 11×11 Gaussian
// window (σ=1.5).
//
// Used by cap_best_sweep as a more perceptual ranking metric than
// PSNR. Captures local structure changes (banding, contour breakup,
// per-line swap shimmer) that pixel-MSE PSNR averages away — the
// horizontal-banding failure mode --cap especially produces in tight
// palettes.
//
// Both images must have identical (width, height); a/b sized at
// least width*height. Returns 0 on shape mismatch.
float compute_msssim(std::span<const Color3f> a,
                     std::span<const Color3f> b,
                     std::size_t width,
                     std::size_t height);

// Ranking metric for cap_best_sweep. msssim (default) catches local
// structural differences (banding, swap shimmer); psnr is the previous
// pure pixel-MSE rank — left as an option so the user can flip and
// compare on a per-image basis via --cap-best-metric.
enum class CapBestMetric { msssim, psnr };

// Multi-restart parallel sweep for any --cap-best CAP-aware encoder.
// Sweeps:
//   - dither_strength: 5 multipliers (0.7, 0.85, 1.0, 1.15, 1.3)
//   - palette_diversity: 4 values when caller's base is 0; otherwise
//     pinned to caller's value (user override always wins)
//   - pre-image jitter: `jitter_count` deterministic hash-based variants
//     of the source (the strongest knob — feeds the encoder's
//     median-cut a different cluster basin per trial)
// Plus 1 baseline trial (caller's exact base_settings + base_diversity
// against the unjittered source) so the multi-restart can never lose
// ground. Iter 0 = baseline.
//
// EncodeFn signature:
//   Result<T>(const Image& trial_input,
//             const dither::Settings& trial_dither,
//             int trial_diversity)
// RenderedFn signature:
//   const Image& (const T& result)
//
// Ranks by rendered-preview PSNR vs the ORIGINAL source — never the
// jittered variant — and returns the highest-PSNR T (or std::nullopt
// if every trial failed). Caller picks jitter_count: SCAP DPF uses
// 24 (8-colour PF2 palette is highly basin-sensitive), SCAP EHB and
// plain CAP use 8 (32-colour and 16-colour palettes have shallower
// basins). User explicitly OK'd unbounded compute on cap_best, so the
// large trial count (5×4×N + 1) is a feature.
template <typename T, typename EncodeFn, typename RenderedFn>
std::optional<T> cap_best_sweep(
    const Image& source,
    const dither::Settings& base_settings,
    int base_diversity,
    int jitter_count,
    EncodeFn encode_fn,
    RenderedFn rendered_fn,
    const std::function<void(float, std::string_view)>& on_progress,
    float jitter_amplitude = 1.0f,
    CapBestMetric metric = CapBestMetric::msssim) {
    struct Trial {
        dither::Settings settings;
        int diversity;
        int jitter_seed;
    };
    std::vector<Trial> trials;
    trials.push_back({base_settings, base_diversity, 0});  // baseline
    const float strengths[] = { 0.7f, 0.85f, 1.0f, 1.15f, 1.3f };
    const int diversities[] = { 0, 1, 2, 3 };
    for (auto s : strengths) {
        for (auto div : diversities) {
            for (int js = 0; js < jitter_count; ++js) {
                auto d = base_settings;
                d.strength = std::clamp(
                    base_settings.strength * s, 0.0f, 2.0f);
                int retry_div = (base_diversity > 0)
                    ? base_diversity : div;
                trials.push_back({d, retry_div, js});
            }
        }
    }

    std::vector<Image> jittered(static_cast<std::size_t>(jitter_count));
    for (int js = 1; js < jitter_count; ++js) {
        jittered[static_cast<std::size_t>(js)] =
            jitter_image(source, static_cast<std::uint32_t>(js),
                         jitter_amplitude);
    }

    std::optional<T> best;
    float best_psnr = -1.0f;
    std::mutex best_mu;
    std::atomic<std::size_t> done{0};
    auto total = trials.size();
    parallel_for(total, [&](std::size_t i) {
        const auto& t = trials[i];
        const Image& trial_input = (t.jitter_seed == 0)
            ? source
            : jittered[static_cast<std::size_t>(t.jitter_seed)];
        auto retry = encode_fn(trial_input, t.settings, t.diversity);
        auto n_done = done.fetch_add(1) + 1;
        if (on_progress) {
            on_progress(static_cast<float>(n_done) /
                        static_cast<float>(total),
                        "cap-best");
        }
        if (!retry) return;
        const Image& rendered = rendered_fn(*retry);
        float score;
        if (metric == CapBestMetric::psnr) {
            score = color_space::compute_psnr_blurred(
                source.pixels(), rendered.pixels(),
                source.width(), source.height());
        } else {
            // MS-SSIM beats PSNR for ranking on banding/shimmer cases.
            score = compute_msssim(
                source.pixels(), rendered.pixels(),
                source.width(), source.height());
        }
        std::lock_guard lk(best_mu);
        if (!best.has_value() || score > best_psnr) {
            best = std::move(*retry);
            best_psnr = score;
        }
    });
    if (on_progress) on_progress(1.0f, "done");
    return best;
}

// Run the full preprocessing → quantize → dither → encode pipeline against
// an in-memory image (PNG/JPEG/WebP autodetected). Single entry point
// shared by the WASM bindings (api.cpp's convert_*) and — once
// REFACTOR_PLAN.md step 3 lands — the CLI dispatch in main.cpp. The
// implementation currently lives in src/api.cpp; this declaration is the
// canonical surface.
Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const api::Options& options);

}  // namespace png2amiga::pipeline
