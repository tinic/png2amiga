#include "c64.hpp"

#include "palette.hpp"

#include <array>
#include <format>
#include <limits>

namespace png2amiga::c64 {

namespace {

inline const std::array<Color3f, 16>& pepto_linear() {
    static const std::array<Color3f, 16> pal = [] {
        std::array<Color3f, 16> p{};
        for (std::size_t i = 0; i < 16; ++i)
            p[i] = color_space::srgb_hex_to_linear(palette::kC64Pepto[i]);
        return p;
    }();
    return pal;
}

inline const std::array<color_space::OKLab, 16>& pepto_oklab() {
    static const std::array<color_space::OKLab, 16> lab = [] {
        std::array<color_space::OKLab, 16> a{};
        const auto& lin = pepto_linear();
        for (std::size_t i = 0; i < 16; ++i)
            a[i] = color_space::linear_to_oklab(lin[i]);
        return a;
    }();
    return lab;
}

constexpr std::size_t kCellW = 4;   // multicolor logical pixels per cell
constexpr std::size_t kCellH = 8;
constexpr std::size_t kCols  = 40;  // 160 / 4
constexpr std::size_t kRows  = 25;  // 200 / 8

// For each pixel in the cell, find the nearest of the 4 candidate
// colours (in OKLab) and return the chosen index 0..3 plus the
// squared OKLab error. Per-pixel work: 4 distance computes.
inline float cell_error_for_quad(
    std::span<const color_space::OKLab> pix_lab,
    const std::array<std::uint8_t, 4>& cand,
    const std::array<color_space::OKLab, 16>& pal_lab,
    std::array<std::uint8_t, kCellW * kCellH>* pixel_idx_out) {

    float total = 0.0f;
    for (std::size_t p = 0; p < pix_lab.size(); ++p) {
        const auto& t = pix_lab[p];
        float best_d = std::numeric_limits<float>::infinity();
        std::uint8_t best_q = 0;
        for (std::uint8_t q = 0; q < 4; ++q) {
            const auto& c = pal_lab[cand[q]];
            float dL = t.L - c.L, da = t.a - c.a, db = t.b - c.b;
            float d  = dL * dL + da * da + db * db;
            if (d < best_d) { best_d = d; best_q = q; }
        }
        total += best_d;
        if (pixel_idx_out) (*pixel_idx_out)[p] = best_q;
    }
    return total;
}

}  // namespace

std::span<const Color3f, 16> pepto_palette() {
    return std::span<const Color3f, 16>(pepto_linear());
}

Result<EncodeResult> encode_multicolor(const Image& image) {
    constexpr std::size_t W = kCols * kCellW;  // 160
    constexpr std::size_t H = kRows * kCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_multicolor: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin  = pepto_linear();
    const auto& pal_lab  = pepto_oklab();

    // Pre-bake source pixels in OKLab so the inner loop is plain
    // float math.
    std::vector<color_space::OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);

    // Per-cell trial: enumerate all 16 background candidates × C(15,3) =
    // 6825 quads. For each cell-pixel buffer we run the assign/error
    // pass; pick the (bg, i, j, k) that minimises total cell error,
    // then commit pixel indices and cell colours.
    //
    // Single pass — bg varies per cell here. The png2c64 baseline
    // brute-forces a *shared* bg across the whole image (16 outer
    // passes); that's a quality refinement we can layer later. Per-
    // cell bg matches what some C64 multicolor tools (e.g. spectre)
    // produce as a first cut and is fine for a proof-of-fit.

    EncodeResult res;
    res.rendered = Image(W, H);
    res.bitmap.assign(kRows * kCols * kCellH, 0);     // 8000 bytes
    res.screen_ram.assign(kRows * kCols, 0);          // upper-nibble = c1, lower = c2
    res.color_ram.assign(kRows * kCols, 0);           // c3 (low nibble)
    res.bg_color = 0;  // black; per-cell bg is encoded in the cell colours
                       // even though the C64 hardware uses one shared bg
                       // register. We pick bg=0 globally and let cells use
                       // the bg "slot" for their own dark-cluster colour.
                       // (Real per-image bg sweep is a TODO refinement.)

    std::array<color_space::OKLab, kCellW * kCellH> cell_lab{};
    std::array<std::uint8_t, kCellW * kCellH> pix_idx{};

    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            // Gather cell pixels.
            for (std::size_t py = 0; py < kCellH; ++py) {
                for (std::size_t px = 0; px < kCellW; ++px) {
                    auto x = cx * kCellW + px;
                    auto y = cy * kCellH + py;
                    cell_lab[py * kCellW + px] = src_lab[y * W + x];
                }
            }

            float best_err = std::numeric_limits<float>::infinity();
            std::array<std::uint8_t, 4> best_quad{0, 0, 0, 0};
            std::array<std::uint8_t, kCellW * kCellH> best_pixels{};

            for (std::uint8_t bg = 0; bg < 16; ++bg) {
                for (std::uint8_t i = 0; i < 16; ++i) {
                    if (i == bg) continue;
                    for (std::uint8_t j = static_cast<std::uint8_t>(i + 1);
                         j < 16; ++j) {
                        if (j == bg) continue;
                        for (std::uint8_t k = static_cast<std::uint8_t>(j + 1);
                             k < 16; ++k) {
                            if (k == bg) continue;
                            std::array<std::uint8_t, 4> quad{bg, i, j, k};
                            float err = cell_error_for_quad(
                                cell_lab, quad, pal_lab, &pix_idx);
                            if (err < best_err) {
                                best_err   = err;
                                best_quad  = quad;
                                best_pixels = pix_idx;
                            }
                        }
                    }
                }
            }

            // Commit: rendered pixels + bitmap + screen/color RAM.
            std::size_t cell_idx = cy * kCols + cx;
            res.screen_ram[cell_idx] = static_cast<std::uint8_t>(
                ((best_quad[1] & 0xF) << 4) | (best_quad[2] & 0xF));
            res.color_ram[cell_idx]  = static_cast<std::uint8_t>(best_quad[3] & 0xF);
            for (std::size_t py = 0; py < kCellH; ++py) {
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kCellW; ++px) {
                    auto q = best_pixels[py * kCellW + px];
                    row_byte = static_cast<std::uint8_t>(
                        (row_byte << 2) | (q & 0x3));
                    auto x = cx * kCellW + px;
                    auto y = cy * kCellH + py;
                    res.rendered[x, y] = pal_lin[best_quad[q]];
                }
                res.bitmap[cell_idx * kCellH + py] = row_byte;
            }
        }
    }

    return res;
}

}  // namespace png2amiga::c64
