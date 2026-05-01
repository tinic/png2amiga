#pragma once

// C64 .prg / .koa / .hir output. Each PRG bundles a small 6502 displayer
// (assembled, embedded as a byte array in c64_displayers.hpp) with the
// encoded image bytes from c64::EncodeResult, producing a runnable
// program for real hardware / VICE / x64sc.
//
// Ported from png2c64's prg.cpp/hpp (commit 1e26df8 added FLI/AFLI;
// PETSCII + multicolor + hires were the original set). Charset PRG is
// not yet ported — needs mc1 / mc2 globals surfaced in EncodeResult
// first.

#include "amiga.hpp"
#include "c64.hpp"
#include "types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace png2amiga::c64::prg {

struct PrgData {
    std::vector<std::uint8_t> bytes;  // load addr (LE16) + program data
};

// Multicolor (40×25 cells) → Koala-format displayer .prg.
Result<PrgData> koala(const EncodeResult& result);

// Hires (40×25 cells) → Art-Studio-format displayer .prg.
Result<PrgData> hires_bitmap(const EncodeResult& result);

// FLI (40×25 cells) → FLI-format displayer .prg.
Result<PrgData> fli(const EncodeResult& result);

// AFLI (40×25 cells) → AFLI-format displayer .prg.
Result<PrgData> afli(const EncodeResult& result);

// PETSCII (40×25 cells) → PETSCII text-mode displayer .prg.
Result<PrgData> petscii_text(const EncodeResult& result);

// Raw Koala Paint format (.koa, no displayer): $6000 + bitmap + screen
// + d800 + bg.
Result<PrgData> koala_raw(const EncodeResult& result);

// Raw Art Studio format (.hir, no displayer): $2000 + bitmap + screen.
Result<PrgData> hires_raw(const EncodeResult& result);

// Auto-dispatch from c64 mode.
Result<PrgData> from_mode(amiga::Mode mode, const EncodeResult& result);

// Reconstruct an EncodeResult (bitmap + screen + color + bg) from the
// concatenated raw_frame the api/pipeline layer stores in PipelineResult.
// Used by convert_prg to bridge from the generic raw_frame back to the
// shape c64::prg writers expect. Sizes per mode are baked in here —
// keep in sync with api.cpp's c64 packing.
Result<EncodeResult> unpack_pipeline_raw(
    amiga::Mode mode,
    const std::vector<std::uint8_t>& raw_frame,
    std::uint8_t bg_color);

// Write PRG bytes to a file.
Result<void> write(std::string_view path, const PrgData& data);

}  // namespace png2amiga::c64::prg
