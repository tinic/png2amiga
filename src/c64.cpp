#include "c64.hpp"

#include "palette.hpp"
#include "petscii_rom.hpp"

#include <array>
#include <bit>
#include <format>
#include <limits>
#include <unordered_map>

namespace png2amiga::c64 {

Metric parse_metric(std::string_view s) noexcept {
    if (s == "blur") return Metric::blur;
    if (s == "mse")  return Metric::mse;
    return Metric::mse;
}

std::string_view metric_name(Metric m) noexcept {
    switch (m) {
    case Metric::blur: return "blur";
    case Metric::mse:  return "mse";
    }
    return "mse";
}

Palette parse_palette(std::string_view s) noexcept {
    if (s == "pepto")    return Palette::pepto;
    if (s == "vice")     return Palette::vice;
    if (s == "colodore") return Palette::colodore;
    if (s == "deekay")   return Palette::deekay;
    if (s == "godot")    return Palette::godot;
    if (s == "c64wiki" || s == "wiki") return Palette::c64wiki;
    if (s == "levy")     return Palette::levy;
    return Palette::colodore;
}

std::string_view palette_name(Palette p) noexcept {
    switch (p) {
    case Palette::pepto:    return "pepto";
    case Palette::vice:     return "vice";
    case Palette::colodore: return "colodore";
    case Palette::deekay:   return "deekay";
    case Palette::godot:    return "godot";
    case Palette::c64wiki:  return "c64wiki";
    case Palette::levy:     return "levy";
    }
    return "pepto";
}

namespace {

const std::array<std::uint32_t, 16>& palette_hex(Palette p) {
    switch (p) {
    case Palette::pepto:    return palette::kC64Pepto;
    case Palette::vice:     return palette::kC64Vice;
    case Palette::colodore: return palette::kC64Colodore;
    case Palette::deekay:   return palette::kC64Deekay;
    case Palette::godot:    return palette::kC64Godot;
    case Palette::c64wiki:  return palette::kC64Wiki;
    case Palette::levy:     return palette::kC64Levy;
    }
    return palette::kC64Pepto;
}

const std::array<Color3f, 16>& palette_linear(Palette p) {
    static const auto cache = [] {
        std::array<std::array<Color3f, 16>, 7> all{};
        Palette ps[] = {Palette::pepto, Palette::vice, Palette::colodore,
                        Palette::deekay, Palette::godot, Palette::c64wiki,
                        Palette::levy};
        for (auto pp : ps) {
            const auto& hex = palette_hex(pp);
            for (std::size_t i = 0; i < 16; ++i)
                all[static_cast<std::size_t>(pp)][i] =
                    color_space::srgb_hex_to_linear(hex[i]);
        }
        return all;
    }();
    return cache[static_cast<std::size_t>(p)];
}

const std::array<color_space::OKLab, 16>& palette_oklab(Palette p) {
    static const auto cache = [] {
        std::array<std::array<color_space::OKLab, 16>, 7> all{};
        for (std::size_t i = 0; i < 7; ++i) {
            const auto& lin = palette_linear(static_cast<Palette>(i));
            for (std::size_t j = 0; j < 16; ++j)
                all[i][j] = color_space::linear_to_oklab(lin[j]);
        }
        return all;
    }();
    return cache[static_cast<std::size_t>(p)];
}

constexpr std::size_t kCellW = 4;   // multicolor logical pixels per cell
constexpr std::size_t kCellH = 8;
constexpr std::size_t kCols  = 40;  // 160 / 4
constexpr std::size_t kRows  = 25;  // 200 / 8

// 3×3 binomial blur kernel — same as cga_text / petscii. sRGB
// (gamma-encoded) space matches what the CRT emits.
constexpr std::array<std::array<float, 3>, 3> kCellBlur = {{
    {1.0f/16, 2.0f/16, 1.0f/16},
    {2.0f/16, 4.0f/16, 2.0f/16},
    {1.0f/16, 2.0f/16, 1.0f/16},
}};

// 9-tap replicate-padded blur table for an arbitrary cell W × H.
template <std::size_t W, std::size_t H>
struct CellTaps {
    struct T { std::uint8_t q; float w; };
    std::array<std::array<T, 9>, W * H> taps;

    constexpr CellTaps() : taps{} {
        for (std::size_t py = 0; py < H; ++py) {
            for (std::size_t px = 0; px < W; ++px) {
                std::size_t out = py * W + px;
                std::size_t k = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = std::clamp(static_cast<int>(py) + dy,
                                        0, static_cast<int>(H) - 1);
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = std::clamp(static_cast<int>(px) + dx,
                                            0, static_cast<int>(W) - 1);
                        taps[out][k++] = {
                            static_cast<std::uint8_t>(
                                static_cast<std::size_t>(ny) * W +
                                static_cast<std::size_t>(nx)),
                            kCellBlur[static_cast<std::size_t>(dy + 1)]
                                     [static_cast<std::size_t>(dx + 1)]};
                    }
                }
            }
        }
    }
};

// Legacy OKLab² nearest-pick helper for hires / FLI / AFLI. These
// modes still score in OKLab pending per-metric integration; the
// Metric parameter is plumbed but ignored. The hot path is the
// inner-loop sum-of-squared-distance — cheap.
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
            float d = dL * dL + da * da + db * db;
            if (d < best_d) { best_d = d; best_q = q; }
        }
        total += best_d;
        if (pixel_idx_out) (*pixel_idx_out)[p] = best_q;
    }
    return total;
}

// Per-cell metric scorer. Phase 1 picks each pixel's nearest of N
// candidates (squared distance in whatever space the caller fed us
// — c64 modes feed OKLab via Color3f). Phase 2 computes:
//   mse  — per-pixel squared error sum.
//   blur — MSE between pre-blurred source and post-blurred rendered.
template <std::size_t N, std::size_t Px>
inline float score_cell(
    const std::array<Color3f, Px>& raw,
    const std::array<Color3f, Px>& blurred_src,
    const std::array<typename CellTaps<4, 8>::T, 9>* /*taps*/,  // see overloads
    const std::array<Color3f, N>& cand,
    Metric metric,
    std::array<std::uint8_t, Px>* idx_out,
    auto&& tap_lookup) {

    // Phase 1: per-pixel nearest-of-N in sRGB MSE.
    std::array<std::uint8_t, Px> idx{};
    std::array<Color3f, Px> rendered{};
    for (std::size_t p = 0; p < Px; ++p) {
        float best = std::numeric_limits<float>::infinity();
        std::uint8_t best_q = 0;
        for (std::uint8_t q = 0; q < N; ++q) {
            float dr = raw[p].r - cand[q].r;
            float dg = raw[p].g - cand[q].g;
            float db = raw[p].b - cand[q].b;
            float d = dr * dr + dg * dg + db * db;
            if (d < best) { best = d; best_q = q; }
        }
        idx[p] = best_q;
        rendered[p] = cand[best_q];
    }
    if (idx_out) *idx_out = idx;

    // Phase 2: score.
    float err = 0.0f;
    switch (metric) {
    case Metric::mse:
        for (std::size_t p = 0; p < Px; ++p) {
            float dr = raw[p].r - rendered[p].r;
            float dg = raw[p].g - rendered[p].g;
            float db = raw[p].b - rendered[p].b;
            err += dr * dr + dg * dg + db * db;
        }
        return err;
    case Metric::blur: {
        // Blur the rendered cell using the same 9-tap kernel; MSE
        // against pre-blurred source. tap_lookup yields the 9 taps
        // for pixel p — caller-provided so this works for any
        // cell size.
        for (std::size_t p = 0; p < Px; ++p) {
            Color3f b{0, 0, 0};
            auto taps = tap_lookup(p);
            for (auto& t : taps) {
                b.r += t.w * rendered[t.q].r;
                b.g += t.w * rendered[t.q].g;
                b.b += t.w * rendered[t.q].b;
            }
            float dr = blurred_src[p].r - b.r;
            float dg = blurred_src[p].g - b.g;
            float db = blurred_src[p].b - b.b;
            err += dr * dr + dg * dg + db * db;
        }
        return err;
    }
    }
    return err;
}

}  // namespace

std::span<const Color3f, 16> palette_colors(Palette p) {
    return std::span<const Color3f, 16>(palette_linear(p));
}

