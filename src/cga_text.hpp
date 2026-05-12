#pragma once

#include "amiga.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::cga_text {

// Progress callback. fraction ∈ [0, 1]; stage is a short label
// ("cga-text" during the per-cell brute force, "done" on terminal
// tick). Pass `{}` / nullptr to silence.
using ProgressCb =
    std::function<void(float fraction, std::string_view stage_name)>;

// Per-cell error metric driving the brute-force glyph + (fg, bg) search.
//
//   mse  — sum of squared OKLab distances per sub-pixel. Optimized in
//          parallel via the sum-decomposition trick (independent argmins
//          for fg and bg). Default; pairs naturally with pixel-level
//          dithering on the input (the encoder picks the dominant 2
//          colors per cell to match a pre-dithered image).
//
//   blur — Pappas-Neuhoff perceptual halftoning. Both source and the
//          rendered cell are convolved with a 3×3 binomial low-pass
//          (≈ HVS contrast sensitivity) before MSE is taken. Encoder
//          naturally picks checker glyphs that *look* right post-blur,
//          replacing the manual mean-bias hack. Requires the input to
//          be the continuous source — pre-dithering destroys the
//          precision the metric needs, so callers should pass
//          dither=none.
//
enum class Metric : std::uint8_t { mse, blur };

// Blur-kernel shape for the Pappas-Neuhoff metric. The kernel is the
// HVS-approximating low-pass that's MSE-compared between source and
// rendered cell. `auto_per_mode` resolves at encode time and currently
// returns binomial for every cga-text mode (kept conservative by user
// request). Wider / anisotropic kernels score higher on SSIMULACRA2
// per-mode but soften high-frequency detail; exposed for opt-in A/B.
//
//   binomial    — 3×3 (≈ Gaussian σ=0.85), conservative default.
//   aniso53     — 5×3 horizontal-favoring Gaussian (best on 8×1, 8×2
//                 cells if you want max SSIMULACRA2).
//   aniso73     — 7×3 horizontal-favoring Gaussian.
//   aniso35     — 3×5 vertical-favoring Gaussian (taller cell modes).
//   aniso37     — 3×7 vertical-favoring Gaussian.
//   wide55      — 5×5 broader symmetric Gaussian σ≈1.0.
//   wide77      — 7×7 wider symmetric Gaussian σ≈1.5 (best on 8×4 /
//                 8×8 cells if you want max SSIMULACRA2).
enum class Kernel : std::uint8_t {
    auto_per_mode, binomial, aniso53, aniso73, aniso35, aniso37,
    wide55, wide77,
};
Kernel parse_kernel(std::string_view s) noexcept;
std::string_view kernel_name(Kernel k) noexcept;
// Resolves Kernel::auto_per_mode to the per-mode default; returns
// `k` unchanged otherwise.
Kernel resolve_kernel(Kernel k, amiga::Mode mode) noexcept;

// Glyph-matched text-mode encoding result.
// For cga_text80x100: 80 cols × 100 rows → 16000 bytes in video RAM layout.
// For cga_text80x200: 80 cols × 200 rows → 32000 bytes (exceeds standard
// 16KB CGA video RAM; only loadable via split buffering at runtime).
struct CgaTextResult {
    // Raw "text mode" bytes: pairs of (char_code, attr) repeating.
    // attr byte layout: bits 7..4 = bg color index (0..15, high bit is the
    // extension — requires blink disabled), bits 3..0 = fg color index.
    std::vector<std::uint8_t> data;
    std::size_t cols{};
    std::size_t rows{};
    std::size_t cell_height_scanlines{};  // 1 or 2
    std::size_t font_height{};            // 8 (CGA) or 14 (EGA)
    // Which scanline(s) of each glyph are used for display. The CRTC lets
    // you configure the character row start at runtime, so we pick the
    // offset that gives the best overall match. 256 glyphs × N offsets
    // effectively gives us 1792 (CGA 8x8) or 3328 (EGA 8x14) distinct
    // bit patterns to search per cell.
    std::uint8_t scanline_offset{0};
    float total_error{};
    // The 16 fg/bg candidate colors the encoder searched against. For CGA
    // text modes this is always the fixed IRGB master palette (kCgaHw). For
    // EGA text modes it is the best 16-of-64 EGA gamut colors picked from
    // the image — load the ATC palette registers with
    // palette::linear_to_ega(color) to reproduce on hardware.
    std::array<Color3f, 16> palette{};
};

// Encode `image` (expected 640×200 after PAR-aware scale) into a glyph
// matched CGA text-mode buffer using the 8×8 CGA ROM font, matching each
// 8×cell_height cell against all 256 glyphs × 16 fg × 16 bg combinations.
// `restrict_chars` (optional) limits the candidate glyph set — pass
// {0x00, 0x13, 0x55, 0xB0, 0xB1, 0xDB} for the Area-5150-style "1K-color"
// encoding (only those characters produce the NTSC artifact patterns).
// If `palette16` is empty, the encoder uses the fixed 16-entry CGA/IBM
// IRGB master palette (palette::kCgaHw) — correct for CGA text modes and
// the default EGA behavior. For EGA text modes you can pass any 16 colors
// (typically picked from the 64-entry IrgbIRGB gamut via
// quantize::ega_histogram); the encoder treats fg/bg attribute nibbles
// as indices into this palette, and the picked colors are returned in
// the `palette` field of the result for ATC palette-register load-up.
Result<CgaTextResult>
encode(const Image& image, amiga::Mode mode,
       std::span<const std::uint8_t> restrict_chars = {},
       std::span<const Color3f> palette16 = {},
       int fixed_offset = -1,        // -1 = search all offsets (best quality);
                                     //  ≥0 = force this scanline_offset (use when
                                     //       target hardware can't shift ROM font —
                                     //       e.g. CGA, which has no custom-font
                                     //       slot like EGA/VGA).
       Metric metric = Metric::blur, // see Metric enum above.
       Kernel kernel = Kernel::auto_per_mode,  // see Kernel enum above.
       ProgressCb on_progress = nullptr);

// Render a CgaTextResult back to an RGB image for preview.
Image render(const CgaTextResult& r);

} // namespace png2amiga::cga_text
