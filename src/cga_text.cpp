#include "cga_text.hpp"

#include "cga_font.hpp"
#include "color_space.hpp"
#include "palette.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

// SIMD gate — AVX2 (Windows MSVC / Linux GCC x86_64) + WASM SIMD only.
// ARM64 GCC -O3 auto-vectorises the scalar loop competitively; manual
// NEON regresses Mac M-series here too (see memory
// feedback_simd_target_arch).
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define PNG2AMIGA_CGA_TEXT_SIMD_AVX2 0
#define PNG2AMIGA_CGA_TEXT_SIMD_WASM 1
#elif defined(__AVX2__)
#include <immintrin.h>
#define PNG2AMIGA_CGA_TEXT_SIMD_AVX2 1
#define PNG2AMIGA_CGA_TEXT_SIMD_WASM 0
#else
#define PNG2AMIGA_CGA_TEXT_SIMD_AVX2 0
#define PNG2AMIGA_CGA_TEXT_SIMD_WASM 0
#endif

namespace png2amiga::cga_text {

namespace {

// ---------------------------------------------------------------------------
// Per-mode default glyph-exclusion predicate — applied when the caller
// doesn't pass an explicit `restrict_chars` span (i.e. the encoder would
// otherwise try all 256 IBM CP437 glyphs).
//
// Each cell-geometry has different aliasing characteristics, so the
// useful glyph palette differs:
//
//   cga_text80x200 (1-scan cells, 8×1 px) — a single scanline of each
//      glyph contributes. Almost every glyph collapses to a one-byte
//      pattern; the dedup pass folds 256 glyphs to ≤ 256 patterns.
//
//   cga_text80x100 (2-scan cells, 8×2 px) — only the top 2 scanlines of
//      each glyph contribute. Most glyphs collapse to a tiny blob; box-
//      drawing characters in particular look like odd dashes.
//
//   cga_text80x50  (4-scan cells, 8×4 px) — half-height glyphs. Letters
//      are starting to be recognisable; box-drawing block (0xB3..0xCF)
//      reads as straight lines through detail and tends to be wrong.
//
//   cga_text80x25  (8-scan cells, 8×8 px) — full glyphs. All 256
//      glyphs are usable in principle, but the box-drawing block still
//      tends to inject "schematic" lines through natural images.
//
// EDIT THIS to play with what's allowed per mode. Add ranges, list
// specific codes, flip the condition, etc.
constexpr bool is_excluded_glyph(amiga::Mode mode, std::uint8_t ch) noexcept {
    switch (mode) {
    case amiga::Mode::cga_text80x200:
    case amiga::Mode::cga_text40x200:
        return false;
    case amiga::Mode::cga_text80x100:
    case amiga::Mode::cga_text40x100:
        return false;
    case amiga::Mode::cga_text80x50:
        return ch >= 0xB3;
    case amiga::Mode::cga_text80x25:
        return false;
    default:
        return false;  // not a cga-text mode; encode() rejects upstream
    }
}

// Map a CGA color index (0..15) to its sRGB-normalized RGB triple.
// Source of truth is palette::kCgaHw (IBM CGA IRGB master palette).
// Returned Color3f channels are sRGB / 255 — NOT linearised. Useful
// in substitute_cell() rules that want to reason about the picked
// fg/bg colors without going through the encoder's linear/OKLab
// pipeline.
inline Color3f cga_index_to_srgb(std::uint8_t idx) noexcept {
    auto hex = palette::kCgaHw[idx & 0x0F];
    return Color3f{
        static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
        static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
        static_cast<float>(hex & 0xFF) / 255.0f,
    };
}

// Per-cell post-process substitution.
//
// Runs once on every cell of the final encoded buffer for every
// cga-text mode. Identity by default — add per-mode/per-glyph rules
// here to break up periodic visual artifacts the picker can't see
// (it scores cells independently, so cross-cell regularities like
// vertical stripes are invisible to the cost function).
//
// Performance is not a concern: this runs once over the cell grid
// after the parallel picker has finished. Add as many rules as
// needed.
//
// Return value: possibly-modified (ch, fg, bg). The picker's per-
// cell error stays as-is; substitutions are expected to be
// visually neutral (same color mass per cell) and just rearrange
// pixels so adjacent cells de-correlate.
struct CellSubst {
    std::uint8_t ch, fg, bg;
};
inline CellSubst substitute_cell(amiga::Mode mode,
                                 std::size_t col,
                                 std::size_t row,
                                 std::uint8_t ch,
                                 std::uint8_t fg,
                                 std::uint8_t bg) noexcept {
    // ---- Rule 1: 0xB1 (medium-shade ▒) phase inversion in 1-scan
    // modes. Each cell shows only row 0 of the glyph (0x55 =
    // ABABABAB). With the same (fg, bg) on vertically adjacent rows
    // the same phase repeats → vertical stripes. Swap fg/bg on
    // every other row so the phase inverts, producing a 1×1 checker.
    // 0xB1 is fg/bg-symmetric (50% mix either way) so the per-cell
    // error metric is preserved.
    if (ch == 0xB1 &&
        (mode == amiga::Mode::cga_text80x200 || mode == amiga::Mode::cga_text40x200)) {
        Color3f fgCol = cga_index_to_srgb(fg);
        Color3f bgCol = cga_index_to_srgb(bg);
        float fgD = color_space::fma_dist_sq(fgCol.r, fgCol.g, fgCol.b);
        float bgD = color_space::fma_dist_sq(bgCol.r, bgCol.g, bgCol.b);
        if (fgD > bgD) {
            std::swap(fg, bg);
        }
        if ((row & 1) == 0) {
            std::swap(fg, bg);
        }
    }
    if (ch == 0xB1 && (mode == amiga::Mode::cga_text80x100 || mode == amiga::Mode::cga_text40x100 ||
                       mode == amiga::Mode::cga_text80x50 || mode == amiga::Mode::cga_text80x25)) {
        Color3f fgCol = cga_index_to_srgb(fg);
        Color3f bgCol = cga_index_to_srgb(bg);
        float fgD = color_space::fma_dist_sq(fgCol.r, fgCol.g, fgCol.b);
        float bgD = color_space::fma_dist_sq(bgCol.r, bgCol.g, bgCol.b);
        if (fgD > bgD) {
            std::swap(fg, bg);
        }
    }

    (void)col;
    return {ch, fg, bg};
}
// ---------------------------------------------------------------------------

// Per-cell metric vectors for each of the 16 CGA master colors. The
// .lab field name is historical — its contents depend on the metric
// space chosen by encode() (currently sRGB; see comment there).
struct CgaPaletteLab {
    std::array<color_space::OKLab, 16> lab;
    std::array<Color3f, 16> rgb;
};

}  // namespace

Kernel parse_kernel(std::string_view s) noexcept {
    if (s == "auto" || s == "auto_per_mode" || s.empty()) return Kernel::auto_per_mode;
    if (s == "binomial") return Kernel::binomial;
    if (s == "aniso53") return Kernel::aniso53;
    if (s == "aniso73") return Kernel::aniso73;
    if (s == "aniso35") return Kernel::aniso35;
    if (s == "aniso37") return Kernel::aniso37;
    if (s == "wide55") return Kernel::wide55;
    if (s == "wide77") return Kernel::wide77;
    return Kernel::auto_per_mode;
}

std::string_view kernel_name(Kernel k) noexcept {
    switch (k) {
    case Kernel::auto_per_mode:
        return "auto";
    case Kernel::binomial:
        return "binomial";
    case Kernel::aniso53:
        return "aniso53";
    case Kernel::aniso73:
        return "aniso73";
    case Kernel::aniso35:
        return "aniso35";
    case Kernel::aniso37:
        return "aniso37";
    case Kernel::wide55:
        return "wide55";
    case Kernel::wide77:
        return "wide77";
    }
    return "auto";
}

Kernel resolve_kernel(Kernel k, amiga::Mode /*mode*/) noexcept {
    if (k != Kernel::auto_per_mode) return k;
    // Default kept as 3×3 binomial across all cga-text modes by user
    // request. Per-mode bench (2026-05-05) showed wider / anisotropic
    // kernels score higher on SSIMULACRA2 (aniso53 +6.73 on 8×1,
    // wide55 +4.71 on 8×2, wide77 +6.51 on 8×4 / +2.23 on 8×8), but
    // the wider kernels can over-soften high-frequency detail. The
    // CLI / web "Kernel" selector exposes the alternatives for users
    // who want them.
    return Kernel::binomial;
}

Result<CgaTextResult> encode(const Image& image,
                             amiga::Mode mode,
                             std::span<const std::uint8_t> restrict_chars,
                             std::span<const Color3f> palette16,
                             int fixed_offset,
                             Metric metric,
                             Kernel kernel,
                             ProgressCb on_progress) {

    if (!amiga::is_cga_text(mode)) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "cga_text::encode: not a CGA text-mode graphics mode",
        }};
    }
    if (!palette16.empty() && palette16.size() != 16) {
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "cga_text::encode: palette must be exactly 16 colors",
        }};
    }

    // CGA 80x100 / 80x50 hardware: 80 cols × 8 px wide cells, 2 or 4
    // hardware scanlines per cell (CRTC max-scan-line=1 for 80x100,
    // max-scan-line=3 for 80x50). The encoder takes the input as
    // hardware-pixel dims (1 source pixel = 1 hardware dot). Callers
    // that have square-pixel source pre-halve the image vertically
    // before invoking the encoder.
    const palette::FontRef& font = palette::kFontCga8x8;
    const std::size_t cell_h = amiga::cga_text_cell_height(mode);
    constexpr std::size_t cell_w = 8u;
    // Stack-allocated cell buffers are sized to the max supported cell
    // height (8 — the 80x25 standard-text-geom cell) so a single code
    // path handles 80x100 (cell_h=2), 80x50 (cell_h=4), and 80x25
    // (cell_h=8). Entries past `cell_n = 8 * cell_h` stay untouched.
    constexpr std::size_t kMaxCellH = 8u;
    constexpr std::size_t kMaxCellN = 8u * kMaxCellH;  // 64
    if (image.width() == 0 || image.height() == 0 || (image.width() % cell_w) != 0 ||
        (image.height() % cell_h) != 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("cga_text::encode: input must be a non-zero "
                        "multiple of {}x{}, got {}x{}",
                        cell_w,
                        cell_h,
                        image.width(),
                        image.height()),
        }};
    }
    const std::size_t disp_w = image.width();
    const std::size_t disp_h = image.height();
    const std::size_t cols = disp_w / cell_w;
    const std::size_t rows = disp_h / cell_h;

    // Candidate character set. If the caller passed an explicit list, use
    // it verbatim — they're driving glyph selection on purpose. If empty,
    // start from all 256 CP437 glyphs and drop anything that the file-level
    // `is_excluded_glyph` predicate rejects (default: box-drawing block
    // 0xB3..0xCF).
    std::vector<std::uint8_t> chars;
    if (restrict_chars.empty()) {
        chars.reserve(256);
        for (int i = 0; i < 256; ++i) {
            auto ch = static_cast<std::uint8_t>(i);
            if (is_excluded_glyph(mode, ch)) continue;
            chars.push_back(ch);
        }
    } else {
        chars.assign(restrict_chars.begin(), restrict_chars.end());
    }

    // Cell metric runs in LINEAR RGB. This is the physically correct
    // space for averaging cell pixels: a CRT or LCD emits photons
    // proportional to linear RGB, so a 50% checker between black and
    // white emits 0.5 linear photons, displays at ~73% sRGB
    // brightness, and the encoder must score it that way.
    //
    // The previous version averaged in sRGB (gamma-encoded), which
    // gave avg(0, 1) = 0.5 sRGB → encoder thought a 50% checker
    // matched 50% sRGB-gray source pixels, so it picked checkers
    // that displayed at +30 % brightness. Required manual
    // brightness/gamma correction on most photographic input.
    //
    // The metric structure (pair dot products, squared norms,
    // closed-form expansion) is space-agnostic — we just keep palette
    // and per-cell vectors in the same space. The struct field is
    // still named .lab for historical reasons; treat it as "metric
    // 3-vector".
    auto to_metric_space = [](const Color3f& c_lin) -> color_space::OKLab {
        return color_space::OKLab{c_lin.r, c_lin.g, c_lin.b};
    };

    CgaPaletteLab pal_local;
    if (!palette16.empty()) {
        for (std::size_t i = 0; i < 16; ++i) {
            pal_local.rgb[i] = palette16[i];
            pal_local.lab[i] = to_metric_space(palette16[i]);
        }
    } else {
        for (std::size_t i = 0; i < 16; ++i) {
            pal_local.rgb[i] = color_space::srgb_hex_to_linear(palette::kCgaHw[i]);
            pal_local.lab[i] = to_metric_space(pal_local.rgb[i]);
        }
    }
    const CgaPaletteLab& pal = pal_local;

    // Try all possible scanline offsets into the glyph (0..glyph_h - cell_h).
    // The CRTC can be programmed to start the character row at any scanline,
    // so what we emit as (char byte + attribute byte) is hardware-legal
    // regardless of offset — the demo just needs the right CRTC setup.
    // 256 glyphs × N offsets distinct bit patterns per cell.
    std::size_t n_offsets = (font.glyph_height + 1) - cell_h;
    std::size_t offset_start = 0;
    std::size_t offset_end = n_offsets;
    if (fixed_offset >= 0 && static_cast<std::size_t>(fixed_offset) < n_offsets) {
        offset_start = static_cast<std::size_t>(fixed_offset);
        offset_end = offset_start + 1;
    }
    CgaTextResult best_result;
    best_result.total_error = std::numeric_limits<float>::infinity();

    // Total per-cell work across all offset trials, used to drive the
    // progress callback. Workers fetch_add a counter per cell finished;
    // a mutex serialises the actual on_progress invocation so parallel
    // jthreads don't race onto stdout.
    std::size_t total_cells = (offset_end - offset_start) * cols * rows;
    if (total_cells == 0) total_cells = 1;
    std::atomic<std::size_t> cells_done_global{0};
    std::mutex progress_mu;
    if (on_progress) on_progress(0.0f, "cga-text");

    for (std::size_t offset = offset_start; offset < offset_end; ++offset) {
        // Dedupe candidate glyphs by their cell-h scanline pattern AND by
        // inversion symmetry — (G, fg=A, bg=B) renders identical pixels to
        // (~G, fg=B, bg=A), and the per-cell search iterates all 16×16
        // (fg, bg) pairs anyway, so keeping both class representatives is
        // wasted work. Glyphs reduce to a `canonical_key` = min(key, ~key)
        // bucket; we keep the lowest-numbered char per bucket.
        //
        // For the standard CGA font + 0..255 candidate set, the dedup
        // collapses 256 chars down to roughly:
        //     cell_h=1 → 19..42, cell_h=2 → 65..134,
        //     cell_h=4 → 164..216, cell_h=8 → 249.
        // (counts vary with `offset`). The inner brute-force (16 fg ×
        // 16 bg × 8*cell_h pixels) then runs once per *distinct
        // pattern class*, not once per char.
        //
        // Fast path: for the bundled CGA font and the full 0..255
        // candidate set (the common case from api.cpp / main.cpp), the
        // canonical bitmap is precomputed at compile time in
        // `kCgaCanonicalBitmaps` — no per-encode hashmap construction.
        // Slow path (different font or caller-supplied `restrict_chars`):
        // recompute the canonical-key buckets inline using the same
        // constexpr helpers, with a stack-allocated linear-search "seen"
        // set since n ≤ 256.
        struct Candidate {
            std::uint8_t ch;        // a representative char for this pattern
            std::uint64_t fg_mask;  // bit i set = pixel i is fg (else bg)
                                    // — u64 holds up to 8×8 = 64 pixels.
        };
        std::vector<Candidate> candidates;
        candidates.reserve(std::min<std::size_t>(chars.size(), 256));

        const bool use_precomputed = (font.data == palette::kFontCga8x8.data) &&
                                     (font.glyph_height == palette::kFontCga8x8.glyph_height) &&
                                     restrict_chars.empty();
        if (use_precomputed) {
            const auto& bitmap =
                palette::kCgaCanonicalBitmaps[palette::cga_cell_h_index(cell_h)][offset];
            for (int i = 0; i < 256; ++i) {
                auto ch = static_cast<std::uint8_t>(i);
                if (!bitmap.test(ch)) continue;
                if (is_excluded_glyph(mode, ch)) continue;
                candidates.push_back({ch, palette::glyph_fg_mask(font, ch, cell_h, offset)});
            }
        } else {
            std::array<std::uint64_t, 256> seen_keys{};
            std::size_t n_seen = 0;
            for (auto ch : chars) {
                auto key = palette::glyph_pattern_key(font, ch, cell_h, offset);
                auto canon = palette::glyph_canonical_key(key, cell_h);
                bool dup = false;
                for (std::size_t k = 0; k < n_seen; ++k) {
                    if (seen_keys[k] == canon) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                seen_keys[n_seen++] = canon;
                candidates.push_back({ch, palette::glyph_fg_mask(font, ch, cell_h, offset)});
            }
        }

        CgaTextResult result;
        result.data.assign(cols * rows * 2, 0);
        result.cols = cols;
        result.rows = rows;
        // Always 2 hardware scanlines per cell — the encoder operates in
        // hardware-pixel space, so cell_height_scanlines == cell_h.
        result.cell_height_scanlines = cell_h;
        result.font_height = font.glyph_height;
        result.scanline_offset = static_cast<std::uint8_t>(offset);

        // Pappas-Neuhoff perceptual halftoning metric: instead of per-pixel
        // MSE between source and rendered, low-pass-filter both with a small
        // HVS-approximating kernel and compute MSE on the blurred versions.
        // For uniform regions this naturally rewards checker glyphs that
        // average to the right color after blur, exactly the way a human
        // perceives them on a CRT — without the artefacts that pure mean-bias
        // produced.
        //
        // Kernel: 3×3 binomial (≈ Gaussian σ=0.85), separable [1,2,1]/4 ⊗ [1,2,1]/4,
        // with replicate padding at cell edges.
        //
        // Resolve auto_per_mode → concrete kernel choice.
        const Kernel kRes = resolve_kernel(kernel, mode);
        constexpr int kKR = 3;            // half-width supports up to 7×7
        constexpr int kKD = 2 * kKR + 1;  // 7
        std::array<std::array<float, kKD>, kKD> kBlurKernel{};
        auto fill_kernel = [&]() {
            for (auto& row : kBlurKernel)
                row.fill(0.0f);
            // Helper: emit a 1D Gaussian of size `n` (must be ≤ kKD), σ.
            auto gauss1d = [&](int n, float sigma, std::array<float, kKD>& out) {
                std::array<float, kKD> raw{};
                int half = n / 2;
                float sum = 0;
                for (int i = 0; i < n; ++i) {
                    float x = static_cast<float>(i - half);
                    raw[static_cast<std::size_t>(i)] = std::exp(-0.5f * x * x / (sigma * sigma));
                    sum += raw[static_cast<std::size_t>(i)];
                }
                for (int i = 0; i < n; ++i)
                    raw[static_cast<std::size_t>(i)] /= sum;
                // Center into kKD slot at offset (kKR - half).
                int off = kKR - half;
                for (auto& v : out)
                    v = 0.0f;
                for (int i = 0; i < n; ++i)
                    out[static_cast<std::size_t>(off + i)] = raw[static_cast<std::size_t>(i)];
            };
            // Build separable kernel kBlurKernel[r][c] = ky[r] * kx[c].
            auto sep_outer = [&](const std::array<float, kKD>& ky,
                                 const std::array<float, kKD>& kx) {
                for (std::size_t r = 0; r < kKD; ++r)
                    for (std::size_t c = 0; c < kKD; ++c)
                        kBlurKernel[r][c] = ky[r] * kx[c];
            };
            std::array<float, kKD> ky{}, kx{};
            // Center the 1D weights into the 7-slot array at offset kKR -
            // half. Used by the binomial branch where we want the exact
            // [1,2,1]/4 weights (Gaussian approximation drifts from the
            // historical default).
            auto place3 = [&](std::array<float, kKD>& dst, float a, float b, float c) {
                for (auto& v : dst)
                    v = 0.0f;
                dst[static_cast<std::size_t>(kKR - 1)] = a;
                dst[static_cast<std::size_t>(kKR)] = b;
                dst[static_cast<std::size_t>(kKR + 1)] = c;
            };
            switch (kRes) {
            case Kernel::aniso53:
                place3(ky, 0.25f, 0.5f, 0.25f);
                gauss1d(5, 1.2f, kx);
                break;
            case Kernel::aniso73:
                place3(ky, 0.25f, 0.5f, 0.25f);
                gauss1d(7, 1.6f, kx);
                break;
            case Kernel::aniso35:
                gauss1d(5, 1.2f, ky);
                place3(kx, 0.25f, 0.5f, 0.25f);
                break;
            case Kernel::aniso37:
                gauss1d(7, 1.6f, ky);
                place3(kx, 0.25f, 0.5f, 0.25f);
                break;
            case Kernel::wide55:
                gauss1d(5, 1.0f, ky);
                gauss1d(5, 1.0f, kx);
                break;
            case Kernel::wide77:
                gauss1d(7, 1.5f, ky);
                gauss1d(7, 1.5f, kx);
                break;
            case Kernel::binomial:
            case Kernel::auto_per_mode:  // resolved above; fallback for safety
            default:
                // Exact [1,2,1]/4 ⊗ [1,2,1]/4 → 3×3 binomial as in the
                // original encoder. Reproduces the historical defaults
                // bit-for-bit.
                place3(ky, 0.25f, 0.5f, 0.25f);
                place3(kx, 0.25f, 0.5f, 0.25f);
                break;
            }
            sep_outer(ky, kx);
        };
        fill_kernel();

        // Per output pixel position, list of (source-pixel-index, weight) taps
        // — 25 entries each, with replicate padding folding edge taps onto
        // boundary pixels (so taps may share q values; that's fine).
        struct Tap {
            std::uint8_t q;
            float w;
        };
        constexpr std::size_t kKernTapN = static_cast<std::size_t>(kKD * kKD);  // 25
        const std::size_t cell_n = 8 * cell_h;
        std::vector<std::array<Tap, kKernTapN>> kernel_taps(cell_n);
        for (std::size_t py = 0; py < cell_h; ++py) {
            for (std::size_t px = 0; px < 8; ++px) {
                std::size_t p_out = py * 8 + px;
                std::size_t k = 0;
                for (int dy = -kKR; dy <= kKR; ++dy) {
                    int ny = std::clamp(static_cast<int>(py) + dy, 0, static_cast<int>(cell_h) - 1);
                    for (int dx = -kKR; dx <= kKR; ++dx) {
                        int nx = std::clamp(static_cast<int>(px) + dx, 0, 7);
                        kernel_taps[p_out][k++] = {static_cast<std::uint8_t>(ny * 8 + nx),
                                                   kBlurKernel[static_cast<std::size_t>(dy + kKR)]
                                                              [static_cast<std::size_t>(dx + kKR)]};
                    }
                }
            }
        }
        // SoA copy + pre-built mask layout for the per-candidate K-build hot
        // loop. The inner branch `if ((fg_mask >> tap.q) & 1u) a += tap.w`
        // showed 138 s / 174 s of CPU (79 %) on AMD uProf for cga-text80x100
        // — MSVC's branch predictor + non-vectorised conditional add was
        // dominating everything else. SoA lets the inner reduce to a
        // branchless FMA: `a = fma(fg_bits[q] ? 1 : 0, w, a)`, expressed
        // here as a precomputed float-per-bit lookup so each candidate only
        // converts its 64-bit fg_mask to a 64-float array once and the
        // per-pixel inner is then 25 contiguous loads + 25 FMAs that the
        // vectoriser handles cleanly.
        std::vector<std::array<std::uint8_t, kKernTapN>> tap_q(cell_n);
        std::vector<std::array<float, kKernTapN>> tap_w(cell_n);
        for (std::size_t p = 0; p < cell_n; ++p) {
            for (std::size_t k = 0; k < kKernTapN; ++k) {
                tap_q[p][k] = kernel_taps[p][k].q;
                tap_w[p][k] = kernel_taps[p][k].w;
            }
        }
        // Dense kernel matrix, transposed: kernel_T[q][p] = sum of weights
        // mapping source pixel q to output pixel p. Sparse-25-taps form is
        // ~25 µops per pixel × per-candidate scalar gather (still 79 % CPU
        // even after the branchless rewrite — MSVC can't fold variable-
        // index gathers into vector ops). Dense form turns the per-pixel
        // tap loop into a single 64-wide mat-vec column-update per candidate:
        //   a[p] = Σ_q kernel_T[q][p] * fg_bits_f[q]
        // For each q with fg_bits_f[q]==1, accumulate kernel_T[q][:] into
        // a[:]. SIMD-friendly: 8-wide AVX2 / 4-wide WASM SIMD over the p
        // axis, broadcast fg_bits_f[q], FMA. No gather, no branch. Multiple
        // taps targeting the same q get folded into kernel_T[q][p] (each
        // pixel has multiple kernel weights from clipped edge taps that
        // alias).
        constexpr std::size_t kCellNMax = 64;
        alignas(32) std::array<std::array<float, kCellNMax>, kCellNMax> kernel_T{};
        for (std::size_t p = 0; p < cell_n; ++p) {
            for (std::size_t k = 0; k < kKernTapN; ++k) {
                kernel_T[tap_q[p][k]][p] += tap_w[p][k];
            }
        }

        // Pre-compute palette dot products and norms (used in the closed-form
        // per-pair error formula below).
        std::array<std::array<float, 16>, 16> pal_dot{};
        std::array<float, 16> pal_norm{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto& pi = pal.lab[i];
            pal_norm[i] = color_space::fma_dist_sq(pi.L, pi.a, pi.b);
            for (std::size_t j = 0; j < 16; ++j) {
                const auto& pj = pal.lab[j];
                pal_dot[i][j] = color_space::fma_dot3(pi.L, pj.L, pi.a, pj.a, pi.b, pj.b);
            }
        }
        // SoA palette LAB — 16 colors in three parallel f32 arrays. Used by
        // the SIMD'd dot_K1 / dot_K2 build inside encode_cell_blur (each
        // candidate hits these 16×3 floats; AoS pal.lab forces a strided
        // gather that 8-wide AVX2 can't load efficiently).
        alignas(32) std::array<float, 16> pal_lab_L{};
        alignas(32) std::array<float, 16> pal_lab_a{};
        alignas(32) std::array<float, 16> pal_lab_b{};
        for (std::size_t i = 0; i < 16; ++i) {
            pal_lab_L[i] = pal.lab[i].L;
            pal_lab_a[i] = pal.lab[i].a;
            pal_lab_b[i] = pal.lab[i].b;
        }

        // Per-cell brute force shared by parallel and sequential paths.
        // Inputs:  cell_lab — 8×cell_h source OKLab values
        // Outputs: best (ch, fg, bg, mask, err) for that cell
        struct CellPick {
            std::uint8_t ch, fg, bg;
            std::uint64_t fg_mask;
            float err;
        };

        // ---- mse: per-pixel OKLab MSE with the sum-decomposition trick ----
        // Independent argmins over fg and bg (16+16 comparisons), so the
        // (fg, bg) search is O(16) per candidate instead of O(256). Fast
        // baseline; pairs naturally with a pre-dithered input image.
        auto encode_cell_mse =
            [&](const std::array<color_space::OKLab, kMaxCellN>& cell_lab) -> CellPick {
            std::array<std::array<float, 16>, kMaxCellN> pix_d{};
            std::array<float, 16> total_sum{};
            for (std::size_t p = 0; p < cell_n; ++p) {
                for (std::size_t c = 0; c < 16; ++c) {
                    auto& a = cell_lab[p];
                    auto& b = pal.lab[c];
                    float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                    float d = color_space::fma_dist_sq(dL, da, db);
                    pix_d[p][c] = d;
                    total_sum[c] += d;
                }
            }
            CellPick best{0, 15, 0, 0, std::numeric_limits<float>::infinity()};
            for (auto& cand : candidates) {
                auto fg_mask = cand.fg_mask;
                std::array<float, 16> sum_fg{};
                auto m = fg_mask;
                while (m) {
                    auto p = static_cast<unsigned>(std::countr_zero(m));
                    m &= m - 1;
                    for (std::size_t c = 0; c < 16; ++c)
                        sum_fg[c] += pix_d[p][c];
                }
                float min_fg = std::numeric_limits<float>::infinity();
                float min_bg = std::numeric_limits<float>::infinity();
                std::uint8_t fg_idx = 0, bg_idx = 0;
                for (std::uint8_t c = 0; c < 16; ++c) {
                    if (sum_fg[c] < min_fg) {
                        min_fg = sum_fg[c];
                        fg_idx = c;
                    }
                    float sb = total_sum[c] - sum_fg[c];
                    if (sb < min_bg) {
                        min_bg = sb;
                        bg_idx = c;
                    }
                }
                float err = min_fg + min_bg;
                if (err < best.err) {
                    best.err = err;
                    best.ch = cand.ch;
                    best.fg = fg_idx;
                    best.bg = bg_idx;
                    best.fg_mask = fg_mask;
                }
            }
            return best;
        };

        // ---- blur: Pappas-Neuhoff perceptual halftoning ----
        // err = ||blurred(source) − blurred(rendered)||². Closed-form pair
        // expansion: K0 − 2·K1·fg − 2·K2·bg + 2·K3·(fg·bg) + K4·||fg||² + K5·||bg||².
        // K0 is per-cell, K1..K5 are per-candidate, per-pair is then ~9 ops.
        auto encode_cell_blur =
            [&](const std::array<color_space::OKLab, kMaxCellN>& cell_lab) -> CellPick {
            std::array<color_space::OKLab, kMaxCellN> blurred;
            float K0 = 0;
            for (std::size_t p = 0; p < cell_n; ++p) {
                color_space::OKLab b{0, 0, 0};
                for (auto& tap : kernel_taps[p]) {
                    auto& v = cell_lab[tap.q];
                    b.L += tap.w * v.L;
                    b.a += tap.w * v.a;
                    b.b += tap.w * v.b;
                }
                blurred[p] = b;
                K0 += color_space::fma_dist_sq(b.L, b.a, b.b);
            }
            CellPick best{0, 15, 0, 0, std::numeric_limits<float>::infinity()};
            // Per-candidate scratch: a[p] = active foreground area at pixel p.
            // Computed via dense kernel mat-vec instead of the per-pixel tap
            // loop — see kernel_T construction comment above.
            alignas(32) std::array<float, kCellNMax> a_arr{};
            for (auto& cand : candidates) {
                auto fg_mask = cand.fg_mask;
                // a[p] = Σ_q kernel_T[q][p] * fg_bit(q). Iterate ONLY over
                // q's where fg_bit is set — bit scan via `m & -m`. Each
                // set bit broadcasts and FMAs kernel_T[q][:] into a_arr[:].
                // For typical glyphs ~half the bits are set, so this is
                // ~32 FMA-vectors per candidate vs the ~25 scalar gathers
                // per pixel × 64 pixels = 1600 scalar ops the old path did.
                std::memset(a_arr.data(), 0, sizeof(float) * kCellNMax);
                {
                    std::uint64_t m = fg_mask;
                    while (m) {
                        auto q = static_cast<std::size_t>(std::countr_zero(m));
                        m &= m - 1;
                        const float* col = kernel_T[q].data();
                        for (std::size_t p = 0; p < kCellNMax; ++p)
                            a_arr[p] += col[p];
                    }
                }
                color_space::OKLab K1{0, 0, 0};
                color_space::OKLab K2{0, 0, 0};
                float K3 = 0, K4 = 0, K5 = 0;
                for (std::size_t p = 0; p < cell_n; ++p) {
                    float a = a_arr[p];
                    float ma = 1.0f - a;
                    K1.L += blurred[p].L * a;
                    K1.a += blurred[p].a * a;
                    K1.b += blurred[p].b * a;
                    K2.L += blurred[p].L * ma;
                    K2.a += blurred[p].a * ma;
                    K2.b += blurred[p].b * ma;
                    K3 += a * ma;
                    K4 += a * a;
                    K5 += ma * ma;
                }
                // dot_K1 / dot_K2 build: 16 colors × 3-float dot with K1/K2.
                // SIMD: 2 chunks of 8-wide (AVX2) or 4 chunks of 4-wide (WASM)
                // over the SoA palette, FMA the three channel products.
                alignas(32) std::array<float, 16> dot_K1, dot_K2;
#if PNG2AMIGA_CGA_TEXT_SIMD_AVX2
                {
                    const __m256 vK1L = _mm256_set1_ps(K1.L);
                    const __m256 vK1a = _mm256_set1_ps(K1.a);
                    const __m256 vK1b = _mm256_set1_ps(K1.b);
                    const __m256 vK2L = _mm256_set1_ps(K2.L);
                    const __m256 vK2a = _mm256_set1_ps(K2.a);
                    const __m256 vK2b = _mm256_set1_ps(K2.b);
                    for (std::size_t base = 0; base < 16; base += 8) {
                        __m256 pL = _mm256_load_ps(pal_lab_L.data() + base);
                        __m256 pa = _mm256_load_ps(pal_lab_a.data() + base);
                        __m256 pb = _mm256_load_ps(pal_lab_b.data() + base);
                        __m256 d1 = _mm256_fmadd_ps(
                            vK1b, pb, _mm256_fmadd_ps(vK1a, pa, _mm256_mul_ps(vK1L, pL)));
                        __m256 d2 = _mm256_fmadd_ps(
                            vK2b, pb, _mm256_fmadd_ps(vK2a, pa, _mm256_mul_ps(vK2L, pL)));
                        _mm256_store_ps(dot_K1.data() + base, d1);
                        _mm256_store_ps(dot_K2.data() + base, d2);
                    }
                }
#elif PNG2AMIGA_CGA_TEXT_SIMD_WASM
                {
                    const v128_t vK1L = wasm_f32x4_splat(K1.L);
                    const v128_t vK1a = wasm_f32x4_splat(K1.a);
                    const v128_t vK1b = wasm_f32x4_splat(K1.b);
                    const v128_t vK2L = wasm_f32x4_splat(K2.L);
                    const v128_t vK2a = wasm_f32x4_splat(K2.a);
                    const v128_t vK2b = wasm_f32x4_splat(K2.b);
                    for (std::size_t base = 0; base < 16; base += 4) {
                        v128_t pL = wasm_v128_load(pal_lab_L.data() + base);
                        v128_t pa = wasm_v128_load(pal_lab_a.data() + base);
                        v128_t pb = wasm_v128_load(pal_lab_b.data() + base);
                        v128_t d1 = wasm_f32x4_add(
                            wasm_f32x4_mul(vK1L, pL),
                            wasm_f32x4_add(wasm_f32x4_mul(vK1a, pa), wasm_f32x4_mul(vK1b, pb)));
                        v128_t d2 = wasm_f32x4_add(
                            wasm_f32x4_mul(vK2L, pL),
                            wasm_f32x4_add(wasm_f32x4_mul(vK2a, pa), wasm_f32x4_mul(vK2b, pb)));
                        wasm_v128_store(dot_K1.data() + base, d1);
                        wasm_v128_store(dot_K2.data() + base, d2);
                    }
                }
#else
                for (std::size_t c = 0; c < 16; ++c) {
                    auto& pl = pal.lab[c];
                    dot_K1[c] = color_space::fma_dot3(K1.L, pl.L, K1.a, pl.a, K1.b, pl.b);
                    dot_K2[c] = color_space::fma_dot3(K2.L, pl.L, K2.a, pl.a, K2.b, pl.b);
                }
#endif

                // Hoist (fg, bg) pair-search invariants. Original inner did
                // ~11 FLOPs per pair (256 pairs × 2.8k ops); the form below
                // pre-builds A[bg] = -2·dot_K2[bg] + K5·pal_norm[bg] (16
                // entries) and C[fg] = K0 - 2·dot_K1[fg] + K4·pal_norm[fg]
                // outside the bg loop, leaving 1 FMA + 2 adds inside.
                // AMDuProf showed this lambda at 95 % of cga-text80x100 CPU
                // pre-SIMD; the SIMD rewrite below batches the 16×16 inner
                // pair-min over 8-lane (AVX2) / 4-lane (WASM) bg vectors.
                alignas(32) std::array<float, 16> A;
                for (std::size_t c = 0; c < 16; ++c)
                    A[c] = std::fma(K5, pal_norm[c], -2.0f * dot_K2[c]);
                const float K3_x2 = 2.0f * K3;

#if PNG2AMIGA_CGA_TEXT_SIMD_AVX2
                {
                    // Track best across all 16×16 pairs in lane-parallel
                    // vectors; horizontal reduce once at the end. Avoids
                    // 16 reductions (one per fg) that a per-fg argmin
                    // would require.
                    const __m256 vK3_x2 = _mm256_set1_ps(K3_x2);
                    const __m256 vA_lo = _mm256_load_ps(A.data() + 0);
                    const __m256 vA_hi = _mm256_load_ps(A.data() + 8);
                    const __m256i lane_bg_lo = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
                    const __m256i lane_bg_hi = _mm256_setr_epi32(8, 9, 10, 11, 12, 13, 14, 15);
                    __m256 best_err_v = _mm256_set1_ps(best.err);
                    __m256i best_fgbg_v = _mm256_setzero_si256();
                    for (std::uint8_t fg = 0; fg < 16; ++fg) {
                        const float C_fg = std::fma(K4, pal_norm[fg], K0 - 2.0f * dot_K1[fg]);
                        const __m256 vC_fg = _mm256_set1_ps(C_fg);
                        const auto& pal_dot_fg = pal_dot[fg];
                        const __m256i fg_lane = _mm256_set1_epi32(fg << 8);
                        // bg 0..7
                        {
                            __m256 pd = _mm256_load_ps(pal_dot_fg.data() + 0);
                            __m256 err = _mm256_fmadd_ps(vK3_x2, pd, _mm256_add_ps(vC_fg, vA_lo));
                            __m256 lt = _mm256_cmp_ps(err, best_err_v, _CMP_LT_OQ);
                            __m256i cur_fgbg = _mm256_or_si256(fg_lane, lane_bg_lo);
                            best_err_v = _mm256_blendv_ps(best_err_v, err, lt);
                            best_fgbg_v = _mm256_castps_si256(
                                _mm256_blendv_ps(_mm256_castsi256_ps(best_fgbg_v),
                                                 _mm256_castsi256_ps(cur_fgbg),
                                                 lt));
                        }
                        // bg 8..15
                        {
                            __m256 pd = _mm256_load_ps(pal_dot_fg.data() + 8);
                            __m256 err = _mm256_fmadd_ps(vK3_x2, pd, _mm256_add_ps(vC_fg, vA_hi));
                            __m256 lt = _mm256_cmp_ps(err, best_err_v, _CMP_LT_OQ);
                            __m256i cur_fgbg = _mm256_or_si256(fg_lane, lane_bg_hi);
                            best_err_v = _mm256_blendv_ps(best_err_v, err, lt);
                            best_fgbg_v = _mm256_castps_si256(
                                _mm256_blendv_ps(_mm256_castsi256_ps(best_fgbg_v),
                                                 _mm256_castsi256_ps(cur_fgbg),
                                                 lt));
                        }
                    }
                    // Horizontal min across 8 lanes; pull the matching index.
                    alignas(32) float ee[8];
                    alignas(32) std::int32_t ii[8];
                    _mm256_store_ps(ee, best_err_v);
                    _mm256_store_si256(reinterpret_cast<__m256i*>(ii), best_fgbg_v);
                    int bk = 0;
                    float bd = ee[0];
                    for (int k = 1; k < 8; ++k)
                        if (ee[k] < bd) {
                            bd = ee[k];
                            bk = k;
                        }
                    if (bd < best.err) {
                        best.err = bd;
                        best.ch = cand.ch;
                        best.fg = static_cast<std::uint8_t>((ii[bk] >> 8) & 0xFF);
                        best.bg = static_cast<std::uint8_t>(ii[bk] & 0xFF);
                        best.fg_mask = fg_mask;
                    }
                }
#elif PNG2AMIGA_CGA_TEXT_SIMD_WASM
                {
                    const v128_t vK3_x2 = wasm_f32x4_splat(K3_x2);
                    const v128_t vA0 = wasm_v128_load(A.data() + 0);
                    const v128_t vA4 = wasm_v128_load(A.data() + 4);
                    const v128_t vA8 = wasm_v128_load(A.data() + 8);
                    const v128_t vA12 = wasm_v128_load(A.data() + 12);
                    const v128_t lane_bg0 = wasm_i32x4_make(0, 1, 2, 3);
                    const v128_t lane_bg4 = wasm_i32x4_make(4, 5, 6, 7);
                    const v128_t lane_bg8 = wasm_i32x4_make(8, 9, 10, 11);
                    const v128_t lane_bg12 = wasm_i32x4_make(12, 13, 14, 15);
                    v128_t best_err_v = wasm_f32x4_splat(best.err);
                    v128_t best_fgbg_v = wasm_i32x4_splat(0);
                    auto step =
                        [&](v128_t pd, v128_t vA, v128_t lane_bg, v128_t vC_fg, v128_t fg_lane) {
                            v128_t e = wasm_f32x4_add(wasm_f32x4_mul(vK3_x2, pd),
                                                      wasm_f32x4_add(vC_fg, vA));
                            v128_t lt = wasm_f32x4_lt(e, best_err_v);
                            v128_t cur = wasm_v128_or(fg_lane, lane_bg);
                            best_err_v = wasm_v128_bitselect(e, best_err_v, lt);
                            best_fgbg_v = wasm_v128_bitselect(cur, best_fgbg_v, lt);
                        };
                    for (std::uint8_t fg = 0; fg < 16; ++fg) {
                        const float C_fg = std::fma(K4, pal_norm[fg], K0 - 2.0f * dot_K1[fg]);
                        const v128_t vC_fg = wasm_f32x4_splat(C_fg);
                        const auto& pal_dot_fg = pal_dot[fg];
                        const v128_t fg_lane = wasm_i32x4_splat(fg << 8);
                        step(wasm_v128_load(pal_dot_fg.data() + 0), vA0, lane_bg0, vC_fg, fg_lane);
                        step(wasm_v128_load(pal_dot_fg.data() + 4), vA4, lane_bg4, vC_fg, fg_lane);
                        step(wasm_v128_load(pal_dot_fg.data() + 8), vA8, lane_bg8, vC_fg, fg_lane);
                        step(wasm_v128_load(pal_dot_fg.data() + 12),
                             vA12,
                             lane_bg12,
                             vC_fg,
                             fg_lane);
                    }
                    alignas(16) float ee[4];
                    alignas(16) std::int32_t ii[4];
                    wasm_v128_store(ee, best_err_v);
                    wasm_v128_store(ii, best_fgbg_v);
                    int bk = 0;
                    float bd = ee[0];
                    for (int k = 1; k < 4; ++k)
                        if (ee[k] < bd) {
                            bd = ee[k];
                            bk = k;
                        }
                    if (bd < best.err) {
                        best.err = bd;
                        best.ch = cand.ch;
                        best.fg = static_cast<std::uint8_t>((ii[bk] >> 8) & 0xFF);
                        best.bg = static_cast<std::uint8_t>(ii[bk] & 0xFF);
                        best.fg_mask = fg_mask;
                    }
                }
#else
                for (std::uint8_t fg = 0; fg < 16; ++fg) {
                    const float C_fg = std::fma(K4, pal_norm[fg], K0 - 2.0f * dot_K1[fg]);
                    const auto& pal_dot_fg = pal_dot[fg];
                    for (std::uint8_t bg = 0; bg < 16; ++bg) {
                        const float err = std::fma(K3_x2, pal_dot_fg[bg], C_fg + A[bg]);
                        if (err < best.err) {
                            best.err = err;
                            best.ch = cand.ch;
                            best.fg = fg;
                            best.bg = bg;
                            best.fg_mask = fg_mask;
                        }
                    }
                }
#endif
            }
            return best;
        };

        auto encode_cell =
            [&](const std::array<color_space::OKLab, kMaxCellN>& cell_lab) -> CellPick {
            switch (metric) {
            case Metric::blur:
                return encode_cell_blur(cell_lab);
            case Metric::mse:
                return encode_cell_mse(cell_lab);
            }
            return encode_cell_blur(cell_lab);
        };

        auto read_cell_source =
            [&](std::size_t col, std::size_t row, std::array<color_space::OKLab, kMaxCellN>& out) {
                for (std::size_t py = 0; py < cell_h; ++py) {
                    for (std::size_t px = 0; px < 8; ++px) {
                        auto img_x = col * 8 + px;
                        auto img_y = row * cell_h + py;
                        out[py * 8 + px] = to_metric_space(image[img_x, img_y]);
                    }
                }
            };

        auto write_cell = [&](std::size_t col, std::size_t row, const CellPick& p) {
            auto off = (row * cols + col) * 2;
            result.data[off] = p.ch;
            // attr: high nibble = bg, low nibble = fg. To use all 16 bg
            // colors (not blink), the user must disable blink via bit 5 of
            // CGA mode register (0x3D8). Our output assumes blink disabled.
            result.data[off + 1] = static_cast<std::uint8_t>((p.bg << 4) | p.fg);
        };

        // Cells are independent, dispatch via atomic counter.
        std::atomic<std::size_t> next_cell{0};
        std::atomic<double> atomic_err{0.0};
        auto worker = [&]() {
            while (true) {
                auto linear = next_cell.fetch_add(1);
                if (linear >= cols * rows) break;
                auto col = linear % cols;
                auto row = linear / cols;
                std::array<color_space::OKLab, kMaxCellN> cell_lab;
                read_cell_source(col, row, cell_lab);
                auto pick = encode_cell(cell_lab);
                write_cell(col, row, pick);
                double old = atomic_err.load(std::memory_order_relaxed);
                while (
                    !atomic_err.compare_exchange_weak(old, old + static_cast<double>(pick.err))) {}
                if (on_progress) {
                    auto done = cells_done_global.fetch_add(1) + 1;
                    // Throttle: every ~1% of total work, take the mutex
                    // and fire the callback. Keeps the bar moving without
                    // contention from N parallel jthreads.
                    std::size_t throttle = std::max<std::size_t>(1, total_cells / 100);
                    if ((done % throttle) == 0 || done == total_cells) {
                        std::lock_guard<std::mutex> lk(progress_mu);
                        on_progress(static_cast<float>(done) / static_cast<float>(total_cells),
                                    "cga-text");
                    }
                }
            }
        };
        auto n = std::max<unsigned>(1, std::thread::hardware_concurrency());
        std::vector<std::jthread> threads;
        threads.reserve(n);
        for (unsigned i = 0; i < n; ++i)
            threads.emplace_back(worker);
        threads.clear();

        result.total_error = static_cast<float>(atomic_err.load());
        for (std::size_t i = 0; i < 16; ++i)
            result.palette[i] = pal.rgb[i];
        if (result.total_error < best_result.total_error) {
            best_result = std::move(result);
        }
    }  // end scanline-offset loop

    // Per-cell substitution post-pass. Runs unconditionally for all
    // cga-text modes; substitute_cell() defaults to identity so modes
    // without rules pay only the loop cost. New rules go in there,
    // not here.
    {
        const std::size_t bcols = best_result.cols;
        const std::size_t brows = best_result.rows;
        for (std::size_t r = 0; r < brows; ++r) {
            for (std::size_t c = 0; c < bcols; ++c) {
                std::size_t off = (r * bcols + c) * 2;
                std::uint8_t ch = best_result.data[off];
                std::uint8_t attr = best_result.data[off + 1];
                std::uint8_t fg = attr & 0x0F;
                std::uint8_t bg = (attr >> 4) & 0x0F;
                auto sub = substitute_cell(mode, c, r, ch, fg, bg);
                best_result.data[off] = sub.ch;
                best_result.data[off + 1] = static_cast<std::uint8_t>((sub.bg << 4) | sub.fg);
            }
        }
    }

    if (on_progress) on_progress(1.0f, "done");
    return best_result;
}