Result<EncodeResult> encode_multicolor(const Image& image, Palette pal,
                                        const dither::Settings& settings,
                                        Metric metric) {
    (void)metric;  // TODO: per-metric brute-force scoring; current
                   // path uses sRGB MSE-equivalent (OKLab² nearest)
                   // for the per-cell quad pick regardless of metric.
    constexpr std::size_t W = kCols * kCellW;  // 160
    constexpr std::size_t H = kRows * kCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_multicolor: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin = palette_linear(pal);
    const auto& pal_lab = palette_oklab(pal);

    // All cell-mode scoring (mse / blur / ssim) operates in OKLab.
    // The score_cell helper does generic 3-vector math; storing
    // OKLab values in Color3f's .r/.g/.b lets the same template
    // handle either space — we just feed it OKLab here. (Earlier
    // attempts in sRGB lost noticeably on c64 cell modes — sRGB
    // nearest-pick is perceptually skewed by the gamma encoding.)
    std::vector<color_space::OKLab> src_lab(W * H);
    std::vector<Color3f>             src_s(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x) {
            auto lab = color_space::linear_to_oklab(image[x, y]);
            src_lab[y * W + x] = lab;
            src_s[y * W + x] = {lab.L, lab.a, lab.b};
        }
    std::array<Color3f, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i) {
        auto lab = color_space::linear_to_oklab(pal_lin[i]);
        pal_s[i] = {lab.L, lab.a, lab.b};
    }

    static constexpr CellTaps<kCellW, kCellH> taps_mc{};
    auto tap_lookup = [](std::size_t p) -> std::span<const CellTaps<kCellW, kCellH>::T, 9> {
        return std::span<const CellTaps<kCellW, kCellH>::T, 9>(taps_mc.taps[p]);
    };

    // Per-cell trial: enumerate all 16 backgrounds × C(15,3) = 6825
    // quads. The brute force scores each quad under the chosen
    // metric; per-cell bg.

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

    // Pass 1: per-cell brute-force quad pick. Two paths:
    //   metric=mse  → OKLab² nearest sum (matches png2c64).
    //   metric=blur → sRGB blur of (raw, rendered) cells, MSE on
    //                 blurred. Empirically beats OKLab-blur on
    //                 chunky display content (cga-text result).
    //   metric=ssim → sRGB SSIM single-window.
    std::vector<std::array<std::uint8_t, 4>> cell_quad(kRows * kCols);
    constexpr std::size_t kCellPx = kCellW * kCellH;  // 32
    std::array<color_space::OKLab, kCellPx> cell_lab{};
    std::array<Color3f, kCellPx> raw{};
    std::array<Color3f, kCellPx> blurred_src{};
    std::array<std::uint8_t, kCellPx> _idx_scratch{};

    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            for (std::size_t py = 0; py < kCellH; ++py) {
                for (std::size_t px = 0; px < kCellW; ++px) {
                    auto src_idx = (cy * kCellH + py) * W + (cx * kCellW + px);
                    cell_lab[py * kCellW + px] = src_lab[src_idx];
                    raw[py * kCellW + px]      = src_s[src_idx];
                }
            }
            // Pre-blur the source cell once (used by metric=blur).
            if (metric == Metric::blur) {
                for (std::size_t p = 0; p < kCellPx; ++p) {
                    Color3f b{0, 0, 0};
                    for (auto& t : taps_mc.taps[p]) {
                        b.r += t.w * raw[t.q].r;
                        b.g += t.w * raw[t.q].g;
                        b.b += t.w * raw[t.q].b;
                    }
                    blurred_src[p] = b;
                }
            }
            float best_err = std::numeric_limits<float>::infinity();
            std::array<std::uint8_t, 4> best_quad{0, 0, 0, 0};
            for (std::uint8_t bg = 0; bg < 16; ++bg) {
                for (std::uint8_t i = 0; i < 16; ++i) {
                    if (i == bg) continue;
                    for (std::uint8_t j = static_cast<std::uint8_t>(i + 1);
                         j < 16; ++j) {
                        if (j == bg) continue;
                        for (std::uint8_t k = static_cast<std::uint8_t>(j + 1);
                             k < 16; ++k) {
                            if (k == bg) continue;
                            float err;
                            if (metric == Metric::mse) {
                                std::array<std::uint8_t, 4> q{bg, i, j, k};
                                err = cell_error_for_quad(
                                    cell_lab, q, pal_lab, &_idx_scratch);
                            } else {
                                std::array<Color3f, 4> cand{
                                    pal_s[bg], pal_s[i], pal_s[j], pal_s[k],
                                };
                                err = score_cell<4, kCellPx>(
                                    raw, blurred_src, nullptr, cand,
                                    metric, nullptr, tap_lookup);
                            }
                            if (err < best_err) {
                                best_err  = err;
                                best_quad = {bg, i, j, k};
                            }
                        }
                    }
                }
            }
            cell_quad[cy * kCols + cx] = best_quad;
        }
    }

    // Pass 2: per-pixel index pick with dither. The pick callback
    // looks up the cell's 4-colour palette and routes through
    // pick_palette_index_with_ostro — same code path the Yliluoma /
    // ED / Ostromoukhov families use elsewhere. diffuse_raw_buffer
    // owns the err_buf, ordered-bias, Riemersma queue, and structure
    // map, so every method it supports works here.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const color_space::OKLab& target,
                    std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kCellH;
        std::size_t cx = x / kCellW;
        const auto& quad = cell_quad[cy * kCols + cx];
        std::array<color_space::OKLab, 4> cp{
            pal_lab[quad[0]], pal_lab[quad[1]],
            pal_lab[quad[2]], pal_lab[quad[3]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target,
            std::span<const color_space::OKLab>(cp),
            x, y, settings.strength, /*k_min=*/0,
            chosen_index, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_index);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    // Render + pack bitmap / screen / color RAM from the dithered
    // per-pixel indices.
    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            std::size_t cell_idx = cy * kCols + cx;
            const auto& quad = cell_quad[cell_idx];
            res.screen_ram[cell_idx] = static_cast<std::uint8_t>(
                ((quad[1] & 0xF) << 4) | (quad[2] & 0xF));
            res.color_ram[cell_idx] = static_cast<std::uint8_t>(quad[3] & 0xF);
            for (std::size_t py = 0; py < kCellH; ++py) {
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kCellW; ++px) {
                    auto x = cx * kCellW + px;
                    auto y = cy * kCellH + py;
                    auto q = static_cast<std::uint8_t>(indices[y * W + x] & 0x3);
                    row_byte = static_cast<std::uint8_t>(
                        (row_byte << 2) | q);
                    res.rendered[x, y] = pal_lin[quad[q]];
                }
                res.bitmap[cell_idx * kCellH + py] = row_byte;
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-hires: 320×200, 8×8 cells, 2 colours per cell (no shared bg).
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kHiCellW = 8;
constexpr std::size_t kHiCellH = 8;
constexpr std::size_t kHiCols  = 40;   // 320 / 8
constexpr std::size_t kHiRows  = 25;   // 200 / 8

// Per-pixel error against a 2-colour pair, returning the chosen index
// 0/1 plus the squared OKLab error.
inline float cell_error_for_pair(
    std::span<const color_space::OKLab> pix_lab,
    std::array<std::uint8_t, 2> pair,
    const std::array<color_space::OKLab, 16>& pal_lab,
    std::array<std::uint8_t, kHiCellW * kHiCellH>* pixel_idx_out) {

    float total = 0.0f;
    auto& a = pal_lab[pair[0]];
    auto& b = pal_lab[pair[1]];
    for (std::size_t p = 0; p < pix_lab.size(); ++p) {
        const auto& t = pix_lab[p];
        float dLa = t.L - a.L, daa = t.a - a.a, dba = t.b - a.b;
        float dLb = t.L - b.L, dab = t.a - b.a, dbb = t.b - b.b;
        float ea = dLa * dLa + daa * daa + dba * dba;
        float eb = dLb * dLb + dab * dab + dbb * dbb;
        if (ea <= eb) { total += ea; if (pixel_idx_out) (*pixel_idx_out)[p] = 0; }
        else          { total += eb; if (pixel_idx_out) (*pixel_idx_out)[p] = 1; }
    }
    return total;
}

}  // namespace

