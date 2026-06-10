#include "ted.hpp"

#include "palette.hpp"
#include "pipeline.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <span>

namespace png2amiga::ted {

namespace {

using color_space::OKLab;

// One TED color: its canonical color byte (luma<<4)|chroma plus the linear /
// OKLab views. chroma 0 = black at every luma → collapsed to (0,0).
struct TedColor {
    std::uint8_t luma;    // 0..7
    std::uint8_t chroma;  // 0..15
    Color3f lin;
    OKLab lab;
};

// Build the 121 unique TED colors. chroma 0 collapses to a single black at
// (luma=0,chroma=0); all other (luma,chroma) pairs are distinct.
const std::vector<TedColor>& ted_colors() {
    static const std::vector<TedColor> cache = [] {
        std::vector<TedColor> v;
        v.reserve(121);
        // black once
        {
            auto lin = color_space::srgb_hex_to_linear(palette::kTedPalette[0]);
            v.push_back({0, 0, lin, color_space::linear_to_oklab(lin)});
        }
        for (std::uint8_t luma = 0; luma < 8; ++luma) {
            for (std::uint8_t chroma = 1; chroma < 16; ++chroma) {
                std::size_t i = static_cast<std::size_t>(luma) * 16 + chroma;
                auto lin = color_space::srgb_hex_to_linear(palette::kTedPalette[i]);
                v.push_back({luma, chroma, lin, color_space::linear_to_oklab(lin)});
            }
        }
        return v;
    }();
    return cache;
}

float oklab_error(const Image& a, const Image& b) {
    auto pa = a.pixels();
    auto pb = b.pixels();
    std::size_t n = std::min(pa.size(), pb.size());
    float e = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
        e += color_space::perceptual_distance_sq(pa[i], pb[i]);
    return e;
}

// ---------------------------------------------------------------------------
// TED hires — 320×200, 8×8 cells, 2 colors per cell (C64-hires-like).
//   pixel 0-bit → color {luma=LUMA[6:4], chroma=CHROMA[3:0]}
//   pixel 1-bit → color {luma=LUMA[2:0], chroma=CHROMA[7:4]}
//   LUMA byte   = (lum0<<4)|lum1   CHROMA byte = (chroma1<<4)|chroma0
// ---------------------------------------------------------------------------
Result<EncodeResult> encode_hires(const Image& image, const dither::Settings& settings) {
    constexpr std::size_t W = 320, H = 200, kC = 8, kR = 8;
    constexpr std::size_t cols = W / kC, rows = H / kR;  // 40 × 25
    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                     std::format("ted hires: expected {}x{}, got {}x{}", W, H,
                                                 image.width(), image.height())}};
    }
    const auto& cols_pal = ted_colors();
    const std::size_t N = cols_pal.size();

    std::vector<OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);

    EncodeResult res;
    res.rendered = Image(W, H);
    res.bitmap.assign(8000, 0);
    res.luma.assign(1000, 0);
    res.chroma.assign(1000, 0);

    // Per-cell pair (index 0 = bit-0 color, index 1 = bit-1 color). With a
    // dither method the pair can MIX, so score by distance to the OKLab
    // segment + mixing-noise penalty on the 3×3-blurred target (see
    // color_space::mix_segment_score; λ=0.09375 is the 8×8-cell tuning
    // from c64 hires — endpoint-min picks the two most-DOMINANT colors
    // and adjacent cells over smooth content snap between them, which is
    // the attribute blockiness). Without dither keep endpoint-min on the
    // raw source (static pick, no mixing possible). 7381 pairs × 64 px
    // per cell — parallel over cells.
    constexpr float kPairMixNoiseLambda = 0.09375f;
    const bool mixing = settings.method != dither::Method::none;
    std::vector<OKLab> blurred_lab;
    if (mixing) {
        // 3×3 binomial blur, image-edge replicate (same model as the c64 /
        // thomson cell scorers — per-cell scoring reads a globally-
        // coherent blurred target).
        blurred_lab.resize(W * H);
        static constexpr std::array<std::array<float, 3>, 3> k = {{
            {1.0f / 16, 2.0f / 16, 1.0f / 16},
            {2.0f / 16, 4.0f / 16, 2.0f / 16},
            {1.0f / 16, 2.0f / 16, 1.0f / 16},
        }};
        for (std::size_t y = 0; y < H; ++y) {
            for (std::size_t x = 0; x < W; ++x) {
                OKLab acc{0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(H) - 1);
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(W) - 1);
                        float w =
                            k[static_cast<std::size_t>(dy + 1)][static_cast<std::size_t>(dx + 1)];
                        const auto& s =
                            src_lab[static_cast<std::size_t>(ny) * W + static_cast<std::size_t>(nx)];
                        acc.L += w * s.L;
                        acc.a += w * s.a;
                        acc.b += w * s.b;
                    }
                }
                blurred_lab[y * W + x] = acc;
            }
        }
    }
    std::vector<std::array<std::uint16_t, 2>> cell_pair(cols * rows);
    pipeline::parallel_for(cols * rows, [&](std::size_t cell_idx) {
        std::size_t cy = cell_idx / cols;
        std::size_t cx = cell_idx % cols;
        const auto& tgt = mixing ? blurred_lab : src_lab;
        std::array<OKLab, kC * kR> cell{};
        for (std::size_t py = 0; py < kR; ++py)
            for (std::size_t px = 0; px < kC; ++px)
                cell[py * kC + px] = tgt[(cy * kR + py) * W + (cx * kC + px)];
        float best = std::numeric_limits<float>::infinity();
        std::array<std::uint16_t, 2> best_pair{0, 0};
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t j = i; j < N; ++j) {
                const auto& a = cols_pal[i].lab;
                const auto& b = cols_pal[j].lab;
                float total = 0.0f;
                if (mixing) {
                    for (auto& t : cell)
                        total += color_space::mix_segment_score(t, a, b, kPairMixNoiseLambda);
                } else {
                    for (auto& t : cell)
                        total += std::min(color_space::fma_dist_sq(t, a),
                                          color_space::fma_dist_sq(t, b));
                }
                if (total < best) {
                    best = total;
                    best_pair = {static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(j)};
                }
            }
        }
        cell_pair[cell_idx] = best_pair;
    });

    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        const auto& pair = cell_pair[(y / kR) * cols + (x / kC)];
        std::array<OKLab, 2> cp{cols_pal[pair[0]].lab, cols_pal[pair[1]].lab};
        std::size_t chosen_idx = 0;
        OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target, std::span<const OKLab>(cp), x, y, settings.strength, 0,
            chosen_idx, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_idx);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    for (std::size_t cy = 0; cy < rows; ++cy) {
        for (std::size_t cx = 0; cx < cols; ++cx) {
            std::size_t cell = cy * cols + cx;
            const auto& pair = cell_pair[cell];
            const auto& c0 = cols_pal[pair[0]];  // bit 0
            const auto& c1 = cols_pal[pair[1]];  // bit 1
            res.luma[cell] = static_cast<std::uint8_t>(((c0.luma & 0x7) << 4) | (c1.luma & 0x7));
            res.chroma[cell] =
                static_cast<std::uint8_t>(((c1.chroma & 0xF) << 4) | (c0.chroma & 0xF));
            for (std::size_t py = 0; py < kR; ++py) {
                std::uint8_t row = 0;
                for (std::size_t px = 0; px < kC; ++px) {
                    std::size_t x = cx * kC + px, y = cy * kR + py;
                    std::uint8_t q = static_cast<std::uint8_t>(indices[y * W + x] & 0x1);
                    row = static_cast<std::uint8_t>((row << 1) | q);
                    res.rendered[x, y] = q ? c1.lin : c0.lin;
                }
                res.bitmap[cell * kR + py] = row;
            }
        }
    }
    res.total_error = oklab_error(image, res.rendered);
    return res;
}

