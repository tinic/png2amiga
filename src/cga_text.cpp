#include "cga_text.hpp"

#include "cga_font.hpp"
#include "color_space.hpp"
#include "palette.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#include <unordered_map>
#include <utility>

namespace png2amiga::cga_text {

namespace {

// Precompute OKLab for each of the 16 CGA master colors so cell-matching
// distance calculations don't recompute per iteration.
struct CgaPaletteLab {
    std::array<color_space::OKLab, 16> lab;
    std::array<Color3f, 16> rgb;
    CgaPaletteLab() {
        for (std::size_t i = 0; i < 16; ++i) {
            rgb[i] = color_space::srgb_hex_to_linear(palette::kCgaHw[i]);
            lab[i] = color_space::linear_to_oklab(rgb[i]);
        }
    }
};

inline const CgaPaletteLab& palette_lab() {
    static const CgaPaletteLab p;
    return p;
}

}  // namespace

Result<CgaTextResult>
encode(const Image& image, amiga::Mode mode,
       std::span<const std::uint8_t> restrict_chars,
       std::span<const Color3f> palette16,
       int fixed_offset) {

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

    // CGA 80x100 : 640x200, 8x8 font, 2-scanline rows.
    const palette::FontRef& font = palette::kFontCga8x8;
    const std::size_t disp_w = 640;
    const std::size_t disp_h = 200u;
    const std::size_t cell_h = 2u;
    const std::size_t cols = 80;
    const std::size_t rows = disp_h / cell_h;

    if (image.width() != disp_w || image.height() != disp_h) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("cga_text::encode: expected {}x{} input, got {}x{}",
                        disp_w, disp_h, image.width(), image.height()),
        }};
    }

    // Candidate character set. If empty, use all 256.
    std::vector<std::uint8_t> chars;
    if (restrict_chars.empty()) {
        chars.resize(256);
        for (int i = 0; i < 256; ++i) chars[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(i);
    } else {
        chars.assign(restrict_chars.begin(), restrict_chars.end());
    }

    // Precompute OKLab for every source cell's pixels (for reuse across
    // glyph/fg/bg trials). If the caller supplied a custom 16-color palette
    // (EGA text modes picking from the 64-entry gamut), build an ad-hoc
    // lookup table; otherwise use the cached fixed-CGA-master palette.
    CgaPaletteLab custom;
    if (!palette16.empty()) {
        for (std::size_t i = 0; i < 16; ++i) {
            custom.rgb[i] = palette16[i];
            custom.lab[i] = color_space::linear_to_oklab(palette16[i]);
        }
    }
    const CgaPaletteLab& pal = palette16.empty() ? palette_lab() : custom;

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
    result.cell_height_scanlines = cell_h;
    result.font_height = font.glyph_height;
    result.scanline_offset = static_cast<std::uint8_t>(offset);

    std::atomic<std::size_t> next_cell{0};
    std::atomic<double> atomic_err{0.0};

    auto worker = [&]() {
        while (true) {
            auto linear = next_cell.fetch_add(1);
            if (linear >= cols * rows) break;
            auto col = linear % cols;
            auto row = linear / cols;

            // Pull the cell's OKLab values (8 × cell_h pixels).
            std::array<color_space::OKLab, 16> cell_lab;  // max 8×2
            for (std::size_t py = 0; py < cell_h; ++py) {
                for (std::size_t px = 0; px < 8; ++px) {
                    auto img_x = col * 8 + px;
                    auto img_y = row * cell_h + py;
                    cell_lab[py * 8 + px] =
                        color_space::linear_to_oklab(image[img_x, img_y]);
                }
            }

            std::uint8_t best_ch = 0, best_fg = 15, best_bg = 0;
            float best_err = std::numeric_limits<float>::infinity();

            // Brute-force: for each glyph candidate, try all 16×16 fg/bg
            // pairs. For each trial, the rendered cell is 8×cell_h pixels
            // where each pixel is fg (if pattern bit is 1) or bg.
            //
            // Precompute, for each pattern cell, the list of fg-mask
            // indices and bg-mask indices, so we can compute per-trial
            // error as sum of per-pixel distances to (lab_fg for fg bits,
            // lab_bg for bg bits).
            //
            // Equivalent faster: for each pixel, compute dist² to EACH of
            // the 16 palette entries (16 values × cell pixel count); then
            // given pattern bits we pick either fg[i] or bg[i] per pixel
            // and look those up.
            std::array<std::array<float, 16>, 16> pix_d{};  // [pixel][color]
            std::array<float, 16> total_sum{};              // [color]
            for (std::size_t p = 0; p < 8 * cell_h; ++p) {
                for (std::size_t c = 0; c < 16; ++c) {
                    auto& a = cell_lab[p]; auto& b = pal.lab[c];
                    float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
                    float d = dL * dL + da * da + db * db;
                    pix_d[p][c] = d;
                    total_sum[c] += d;
                }
            }

            // For each candidate pattern, find the optimal (fg, bg) pair.
            //
            // Trick: fg and bg partition the 8*cell_h pixels into disjoint
            // sets. The per-trial error splits as
            //     err(pattern, fg, bg) = Σ_{p∈fg_mask} pix_d[p][fg]
            //                          + Σ_{p∉fg_mask} pix_d[p][bg]
            //                          = sum_fg[fg] + sum_bg[bg]
            // where sum_bg[c] = total_sum[c] − sum_fg[c]. The two sums are
            // independent, so the best pair is (argmin sum_fg, argmin sum_bg)
            // — 16 comparisons per role instead of 256 full trials.
            //
            // We still iterate unique cell patterns only (built once per
            // offset outside the per-cell loop, with fg_mask precomputed).
            // Top scanline bits high..low map to pixels 0..7 left-to-right.
            for (auto& cand : candidates) {
                auto fg_mask = cand.fg_mask;
                std::array<float, 16> sum_fg{};
                // Iterate set bits of fg_mask: each bit is one fg pixel
                // index p in [0, 8*cell_h).
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
                        min_fg = sum_fg[c]; fg_idx = c;
                    }
                    float sb = total_sum[c] - sum_fg[c];
                    if (sb < min_bg) {
                        min_bg = sb; bg_idx = c;
                    }
                }
                float err = min_fg + min_bg;
                if (err < best_err) {
                    best_err = err;
                    best_ch = cand.ch;
                    best_fg = fg_idx;
                    best_bg = bg_idx;
                }
            }

            auto off = (row * cols + col) * 2;
            result.data[off] = best_ch;
            // attr: high nibble = bg, low nibble = fg. To use all 16 bg
            // colors (not blink), the user must disable blink via bit 5 of
            // CGA mode register (0x3D8). Our output assumes blink disabled.
            result.data[off + 1] =
                static_cast<std::uint8_t>((best_bg << 4) | best_fg);

            double old = atomic_err.load(std::memory_order_relaxed);
            while (!atomic_err.compare_exchange_weak(
                old, old + static_cast<double>(best_err))) {}
        }
    };

#ifdef __EMSCRIPTEN__
        // Emscripten without -pthread has no real std::thread; constructing
        // one aborts the WASM VM. Run the worker lambda inline on the main
        // thread — single-threaded encode is slower but correct.
        worker();
#else
        auto n = std::max<unsigned>(1, std::thread::hardware_concurrency());
        std::vector<std::jthread> threads;
        threads.reserve(n);
        for (unsigned i = 0; i < n; ++i) threads.emplace_back(worker);
        threads.clear();  // join on destruction
#endif

        result.total_error = static_cast<float>(atomic_err.load());
        for (std::size_t i = 0; i < 16; ++i) result.palette[i] = pal.rgb[i];
        if (result.total_error < best_result.total_error) {
            best_result = std::move(result);
        }
    }  // end scanline-offset loop
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
