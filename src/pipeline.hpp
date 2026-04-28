#pragma once

#include "amiga.hpp"
#include "bitplane.hpp"
#include "cheader.hpp"
#include "copper.hpp"
#include "scap.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
