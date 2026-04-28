#pragma once

#include "cheader.hpp"

#include <string>
#include <string_view>

namespace png2amiga::pipeline {

// Filename stem -> C identifier (lowercased, non-alphanumeric -> '_',
// leading digit prefixed with '_'). Empty result becomes "image".
std::string derive_symbol_name(std::string_view path);

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
