#pragma once

// CGA composite NTSC simulator + encoder.
//
// Ports Andrew Jenner ("reenigne")'s sampled-chroma-multiplexer model — the
// same algorithm DOSBox / 86Box / MartyPC use for composite-output
// emulation. Each output pixel depends on a sliding 10-pixel window of
// 2bpp / 4bpp RGBI input, so the artifact colours (purple from
// alternating-blue patterns, etc.) emerge naturally instead of being
// faked with a fixed 16-colour palette.
//
// Two entry points:
//   decode_line(in, out, params)  — turn a row of 4-bit RGBI pixels into
//                                   the same sRGB the user would see on
//                                   an NTSC TV (or MartyPC).
//   encode_line(src, out, params) — best-effort: pick the 4-bit RGBI
//                                   pixel sequence whose decoder output
//                                   most closely matches `src` in OKLab.

#include "color_space.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::cga_composite {

// Parameters for the NTSC composite simulator. We pin the analog-stage
// knobs (hue/sat/contrast/luma/sharpness) at MartyPC's "stock CGA
// monitor" preset and only expose the one knob that actually changes
// the artifact-colour table: which CGA card revision generated the
// signal. Pre-1983 IBM 5150 vs the revised 1983+ card use different
// chroma-burst phase and the new card adds a per-channel intensity
// term — both visibly change the resulting colour set.
struct Params {
    bool new_cga = false;          // false = 1981 IBM 5150, true = 1983+ revision
    std::uint8_t cgamode = 0x0A;   // 320×200 graphics, colour-burst on
};

// Precomputed 1024-entry composite signal table + the rotated YIQ→RGB
// matrix coefficients, derived from Params + cgamode. Build once per
// frame; reuse across all rows.
struct Context {
    std::array<std::int32_t, 1024> composite_table{};
    std::int32_t video_ri = 0, video_rq = 0;
    std::int32_t video_gi = 0, video_gq = 0;
    std::int32_t video_bi = 0, video_bq = 0;
    std::int32_t video_sharpness = 0;
    std::uint8_t cgamode = 0;
    bool bw = false;
};

// Build Context from params. Expensive (1024-entry LUT fill + trig +
// matrix); call once per change of params, not per row.
Context make_context(const Params& p);

// Decode one row of 4-bit RGBI pixels (one byte per pixel, low nibble
// used) into linear Color3f via NTSC chroma demodulation. `out` must
// have at least `in.size()` entries.
void decode_line(std::span<const std::uint8_t> in,
                 std::span<Color3f> out,
                 const Context& ctx,
                 std::uint8_t border = 0);

// Pick the 4-bit RGBI pixel sequence whose decode_line() result best
// matches `src` in OKLab. `out` and `src` are width-N row data. Beam
// width is internal and fixed at 64 (a 320-wide × 16-candidate sweep
// on Kodak-24 showed doubling barely moves S2).
void encode_line(std::span<const Color3f> src,
                 std::span<std::uint8_t> out,  // 0..15 per pixel
                 const Context& ctx);

}  // namespace png2amiga::cga_composite