Result<EncodeResult> encode_hires(const Image& image, Palette pal,
                                   const dither::Settings& settings,
                                   Metric metric) {
    (void)metric;  // TODO: per-metric brute-force scoring.
    constexpr std::size_t W = kHiCols * kHiCellW;  // 320
    constexpr std::size_t H = kHiRows * kHiCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_hires: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin = palette_linear(pal);
    const auto& pal_lab = palette_oklab(pal);

    std::vector<color_space::OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);
    std::array<Color3f, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i)
        pal_s[i] = {pal_lab[i].L, pal_lab[i].a, pal_lab[i].b};

    EncodeResult res;
    res.rendered = Image(W, H);
    res.bitmap.assign(kHiRows * kHiCols * kHiCellH, 0);  // 8000 bytes
    res.screen_ram.assign(kHiRows * kHiCols, 0);          // 1000 bytes
    // color_ram unused for hires — left empty.
    res.bg_color = 0;

    static constexpr CellTaps<kHiCellW, kHiCellH> taps_hi{};
    auto tap_lookup = [](std::size_t p)
        -> std::span<const CellTaps<kHiCellW, kHiCellH>::T, 9> {
        return std::span<const CellTaps<kHiCellW, kHiCellH>::T, 9>(taps_hi.taps[p]);
    };

    // Pass 1: per-cell pair pick. C(16, 2) = 120 pairs. Metric
    // dispatch:
    //   mse  → cell_error_for_pair (OKLab² nearest sum, fast).
    //   blur → score_cell<2, 64> with pre-blurred source vs blurred
    //          rendered; OKLab is treated as a generic 3-vec.
    //   ssim → score_cell<2, 64> single-window stats.
    constexpr std::size_t kHiCellPx = kHiCellW * kHiCellH;  // 64
    std::vector<std::array<std::uint8_t, 2>> cell_pair(kHiRows * kHiCols);
    std::array<color_space::OKLab, kHiCellPx> cell_lab{};
    std::array<Color3f, kHiCellPx>             raw{};
    std::array<Color3f, kHiCellPx>             blurred_src{};
    std::array<std::uint8_t, kHiCellPx>        pix_idx{};
    for (std::size_t cy = 0; cy < kHiRows; ++cy) {
        for (std::size_t cx = 0; cx < kHiCols; ++cx) {
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto idx = (cy * kHiCellH + py) * W + (cx * kHiCellW + px);
                    cell_lab[py * kHiCellW + px] = src_lab[idx];
                    raw[py * kHiCellW + px] = {
                        src_lab[idx].L, src_lab[idx].a, src_lab[idx].b
                    };
                }
            }
            if (metric == Metric::blur) {
                for (std::size_t p = 0; p < kHiCellPx; ++p) {
                    Color3f b{0, 0, 0};
                    for (auto& t : taps_hi.taps[p]) {
                        b.r += t.w * raw[t.q].r;
                        b.g += t.w * raw[t.q].g;
                        b.b += t.w * raw[t.q].b;
                    }
                    blurred_src[p] = b;
                }
            }
            float best_err = std::numeric_limits<float>::infinity();
            std::array<std::uint8_t, 2> best_pair{0, 0};
            for (std::uint8_t i = 0; i < 16; ++i) {
                for (std::uint8_t j = static_cast<std::uint8_t>(i + 1);
                     j < 16; ++j) {
                    float err;
                    if (metric == Metric::mse) {
                        std::array<std::uint8_t, 4> q{i, j, 0, 0};
                        // Reuse cell_error_for_quad with last 2 unused;
                        // simpler: inline the 2-color error.
                        const auto& a = pal_lab[i];
                        const auto& b = pal_lab[j];
                        float total = 0.0f;
                        for (std::size_t p = 0; p < kHiCellPx; ++p) {
                            const auto& t = cell_lab[p];
                            float dLa = t.L - a.L, daa = t.a - a.a, dba = t.b - a.b;
                            float dLb = t.L - b.L, dab = t.a - b.a, dbb = t.b - b.b;
                            float ea = dLa*dLa + daa*daa + dba*dba;
                            float eb = dLb*dLb + dab*dab + dbb*dbb;
                            total += std::min(ea, eb);
                        }
                        err = total;
                        (void)q;
                    } else {
                        std::array<Color3f, 2> cand{pal_s[i], pal_s[j]};
                        err = score_cell<2, kHiCellPx>(
                            raw, blurred_src, nullptr, cand,
                            metric, &pix_idx, tap_lookup);
                    }
                    if (err < best_err) {
                        best_err  = err;
                        best_pair = {i, j};
                    }
                }
            }
            cell_pair[cy * kHiCols + cx] = best_pair;
        }
    }

    // Pass 2: per-pixel dither via diffuse_raw_buffer with per-cell
    // 2-colour palette callback. Index 0/1 written to the bitmap MSB-
    // first within each row byte.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const color_space::OKLab& target,
                    std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kHiCellH;
        std::size_t cx = x / kHiCellW;
        const auto& pair = cell_pair[cy * kHiCols + cx];
        std::array<color_space::OKLab, 2> cp{
            pal_lab[pair[0]], pal_lab[pair[1]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target,
            std::span<const color_space::OKLab>(cp),
            x, y, settings.strength, /*k_min=*/0,
            chosen_index, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_index);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    for (std::size_t cy = 0; cy < kHiRows; ++cy) {
        for (std::size_t cx = 0; cx < kHiCols; ++cx) {
            std::size_t cell_idx = cy * kHiCols + cx;
            const auto& pair = cell_pair[cell_idx];
            // Screen RAM: upper nibble = c1 (foreground / index 1),
            // lower = c0 (background / index 0).
            res.screen_ram[cell_idx] = static_cast<std::uint8_t>(
                ((pair[1] & 0xF) << 4) | (pair[0] & 0xF));
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto x = cx * kHiCellW + px;
                    auto y = cy * kHiCellH + py;
                    auto q = static_cast<std::uint8_t>(indices[y * W + x] & 0x1);
                    row_byte = static_cast<std::uint8_t>(
                        (row_byte << 1) | q);
                    res.rendered[x, y] = pal_lin[pair[q]];
                }
                res.bitmap[cell_idx * kHiCellH + py] = row_byte;
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-FLI: 160×200 multicolor + per-row (c1, c2) screen colours
//          within each 4×8 cell + per-cell color_ram (c3) + global bg.
// ---------------------------------------------------------------------------

Result<EncodeResult> encode_fli(const Image& image, Palette pal,
                                 const dither::Settings& settings,
                                 Metric metric) {
    constexpr std::size_t W = kCols * kCellW;  // 160
    constexpr std::size_t H = kRows * kCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_fli: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin = palette_linear(pal);
    const auto& pal_lab = palette_oklab(pal);

    std::vector<color_space::OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);
    std::array<Color3f, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i)
        pal_s[i] = {pal_lab[i].L, pal_lab[i].a, pal_lab[i].b};

    // FLI scores per-row (each row of a 4×8 cell has its own
    // 4-colour palette). Per-row blur uses a 4-pixel × 1-row tap
    // table; the 3×3 binomial collapses vertically (replicate-pad)
    // to a [1,2,1]/4 horizontal kernel, which is a sensible 1D PN
    // approximation for these strip-shaped rows.
    static constexpr CellTaps<kCellW, 1> taps_row{};
    auto tap_lookup_row = [](std::size_t p)
        -> std::span<const CellTaps<kCellW, 1>::T, 9> {
        return std::span<const CellTaps<kCellW, 1>::T, 9>(taps_row.taps[p]);
    };

    constexpr std::uint8_t bg = 0;  // global bg fixed at black for now

    EncodeResult res;
    res.rendered = Image(W, H);        // 160×200 logical
    res.bitmap.assign(kRows * kCols * kCellH, 0);   // 8000 bytes
    res.screen_ram.assign(kCellH * kRows * kCols, 0);  // 8000 bytes (8 RAMs)
    res.color_ram.assign(kRows * kCols, 0);         // 1000 bytes
    res.bg_color = bg;

    // Pass 1: per-cell, per-row brute force.
    //   For each candidate color_ram value cr ∈ {0..15} \ {bg}:
    //     For each of 8 rows: try every (c1, c2) pair from
    //       {0..15} \ {bg, cr}, score row error against the row's
    //       4 pixels under the 4-colour set {bg, c1, c2, cr}.
    //     Sum row errors → cell error for this cr.
    //   Pick the cr that minimises total cell error, plus its
    //   row-best (c1, c2) pairs.
    //
    // Per-cell quad layout for pass 2: cell_quads[cell][row] = {bg, c1, c2, cr}.
    std::vector<std::array<std::array<std::uint8_t, 4>, kCellH>>
        cell_quads(kRows * kCols);
    std::vector<std::uint8_t> cell_cr(kRows * kCols, 0);

    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            std::size_t cell_idx = cy * kCols + cx;

            float best_total = std::numeric_limits<float>::infinity();
            std::uint8_t best_cr = 0;
            std::array<std::array<std::uint8_t, 4>, kCellH> best_rows{};

            for (std::uint8_t cr = 0; cr < 16; ++cr) {
                if (cr == bg) continue;
                float total = 0.0f;
                std::array<std::array<std::uint8_t, 4>, kCellH> row_quads{};
                for (std::size_t py = 0; py < kCellH; ++py) {
                    // Gather row pixels in OKLab + Color3f-as-OKLab.
                    std::array<color_space::OKLab, kCellW> row_lab{};
                    std::array<Color3f, kCellW> row_raw{};
                    std::array<Color3f, kCellW> row_blurred{};
                    for (std::size_t px = 0; px < kCellW; ++px) {
                        auto idx = (cy * kCellH + py) * W + (cx * kCellW + px);
                        row_lab[px] = src_lab[idx];
                        row_raw[px] = {row_lab[px].L, row_lab[px].a, row_lab[px].b};
                    }
                    if (metric == Metric::blur) {
                        for (std::size_t p = 0; p < kCellW; ++p) {
                            Color3f b{0, 0, 0};
                            for (auto& t : taps_row.taps[p]) {
                                b.r += t.w * row_raw[t.q].r;
                                b.g += t.w * row_raw[t.q].g;
                                b.b += t.w * row_raw[t.q].b;
                            }
                            row_blurred[p] = b;
                        }
                    }
                    float best_row = std::numeric_limits<float>::infinity();
                    std::array<std::uint8_t, 4> best_row_quad{bg, 0, 0, cr};
                    for (std::uint8_t c1 = 0; c1 < 16; ++c1) {
                        if (c1 == bg || c1 == cr) continue;
                        for (std::uint8_t c2 = static_cast<std::uint8_t>(c1 + 1);
                             c2 < 16; ++c2) {
                            if (c2 == bg || c2 == cr) continue;
                            float e;
                            if (metric == Metric::mse) {
                                std::array<std::uint8_t, 4> q{bg, c1, c2, cr};
                                e = cell_error_for_quad(
                                    std::span<const color_space::OKLab>(row_lab),
                                    q, pal_lab, nullptr);
                            } else {
                                std::array<Color3f, 4> cand{
                                    pal_s[bg], pal_s[c1], pal_s[c2], pal_s[cr],
                                };
                                e = score_cell<4, kCellW>(
                                    row_raw, row_blurred, nullptr, cand,
                                    metric, nullptr, tap_lookup_row);
                            }
                            if (e < best_row) {
                                best_row = e;
                                best_row_quad = {bg, c1, c2, cr};
                            }
                        }
                    }
                    total += best_row;
                    row_quads[py] = best_row_quad;
                }
                if (total < best_total) {
                    best_total = total;
                    best_cr = cr;
                    best_rows = row_quads;
                }
            }

            cell_cr[cell_idx] = best_cr;
            cell_quads[cell_idx] = best_rows;
        }
    }

    // Pass 2: per-pixel dither using the cell's row-specific 4-colour set.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const color_space::OKLab& target,
                    std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kCellH;
        std::size_t cx = x / kCellW;
        std::size_t py = y % kCellH;
        const auto& quad = cell_quads[cy * kCols + cx][py];
        std::array<color_space::OKLab, 4> cp{
            pal_lab[quad[0]], pal_lab[quad[1]],
            pal_lab[quad[2]], pal_lab[quad[3]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target,
            std::span<const color_space::OKLab>(cp),
            x, y, settings.strength, /*k_min=*/0,
            chosen_index, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_index);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    // Pack bitmap (40×25×8) + 8 screen RAMs + color RAM, render preview.
    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            std::size_t cell_idx = cy * kCols + cx;
            res.color_ram[cell_idx] = static_cast<std::uint8_t>(
                cell_cr[cell_idx] & 0xF);
            for (std::size_t py = 0; py < kCellH; ++py) {
                const auto& quad = cell_quads[cell_idx][py];
                // Screen RAM[py][cell_idx] = upper nibble c1, lower c2.
                res.screen_ram[py * kRows * kCols + cell_idx] =
                    static_cast<std::uint8_t>(
                        ((quad[1] & 0xF) << 4) | (quad[2] & 0xF));
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kCellW; ++px) {
                    auto x = cx * kCellW + px;
                    auto y = cy * kCellH + py;
                    auto q = static_cast<std::uint8_t>(
                        indices[y * W + x] & 0x3);
                    row_byte = static_cast<std::uint8_t>(
                        (row_byte << 2) | q);
                    res.rendered[x, y] = pal_lin[quad[q]];
                }
                res.bitmap[cell_idx * kCellH + py] = row_byte;
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-AFLI: 320×200 hires + per-row (c0, c1) pair within each 8×8 cell.
// ---------------------------------------------------------------------------

Result<EncodeResult> encode_afli(const Image& image, Palette pal,
                                  const dither::Settings& settings,
                                  Metric metric) {
    constexpr std::size_t W = kHiCols * kHiCellW;  // 320
    constexpr std::size_t H = kHiRows * kHiCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_afli: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin = palette_linear(pal);
    const auto& pal_lab = palette_oklab(pal);

    std::vector<color_space::OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);
    std::array<Color3f, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i)
        pal_s[i] = {pal_lab[i].L, pal_lab[i].a, pal_lab[i].b};

    static constexpr CellTaps<kHiCellW, 1> taps_afli_row{};
    auto tap_lookup_row = [](std::size_t p)
        -> std::span<const CellTaps<kHiCellW, 1>::T, 9> {
        return std::span<const CellTaps<kHiCellW, 1>::T, 9>(taps_afli_row.taps[p]);
    };

    EncodeResult res;
    res.rendered = Image(W, H);
    res.bitmap.assign(kHiRows * kHiCols * kHiCellH, 0);
    res.screen_ram.assign(kHiCellH * kHiRows * kHiCols, 0);  // 8 × 1000
    // color_ram unused for AFLI.
    res.bg_color = 0;

    // Pass 1: per-cell-row brute force C(16, 2) = 120 pairs.
    std::vector<std::array<std::array<std::uint8_t, 2>, kHiCellH>>
        cell_pairs(kHiRows * kHiCols);
    for (std::size_t cy = 0; cy < kHiRows; ++cy) {
        for (std::size_t cx = 0; cx < kHiCols; ++cx) {
            std::size_t cell_idx = cy * kHiCols + cx;
            std::array<std::array<std::uint8_t, 2>, kHiCellH> row_pairs{};
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                std::array<color_space::OKLab, kHiCellW> row_lab{};
                std::array<Color3f, kHiCellW>            row_raw{};
                std::array<Color3f, kHiCellW>            row_blurred{};
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto idx = (cy * kHiCellH + py) * W + (cx * kHiCellW + px);
                    row_lab[px] = src_lab[idx];
                    row_raw[px] = {row_lab[px].L, row_lab[px].a, row_lab[px].b};
                }
                if (metric == Metric::blur) {
                    for (std::size_t p = 0; p < kHiCellW; ++p) {
                        Color3f b{0, 0, 0};
                        for (auto& t : taps_afli_row.taps[p]) {
                            b.r += t.w * row_raw[t.q].r;
                            b.g += t.w * row_raw[t.q].g;
                            b.b += t.w * row_raw[t.q].b;
                        }
                        row_blurred[p] = b;
                    }
                }
                float best = std::numeric_limits<float>::infinity();
                std::array<std::uint8_t, 2> best_pair{0, 0};
                for (std::uint8_t i = 0; i < 16; ++i) {
                    for (std::uint8_t j = static_cast<std::uint8_t>(i + 1);
                         j < 16; ++j) {
                        float e;
                        if (metric == Metric::mse) {
                            std::array<std::uint8_t, 2> p{i, j};
                            e = cell_error_for_pair(
                                std::span<const color_space::OKLab>(row_lab),
                                p, pal_lab, nullptr);
                        } else {
                            std::array<Color3f, 2> cand{pal_s[i], pal_s[j]};
                            e = score_cell<2, kHiCellW>(
                                row_raw, row_blurred, nullptr, cand,
                                metric, nullptr, tap_lookup_row);
                        }
                        if (e < best) {
                            best = e;
                            best_pair = {i, j};
                        }
                    }
                }
                row_pairs[py] = best_pair;
            }
            cell_pairs[cell_idx] = row_pairs;
        }
    }

    // Pass 2: per-pixel dither against per-row 2-colour palette.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const color_space::OKLab& target,
                    std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kHiCellH;
        std::size_t cx = x / kHiCellW;
        std::size_t py = y % kHiCellH;
        const auto& pair = cell_pairs[cy * kHiCols + cx][py];
        std::array<color_space::OKLab, 2> cp{
            pal_lab[pair[0]], pal_lab[pair[1]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target,
            std::span<const color_space::OKLab>(cp),
            x, y, settings.strength, /*k_min=*/0,
            chosen_index, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_index);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    // Pack bitmap + screen RAMs, render preview.
    for (std::size_t cy = 0; cy < kHiRows; ++cy) {
        for (std::size_t cx = 0; cx < kHiCols; ++cx) {
            std::size_t cell_idx = cy * kHiCols + cx;
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                const auto& pair = cell_pairs[cell_idx][py];
                res.screen_ram[py * kHiRows * kHiCols + cell_idx] =
                    static_cast<std::uint8_t>(
                        ((pair[1] & 0xF) << 4) | (pair[0] & 0xF));
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto x = cx * kHiCellW + px;
                    auto y = cy * kHiCellH + py;
                    auto q = static_cast<std::uint8_t>(
                        indices[y * W + x] & 0x1);
                    row_byte = static_cast<std::uint8_t>(
                        (row_byte << 1) | q);
                    res.rendered[x, y] = pal_lin[pair[q]];
                }
                res.bitmap[cell_idx * kHiCellH + py] = row_byte;
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-charset-hires: 320×200, 8×8 cells, 2 colours per cell. Per-cell
// quantisation matches encode_hires; the resulting 8-byte glyph
// patterns are then deduplicated and merged-by-Hamming-distance to
// fit the 256-slot charset budget.
// ---------------------------------------------------------------------------

namespace {

inline std::uint64_t hi_pattern_64(
    const std::array<std::uint8_t, kHiCellW * kHiCellH>& pix_idx) {
    // Pack the 64 0/1 indices into a single uint64 (row 0 lowest 8
    // bits). Used as the dedup key.
    std::uint64_t key = 0;
    for (std::size_t i = 0; i < kHiCellW * kHiCellH; ++i) {
        if (pix_idx[i]) key |= (std::uint64_t{1} << i);
    }
    return key;
}

inline std::array<std::uint8_t, 8> hi_pattern_bytes(std::uint64_t key) {
    // row r ↔ bits r*8 .. r*8+7 (column 0 = MSB of byte).
    std::array<std::uint8_t, 8> out{};
    for (std::size_t r = 0; r < 8; ++r) {
        std::uint8_t b = 0;
        for (std::size_t c = 0; c < 8; ++c) {
            if ((key >> (r * 8 + c)) & 1) b |= static_cast<std::uint8_t>(0x80 >> c);
        }
        out[r] = b;
    }
    return out;
}

inline int popcount_xor(std::uint64_t a, std::uint64_t b) {
    return std::popcount(a ^ b);
}

}  // namespace

Result<EncodeResult> encode_charset_hires(const Image& image, Palette pal,
                                           const dither::Settings& settings,
                                           Metric metric) {
    constexpr std::size_t W = kHiCols * kHiCellW;  // 320
    constexpr std::size_t H = kHiRows * kHiCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_charset_hires: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin = palette_linear(pal);
    const auto& pal_lab = palette_oklab(pal);
    std::vector<color_space::OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);

    constexpr std::size_t kHiCellPx = kHiCellW * kHiCellH;  // 64
    constexpr std::size_t kCells    = kHiRows * kHiCols;    // 1000

    // Pass 1: per-cell brute-force pair pick. Metric dispatch
    // matches encode_hires — mse uses inlined OKLab² nearest sum
    // (fast); blur uses score_cell<2, 64> with full 2D 3×3 binomial
    // blur on (raw, rendered).
    std::array<Color3f, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i)
        pal_s[i] = {pal_lab[i].L, pal_lab[i].a, pal_lab[i].b};
    static constexpr CellTaps<kHiCellW, kHiCellH> taps_ch{};
    auto tap_lookup = [](std::size_t p)
        -> std::span<const CellTaps<kHiCellW, kHiCellH>::T, 9> {
        return std::span<const CellTaps<kHiCellW, kHiCellH>::T, 9>(taps_ch.taps[p]);
    };
    std::array<Color3f, kHiCellPx> raw_cell{};
    std::array<Color3f, kHiCellPx> blurred_src{};

    struct CellPick {
        std::array<std::uint8_t, 2> pair;  // (c0, c1) palette indices
        std::uint64_t pattern;             // 64-bit fg/bg mask
    };
    std::vector<CellPick> cells(kCells);
    std::vector<std::array<std::uint8_t, 2>> cell_pair(kCells);
    std::array<color_space::OKLab, kHiCellPx> cell_lab{};
    std::array<std::uint8_t, kHiCellPx> pix_idx{};
    for (std::size_t cy = 0; cy < kHiRows; ++cy) {
        for (std::size_t cx = 0; cx < kHiCols; ++cx) {
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto idx = (cy * kHiCellH + py) * W + (cx * kHiCellW + px);
                    cell_lab[py * kHiCellW + px] = src_lab[idx];
                    raw_cell[py * kHiCellW + px] = {
                        src_lab[idx].L, src_lab[idx].a, src_lab[idx].b
                    };
                }
            }
            if (metric == Metric::blur) {
                for (std::size_t p = 0; p < kHiCellPx; ++p) {
                    Color3f b{0, 0, 0};
                    for (auto& t : taps_ch.taps[p]) {
                        b.r += t.w * raw_cell[t.q].r;
                        b.g += t.w * raw_cell[t.q].g;
                        b.b += t.w * raw_cell[t.q].b;
                    }
                    blurred_src[p] = b;
                }
            }
            float best_err = std::numeric_limits<float>::infinity();
            std::array<std::uint8_t, 2> best_pair{0, 0};
            for (std::uint8_t i = 0; i < 16; ++i) {
                for (std::uint8_t j = static_cast<std::uint8_t>(i + 1);
                     j < 16; ++j) {
                    float err;
                    if (metric == Metric::mse) {
                        std::array<std::uint8_t, 2> pair{i, j};
                        err = cell_error_for_pair(
                            cell_lab, pair, pal_lab, &pix_idx);
                    } else {
                        std::array<Color3f, 2> cand{pal_s[i], pal_s[j]};
                        err = score_cell<2, kHiCellPx>(
                            raw_cell, blurred_src, nullptr, cand,
                            metric, &pix_idx, tap_lookup);
                    }
                    if (err < best_err) {
                        best_err = err;
                        best_pair = {i, j};
                    }
                }
            }
            cell_pair[cy * kHiCols + cx] = best_pair;
        }
    }

    // Pass 2: per-pixel dither via diffuse_raw_buffer using the
    // cell's 2-colour palette. Produces a dithered index per pixel
    // (different from the brute-force nearest-pick — error diffusion
    // pushes residuals across pixels). The resulting per-pixel
    // pattern is what the dedup/merge phase keys on, so dither
    // shapes the glyph budget directly.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const color_space::OKLab& target,
                    std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kHiCellH;
        std::size_t cx = x / kHiCellW;
        const auto& pair = cell_pair[cy * kHiCols + cx];
        std::array<color_space::OKLab, 2> cp{
            pal_lab[pair[0]], pal_lab[pair[1]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target,
            std::span<const color_space::OKLab>(cp),
            x, y, settings.strength, /*k_min=*/0,
            chosen_index, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_index);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    // Pack the dithered per-pixel indices into per-cell 64-bit
    // patterns + final cells[].
    for (std::size_t cy = 0; cy < kHiRows; ++cy) {
        for (std::size_t cx = 0; cx < kHiCols; ++cx) {
            std::array<std::uint8_t, kHiCellPx> px{};
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                for (std::size_t pxx = 0; pxx < kHiCellW; ++pxx) {
                    auto x = cx * kHiCellW + pxx;
                    auto y = cy * kHiCellH + py;
                    px[py * kHiCellW + pxx] = indices[y * W + x] & 1;
                }
            }
            cells[cy * kHiCols + cx] = {
                cell_pair[cy * kHiCols + cx],
                hi_pattern_64(px)
            };
        }
    }

    // Pass 2: dedup by 64-bit pattern. cell_to_glyph[i] = index into
    // the unique-glyph list.
    std::vector<std::uint64_t> glyphs;
    std::vector<std::vector<std::size_t>> glyph_cells;  // cells per glyph
    std::unordered_map<std::uint64_t, std::size_t> pat_to_glyph;
    glyphs.reserve(kCells);
    glyph_cells.reserve(kCells);
    std::vector<std::size_t> cell_to_glyph(kCells, 0);
    for (std::size_t i = 0; i < kCells; ++i) {
        auto p = cells[i].pattern;
        auto it = pat_to_glyph.find(p);
        if (it == pat_to_glyph.end()) {
            std::size_t idx = glyphs.size();
            pat_to_glyph[p] = idx;
            glyphs.push_back(p);
            glyph_cells.emplace_back();
            it = pat_to_glyph.find(p);
        }
        glyph_cells[it->second].push_back(i);
        cell_to_glyph[i] = it->second;
    }

    // Pass 3: merge by Hamming distance until ≤256 unique glyphs.
    // Reserve glyph 0 for the all-zero (empty) pattern if the image
    // produces one — it's a natural snap target. Otherwise the first
    // 256 surviving glyphs land at indices 0..255.
    while (glyphs.size() > 256) {
        // Find closest pair (smallest popcount of XOR). Brute O(N²)
        // is fine here — N ≤ ~5000 cells in practice.
        std::size_t best_a = 0, best_b = 1;
        int best_dist = std::numeric_limits<int>::max();
        for (std::size_t a = 0; a < glyphs.size(); ++a) {
            for (std::size_t b = a + 1; b < glyphs.size(); ++b) {
                int d = popcount_xor(glyphs[a], glyphs[b]);
                if (d < best_dist) {
                    best_dist = d;
                    best_a = a; best_b = b;
                }
            }
        }
        // Merge b → a (keep larger cell-count slot).
        std::size_t keep = best_a, drop = best_b;
        if (glyph_cells[keep].size() < glyph_cells[drop].size())
            std::swap(keep, drop);
        for (auto ci : glyph_cells[drop]) cell_to_glyph[ci] = keep;
        glyph_cells[keep].insert(glyph_cells[keep].end(),
                                  glyph_cells[drop].begin(),
                                  glyph_cells[drop].end());
        // Erase drop. Re-index everything > drop down by 1.
        glyphs.erase(glyphs.begin() + static_cast<std::ptrdiff_t>(drop));
        glyph_cells.erase(glyph_cells.begin() + static_cast<std::ptrdiff_t>(drop));
        for (auto& g : cell_to_glyph) {
            if (g > drop) --g;
        }
    }

    // Build charset_data: 256 × 8 bytes (zero-fill unused entries).
    std::array<std::uint8_t, 2048> charset_data{};
    for (std::size_t g = 0; g < glyphs.size() && g < 256; ++g) {
        auto bytes = hi_pattern_bytes(glyphs[g]);
        for (std::size_t r = 0; r < 8; ++r)
            charset_data[g * 8 + r] = bytes[r];
    }

    EncodeResult res;
    res.rendered = Image(W, H);
    // Stash charset in `bitmap` for now (first 2048 bytes); a proper
    // charset_data field can come with the .h writer.
    res.bitmap.assign(2048, 0);
    for (std::size_t i = 0; i < 2048; ++i) res.bitmap[i] = charset_data[i];
    res.screen_ram.assign(kCells, 0);   // char codes per cell
    res.color_ram.assign(kCells, 0);    // upper=c1, lower=c0
    res.bg_color = 0;

    // Render + pack screen / colour. screen_ram[cell] = glyph index.
    for (std::size_t i = 0; i < kCells; ++i) {
        auto cy = i / kHiCols;
        auto cx = i % kHiCols;
        std::uint8_t glyph_idx = static_cast<std::uint8_t>(
            std::min(cell_to_glyph[i], std::size_t{255}));
        res.screen_ram[i] = glyph_idx;
        const auto& pair = cells[i].pair;
        res.color_ram[i] = static_cast<std::uint8_t>(
            ((pair[1] & 0xF) << 4) | (pair[0] & 0xF));
        // Render: each pixel is c0 (bg) or c1 (fg) per the post-merge
        // glyph pattern.
        std::uint64_t merged_pattern = glyphs[cell_to_glyph[i]];
        for (std::size_t py = 0; py < kHiCellH; ++py) {
            for (std::size_t px = 0; px < kHiCellW; ++px) {
                auto x = cx * kHiCellW + px;
                auto y = cy * kHiCellH + py;
                std::size_t bit = py * kHiCellW + px;
                auto q = static_cast<std::uint8_t>(
                    (merged_pattern >> bit) & 1ULL);
                res.rendered[x, y] = pal_lin[pair[q]];
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-charset-multicolor: 160×200 logical, 4×8 cells, 4 colours per
// cell (1 shared bg + 2 shared mc + 1 per-cell fg). Global bg / mc1 /
// mc2 are picked by brute-force outer loop; per-cell fg picked per
// cell; per-pixel index assignment runs through diffuse_raw_buffer
// with a per-cell 4-colour palette callback so dither can reshape
// the pattern.
// ---------------------------------------------------------------------------

namespace {
inline std::uint64_t mc_pattern_64(
    const std::array<std::uint8_t, kCellW * kCellH>& pix_idx) {
    std::uint64_t key = 0;
    for (std::size_t i = 0; i < kCellW * kCellH; ++i) {
        key |= (static_cast<std::uint64_t>(pix_idx[i] & 0x3) << (i * 2));
    }
    return key;
}
inline std::array<std::uint8_t, 8> mc_pattern_bytes(std::uint64_t key) {
    // row r ↔ 8 bits at (r * 8). 4 pixels × 2 bits = 8 bits per row.
    std::array<std::uint8_t, 8> out{};
    for (std::size_t r = 0; r < 8; ++r) {
        std::uint8_t b = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            std::uint8_t q = (key >> ((r * 4 + c) * 2)) & 0x3;
            b = static_cast<std::uint8_t>((b << 2) | q);
        }
        out[r] = b;
    }
    return out;
}
}  // namespace

Result<EncodeResult> encode_charset_multicolor(const Image& image,
                                                Palette pal,
                                                const dither::Settings& settings,
                                                Metric metric) {
    constexpr std::size_t W = kCols * kCellW;  // 160
    constexpr std::size_t H = kRows * kCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_charset_multicolor: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin = palette_linear(pal);
    const auto& pal_lab = palette_oklab(pal);
    std::vector<color_space::OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);
    std::array<Color3f, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i)
        pal_s[i] = {pal_lab[i].L, pal_lab[i].a, pal_lab[i].b};

    static constexpr CellTaps<kCellW, kCellH> taps_mcch{};
    auto tap_lookup = [](std::size_t p)
        -> std::span<const CellTaps<kCellW, kCellH>::T, 9> {
        return std::span<const CellTaps<kCellW, kCellH>::T, 9>(taps_mcch.taps[p]);
    };

    constexpr std::uint8_t bg = 0;
    constexpr std::size_t kCells = kRows * kCols;
    constexpr std::size_t kCellPx = kCellW * kCellH;  // 32

    // Outer pass: brute-force shared (mc1, mc2). For each candidate,
    // iterate cells, pick best fg ∈ remaining 13 colours, sum total
    // image error. Pick global (mc1, mc2) with lowest total.
    std::array<color_space::OKLab, kCellPx> cell_lab{};
    std::array<Color3f, kCellPx> raw_cell{};
    std::array<Color3f, kCellPx> blurred_src{};

    float best_total = std::numeric_limits<float>::infinity();
    std::uint8_t best_mc1 = 1, best_mc2 = 2;
    std::vector<std::uint8_t> best_fg(kCells, 0);

    for (std::uint8_t mc1 = 0; mc1 < 16; ++mc1) {
        if (mc1 == bg) continue;
        for (std::uint8_t mc2 = static_cast<std::uint8_t>(mc1 + 1); mc2 < 16; ++mc2) {
            if (mc2 == bg) continue;
            float total = 0.0f;
            std::vector<std::uint8_t> fgs(kCells, 0);
            for (std::size_t cy = 0; cy < kRows; ++cy) {
                for (std::size_t cx = 0; cx < kCols; ++cx) {
                    for (std::size_t py = 0; py < kCellH; ++py) {
                        for (std::size_t px = 0; px < kCellW; ++px) {
                            auto idx = (cy * kCellH + py) * W + (cx * kCellW + px);
                            cell_lab[py * kCellW + px] = src_lab[idx];
                            raw_cell[py * kCellW + px] = {
                                src_lab[idx].L, src_lab[idx].a, src_lab[idx].b
                            };
                        }
                    }
                    if (metric == Metric::blur) {
                        for (std::size_t p = 0; p < kCellPx; ++p) {
                            Color3f b{0, 0, 0};
                            for (auto& t : taps_mcch.taps[p]) {
                                b.r += t.w * raw_cell[t.q].r;
                                b.g += t.w * raw_cell[t.q].g;
                                b.b += t.w * raw_cell[t.q].b;
                            }
                            blurred_src[p] = b;
                        }
                    }
                    float cell_best = std::numeric_limits<float>::infinity();
                    std::uint8_t cell_fg = 0;
                    for (std::uint8_t fg = 0; fg < 16; ++fg) {
                        if (fg == bg || fg == mc1 || fg == mc2) continue;
                        float err;
                        if (metric == Metric::mse) {
                            std::array<std::uint8_t, 4> q{bg, mc1, mc2, fg};
                            err = cell_error_for_quad(
                                cell_lab, q, pal_lab, nullptr);
                        } else {
                            std::array<Color3f, 4> cand{
                                pal_s[bg], pal_s[mc1], pal_s[mc2], pal_s[fg],
                            };
                            err = score_cell<4, kCellPx>(
                                raw_cell, blurred_src, nullptr, cand,
                                metric, nullptr, tap_lookup);
                        }
                        if (err < cell_best) {
                            cell_best = err;
                            cell_fg = fg;
                        }
                    }
                    total += cell_best;
                    fgs[cy * kCols + cx] = cell_fg;
                }
            }
            if (total < best_total) {
                best_total = total;
                best_mc1 = mc1;
                best_mc2 = mc2;
                best_fg  = std::move(fgs);
            }
        }
    }

    // Pass 2: per-pixel dither using each cell's 4-colour palette
    // {bg, mc1, mc2, fg} via diffuse_raw_buffer.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick = [&](const color_space::OKLab& target,
                    std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kCellH;
        std::size_t cx = x / kCellW;
        std::uint8_t fg = best_fg[cy * kCols + cx];
        std::array<color_space::OKLab, 4> cp{
            pal_lab[bg], pal_lab[best_mc1], pal_lab[best_mc2], pal_lab[fg],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, target,
            std::span<const color_space::OKLab>(cp),
            x, y, settings.strength, /*k_min=*/0,
            chosen_index, chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_index);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    // Pack per-cell 64-bit patterns from dithered indices.
    std::vector<std::uint64_t> patterns(kCells, 0);
    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            std::array<std::uint8_t, kCellPx> px{};
            for (std::size_t py = 0; py < kCellH; ++py) {
                for (std::size_t pxx = 0; pxx < kCellW; ++pxx) {
                    auto x = cx * kCellW + pxx;
                    auto y = cy * kCellH + py;
                    px[py * kCellW + pxx] = indices[y * W + x] & 0x3;
                }
            }
            patterns[cy * kCols + cx] = mc_pattern_64(px);
        }
    }

    // Dedup by 64-bit pattern.
    std::vector<std::uint64_t> glyphs;
    std::vector<std::vector<std::size_t>> glyph_cells;
    std::unordered_map<std::uint64_t, std::size_t> pat_to_glyph;
    glyphs.reserve(kCells);
    std::vector<std::size_t> cell_to_glyph(kCells, 0);
    for (std::size_t i = 0; i < kCells; ++i) {
        auto p = patterns[i];
        auto it = pat_to_glyph.find(p);
        if (it == pat_to_glyph.end()) {
            std::size_t idx = glyphs.size();
            pat_to_glyph[p] = idx;
            glyphs.push_back(p);
            glyph_cells.emplace_back();
            it = pat_to_glyph.find(p);
        }
        glyph_cells[it->second].push_back(i);
        cell_to_glyph[i] = it->second;
    }

    // Hamming-distance merge until ≤256 unique glyphs. The 64-bit
    // key holds 32 × 2-bit indices; we count differing 2-bit
    // positions (any bit difference ⇒ pixel mismatch).
    auto mc_dist = [](std::uint64_t a, std::uint64_t b) {
        std::uint64_t diff = a ^ b;
        // Collapse pairs of bits → 1 if either bit differs.
        std::uint64_t any = (diff | (diff >> 1)) & 0x5555555555555555ULL;
        return std::popcount(any);
    };
    while (glyphs.size() > 256) {
        std::size_t a_best = 0, b_best = 1;
        int best_d = std::numeric_limits<int>::max();
        for (std::size_t a = 0; a < glyphs.size(); ++a) {
            for (std::size_t b = a + 1; b < glyphs.size(); ++b) {
                int d = mc_dist(glyphs[a], glyphs[b]);
                if (d < best_d) { best_d = d; a_best = a; b_best = b; }
            }
        }
        std::size_t keep = a_best, drop = b_best;
        if (glyph_cells[keep].size() < glyph_cells[drop].size())
            std::swap(keep, drop);
        for (auto ci : glyph_cells[drop]) cell_to_glyph[ci] = keep;
        glyph_cells[keep].insert(glyph_cells[keep].end(),
                                  glyph_cells[drop].begin(),
                                  glyph_cells[drop].end());
        glyphs.erase(glyphs.begin() + static_cast<std::ptrdiff_t>(drop));
        glyph_cells.erase(glyph_cells.begin() + static_cast<std::ptrdiff_t>(drop));
        for (auto& g : cell_to_glyph) if (g > drop) --g;
    }

    EncodeResult res;
    res.rendered = Image(W, H);  // 160×200 logical (web preview pairs)
    res.bitmap.assign(2048, 0);
    for (std::size_t g = 0; g < glyphs.size() && g < 256; ++g) {
        auto bytes = mc_pattern_bytes(glyphs[g]);
        for (std::size_t r = 0; r < 8; ++r)
            res.bitmap[g * 8 + r] = bytes[r];
    }
    res.screen_ram.assign(kCells, 0);
    res.color_ram.assign(kCells, 0);
    res.bg_color = bg;

    for (std::size_t i = 0; i < kCells; ++i) {
        std::uint8_t glyph_idx = static_cast<std::uint8_t>(
            std::min(cell_to_glyph[i], std::size_t{255}));
        res.screen_ram[i] = glyph_idx;
        res.color_ram[i]  = best_fg[i] & 0xF;
        std::uint64_t merged = glyphs[cell_to_glyph[i]];
        auto cy = i / kCols;
        auto cx = i % kCols;
        for (std::size_t py = 0; py < kCellH; ++py) {
            for (std::size_t px = 0; px < kCellW; ++px) {
                auto x = cx * kCellW + px;
                auto y = cy * kCellH + py;
                std::size_t bit = (py * kCellW + px) * 2;
                std::uint8_t q = (merged >> bit) & 0x3;
                std::uint8_t pal_idx;
                switch (q) {
                case 0: pal_idx = bg; break;
                case 1: pal_idx = best_mc1; break;
                case 2: pal_idx = best_mc2; break;
                default: pal_idx = best_fg[i]; break;
                }
                res.rendered[x, y] = pal_lin[pal_idx];
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-PETSCII: 320×200 text mode (40×25 cells × 8×8 PETSCII glyphs).
// Per cell: (char, fg) ∈ 256 × 16; global bg (one of 16) brute-forced
// over the full image. Pappas-Neuhoff sRGB blur metric — same shape as
// cga-text80x100 but with the C64 character ROM and VIC-II palette.
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kPetW    = 320;
constexpr std::size_t kPetH    = 200;
constexpr std::size_t kPetCellW = 8;
constexpr std::size_t kPetCellH = 8;
constexpr std::size_t kPetCols = 40;
constexpr std::size_t kPetRows = 25;
constexpr std::size_t kPetCellN = kPetCellW * kPetCellH;  // 64

// Pappas-Neuhoff blur kernel — 3×3 binomial separable [1,2,1]/4 ⊗
// [1,2,1]/4 (≈ Gaussian σ ≈ 0.85). Replicate-padded at cell edges.
// Same kernel cga_text uses — picked because the per-cell blur
// matches what the eye averages on a CRT, in sRGB (gamma-encoded)
// space which matches the display.
constexpr std::array<std::array<float, 3>, 3> kBlurKernel = {{
    {1.0f/16, 2.0f/16, 1.0f/16},
    {2.0f/16, 4.0f/16, 2.0f/16},
    {1.0f/16, 2.0f/16, 1.0f/16},
}};

struct Tap { std::uint8_t q; float w; };

// 9 taps per pixel, replicate-padded so pixels at cell edges fold
// edge taps onto boundary pixels (taps may share q values; harmless).
std::array<std::array<Tap, 9>, kPetCellN> build_petscii_taps() {
    std::array<std::array<Tap, 9>, kPetCellN> taps{};
    for (std::size_t py = 0; py < kPetCellH; ++py) {
        for (std::size_t px = 0; px < kPetCellW; ++px) {
            std::size_t p_out = py * kPetCellW + px;
            std::size_t k = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(static_cast<int>(py) + dy, 0,
                                    static_cast<int>(kPetCellH) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(static_cast<int>(px) + dx, 0,
                                        static_cast<int>(kPetCellW) - 1);
                    taps[p_out][k++] = {
                        static_cast<std::uint8_t>(
                            static_cast<std::size_t>(ny) * kPetCellW +
                            static_cast<std::size_t>(nx)),
                        kBlurKernel[static_cast<std::size_t>(dy + 1)]
                                   [static_cast<std::size_t>(dx + 1)]};
                }
            }
        }
    }
    return taps;
}

// Per-glyph blur stats: a[p] = sum of taps that touch fg pixels; ma[p]
// = (1 - a[p]) is the bg fraction. Then per-pair (fg, bg) blur cost is
// closed-form (see cga_text comments).
struct GlyphPre {
    std::uint64_t fg_mask;            // raw fg bits, 64 pixels
    std::array<float, kPetCellN> a;   // fg weight at each pixel post-blur
    float K3;                         // Σ a·(1-a)
    float K4;                         // Σ a²
    float K5;                         // Σ (1-a)²
};

std::array<GlyphPre, 256> build_glyph_precompute(
    const std::array<std::array<Tap, 9>, kPetCellN>& taps) {
    std::array<GlyphPre, 256> out{};
    for (std::size_t g = 0; g < 256; ++g) {
        // Decode the 8-byte glyph into a 64-bit fg mask.
        std::uint64_t fg_mask = 0;
        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t b = petscii::character_rom[g * 8 + row];
            for (std::size_t col = 0; col < 8; ++col) {
                std::size_t p = row * 8 + col;
                if ((b >> (7 - col)) & 1) fg_mask |= (1ULL << p);
            }
        }
        auto& gp = out[g];
        gp.fg_mask = fg_mask;
        gp.K3 = 0; gp.K4 = 0; gp.K5 = 0;
        for (std::size_t p = 0; p < kPetCellN; ++p) {
            float a = 0;
            for (auto& t : taps[p]) {
                if ((fg_mask >> t.q) & 1ULL) a += t.w;
            }
            gp.a[p] = a;
            float ma = 1.0f - a;
            gp.K3 += a * ma;
            gp.K4 += a * a;
            gp.K5 += ma * ma;
        }
    }
    return out;
}

}  // namespace

