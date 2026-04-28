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
#include <string>
#include <string_view>
#include <vector>

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

}  // namespace png2amiga::pipeline