// ---------------------------------------------------------------------------
// TED multicolor — 160×200, 4×8 cells, 4 colors per cell:
//   00 → GLOBAL bg0 ($FF15)   11 → GLOBAL bg1 ($FF16)
//   01 → per-cell {luma=LUMA[2:0], chroma=CHROMA[7:4]}
//   10 → per-cell {luma=LUMA[6:4], chroma=CHROMA[3:0]}
// ---------------------------------------------------------------------------
Result<EncodeResult> encode_multicolor(const Image& image, const dither::Settings& settings) {
    constexpr std::size_t W = 160, H = 200, kC = 4, kR = 8;
    constexpr std::size_t cols = W / kC, rows = H / kR;  // 40 × 25
    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                     std::format("ted multicolor: expected {}x{}, got {}x{}", W, H,
                                                 image.width(), image.height())}};
    }
    const auto& cols_pal = ted_colors();
    const std::size_t N = cols_pal.size();

    std::vector<OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);

    // Choose 2 global background colors: the two most-representative TED
    // colors over the whole image. Greedy: pick the nearest-sum-minimizing
    // first color, then the second that most reduces residual error. (Same
    // shape as a 2-color global-bg search; mirrors c64-multicolor's single
    // global bg, extended to a pair.)
    std::uint16_t g0 = 0, g1 = 0;
    {
        // First global: minimizes total nearest distance alone.
        float best = std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < N; ++i) {
            float total = 0.0f;
            const auto& a = cols_pal[i].lab;
            for (auto& t : src_lab)
                total += color_space::fma_dist_sq(t, a);
            if (total < best) {
                best = total;
                g0 = static_cast<std::uint16_t>(i);
            }
        }
        // Second global: minimizes min(d(g0), d(g1)) over the image.
        best = std::numeric_limits<float>::infinity();
        const auto& a = cols_pal[g0].lab;
        for (std::size_t j = 0; j < N; ++j) {
            if (j == g0) continue;
            float total = 0.0f;
            const auto& b = cols_pal[j].lab;
            for (auto& t : src_lab)
                total += std::min(color_space::fma_dist_sq(t, a), color_space::fma_dist_sq(t, b));
            if (total < best) {
                best = total;
                g1 = static_cast<std::uint16_t>(j);
            }
        }
    }

    EncodeResult res;
    res.rendered = Image(W, H);
    res.bitmap.assign(8000, 0);
    res.luma.assign(1000, 0);
    res.chroma.assign(1000, 0);
    res.bg0 = static_cast<std::uint8_t>((cols_pal[g0].luma << 4) | cols_pal[g0].chroma);
    res.bg1 = static_cast<std::uint8_t>((cols_pal[g1].luma << 4) | cols_pal[g1].chroma);

    // Per-cell: choose the 2 per-cell colors (pc01, pc10). The 4-color cell
    // candidate set is {g0, pc01, pc10, g1}; brute force pc01/pc10 over 121².
    std::vector<std::array<std::uint16_t, 2>> cell_pc(cols * rows);
    for (std::size_t cy = 0; cy < rows; ++cy) {
        for (std::size_t cx = 0; cx < cols; ++cx) {
            std::array<OKLab, kC * kR> cell{};
            for (std::size_t py = 0; py < kR; ++py)
                for (std::size_t px = 0; px < kC; ++px)
                    cell[py * kC + px] = src_lab[(cy * kR + py) * W + (cx * kC + px)];
            const auto& lg0 = cols_pal[g0].lab;
            const auto& lg1 = cols_pal[g1].lab;
            float best = std::numeric_limits<float>::infinity();
            std::array<std::uint16_t, 2> best_pc{g0, g1};
            for (std::size_t a = 0; a < N; ++a) {
                const auto& la = cols_pal[a].lab;
                for (std::size_t b = 0; b < N; ++b) {
                    const auto& lb = cols_pal[b].lab;
                    float total = 0.0f;
                    for (auto& t : cell) {
                        float d = color_space::fma_dist_sq(t, lg0);
                        d = std::min(d, color_space::fma_dist_sq(t, lg1));
                        d = std::min(d, color_space::fma_dist_sq(t, la));
                        d = std::min(d, color_space::fma_dist_sq(t, lb));
                        total += d;
                    }
                    if (total < best) {
                        best = total;
                        best_pc = {static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(b)};
                    }
                }
            }
            cell_pc[cy * cols + cx] = best_pc;
        }
    }

    // Dither pass: 4-color candidate per cell {g0(00), pc01(01), pc10(10),
    // g1(11)}. The 2-bit code is mapped on output below.
    std::vector<std::uint8_t> codes(W * H, 0);
    auto pick = [&](const OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        const auto& pc = cell_pc[(y / kR) * cols + (x / kC)];
        // Candidate order matches the 2-bit code: 00,01,10,11.
        std::array<OKLab, 4> cand{cols_pal[g0].lab, cols_pal[pc[0]].lab, cols_pal[pc[1]].lab,
                                  cols_pal[g1].lab};
        std::size_t chosen_idx = 0;
        OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target, std::span<const OKLab>(cand), x, y, settings.strength, 0,
            chosen_idx, chosen);
        codes[y * W + x] = static_cast<std::uint8_t>(chosen_idx);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    auto code_color = [&](std::size_t cell, std::uint8_t code) -> const TedColor& {
        const auto& pc = cell_pc[cell];
        switch (code) {
        case 0: return cols_pal[g0];
        case 1: return cols_pal[pc[0]];
        case 2: return cols_pal[pc[1]];
        default: return cols_pal[g1];
        }
    };

    for (std::size_t cy = 0; cy < rows; ++cy) {
        for (std::size_t cx = 0; cx < cols; ++cx) {
            std::size_t cell = cy * cols + cx;
            const auto& pc = cell_pc[cell];
            const auto& c01 = cols_pal[pc[0]];  // code 01
            const auto& c10 = cols_pal[pc[1]];  // code 10
            // 01 → luma LUMA[2:0], chroma CHROMA[7:4]
            // 10 → luma LUMA[6:4], chroma CHROMA[3:0]
            res.luma[cell] = static_cast<std::uint8_t>(((c10.luma & 0x7) << 4) | (c01.luma & 0x7));
            res.chroma[cell] =
                static_cast<std::uint8_t>(((c01.chroma & 0xF) << 4) | (c10.chroma & 0xF));
            for (std::size_t py = 0; py < kR; ++py) {
                std::uint8_t row = 0;
                for (std::size_t px = 0; px < kC; ++px) {
                    std::size_t x = cx * kC + px, y = cy * kR + py;
                    std::uint8_t code = static_cast<std::uint8_t>(codes[y * W + x] & 0x3);
                    row = static_cast<std::uint8_t>((row << 2) | code);
                    res.rendered[x, y] = code_color(cell, code).lin;
                }
                res.bitmap[cell * kR + py] = row;
            }
        }
    }
    res.total_error = oklab_error(image, res.rendered);
    return res;
}

}  // namespace

Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings) {
    if (mode == amiga::Mode::ted_hires) return encode_hires(image, settings);
    if (mode == amiga::Mode::ted_multicolor) return encode_multicolor(image, settings);
    return std::unexpected{Error{ErrorCode::unsupported_mode, "ted::encode: not a TED mode"}};
}

}  // namespace png2amiga::ted