Result<EncodeResult> encode_petscii(const Image& image, Palette pal,
                                     const dither::Settings& /*settings*/,
                                     Metric metric,
                                     bool graphics_only) {
    if (image.width() != kPetW || image.height() != kPetH) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_petscii: expected {}x{} input, got {}x{}",
                        kPetW, kPetH, image.width(), image.height()),
        }};
    }

    const auto& pal_lin = palette_linear(pal);

    // PETSCII metric scoring runs in OKLab — perceptually-uniform
    // distance for nearest-pick + linear-light averaging through
    // OKLab's L channel for blur. (Matches png2c64; an earlier sRGB
    // experiment looked noticeably worse on c64 content.) The
    // 3-vector math below is space-agnostic; we just feed OKLab in.
    auto to_sblur = [](const Color3f& lin) -> color_space::OKLab {
        return color_space::linear_to_oklab(lin);
    };
    std::array<color_space::OKLab, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i) pal_s[i] = to_sblur(pal_lin[i]);

    // Pre-bake palette dot products and norms for the closed-form
    // per-pair error formula.
    std::array<std::array<float, 16>, 16> pal_dot{};
    std::array<float, 16> pal_norm{};
    for (std::size_t i = 0; i < 16; ++i) {
        pal_norm[i] = pal_s[i].L * pal_s[i].L
                    + pal_s[i].a * pal_s[i].a
                    + pal_s[i].b * pal_s[i].b;
        for (std::size_t j = 0; j < 16; ++j) {
            pal_dot[i][j] = pal_s[i].L * pal_s[j].L
                          + pal_s[i].a * pal_s[j].a
                          + pal_s[i].b * pal_s[j].b;
        }
    }

    static const auto taps  = build_petscii_taps();
    static const auto glyph = build_glyph_precompute(taps);

    EncodeResult res;
    res.rendered = Image(kPetW, kPetH);
    res.bitmap.clear();                              // text mode: no bitmap
    res.screen_ram.assign(kPetCols * kPetRows, 0);   // 1000 bytes (char codes)
    res.color_ram.assign(kPetCols * kPetRows, 0);    // 1000 bytes (per-cell fg)
    res.bg_color = 0;

    // Outer brute-force loop: try each of 16 backgrounds globally.
    // For each bg, score every cell's best (glyph, fg) and accumulate
    // total error. Pick the bg that minimises the total.
    std::array<std::uint8_t, kPetCols * kPetRows> best_chars{};
    std::array<std::uint8_t, kPetCols * kPetRows> best_fgs{};
    float best_total = std::numeric_limits<float>::infinity();
    std::uint8_t best_bg = 0;

    // Per-cell raw sRGB-as-3vec view (used by mse + ssim) and post-
    // blur view (used by blur metric's closed-form pair expansion).
    // K0 = Σ ‖blurred[p]‖² is the bg/fg-agnostic constant for blur.
    std::vector<std::array<color_space::OKLab, kPetCellN>>
        cell_raw(kPetRows * kPetCols);
    std::vector<std::array<color_space::OKLab, kPetCellN>>
        cell_blur(kPetRows * kPetCols);
    std::vector<float> cell_K0(kPetRows * kPetCols, 0.0f);
    for (std::size_t cy = 0; cy < kPetRows; ++cy) {
        for (std::size_t cx = 0; cx < kPetCols; ++cx) {
            std::array<color_space::OKLab, kPetCellN> raw{};
            for (std::size_t py = 0; py < kPetCellH; ++py) {
                for (std::size_t px = 0; px < kPetCellW; ++px) {
                    auto x = cx * kPetCellW + px;
                    auto y = cy * kPetCellH + py;
                    raw[py * kPetCellW + px] = to_sblur(image[x, y]);
                }
            }
            // Blur each pixel via 9-tap kernel. Replicate-padded at
            // cell edges (matches cga_text's convention).
            float K0 = 0;
            std::array<color_space::OKLab, kPetCellN> blurred{};
            for (std::size_t p = 0; p < kPetCellN; ++p) {
                color_space::OKLab b{0, 0, 0};
                for (auto& t : taps[p]) {
                    auto& v = raw[t.q];
                    b.L += t.w * v.L;
                    b.a += t.w * v.a;
                    b.b += t.w * v.b;
                }
                blurred[p] = b;
                K0 += b.L * b.L + b.a * b.a + b.b * b.b;
            }
            cell_raw[cy * kPetCols + cx]  = raw;
            cell_blur[cy * kPetCols + cx] = blurred;
            cell_K0[cy * kPetCols + cx]   = K0;
        }
    }

    auto score_pair_blur = [&](float K0,
                                const std::array<color_space::OKLab, kPetCellN>& blurred,
                                const GlyphPre& gp,
                                std::uint8_t fg, std::uint8_t bg) {
        color_space::OKLab K1{0, 0, 0};
        color_space::OKLab K2{0, 0, 0};
        for (std::size_t p = 0; p < kPetCellN; ++p) {
            float a = gp.a[p];
            float ma = 1.0f - a;
            K1.L += blurred[p].L * a;
            K1.a += blurred[p].a * a;
            K1.b += blurred[p].b * a;
            K2.L += blurred[p].L * ma;
            K2.a += blurred[p].a * ma;
            K2.b += blurred[p].b * ma;
        }
        const auto& pf = pal_s[fg];
        const auto& pb = pal_s[bg];
        float dot_K1 = K1.L * pf.L + K1.a * pf.a + K1.b * pf.b;
        float dot_K2 = K2.L * pb.L + K2.a * pb.a + K2.b * pb.b;
        return K0
            - 2.0f * dot_K1 - 2.0f * dot_K2
            + 2.0f * gp.K3 * pal_dot[fg][bg]
            + gp.K4 * pal_norm[fg]
            + gp.K5 * pal_norm[bg];
    };

    auto score_pair_mse = [&](const std::array<color_space::OKLab, kPetCellN>& raw,
                              const GlyphPre& gp,
                              std::uint8_t fg, std::uint8_t bg) {
        const auto& pf = pal_s[fg];
        const auto& pb = pal_s[bg];
        float err = 0.0f;
        for (std::size_t p = 0; p < kPetCellN; ++p) {
            const auto& c = ((gp.fg_mask >> p) & 1ULL) ? pf : pb;
            float dL = raw[p].L - c.L;
            float da = raw[p].a - c.a;
            float db = raw[p].b - c.b;
            err += dL * dL + da * da + db * db;
        }
        return err;
    };

    for (std::uint8_t bg = 0; bg < 16; ++bg) {
        float total = 0.0f;
        std::array<std::uint8_t, kPetCols * kPetRows> chars{};
        std::array<std::uint8_t, kPetCols * kPetRows> fgs{};
        for (std::size_t cy = 0; cy < kPetRows; ++cy) {
            for (std::size_t cx = 0; cx < kPetCols; ++cx) {
                std::size_t cell_idx = cy * kPetCols + cx;
                const auto& raw     = cell_raw[cell_idx];
                const auto& blurred = cell_blur[cell_idx];
                float K0 = cell_K0[cell_idx];

                float best_err = std::numeric_limits<float>::infinity();
                std::uint8_t best_ch = 0;
                std::uint8_t best_fg = bg;

                for (std::size_t g = 0; g < 256; ++g) {
                    if (graphics_only && !petscii::is_graphic_char(
                            static_cast<std::uint8_t>(g))) continue;
                    const auto& gp = glyph[g];
                    for (std::uint8_t fg = 0; fg < 16; ++fg) {
                        if (fg == bg) continue;
                        float err = 0.0f;
                        switch (metric) {
                        case Metric::blur:
                            err = score_pair_blur(K0, blurred, gp, fg, bg); break;
                        case Metric::mse:
                            err = score_pair_mse(raw, gp, fg, bg); break;
                        }
                        if (err < best_err) {
                            best_err = err;
                            best_ch = static_cast<std::uint8_t>(g);
                            best_fg = fg;
                        }
                    }
                }
                total += best_err;
                chars[cell_idx] = best_ch;
                fgs[cell_idx]   = best_fg;
            }
        }
        if (total < best_total) {
            best_total = total;
            best_bg    = bg;
            best_chars = chars;
            best_fgs   = fgs;
        }
    }

    res.bg_color = best_bg;
    for (std::size_t i = 0; i < kPetCols * kPetRows; ++i) {
        res.screen_ram[i] = best_chars[i];
        res.color_ram[i]  = best_fgs[i];
    }

    // Render the preview by painting each cell with its chosen
    // (char, fg) over the global bg.
    for (std::size_t cy = 0; cy < kPetRows; ++cy) {
        for (std::size_t cx = 0; cx < kPetCols; ++cx) {
            std::size_t cell_idx = cy * kPetCols + cx;
            std::uint8_t ch = best_chars[cell_idx];
            std::uint8_t fg = best_fgs[cell_idx];
            for (std::size_t py = 0; py < kPetCellH; ++py) {
                std::uint8_t row_bits =
                    petscii::character_rom[ch * 8 + py];
                for (std::size_t px = 0; px < kPetCellW; ++px) {
                    bool fg_pixel = (row_bits >> (7 - px)) & 1;
                    auto col = fg_pixel ? pal_lin[fg] : pal_lin[best_bg];
                    res.rendered[cx * kPetCellW + px,
                                 cy * kPetCellH + py] = col;
                }
            }
        }
    }

    return res;
}

}  // namespace png2amiga::c64
