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
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#include <unordered_map>
#include <utility>

namespace png2amiga::cga_text {

namespace {

// Per-cell metric vectors for each of the 16 CGA master colors. The
// .lab field name is historical — its contents depend on the metric
// space chosen by encode() (currently sRGB; see comment there).
struct CgaPaletteLab {
    std::array<color_space::OKLab, 16> lab;
    std::array<Color3f, 16> rgb;
};

}  // namespace

Result<CgaTextResult>
encode(const Image& image, amiga::Mode mode,
       std::span<const std::uint8_t> restrict_chars,
       std::span<const Color3f> palette16,
       int fixed_offset,
       Metric metric,
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

    // CGA 80x100 hardware: 80 cols × 8 px wide cells, 2 hardware scanlines
    // tall. The encoder takes the input as hardware-pixel dims (1 source
    // pixel = 1 hardware dot). Callers that have square-pixel source pre-
    // halve the image vertically before invoking the encoder. Single
    // mode — no canonical/freeform branch here.
    const palette::FontRef& font = palette::kFontCga8x8;
    constexpr std::size_t cell_h = 2u;
    constexpr std::size_t cell_w = 8u;
    if (image.width()  == 0 || image.height() == 0
        || (image.width()  % cell_w) != 0
        || (image.height() % cell_h) != 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("cga_text::encode: input must be a non-zero "
                        "multiple of {}x{}, got {}x{}",
                        cell_w, cell_h, image.width(), image.height()),
        }};
    }
    const std::size_t disp_w = image.width();
    const std::size_t disp_h = image.height();
    const std::size_t cols = disp_w / cell_w;
    const std::size_t rows = disp_h / cell_h;

    // Candidate character set. If empty, use all 256.
    std::vector<std::uint8_t> chars;
    if (restrict_chars.empty()) {
        chars.resize(256);
        for (int i = 0; i < 256; ++i) chars[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(i);
    } else {
        chars.assign(restrict_chars.begin(), restrict_chars.end());
    }

    // The Pappas-Neuhoff blur metric runs in sRGB (gamma-encoded)
    // space, not OKLab. Rationale: PN was designed for the
    // gamma-encoded signal a CRT actually emits, and the eye's
    // post-blur perception of a CGA-text cell averaging fg/bg via
    // checker patterns matches sRGB-domain blur much better than
    // OKLab on chroma-rich content. A/B on electrichues02 showed
    // sRGB +2.14 SSIMULACRA2 with markedly better foreground-vs-
    // background separation in dark midtones (reds and market
    // detail register; OKLab was dominated by blue noise).
    //
    // The metric structure (pair dot products, squared norms,
    // closed-form expansion) is space-agnostic — we just keep palette
    // and per-cell vectors in the same space. The struct field is
    // still named .lab for historical reasons; treat it as "metric
    // 3-vector".
    auto to_metric_space = [](const Color3f& c_lin) -> color_space::OKLab {
        auto s = color_space::linear_to_srgb(c_lin).clamped();
        return color_space::OKLab{s.r, s.g, s.b};
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
    if (fixed_offset >= 0 &&
        static_cast<std::size_t>(fixed_offset) < n_offsets) {
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
        // Dedupe candidate glyphs by their cell-h scanline pattern. Many
        // glyphs share the same top-of-cell bit pattern — e.g., at offset 0
        // with cell_h=1, most lowercase letters and punctuation have a blank
        // top scanline, so the "pattern = 0" cluster collapses from dozens
        // of glyphs down to a single representative. The inner brute-force
        // (16 fg × 16 bg × 8*cell_h pixels) then runs once per *distinct
        // pattern*, not once per char. The only output difference is which
        // representative char code we emit for a given pattern; any char
        // mapped to that pattern produces identical pixels on screen.
        //
        // We also precompute the 8*cell_h-bit fg_mask once per candidate
        // here, outside the per-cell loop — the old code rebuilt it per
        // (cell × glyph), which dominated the constant factor.
        struct Candidate {
            std::uint8_t ch;        // a representative char for this pattern
            std::uint32_t fg_mask;  // bit i set = pixel i is fg (else bg)
        };
        std::vector<Candidate> candidates;
        candidates.reserve(std::min<std::size_t>(chars.size(), 256));
        std::unordered_map<std::uint16_t, std::size_t> pattern_to_idx;
        pattern_to_idx.reserve(chars.size() * 2);
        for (auto ch : chars) {
            std::array<std::uint8_t, 2> pat{};
            for (std::size_t line = 0; line < cell_h; ++line)
                pat[line] = palette::font_scanline(font, ch, offset + line);
            std::uint16_t key = static_cast<std::uint16_t>(
                pat[0] | (cell_h > 1
                              ? static_cast<unsigned>(pat[1]) << 8
                              : 0u));
            if (pattern_to_idx.contains(key)) continue;
            std::uint32_t fg_mask = 0;
            for (std::size_t line = 0; line < cell_h; ++line) {
                auto sl = pat[line];
                for (std::size_t px = 0; px < 8; ++px) {
                    if (sl & (0x80u >> px))
                        fg_mask |= (1u << (line * 8 + px));
                }
            }
            pattern_to_idx.emplace(key, candidates.size());
            candidates.push_back({ch, fg_mask});
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
    // average to the right colour after blur, exactly the way a human
    // perceives them on a CRT — without the artefacts that pure mean-bias
    // produced.
    //
    // Kernel: 3×3 binomial (≈ Gaussian σ=0.85), separable [1,2,1]/4 ⊗ [1,2,1]/4,
    // with replicate padding at cell edges.
    constexpr std::array<std::array<float, 3>, 3> kBlurKernel = {{
        {1.0f/16, 2.0f/16, 1.0f/16},
        {2.0f/16, 4.0f/16, 2.0f/16},
        {1.0f/16, 2.0f/16, 1.0f/16},
    }};

    // Per output pixel position, list of (source-pixel-index, weight) taps
    // — 9 entries each, with replicate padding folding edge taps onto
    // boundary pixels (so taps may share q values; that's fine).
    struct Tap { std::uint8_t q; float w; };
    const std::size_t cell_n = 8 * cell_h;
    std::vector<std::array<Tap, 9>> kernel_taps(cell_n);
    for (std::size_t py = 0; py < cell_h; ++py) {
        for (std::size_t px = 0; px < 8; ++px) {
            std::size_t p_out = py * 8 + px;
            std::size_t k = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(static_cast<int>(py) + dy, 0,
                                    static_cast<int>(cell_h) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(static_cast<int>(px) + dx, 0, 7);
                    kernel_taps[p_out][k++] = {
                        static_cast<std::uint8_t>(ny * 8 + nx),
                        kBlurKernel[static_cast<std::size_t>(dy + 1)]
                                   [static_cast<std::size_t>(dx + 1)]};
                }
            }
        }
    }

    // Pre-compute palette dot products and norms (used in the closed-form
    // per-pair error formula below).
    std::array<std::array<float, 16>, 16> pal_dot{};
    std::array<float, 16> pal_norm{};
    for (std::size_t i = 0; i < 16; ++i) {
        pal_norm[i] = pal.lab[i].L * pal.lab[i].L
                    + pal.lab[i].a * pal.lab[i].a
                    + pal.lab[i].b * pal.lab[i].b;
        for (std::size_t j = 0; j < 16; ++j) {
            pal_dot[i][j] = pal.lab[i].L * pal.lab[j].L
                          + pal.lab[i].a * pal.lab[j].a
                          + pal.lab[i].b * pal.lab[j].b;
        }
    }

    // Per-cell brute force shared by parallel and sequential paths.
    // Inputs:  cell_lab — 8×cell_h source OKLab values
    // Outputs: best (ch, fg, bg, mask, err) for that cell
    struct CellPick {
        std::uint8_t ch, fg, bg;
        std::uint32_t fg_mask;
        float err;
    };

    // ---- mse: per-pixel OKLab MSE with the sum-decomposition trick ----
    // Independent argmins over fg and bg (16+16 comparisons), so the
    // (fg, bg) search is O(16) per candidate instead of O(256). Fast
    // baseline; pairs naturally with a pre-dithered input image.
    auto encode_cell_mse = [&](const std::array<color_space::OKLab, 16>& cell_lab) -> CellPick {
        std::array<std::array<float, 16>, 16> pix_d{};
        std::array<float, 16> total_sum{};
        for (std::size_t p = 0; p < cell_n; ++p) {
            for (std::size_t c = 0; c < 16; ++c) {
                auto& a = cell_lab[p]; auto& b = pal.lab[c];
                float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                float d = dL * dL + da * da + db * db;
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
                if (sum_fg[c] < min_fg) { min_fg = sum_fg[c]; fg_idx = c; }
                float sb = total_sum[c] - sum_fg[c];
                if (sb < min_bg) { min_bg = sb; bg_idx = c; }
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
    auto encode_cell_blur = [&](const std::array<color_space::OKLab, 16>& cell_lab) -> CellPick {
        std::array<color_space::OKLab, 16> blurred;
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
            K0 += b.L * b.L + b.a * b.a + b.b * b.b;
        }
        CellPick best{0, 15, 0, 0, std::numeric_limits<float>::infinity()};
        for (auto& cand : candidates) {
            auto fg_mask = cand.fg_mask;
            color_space::OKLab K1{0, 0, 0};
            color_space::OKLab K2{0, 0, 0};
            float K3 = 0, K4 = 0, K5 = 0;
            for (std::size_t p = 0; p < cell_n; ++p) {
                float a = 0;
                for (auto& tap : kernel_taps[p]) {
                    if ((fg_mask >> tap.q) & 1u) a += tap.w;
                }
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
            std::array<float, 16> dot_K1, dot_K2;
            for (std::size_t c = 0; c < 16; ++c) {
                auto& pl = pal.lab[c];
                dot_K1[c] = K1.L * pl.L + K1.a * pl.a + K1.b * pl.b;
                dot_K2[c] = K2.L * pl.L + K2.a * pl.a + K2.b * pl.b;
            }
            for (std::uint8_t fg = 0; fg < 16; ++fg) {
                for (std::uint8_t bg = 0; bg < 16; ++bg) {
                    float err = K0
                              - 2.0f * dot_K1[fg]
                              - 2.0f * dot_K2[bg]
                              + 2.0f * K3 * pal_dot[fg][bg]
                              + K4 * pal_norm[fg]
                              + K5 * pal_norm[bg];
                    if (err < best.err) {
                        best.err = err;
                        best.ch = cand.ch;
                        best.fg = fg;
                        best.bg = bg;
                        best.fg_mask = fg_mask;
                    }
                }
            }
        }
        return best;
    };

    auto encode_cell = [&](const std::array<color_space::OKLab, 16>& cell_lab) -> CellPick {
        switch (metric) {
        case Metric::blur: return encode_cell_blur(cell_lab);
        case Metric::mse:  return encode_cell_mse(cell_lab);
        }
        return encode_cell_blur(cell_lab);
    };

    auto read_cell_source = [&](std::size_t col, std::size_t row,
                                std::array<color_space::OKLab, 16>& out) {
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
        result.data[off + 1] =
            static_cast<std::uint8_t>((p.bg << 4) | p.fg);
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
            std::array<color_space::OKLab, 16> cell_lab;
            read_cell_source(col, row, cell_lab);
            auto pick = encode_cell(cell_lab);
            write_cell(col, row, pick);
            double old = atomic_err.load(std::memory_order_relaxed);
            while (!atomic_err.compare_exchange_weak(
                old, old + static_cast<double>(pick.err))) {}
            if (on_progress) {
                auto done = cells_done_global.fetch_add(1) + 1;
                // Throttle: every ~1% of total work, take the mutex
                // and fire the callback. Keeps the bar moving without
                // contention from N parallel jthreads.
                std::size_t throttle = std::max<std::size_t>(
                    1, total_cells / 100);
                if ((done % throttle) == 0 || done == total_cells) {
                    std::lock_guard<std::mutex> lk(progress_mu);
                    on_progress(
                        static_cast<float>(done) /
                            static_cast<float>(total_cells),
                        "cga-text");
                }
            }
        }
    };
#ifdef __EMSCRIPTEN__
    worker();
#else
    auto n = std::max<unsigned>(1, std::thread::hardware_concurrency());
    std::vector<std::jthread> threads;
    threads.reserve(n);
    for (unsigned i = 0; i < n; ++i) threads.emplace_back(worker);
    threads.clear();
#endif

    result.total_error = static_cast<float>(atomic_err.load());
    for (std::size_t i = 0; i < 16; ++i) result.palette[i] = pal.rgb[i];
    if (result.total_error < best_result.total_error) {
        best_result = std::move(result);
    }
    }  // end scanline-offset loop
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
        if (c.r != 0 || c.g != 0 || c.b != 0) { zeroed = false; break; }
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
                auto sl = palette::font_scanline(
                    font, ch, r.scanline_offset + line);
                for (std::size_t px = 0; px < 8; ++px) {
                    auto c = (sl & (0x80u >> px)) ? fg : bg;
                    img[col * 8 + px, row * r.cell_height_scanlines + line]
                        = pal_rgb[c];
                }
            }
        }
    }
    return img;
}

}  // namespace png2amiga::cga_text