Image render(const CgaTextResult& r) {
    // Use the palette stored in the result. For CGA text it equals the
    // fixed IRGB master set; for EGA text it's the image-picked 16-of-64.
    // Fallback to the fixed master for legacy results with zeroed palette
    // (all-black triggers → detect by checking slot 15 is non-zero, since
    // kCgaHw[15] = white).
    std::array<Color3f, 16> pal_rgb = r.palette;
    bool zeroed = true;
    for (auto& c : pal_rgb)
        if (c.r != 0 || c.g != 0 || c.b != 0) {
            zeroed = false;
            break;
        }
    if (zeroed) {
        for (std::size_t i = 0; i < 16; ++i) {
            pal_rgb[i] = color_space::srgb_hex_to_linear(palette::kCgaHw[i]);
        }
    }
    palette::FontRef font = palette::kFontCga8x8;
    auto h = r.rows * r.cell_height_scanlines;
    Image img(r.cols * 8, h);
    for (std::size_t row = 0; row < r.rows; ++row) {
        for (std::size_t col = 0; col < r.cols; ++col) {
            auto off = (row * r.cols + col) * 2;
            auto ch = r.data[off];
            auto attr = r.data[off + 1];
            auto fg = static_cast<std::uint8_t>(attr & 0xF);
            auto bg = static_cast<std::uint8_t>((attr >> 4) & 0xF);
            for (std::size_t line = 0; line < r.cell_height_scanlines; ++line) {
                auto sl = palette::font_scanline(font, ch, r.scanline_offset + line);
                for (std::size_t px = 0; px < 8; ++px) {
                    auto c = (sl & (0x80u >> px)) ? fg : bg;
                    img[col * 8 + px, row * r.cell_height_scanlines + line] = pal_rgb[c];
                }
            }
        }
    }
    return img;
}

}  // namespace png2amiga::cga_text
