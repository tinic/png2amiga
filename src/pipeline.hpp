#pragma once

#include "amiga.hpp"
#include "cheader.hpp"

#include <optional>
#include <string>
#include <string_view>

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

}  // namespace png2amiga::pipeline
