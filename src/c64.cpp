#include "c64.hpp"

#include "palette.hpp"
#include "petscii_rom.hpp"
#include "pipeline.hpp"
#include "ssimulacra2.hpp"

#include <atomic>

#include <array>
#include <bit>
#include <cctype>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace png2amiga::c64 {

Metric parse_metric(std::string_view s) noexcept {
    if (s == "blur") return Metric::blur;
    if (s == "mse") return Metric::mse;
    return Metric::mse;
}

std::string_view metric_name(Metric m) noexcept {
    switch (m) {
    case Metric::blur:
        return "blur";
    case Metric::mse:
        return "mse";
    }
    return "mse";
}

Palette parse_palette(std::string_view s) noexcept {
    if (s == "pepto") return Palette::pepto;
    if (s == "vice") return Palette::vice;
    if (s == "colodore") return Palette::colodore;
    if (s == "deekay") return Palette::deekay;
    if (s == "godot") return Palette::godot;
    if (s == "c64wiki" || s == "wiki") return Palette::c64wiki;
    if (s == "levy") return Palette::levy;
    return Palette::colodore;
}

std::string_view palette_name(Palette p) noexcept {
    switch (p) {
    case Palette::pepto:
        return "pepto";
    case Palette::vice:
        return "vice";
    case Palette::colodore:
        return "colodore";
    case Palette::deekay:
        return "deekay";
    case Palette::godot:
        return "godot";
    case Palette::c64wiki:
        return "c64wiki";
    case Palette::levy:
        return "levy";
    }
    return "pepto";
}

namespace {

const std::array<std::uint32_t, 16>& palette_hex(Palette p) {
    switch (p) {
    case Palette::pepto:
        return palette::kC64Pepto;
    case Palette::vice:
        return palette::kC64Vice;
    case Palette::colodore:
        return palette::kC64Colodore;
    case Palette::deekay:
        return palette::kC64Deekay;
    case Palette::godot:
        return palette::kC64Godot;
    case Palette::c64wiki:
        return palette::kC64Wiki;
    case Palette::levy:
        return palette::kC64Levy;
    }
    return palette::kC64Pepto;
}

const std::array<Color3f, 16>& palette_linear(Palette p) {
    static const auto cache = [] {
        std::array<std::array<Color3f, 16>, 7> all{};
        Palette ps[] = {Palette::pepto,
                        Palette::vice,
                        Palette::colodore,
                        Palette::deekay,
                        Palette::godot,
                        Palette::c64wiki,
                        Palette::levy};
        for (auto pp : ps) {
            const auto& hex = palette_hex(pp);
            for (std::size_t i = 0; i < 16; ++i)
                all[static_cast<std::size_t>(pp)][i] = color_space::srgb_hex_to_linear(hex[i]);
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

constexpr std::size_t kCellW = 4;  // multicolor logical pixels per cell
constexpr std::size_t kCellH = 8;
constexpr std::size_t kCols = 40;  // 160 / 4
constexpr std::size_t kRows = 25;  // 200 / 8

// FLI / AFLI hardware bug: the leftmost 3 character columns always
// display the global $D021 background color (on real VIC-II the
// per-row screen-pointer reload happens too late to read valid data
// for cells 0..2). The encoder emits whatever bytes it wants there;
// we overpaint the preview so the PNG matches what hardware shows.
constexpr std::size_t kFliBugCols = 3;

// 3×3 binomial blur kernel — same as cga_text / petscii. sRGB
// (gamma-encoded) space matches what the CRT emits.
constexpr std::array<std::array<float, 3>, 3> kCellBlur = {{
    {1.0f / 16, 2.0f / 16, 1.0f / 16},
    {2.0f / 16, 4.0f / 16, 2.0f / 16},
    {1.0f / 16, 2.0f / 16, 1.0f / 16},
}};

// Floyd-Steinberg dither against the full 16-color palette over the
// whole image (no cell constraints). Returned per-pixel index map drives
// "global-FS palette coherence": each cell picks its own slot palette
// from the top-N most-used colors in its region of this output. Without
// it, flat cells snap to a single color and dump diffusion error onto
// the next cell that happens to have a more diverse palette — visible
// as 4×8 / 8×8 cell-boundary blocking under error diffusion. (Ported
// from png2c64 commits 949d539 / b46e2a1.)
inline std::vector<std::uint8_t> global_fs_indices(
    const Image& image, const std::array<color_space::OKLab, 16>& pal_lab) {
    auto W = image.width();
    auto H = image.height();
    std::vector<color_space::OKLab> img_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            img_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);

    constexpr std::array<std::array<float, 3>, 4> fs_kernel = {{
        {1.0f, 0.0f, 7.0f / 16.0f},
        {-1.0f, 1.0f, 3.0f / 16.0f},
        {0.0f, 1.0f, 5.0f / 16.0f},
        {1.0f, 1.0f, 1.0f / 16.0f},
    }};
    constexpr float kEc = 0.12f;
    auto clamp = [](color_space::OKLab e, float m) {
        return color_space::OKLab{
            std::clamp(e.L, -m, m),
            std::clamp(e.a, -m, m),
            std::clamp(e.b, -m, m),
        };
    };

    std::vector<color_space::OKLab> err_buf(W * H);
    std::vector<std::uint8_t> out(W * H);
    for (std::size_t y = 0; y < H; ++y) {
        bool reverse = (y & 1) != 0;
        // Hoist serpentine direction sign out of the per-pixel kernel
        // loop below — replaces `reverse ? -k[0] : k[0]` cmov with a
        // constant multiply.
        const float dir = reverse ? -1.0f : 1.0f;
        for (std::size_t step = 0; step < W; ++step) {
            auto x = reverse ? (W - 1 - step) : step;
            auto idx = y * W + x;
            auto ce = clamp(err_buf[idx], kEc);
            color_space::OKLab adj{
                img_lab[idx].L + ce.L,
                img_lab[idx].a + ce.a,
                img_lab[idx].b + ce.b,
            };
            float bd = std::numeric_limits<float>::max();
            std::uint8_t bi = 0;
            for (std::size_t c = 0; c < 16; ++c) {
                auto& cl = pal_lab[c];
                float dL = adj.L - cl.L;
                float da = adj.a - cl.a;
                float db = adj.b - cl.b;
                float d = color_space::fma_dist_sq(dL, da, db);
                if (d < bd) {
                    bd = d;
                    bi = static_cast<std::uint8_t>(c);
                }
            }
            out[idx] = bi;
            const auto& cl = pal_lab[bi];
            color_space::OKLab qe{
                adj.L - cl.L,
                adj.a - cl.a,
                adj.b - cl.b,
            };
            for (auto& k : fs_kernel) {
                int nx = static_cast<int>(x) + static_cast<int>(k[0] * dir);
                int ny = static_cast<int>(y) + static_cast<int>(k[1]);
                if (nx < 0 || static_cast<std::size_t>(nx) >= W || ny < 0 ||
                    static_cast<std::size_t>(ny) >= H)
                    continue;
                auto nidx = static_cast<std::size_t>(ny) * W + static_cast<std::size_t>(nx);
                err_buf[nidx] = clamp(
                    {
                        err_buf[nidx].L + qe.L * k[2],
                        err_buf[nidx].a + qe.a * k[2],
                        err_buf[nidx].b + qe.b * k[2],
                    },
                    kEc);
            }
        }
    }
    return out;
}

// 3×3 binomial blur on a flat row-major Color3f buffer, with image-edge
// (not cell-edge) replicate padding. Used by the blur metric so the
// per-cell scoring reads from a globally-coherent blurred target —
// removes the 4×N / 8×N seams produced by per-cell replicate-pad blur
// in flat gradient regions.
inline std::vector<Color3f> global_blur_3x3(std::span<const Color3f> src,
                                            std::size_t W,
                                            std::size_t H) {
    std::vector<Color3f> out(W * H);
    for (std::size_t y = 0; y < H; ++y) {
        for (std::size_t x = 0; x < W; ++x) {
            Color3f acc{0, 0, 0};
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(H) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(W) - 1);
                    float w = kCellBlur[static_cast<std::size_t>(dy + 1)]
                                       [static_cast<std::size_t>(dx + 1)];
                    const auto& s =
                        src[static_cast<std::size_t>(ny) * W + static_cast<std::size_t>(nx)];
                    acc.r += w * s.r;
                    acc.g += w * s.g;
                    acc.b += w * s.b;
                }
            }
            out[y * W + x] = acc;
        }
    }
    return out;
}

// 9-tap replicate-padded blur table for an arbitrary cell W × H.
template<std::size_t W, std::size_t H>
struct CellTaps {
    struct T {
        std::uint8_t q;
        float w;
    };
    std::array<std::array<T, 9>, W * H> taps{};

    constexpr CellTaps() {
        for (std::size_t py = 0; py < H; ++py) {
            for (std::size_t px = 0; px < W; ++px) {
                std::size_t out = py * W + px;
                std::size_t k = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = std::clamp(static_cast<int>(py) + dy, 0, static_cast<int>(H) - 1);
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = std::clamp(static_cast<int>(px) + dx, 0, static_cast<int>(W) - 1);
                        taps[out][k++] = {
                            static_cast<std::uint8_t>(static_cast<std::size_t>(ny) * W +
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
inline float cell_error_for_quad(std::span<const color_space::OKLab> pix_lab,
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
            float d = color_space::fma_dist_sq(dL, da, db);
            if (d < best_d) {
                best_d = d;
                best_q = q;
            }
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
template<std::size_t N, std::size_t Px>
inline float score_cell(const std::array<Color3f, Px>& raw,
                        const std::array<Color3f, Px>& blurred_src,
                        const std::array<typename CellTaps<4, 8>::T, 9>* /*taps*/,  // see overloads
                        const std::array<Color3f, N>& cand,
                        Metric metric,
                        std::array<std::uint8_t, Px>* idx_out,
                        auto&& tap_lookup) {

    // Phase 1: per-pixel nearest-of-N in sRGB MSE.
    std::array<std::uint8_t, Px> idx{};
    std::array<Color3f, Px> rendered{};
    static_assert(N <= 16, "score_cell candidate count fits in uint8_t");
    for (std::size_t p = 0; p < Px; ++p) {
        float best = std::numeric_limits<float>::infinity();
        std::uint8_t best_q = 0;
        for (std::uint8_t q = 0; q < static_cast<std::uint8_t>(N); ++q) {
            float dr = raw[p].r - cand[q].r;
            float dg = raw[p].g - cand[q].g;
            float db = raw[p].b - cand[q].b;
            float d = color_space::fma_dist_sq(dr, dg, db);
            if (d < best) {
                best = d;
                best_q = q;
            }
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
            err += color_space::fma_dist_sq(dr, dg, db);
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
            err += color_space::fma_dist_sq(dr, dg, db);
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

Result<EncodeResult> encode_multicolor(const Image& image,
                                       Palette pal,
                                       const dither::Settings& settings,
                                       Metric metric) {
    (void)metric;                              // TODO: per-metric brute-force scoring; current
                                               // path uses sRGB MSE-equivalent (OKLab² nearest)
                                               // for the per-cell quad pick regardless of metric.
    constexpr std::size_t W = kCols * kCellW;  // 160
    constexpr std::size_t H = kRows * kCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_multicolor: expected {}x{} input, got {}x{}",
                        W,
                        H,
                        image.width(),
                        image.height()),
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
    std::vector<Color3f> src_s(W * H);
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
    res.bitmap.assign(kRows * kCols * kCellH, 0);  // 8000 bytes
    res.screen_ram.assign(kRows * kCols, 0);       // upper-nibble = c1, lower = c2
    res.color_ram.assign(kRows * kCols, 0);        // c3 (low nibble)
    res.bg_color = 0;  // black; per-cell bg is encoded in the cell colors
                       // even though the C64 hardware uses one shared bg
                       // register. We pick bg=0 globally and let cells use
                       // the bg "slot" for their own dark-cluster color.
                       // (Real per-image bg sweep is a TODO refinement.)

    // Pass 1: per-cell quad pick. Two paths:
    //
    //   - Any dither method (!= none): use "global-FS palette
    //     coherence". Run a full-image FS dither against all 16
    //     palette colors, then take the top-4 most-used colors in
    //     each cell as that cell's quad. Stops flat cells from snapping
    //     to one color — they get coherent gradients across cell
    //     boundaries instead of dumping accumulated diffusion error
    //     onto the next cell. Removes 4×8 / 8×8 cell-boundary blocking.
    //     Ported from png2c64 (949d539 / b46e2a1); we apply it across
    //     ordered + ED + palette-aware since all of them dither
    //     against the cell palette and benefit from a coherent
    //     neighborhood.
    //   - method == none: per-cell brute force MSE/blur — picks the
    //     optimal static palette since there's no dither budget to
    //     spread error.
    std::vector<std::array<std::uint8_t, 4>> cell_quad(kRows * kCols);
    constexpr std::size_t kCellPx = kCellW * kCellH;  // 32

    if (settings.method != dither::Method::none) {
        auto fs = global_fs_indices(image, pal_lab);
        for (std::size_t cy = 0; cy < kRows; ++cy) {
            for (std::size_t cx = 0; cx < kCols; ++cx) {
                std::array<std::uint16_t, 16> hist{};
                for (std::size_t py = 0; py < kCellH; ++py)
                    for (std::size_t px = 0; px < kCellW; ++px)
                        ++hist[fs[(cy * kCellH + py) * W + (cx * kCellW + px)]];
                std::array<std::uint8_t, 4> top{0, 0, 0, 0};
                for (std::size_t s = 0; s < 4; ++s) {
                    std::uint16_t best_cnt = 0;
                    std::uint8_t best_c = 0;
                    for (std::size_t c = 0; c < 16; ++c) {
                        if (hist[c] > best_cnt) {
                            best_cnt = hist[c];
                            best_c = static_cast<std::uint8_t>(c);
                        }
                    }
                    if (best_cnt == 0) break;
                    top[s] = best_c;
                    hist[best_c] = 0;
                }
                cell_quad[cy * kCols + cx] = top;
            }
        }
    } else {
        std::array<color_space::OKLab, kCellPx> cell_lab{};
        std::array<Color3f, kCellPx> raw{};
        std::array<Color3f, kCellPx> blurred_src{};
        std::array<std::uint8_t, kCellPx> _idx_scratch{};

        // Global source blur for metric=blur — replaces the per-cell
        // replicate-pad blur (which produced 4×N seams in gradients)
        // with an image-level 3×3 binomial blur.
        std::vector<Color3f> global_blurred;
        if (metric == Metric::blur) {
            global_blurred = global_blur_3x3(std::span<const Color3f>(src_s), W, H);
        }

        for (std::size_t cy = 0; cy < kRows; ++cy) {
            for (std::size_t cx = 0; cx < kCols; ++cx) {
                for (std::size_t py = 0; py < kCellH; ++py) {
                    for (std::size_t px = 0; px < kCellW; ++px) {
                        auto src_idx = (cy * kCellH + py) * W + (cx * kCellW + px);
                        cell_lab[py * kCellW + px] = src_lab[src_idx];
                        raw[py * kCellW + px] = src_s[src_idx];
                        if (metric == Metric::blur) {
                            blurred_src[py * kCellW + px] = global_blurred[src_idx];
                        }
                    }
                }
                float best_err = std::numeric_limits<float>::infinity();
                std::array<std::uint8_t, 4> best_quad{0, 0, 0, 0};
                for (std::uint8_t bg = 0; bg < 16; ++bg) {
                    for (std::uint8_t i = 0; i < 16; ++i) {
                        if (i == bg) continue;
                        for (std::uint8_t j = static_cast<std::uint8_t>(i + 1); j < 16; ++j) {
                            if (j == bg) continue;
                            for (std::uint8_t k = static_cast<std::uint8_t>(j + 1); k < 16; ++k) {
                                if (k == bg) continue;
                                float err;
                                if (metric == Metric::mse) {
                                    std::array<std::uint8_t, 4> q{bg, i, j, k};
                                    err = cell_error_for_quad(cell_lab, q, pal_lab, &_idx_scratch);
                                } else {
                                    std::array<Color3f, 4> cand{
                                        pal_s[bg],
                                        pal_s[i],
                                        pal_s[j],
                                        pal_s[k],
                                    };
                                    err = score_cell<4, kCellPx>(raw,
                                                                 blurred_src,
                                                                 nullptr,
                                                                 cand,
                                                                 metric,
                                                                 nullptr,
                                                                 tap_lookup);
                                }
                                if (err < best_err) {
                                    best_err = err;
                                    best_quad = {bg, i, j, k};
                                }
                            }
                        }
                    }
                }
                cell_quad[cy * kCols + cx] = best_quad;
            }
        }
    }

    // Pass 2: per-pixel index pick with dither. The pick callback
    // looks up the cell's 4-color palette and routes through
    // pick_palette_index_with_ostro — same code path the Yliluoma /
    // ED / Ostromoukhov families use elsewhere. diffuse_raw_buffer
    // owns the err_buf, ordered-bias, Riemersma queue, and structure
    // map, so every method it supports works here.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick =
        [&](const color_space::OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kCellH;
        std::size_t cx = x / kCellW;
        const auto& quad = cell_quad[cy * kCols + cx];
        std::array<color_space::OKLab, 4> cp{
            pal_lab[quad[0]],
            pal_lab[quad[1]],
            pal_lab[quad[2]],
            pal_lab[quad[3]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(settings.method,
                                                          target,
                                                          std::span<const color_space::OKLab>(cp),
                                                          x,
                                                          y,
                                                          settings.strength,
                                                          /*k_min=*/0,
                                                          chosen_index,
                                                          chosen);
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
            res.screen_ram[cell_idx] = static_cast<std::uint8_t>(((quad[1] & 0xF) << 4) |
                                                                 (quad[2] & 0xF));
            res.color_ram[cell_idx] = static_cast<std::uint8_t>(quad[3] & 0xF);
            for (std::size_t py = 0; py < kCellH; ++py) {
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kCellW; ++px) {
                    auto x = cx * kCellW + px;
                    auto y = cy * kCellH + py;
                    auto q = static_cast<std::uint8_t>(indices[y * W + x] & 0x3);
                    row_byte = static_cast<std::uint8_t>((row_byte << 2) | q);
                    res.rendered[x, y] = pal_lin[quad[q]];
                }
                res.bitmap[cell_idx * kCellH + py] = row_byte;
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-hires: 320×200, 8×8 cells, 2 colors per cell (no shared bg).
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kHiCellW = 8;
constexpr std::size_t kHiCellH = 8;
constexpr std::size_t kHiCols = 40;  // 320 / 8
constexpr std::size_t kHiRows = 25;  // 200 / 8

// Per-pixel error against a 2-color pair, returning the chosen index
// 0/1 plus the squared OKLab error.
inline float cell_error_for_pair(std::span<const color_space::OKLab> pix_lab,
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
        float ea = color_space::fma_dist_sq(dLa, daa, dba);
        float eb = color_space::fma_dist_sq(dLb, dab, dbb);
        if (ea <= eb) {
            total += ea;
            if (pixel_idx_out) (*pixel_idx_out)[p] = 0;
        } else {
            total += eb;
            if (pixel_idx_out) (*pixel_idx_out)[p] = 1;
        }
    }
    return total;
}

}  // namespace

Result<EncodeResult> encode_hires(const Image& image,
                                  Palette pal,
                                  const dither::Settings& settings,
                                  Metric metric) {
    (void)metric;                                  // TODO: per-metric brute-force scoring.
    constexpr std::size_t W = kHiCols * kHiCellW;  // 320
    constexpr std::size_t H = kHiRows * kHiCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_hires: expected {}x{} input, got {}x{}",
                        W,
                        H,
                        image.width(),
                        image.height()),
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
    res.screen_ram.assign(kHiRows * kHiCols, 0);         // 1000 bytes
    // color_ram unused for hires — left empty.
    res.bg_color = 0;

    static constexpr CellTaps<kHiCellW, kHiCellH> taps_hi{};
    auto tap_lookup = [](std::size_t p) -> std::span<const CellTaps<kHiCellW, kHiCellH>::T, 9> {
        return std::span<const CellTaps<kHiCellW, kHiCellH>::T, 9>(taps_hi.taps[p]);
    };

    // Pass 1: per-cell pair pick. Two paths:
    //
    //   - Any dither method (!= none): global-FS palette coherence —
    //     top-2 most-used colors in each cell's region of a full-
    //     palette FS dither output. Same fix as encode_multicolor.
    //   - method == none: per-cell brute force C(16,2) = 120 pairs
    //     with mse/blur scoring (no dither = static MSE-optimal pair).
    constexpr std::size_t kHiCellPx = kHiCellW * kHiCellH;  // 64
    std::vector<std::array<std::uint8_t, 2>> cell_pair(kHiRows * kHiCols);

    if (settings.method != dither::Method::none) {
        auto fs = global_fs_indices(image, pal_lab);
        for (std::size_t cy = 0; cy < kHiRows; ++cy) {
            for (std::size_t cx = 0; cx < kHiCols; ++cx) {
                std::array<std::uint16_t, 16> hist{};
                for (std::size_t py = 0; py < kHiCellH; ++py)
                    for (std::size_t px = 0; px < kHiCellW; ++px)
                        ++hist[fs[(cy * kHiCellH + py) * W + (cx * kHiCellW + px)]];
                std::array<std::uint8_t, 2> top{0, 0};
                for (std::size_t s = 0; s < 2; ++s) {
                    std::uint16_t best_cnt = 0;
                    std::uint8_t best_c = top[0];
                    for (std::size_t c = 0; c < 16; ++c) {
                        if (hist[c] > best_cnt) {
                            best_cnt = hist[c];
                            best_c = static_cast<std::uint8_t>(c);
                        }
                    }
                    top[s] = best_c;
                    hist[best_c] = 0;
                }
                cell_pair[cy * kHiCols + cx] = top;
            }
        }
    } else {
        std::array<color_space::OKLab, kHiCellPx> cell_lab{};
        std::array<Color3f, kHiCellPx> raw{};
        std::array<Color3f, kHiCellPx> blurred_src{};
        std::array<std::uint8_t, kHiCellPx> pix_idx{};

        std::vector<Color3f> src_s_buf;
        std::vector<Color3f> global_blurred;
        if (metric == Metric::blur) {
            src_s_buf.resize(W * H);
            for (std::size_t i = 0; i < W * H; ++i)
                src_s_buf[i] = {src_lab[i].L, src_lab[i].a, src_lab[i].b};
            global_blurred = global_blur_3x3(std::span<const Color3f>(src_s_buf), W, H);
        }

        for (std::size_t cy = 0; cy < kHiRows; ++cy) {
            for (std::size_t cx = 0; cx < kHiCols; ++cx) {
                for (std::size_t py = 0; py < kHiCellH; ++py) {
                    for (std::size_t px = 0; px < kHiCellW; ++px) {
                        auto idx = (cy * kHiCellH + py) * W + (cx * kHiCellW + px);
                        cell_lab[py * kHiCellW + px] = src_lab[idx];
                        raw[py * kHiCellW + px] = {src_lab[idx].L, src_lab[idx].a, src_lab[idx].b};
                        if (metric == Metric::blur) {
                            blurred_src[py * kHiCellW + px] = global_blurred[idx];
                        }
                    }
                }
                float best_err = std::numeric_limits<float>::infinity();
                std::array<std::uint8_t, 2> best_pair{0, 0};
                for (std::uint8_t i = 0; i < 16; ++i) {
                    for (std::uint8_t j = static_cast<std::uint8_t>(i + 1); j < 16; ++j) {
                        float err;
                        if (metric == Metric::mse) {
                            const auto& a = pal_lab[i];
                            const auto& b = pal_lab[j];
                            float total = 0.0f;
                            for (std::size_t p = 0; p < kHiCellPx; ++p) {
                                const auto& t = cell_lab[p];
                                float dLa = t.L - a.L, daa = t.a - a.a, dba = t.b - a.b;
                                float dLb = t.L - b.L, dab = t.a - b.a, dbb = t.b - b.b;
                                float ea = color_space::fma_dist_sq(dLa, daa, dba);
                                float eb = color_space::fma_dist_sq(dLb, dab, dbb);
                                total += std::min(ea, eb);
                            }
                            err = total;
                        } else {
                            std::array<Color3f, 2> cand{pal_s[i], pal_s[j]};
                            err = score_cell<2, kHiCellPx>(
                                raw, blurred_src, nullptr, cand, metric, &pix_idx, tap_lookup);
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
    }

    // Pass 2: per-pixel dither via diffuse_raw_buffer with per-cell
    // 2-color palette callback. Index 0/1 written to the bitmap MSB-
    // first within each row byte.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick =
        [&](const color_space::OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kHiCellH;
        std::size_t cx = x / kHiCellW;
        const auto& pair = cell_pair[cy * kHiCols + cx];
        std::array<color_space::OKLab, 2> cp{
            pal_lab[pair[0]],
            pal_lab[pair[1]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(settings.method,
                                                          target,
                                                          std::span<const color_space::OKLab>(cp),
                                                          x,
                                                          y,
                                                          settings.strength,
                                                          /*k_min=*/0,
                                                          chosen_index,
                                                          chosen);
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
            res.screen_ram[cell_idx] = static_cast<std::uint8_t>(((pair[1] & 0xF) << 4) |
                                                                 (pair[0] & 0xF));
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto x = cx * kHiCellW + px;
                    auto y = cy * kHiCellH + py;
                    auto q = static_cast<std::uint8_t>(indices[y * W + x] & 0x1);
                    row_byte = static_cast<std::uint8_t>((row_byte << 1) | q);
                    res.rendered[x, y] = pal_lin[pair[q]];
                }
                res.bitmap[cell_idx * kHiCellH + py] = row_byte;
            }
        }
    }

    return res;
}

namespace {

// Shared FLI/AFLI row-strip pair scoring (ported from thomson.cpp
// forme-couleur — same per-scanline attribute geometry, retuned for the
// C64 palette): with a dither method the colors of a strip can MIX, so a
// candidate set is scored per pixel as the min over its OKLab segments of
// distance-to-segment plus the mixing-noise penalty λ·u(1−u)·|A−B|²
// (Bernoulli mix variance — keeps far-apart pairs from winning on mean
// alone and showing up as checkerboard noise). Plain OKLab distance: the
// C64 palette has real darks, so Thomson's chroma-weighted metric and
// neighbor-coherence discount both LOSE here (sweep dcd2f83). The strip
// decision happens lazily at strip entry inside the ED pass, on the
// 3×3-blurred target shifted by the damped feedback delta — raw deltas
// carry dither PHASE error and chasing them oscillates.
constexpr float kRowPairMixNoiseLambda = 0.1875f;
constexpr float kRowPairFeedbackScale = 0.5f;
constexpr float kRowPairFeedbackClamp = 0.04f;

inline float strip_segment_score(const color_space::OKLab& t,
                                 const color_space::OKLab& a,
                                 const color_space::OKLab& b) {
    float dL = b.L - a.L, da = b.a - a.a, db = b.b - a.b;
    float seg_sq = color_space::fma_dist_sq(dL, da, db);
    if (seg_sq <= 0.0f) return color_space::fma_dist_sq(t, a);
    float u = ((t.L - a.L) * dL + (t.a - a.a) * da + (t.b - a.b) * db) / seg_sq;
    u = std::clamp(u, 0.0f, 1.0f);
    float eL = t.L - (a.L + u * dL);
    float ea = t.a - (a.a + u * da);
    float eb = t.b - (a.b + u * db);
    return color_space::fma_dist_sq(eL, ea, eb) +
           kRowPairMixNoiseLambda * u * (1.0f - u) * seg_sq;
}

inline float damp_feedback(float v) {
    return std::clamp(v * kRowPairFeedbackScale, -kRowPairFeedbackClamp, kRowPairFeedbackClamp);
}

}  // namespace

// ---------------------------------------------------------------------------
// c64-FLI: 160×200 multicolor + per-row (c1, c2) screen colors
//          within each 4×8 cell + per-cell color_ram (c3) + global bg.
// ---------------------------------------------------------------------------

Result<EncodeResult> encode_fli(const Image& image,
                                Palette pal,
                                const dither::Settings& settings,
                                Metric metric) {
    constexpr std::size_t W = kCols * kCellW;  // 160
    constexpr std::size_t H = kRows * kCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_fli: expected {}x{} input, got {}x{}",
                        W,
                        H,
                        image.width(),
                        image.height()),
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
    // 4-color palette). Per-row blur uses a 4-pixel × 1-row tap
    // table; the 3×3 binomial collapses vertically (replicate-pad)
    // to a [1,2,1]/4 horizontal kernel, which is a sensible 1D PN
    // approximation for these strip-shaped rows.
    static constexpr CellTaps<kCellW, 1> taps_row{};
    auto tap_lookup_row = [](std::size_t p) -> std::span<const CellTaps<kCellW, 1>::T, 9> {
        return std::span<const CellTaps<kCellW, 1>::T, 9>(taps_row.taps[p]);
    };

    constexpr std::uint8_t bg = 0;  // global bg fixed at black for now

    EncodeResult res;
    res.rendered = Image(W, H);                        // 160×200 logical
    res.bitmap.assign(kRows * kCols * kCellH, 0);      // 8000 bytes
    res.screen_ram.assign(kCellH * kRows * kCols, 0);  // 8000 bytes (8 RAMs)
    res.color_ram.assign(kRows * kCols, 0);            // 1000 bytes
    res.bg_color = bg;

    // Pass 1: per-cell, per-row brute force.
    //   For each candidate color_ram value cr ∈ {0..15} \ {bg}:
    //     For each of 8 rows: try every (c1, c2) pair from
    //       {0..15} \ {bg, cr}, score row error against the row's
    //       4 pixels under the 4-color set {bg, c1, c2, cr}.
    //     Sum row errors → cell error for this cr.
    //   Pick the cr that minimises total cell error, plus its
    //   row-best (c1, c2) pairs.
    //
    // Per-cell quad layout for pass 2: cell_quads[cell][row] = {bg, c1, c2, cr}.
    std::vector<std::array<std::array<std::uint8_t, 4>, kCellH>> cell_quads(kRows * kCols);
    std::vector<std::uint8_t> cell_cr(kRows * kCols, 0);

    // Global-FS palette coherence path (any dither method != none).
    // For each cell: cr = most-used non-bg color in the cell's region
    // of a full-palette FS dither output. Each row's (c1, c2) screen
    // pair = top-2 of that row's pixels excluding bg and cr. Same
    // fix as encode_multicolor — stops the per-row 4-color palette
    // from snapping into 1 dominant color and creating cell-boundary
    // blocking under error diffusion.
    //
    // NOTE: the AFLI/Thomson segment-scored quad selection (static
    // segment-scored cr pre-pass + lazy ED-feedback (c1, c2) per strip,
    // min over the quad's 6 OKLab segments) was implemented and measured
    // here 2026-06-09: a quality WASH (examples-23 mean +0.09 dB /
    // S2 −0.12; λ and feedback probes all worse) at 4× the encode time.
    // 4 colors per 4-px row is loose enough that the histogram pick
    // barely binds — don't re-attempt without a new idea.
    if (settings.method != dither::Method::none) {
        auto fs = global_fs_indices(image, pal_lab);
        for (std::size_t cy = 0; cy < kRows; ++cy) {
            for (std::size_t cx = 0; cx < kCols; ++cx) {
                std::size_t cell_idx = cy * kCols + cx;
                // Cell histogram → cr = top non-bg color.
                std::array<std::uint16_t, 16> cell_hist{};
                for (std::size_t py = 0; py < kCellH; ++py)
                    for (std::size_t px = 0; px < kCellW; ++px)
                        ++cell_hist[fs[(cy * kCellH + py) * W + (cx * kCellW + px)]];
                cell_hist[bg] = 0;
                std::uint8_t cr = bg;
                std::uint16_t best_cnt = 0;
                for (std::size_t c = 0; c < 16; ++c) {
                    if (cell_hist[c] > best_cnt) {
                        best_cnt = cell_hist[c];
                        cr = static_cast<std::uint8_t>(c);
                    }
                }
                cell_cr[cell_idx] = cr;
                std::array<std::array<std::uint8_t, 4>, kCellH> rows{};
                for (std::size_t py = 0; py < kCellH; ++py) {
                    std::array<std::uint16_t, 16> row_hist{};
                    for (std::size_t px = 0; px < kCellW; ++px)
                        ++row_hist[fs[(cy * kCellH + py) * W + (cx * kCellW + px)]];
                    row_hist[bg] = 0;
                    row_hist[cr] = 0;
                    std::uint8_t t0 = cr, t1 = cr;
                    std::uint16_t c0 = 0, c1 = 0;
                    for (std::size_t c = 0; c < 16; ++c) {
                        if (row_hist[c] > c0) {
                            c1 = c0;
                            t1 = t0;
                            c0 = row_hist[c];
                            t0 = static_cast<std::uint8_t>(c);
                        } else if (row_hist[c] > c1) {
                            c1 = row_hist[c];
                            t1 = static_cast<std::uint8_t>(c);
                        }
                    }
                    rows[py] = {bg, t0, t1, cr};
                }
                cell_quads[cell_idx] = rows;
            }
        }
    } else {

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
                            for (std::uint8_t c2 = static_cast<std::uint8_t>(c1 + 1); c2 < 16;
                                 ++c2) {
                                if (c2 == bg || c2 == cr) continue;
                                float e;
                                if (metric == Metric::mse) {
                                    std::array<std::uint8_t, 4> q{bg, c1, c2, cr};
                                    e = cell_error_for_quad(
                                        std::span<const color_space::OKLab>(row_lab),
                                        q,
                                        pal_lab,
                                        nullptr);
                                } else {
                                    std::array<Color3f, 4> cand{
                                        pal_s[bg],
                                        pal_s[c1],
                                        pal_s[c2],
                                        pal_s[cr],
                                    };
                                    e = score_cell<4, kCellW>(row_raw,
                                                              row_blurred,
                                                              nullptr,
                                                              cand,
                                                              metric,
                                                              nullptr,
                                                              tap_lookup_row);
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
    }  // end else (method == none brute-force path)

    // Pass 2: per-pixel dither using the cell's row-specific 4-color set.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick =
        [&](const color_space::OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kCellH;
        std::size_t cx = x / kCellW;
        std::size_t py = y % kCellH;
        const auto& quad = cell_quads[cy * kCols + cx][py];
        std::array<color_space::OKLab, 4> cp{
            pal_lab[quad[0]],
            pal_lab[quad[1]],
            pal_lab[quad[2]],
            pal_lab[quad[3]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(settings.method,
                                                          target,
                                                          std::span<const color_space::OKLab>(cp),
                                                          x,
                                                          y,
                                                          settings.strength,
                                                          /*k_min=*/0,
                                                          chosen_index,
                                                          chosen);
        indices[y * W + x] = static_cast<std::uint8_t>(chosen_index);
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);

    // Pack bitmap (40×25×8) + 8 screen RAMs + color RAM, render preview.
    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            std::size_t cell_idx = cy * kCols + cx;
            res.color_ram[cell_idx] = static_cast<std::uint8_t>(cell_cr[cell_idx] & 0xF);
            for (std::size_t py = 0; py < kCellH; ++py) {
                const auto& quad = cell_quads[cell_idx][py];
                // Screen RAM[py][cell_idx] = upper nibble c1, lower c2.
                res.screen_ram[py * kRows * kCols + cell_idx] = static_cast<std::uint8_t>(
                    ((quad[1] & 0xF) << 4) | (quad[2] & 0xF));
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kCellW; ++px) {
                    auto x = cx * kCellW + px;
                    auto y = cy * kCellH + py;
                    auto q = static_cast<std::uint8_t>(indices[y * W + x] & 0x3);
                    row_byte = static_cast<std::uint8_t>((row_byte << 2) | q);
                    res.rendered[x, y] = (cx < kFliBugCols) ? pal_lin[bg] : pal_lin[quad[q]];
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

Result<EncodeResult> encode_afli(const Image& image,
                                 Palette pal,
                                 const dither::Settings& settings,
                                 Metric metric) {
    constexpr std::size_t W = kHiCols * kHiCellW;  // 320
    constexpr std::size_t H = kHiRows * kHiCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_afli: expected {}x{} input, got {}x{}",
                        W,
                        H,
                        image.width(),
                        image.height()),
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
    auto tap_lookup_row = [](std::size_t p) -> std::span<const CellTaps<kHiCellW, 1>::T, 9> {
        return std::span<const CellTaps<kHiCellW, 1>::T, 9>(taps_afli_row.taps[p]);
    };

    EncodeResult res;
    res.rendered = Image(W, H);
    res.bitmap.assign(kHiRows * kHiCols * kHiCellH, 0);
    res.screen_ram.assign(kHiCellH * kHiRows * kHiCols, 0);  // 8 × 1000
    // color_ram unused for AFLI.
    res.bg_color = 0;

    // Pass 1: per-cell-row pair pick.
    //   - method != none: pairs are decided lazily during the dither pass
    //     (see decide_row_pair below) — the old 8-sample FS-histogram
    //     top-2 was pure noise at 8×1 and caused horizontal tearing.
    //   - method == none: per-row brute force C(16,2) = 120 pairs.
    std::vector<std::array<std::array<std::uint8_t, 2>, kHiCellH>> cell_pairs(kHiRows * kHiCols);

    // Dither-aware per-row-strip pair scoring (shared strip_segment_score
    // block above encode_fli): all 136 pairs against the 3×3-blurred OKLab
    // target shifted by the damped ED feedback delta at strip entry.
    std::vector<std::uint8_t> strip_decided(kHiCols * H, 0);
    std::vector<color_space::OKLab> blurred_lab;
    if (settings.method != dither::Method::none) {
        std::vector<Color3f> src_s(W * H);
        for (std::size_t i = 0; i < W * H; ++i)
            src_s[i] = {src_lab[i].L, src_lab[i].a, src_lab[i].b};
        auto blurred_s = global_blur_3x3(std::span<const Color3f>(src_s), W, H);
        blurred_lab.resize(W * H);
        for (std::size_t i = 0; i < W * H; ++i)
            blurred_lab[i] = {blurred_s[i].r, blurred_s[i].g, blurred_s[i].b};
    }
    auto decide_row_pair = [&](std::size_t cx, std::size_t y, const color_space::OKLab& delta) {
        std::array<color_space::OKLab, kHiCellW> strip{};
        for (std::size_t px = 0; px < kHiCellW; ++px) {
            const auto& s = blurred_lab[y * W + cx * kHiCellW + px];
            strip[px] = {s.L + delta.L, s.a + delta.a, s.b + delta.b};
        }
        float best_err = std::numeric_limits<float>::infinity();
        std::array<std::uint8_t, 2> best{0, 0};
        for (std::size_t i = 0; i < 16; ++i) {
            for (std::size_t j = i; j < 16; ++j) {
                float total = 0.0f;
                for (std::size_t p = 0; p < kHiCellW; ++p)
                    total += strip_segment_score(strip[p], pal_lab[i], pal_lab[j]);
                if (total < best_err) {
                    best_err = total;
                    best = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(j)};
                }
            }
        }
        cell_pairs[(y / kHiCellH) * kHiCols + cx][y % kHiCellH] = best;
        strip_decided[y * kHiCols + cx] = 1;
    };

    if (settings.method == dither::Method::none) {
        for (std::size_t cy = 0; cy < kHiRows; ++cy) {
            for (std::size_t cx = 0; cx < kHiCols; ++cx) {
                std::size_t cell_idx = cy * kHiCols + cx;
                std::array<std::array<std::uint8_t, 2>, kHiCellH> row_pairs{};
                for (std::size_t py = 0; py < kHiCellH; ++py) {
                    std::array<color_space::OKLab, kHiCellW> row_lab{};
                    std::array<Color3f, kHiCellW> row_raw{};
                    std::array<Color3f, kHiCellW> row_blurred{};
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
                        for (std::uint8_t j = static_cast<std::uint8_t>(i + 1); j < 16; ++j) {
                            float e;
                            if (metric == Metric::mse) {
                                std::array<std::uint8_t, 2> p{i, j};
                                e = cell_error_for_pair(
                                    std::span<const color_space::OKLab>(row_lab),
                                    p,
                                    pal_lab,
                                    nullptr);
                            } else {
                                std::array<Color3f, 2> cand{pal_s[i], pal_s[j]};
                                e = score_cell<2, kHiCellW>(row_raw,
                                                            row_blurred,
                                                            nullptr,
                                                            cand,
                                                            metric,
                                                            nullptr,
                                                            tap_lookup_row);
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
    }  // end method == none brute-force pre-pass

    // Pass 2: per-pixel dither against per-row 2-color palette. With a
    // dither method the row pair is decided lazily at strip ENTRY from
    // the blurred target shifted by the damped ED feedback delta.
    std::vector<std::uint8_t> indices(W * H, 0);
    auto pick =
        [&](const color_space::OKLab& target, std::size_t x, std::size_t y) -> dither::PickResult {
        std::size_t cy = y / kHiCellH;
        std::size_t cx = x / kHiCellW;
        std::size_t py = y % kHiCellH;
        if (settings.method != dither::Method::none && !strip_decided[y * kHiCols + cx]) {
            const auto& s = src_lab[y * W + x];
            decide_row_pair(cx,
                            y,
                            {damp_feedback(target.L - s.L),
                             damp_feedback(target.a - s.a),
                             damp_feedback(target.b - s.b)});
        }
        const auto& pair = cell_pairs[cy * kHiCols + cx][py];
        std::array<color_space::OKLab, 2> cp{
            pal_lab[pair[0]],
            pal_lab[pair[1]],
        };
        std::size_t chosen_index = 0;
        color_space::OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(settings.method,
                                                          target,
                                                          std::span<const color_space::OKLab>(cp),
                                                          x,
                                                          y,
                                                          settings.strength,
                                                          /*k_min=*/0,
                                                          chosen_index,
                                                          chosen);
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
                res.screen_ram[py * kHiRows * kHiCols + cell_idx] = static_cast<std::uint8_t>(
                    ((pair[1] & 0xF) << 4) | (pair[0] & 0xF));
                std::uint8_t row_byte = 0;
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto x = cx * kHiCellW + px;
                    auto y = cy * kHiCellH + py;
                    auto q = static_cast<std::uint8_t>(indices[y * W + x] & 0x1);
                    row_byte = static_cast<std::uint8_t>((row_byte << 1) | q);
                    res.rendered[x, y] = (cx < kFliBugCols) ? pal_lin[res.bg_color & 0xF]
                                                            : pal_lin[pair[q]];
                }
                res.bitmap[cell_idx * kHiCellH + py] = row_byte;
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-charset-hires: 320×200, 8×8 cells, 2 colors per cell. Per-cell
// quantisation matches encode_hires; the resulting 8-byte glyph
// patterns are then deduplicated and merged-by-Hamming-distance to
// fit the 256-slot charset budget.
// ---------------------------------------------------------------------------

namespace {

inline std::uint64_t hi_pattern_64(const std::array<std::uint8_t, kHiCellW * kHiCellH>& pix_idx) {
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

Result<EncodeResult> encode_charset_hires(const Image& image,
                                          Palette pal,
                                          const dither::Settings& settings,
                                          Metric metric,
                                          std::size_t tile_budget,
                                          std::size_t tile_reserve) {
    auto W = image.width();
    auto H = image.height();
    if (W == 0 || H == 0 || (W % kHiCellW) != 0 || (H % kHiCellH) != 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_charset_hires: input must be a "
                        "non-zero multiple of {}x{}, got {}x{}",
                        kHiCellW,
                        kHiCellH,
                        W,
                        H),
        }};
    }
    auto cs_cols = W / kHiCellW;
    auto cs_rows = H / kHiCellH;
    auto kCells = cs_cols * cs_rows;
    // c64 char index is 8-bit; cap budget at 256 in v1 even if caller
    // requests more (banking is the runtime's job — we still emit the
    // full glyph catalog but screen_ram truncates entries past 255).
    std::size_t budget = std::min(tile_budget, std::size_t{256});
    std::size_t effective_budget = (budget > tile_reserve) ? (budget - tile_reserve) : 1;

    const auto& pal_lin = palette_linear(pal);
    const auto& pal_lab = palette_oklab(pal);
    std::vector<color_space::OKLab> src_lab(W * H);
    for (std::size_t y = 0; y < H; ++y)
        for (std::size_t x = 0; x < W; ++x)
            src_lab[y * W + x] = color_space::linear_to_oklab(image[x, y]);

    constexpr std::size_t kHiCellPx = kHiCellW * kHiCellH;  // 64

    // Pass 1: per-cell brute-force pair pick. Metric dispatch
    // matches encode_hires — mse uses inlined OKLab² nearest sum
    // (fast); blur uses score_cell<2, 64> with full 2D 3×3 binomial
    // blur on (raw, rendered).
    std::array<Color3f, 16> pal_s{};
    for (std::size_t i = 0; i < 16; ++i)
        pal_s[i] = {pal_lab[i].L, pal_lab[i].a, pal_lab[i].b};
    static constexpr CellTaps<kHiCellW, kHiCellH> taps_ch{};
    auto tap_lookup = [](std::size_t p) -> std::span<const CellTaps<kHiCellW, kHiCellH>::T, 9> {
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
    for (std::size_t cy = 0; cy < cs_rows; ++cy) {
        for (std::size_t cx = 0; cx < cs_cols; ++cx) {
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    auto idx = (cy * kHiCellH + py) * W + (cx * kHiCellW + px);
                    cell_lab[py * kHiCellW + px] = src_lab[idx];
                    raw_cell[py * kHiCellW + px] = {src_lab[idx].L, src_lab[idx].a, src_lab[idx].b};
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
                for (std::uint8_t j = static_cast<std::uint8_t>(i + 1); j < 16; ++j) {
                    float err;
                    if (metric == Metric::mse) {
                        std::array<std::uint8_t, 2> pair{i, j};
                        err = cell_error_for_pair(cell_lab, pair, pal_lab, &pix_idx);
                    } else {
                        std::array<Color3f, 2> cand{pal_s[i], pal_s[j]};
                        err = score_cell<2, kHiCellPx>(
                            raw_cell, blurred_src, nullptr, cand, metric, &pix_idx, tap_lookup);
                    }
                    if (err < best_err) {
                        best_err = err;
                        best_pair = {i, j};
                    }
                }
            }
            cell_pair[cy * cs_cols + cx] = best_pair;
        }
    }

    // Pass 2: per-cell mirrored-3×3 ED.
    //
    // Charset modes dedup glyph patterns to fit a 256-tile budget. If
    // we ran a single global ED across the image, two visually
    // identical cells in different positions would dither *differently*
    // (FS depends on scan order + accumulated upstream error) and burn
    // two glyph slots instead of one. Per-cell ED would also be wrong:
    // each cell would start with a cold err_buf and snap into a
    // different pattern than its neighbor with the same content.
    //
    // Fix: build a 3W×3H buffer for each cell where the center block
    // is the source cell and the 8 surrounding blocks are mirror
    // reflections (h-flip on left/right, v-flip on top/bottom, both
    // on corners). Run ED across the full 3W×3H, take the center
    // W×H. Identical source cells produce identical mirrored buffers
    // → identical post-ED patterns → clean dedup. The mirror also
    // primes err_buf with realistic upstream content so the center
    // pattern is the steady-state response, not a transient.
    constexpr std::size_t k3W = 3 * kHiCellW;
    constexpr std::size_t k3H = 3 * kHiCellH;
    std::vector<std::uint8_t> indices(W * H, 0);
    Image block(k3W, k3H);
    std::vector<std::uint8_t> block_idx(k3W * k3H, 0);
    for (std::size_t cy = 0; cy < cs_rows; ++cy) {
        for (std::size_t cx = 0; cx < cs_cols; ++cx) {
            // Mirror-fill the 3W×3H block.
            for (std::size_t by = 0; by < 3; ++by) {
                for (std::size_t bx = 0; bx < 3; ++bx) {
                    for (std::size_t ly = 0; ly < kHiCellH; ++ly) {
                        std::size_t sy = (by == 1) ? ly : (kHiCellH - 1 - ly);
                        for (std::size_t lx = 0; lx < kHiCellW; ++lx) {
                            std::size_t sx = (bx == 1) ? lx : (kHiCellW - 1 - lx);
                            block[bx * kHiCellW + lx, by * kHiCellH + ly] =
                                image[cx * kHiCellW + sx, cy * kHiCellH + sy];
                        }
                    }
                }
            }
            const auto& pair = cell_pair[cy * cs_cols + cx];
            std::array<color_space::OKLab, 2> cp{
                pal_lab[pair[0]],
                pal_lab[pair[1]],
            };
            std::fill(block_idx.begin(), block_idx.end(), std::uint8_t{0});
            auto pick = [&](const color_space::OKLab& target,
                            std::size_t bx,
                            std::size_t by) -> dither::PickResult {
                std::size_t chosen_index = 0;
                color_space::OKLab chosen{};
                float thr = dither::pick_palette_index_with_ostro(
                    settings.method,
                    target,
                    std::span<const color_space::OKLab>(cp),
                    bx,
                    by,
                    settings.strength,
                    /*k_min=*/0,
                    chosen_index,
                    chosen);
                block_idx[by * k3W + bx] = static_cast<std::uint8_t>(chosen_index);
                return {chosen, thr};
            };
            (void)dither::diffuse_raw_buffer(block, settings, pick);
            // Copy center W×H back to the global indices buffer.
            for (std::size_t ly = 0; ly < kHiCellH; ++ly) {
                for (std::size_t lx = 0; lx < kHiCellW; ++lx) {
                    auto block_off = (kHiCellH + ly) * k3W + (kHiCellW + lx);
                    auto global_off = (cy * kHiCellH + ly) * W + (cx * kHiCellW + lx);
                    indices[global_off] = block_idx[block_off];
                }
            }
        }
    }

    // Pack the dithered per-pixel indices into per-cell 64-bit
    // patterns + final cells[].
    for (std::size_t cy = 0; cy < cs_rows; ++cy) {
        for (std::size_t cx = 0; cx < cs_cols; ++cx) {
            std::array<std::uint8_t, kHiCellPx> px{};
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                for (std::size_t pxx = 0; pxx < kHiCellW; ++pxx) {
                    auto x = cx * kHiCellW + pxx;
                    auto y = cy * kHiCellH + py;
                    px[py * kHiCellW + pxx] = indices[y * W + x] & 1;
                }
            }
            cells[cy * cs_cols + cx] = {cell_pair[cy * cs_cols + cx], hi_pattern_64(px)};
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
    while (glyphs.size() > effective_budget) {
        // Find closest pair (smallest popcount of XOR). Brute O(N²)
        // is fine here — N ≤ ~5000 cells in practice.
        std::size_t best_a = 0, best_b = 1;
        int best_dist = std::numeric_limits<int>::max();
        for (std::size_t a = 0; a < glyphs.size(); ++a) {
            for (std::size_t b = a + 1; b < glyphs.size(); ++b) {
                int d = popcount_xor(glyphs[a], glyphs[b]);
                if (d < best_dist) {
                    best_dist = d;
                    best_a = a;
                    best_b = b;
                }
            }
        }
        // Merge b → a (keep larger cell-count slot).
        std::size_t keep = best_a, drop = best_b;
        if (glyph_cells[keep].size() < glyph_cells[drop].size()) std::swap(keep, drop);
        for (auto ci : glyph_cells[drop])
            cell_to_glyph[ci] = keep;
        glyph_cells[keep].insert(
            glyph_cells[keep].end(), glyph_cells[drop].begin(), glyph_cells[drop].end());
        // Erase drop. Re-index everything > drop down by 1.
        glyphs.erase(glyphs.begin() + static_cast<std::ptrdiff_t>(drop));
        glyph_cells.erase(glyph_cells.begin() + static_cast<std::ptrdiff_t>(drop));
        for (auto& g : cell_to_glyph) {
            if (g > drop) --g;
        }
    }

    // Build charset_data: unique_glyphs × 8 bytes, tightly packed.
    // c64 char index is 8-bit; cap emitted catalog at 256 in v1.
    auto unique_glyphs = std::min(glyphs.size(), std::size_t{256});
    std::vector<std::uint8_t> charset_data(unique_glyphs * 8, 0);
    for (std::size_t g = 0; g < unique_glyphs; ++g) {
        auto bytes = hi_pattern_bytes(glyphs[g]);
        for (std::size_t r = 0; r < 8; ++r)
            charset_data[g * 8 + r] = bytes[r];
    }

    EncodeResult res;
    res.rendered = Image(W, H);
    res.cols = cs_cols;
    res.rows = cs_rows;
    res.unique_glyphs = unique_glyphs;
    res.bitmap = std::move(charset_data);
    res.screen_ram.assign(kCells, 0);  // char codes per cell
    res.color_ram.assign(kCells, 0);   // upper=c1, lower=c0
    res.bg_color = 0;

    // Render + pack screen / color. screen_ram[cell] = glyph index.
    for (std::size_t i = 0; i < kCells; ++i) {
        auto cy = i / cs_cols;
        auto cx = i % cs_cols;
        std::uint8_t glyph_idx = static_cast<std::uint8_t>(
            std::min(cell_to_glyph[i], std::size_t{255}));
        res.screen_ram[i] = glyph_idx;
        const auto& pair = cells[i].pair;
        res.color_ram[i] = static_cast<std::uint8_t>(((pair[1] & 0xF) << 4) | (pair[0] & 0xF));
        // Render: each pixel is c0 (bg) or c1 (fg) per the post-merge
        // glyph pattern.
        std::uint64_t merged_pattern = glyphs[cell_to_glyph[i]];
        for (std::size_t py = 0; py < kHiCellH; ++py) {
            for (std::size_t px = 0; px < kHiCellW; ++px) {
                auto x = cx * kHiCellW + px;
                auto y = cy * kHiCellH + py;
                std::size_t bit = py * kHiCellW + px;
                auto q = static_cast<std::uint8_t>((merged_pattern >> bit) & 1ULL);
                res.rendered[x, y] = pal_lin[pair[q]];
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// c64-charset-multicolor: 160×200 logical, 4×8 cells, 4 colors per
// cell (1 shared bg + 2 shared mc + 1 per-cell fg). Global bg / mc1 /
// mc2 are picked by brute-force outer loop; per-cell fg picked per
// cell; per-pixel index assignment runs through diffuse_raw_buffer
// with a per-cell 4-color palette callback so dither can reshape
// the pattern.
// ---------------------------------------------------------------------------

namespace {
inline std::uint64_t mc_pattern_64(const std::array<std::uint8_t, kCellW * kCellH>& pix_idx) {
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
                                               Metric metric,
                                               std::size_t tile_budget,
                                               std::size_t tile_reserve) {
    auto W = image.width();
    auto H = image.height();
    if (W == 0 || H == 0 || (W % kCellW) != 0 || (H % kCellH) != 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_charset_multicolor: input must be a "
                        "non-zero multiple of {}x{}, got {}x{}",
                        kCellW,
                        kCellH,
                        W,
                        H),
        }};
    }
    auto cs_cols = W / kCellW;
    auto cs_rows = H / kCellH;
    std::size_t budget = std::min(tile_budget, std::size_t{256});
    std::size_t effective_budget = (budget > tile_reserve) ? (budget - tile_reserve) : 1;

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
    auto tap_lookup = [](std::size_t p) -> std::span<const CellTaps<kCellW, kCellH>::T, 9> {
        return std::span<const CellTaps<kCellW, kCellH>::T, 9>(taps_mcch.taps[p]);
    };

    constexpr std::uint8_t bg = 0;
    auto kCells = cs_cols * cs_rows;
    constexpr std::size_t kCellPx = kCellW * kCellH;  // 32

    // Outer pass: brute-force shared (mc1, mc2). For each candidate,
    // iterate cells, pick best fg ∈ remaining 13 colors, sum total
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
            for (std::size_t cy = 0; cy < cs_rows; ++cy) {
                for (std::size_t cx = 0; cx < cs_cols; ++cx) {
                    for (std::size_t py = 0; py < kCellH; ++py) {
                        for (std::size_t px = 0; px < kCellW; ++px) {
                            auto idx = (cy * kCellH + py) * W + (cx * kCellW + px);
                            cell_lab[py * kCellW + px] = src_lab[idx];
                            raw_cell[py * kCellW + px] = {
                                src_lab[idx].L, src_lab[idx].a, src_lab[idx].b};
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
                            err = cell_error_for_quad(cell_lab, q, pal_lab, nullptr);
                        } else {
                            std::array<Color3f, 4> cand{
                                pal_s[bg],
                                pal_s[mc1],
                                pal_s[mc2],
                                pal_s[fg],
                            };
                            err = score_cell<4, kCellPx>(
                                raw_cell, blurred_src, nullptr, cand, metric, nullptr, tap_lookup);
                        }
                        if (err < cell_best) {
                            cell_best = err;
                            cell_fg = fg;
                        }
                    }
                    total += cell_best;
                    fgs[cy * cs_cols + cx] = cell_fg;
                }
            }
            if (total < best_total) {
                best_total = total;
                best_mc1 = mc1;
                best_mc2 = mc2;
                best_fg = std::move(fgs);
            }
        }
    }

    // Pass 2: per-cell mirrored-3×3 ED. Same pattern as
    // encode_charset_hires — each cell's ED runs on a 3W×3H buffer
    // with mirror-reflected neighbors, producing a self-consistent
    // dither that's identical for identical source cells (so dedup
    // collapses cleanly to ≤256 glyphs).
    constexpr std::size_t k3W = 3 * kCellW;
    constexpr std::size_t k3H = 3 * kCellH;
    std::vector<std::uint8_t> indices(W * H, 0);
    Image block(k3W, k3H);
    std::vector<std::uint8_t> block_idx(k3W * k3H, 0);
    for (std::size_t cy = 0; cy < cs_rows; ++cy) {
        for (std::size_t cx = 0; cx < cs_cols; ++cx) {
            for (std::size_t by = 0; by < 3; ++by) {
                for (std::size_t bx = 0; bx < 3; ++bx) {
                    for (std::size_t ly = 0; ly < kCellH; ++ly) {
                        std::size_t sy = (by == 1) ? ly : (kCellH - 1 - ly);
                        for (std::size_t lx = 0; lx < kCellW; ++lx) {
                            std::size_t sx = (bx == 1) ? lx : (kCellW - 1 - lx);
                            block[bx * kCellW + lx, by * kCellH + ly] =
                                image[cx * kCellW + sx, cy * kCellH + sy];
                        }
                    }
                }
            }
            std::uint8_t fg = best_fg[cy * cs_cols + cx];
            std::array<color_space::OKLab, 4> cp{
                pal_lab[bg],
                pal_lab[best_mc1],
                pal_lab[best_mc2],
                pal_lab[fg],
            };
            std::fill(block_idx.begin(), block_idx.end(), std::uint8_t{0});
            auto pick = [&](const color_space::OKLab& target,
                            std::size_t bx,
                            std::size_t by) -> dither::PickResult {
                std::size_t chosen_index = 0;
                color_space::OKLab chosen{};
                float thr = dither::pick_palette_index_with_ostro(
                    settings.method,
                    target,
                    std::span<const color_space::OKLab>(cp),
                    bx,
                    by,
                    settings.strength,
                    /*k_min=*/0,
                    chosen_index,
                    chosen);
                block_idx[by * k3W + bx] = static_cast<std::uint8_t>(chosen_index);
                return {chosen, thr};
            };
            (void)dither::diffuse_raw_buffer(block, settings, pick);
            for (std::size_t ly = 0; ly < kCellH; ++ly) {
                for (std::size_t lx = 0; lx < kCellW; ++lx) {
                    auto block_off = (kCellH + ly) * k3W + (kCellW + lx);
                    auto global_off = (cy * kCellH + ly) * W + (cx * kCellW + lx);
                    indices[global_off] = block_idx[block_off];
                }
            }
        }
    }

    // Pack per-cell 64-bit patterns from dithered indices.
    std::vector<std::uint64_t> patterns(kCells, 0);
    for (std::size_t cy = 0; cy < cs_rows; ++cy) {
        for (std::size_t cx = 0; cx < cs_cols; ++cx) {
            std::array<std::uint8_t, kCellPx> px{};
            for (std::size_t py = 0; py < kCellH; ++py) {
                for (std::size_t pxx = 0; pxx < kCellW; ++pxx) {
                    auto x = cx * kCellW + pxx;
                    auto y = cy * kCellH + py;
                    px[py * kCellW + pxx] = indices[y * W + x] & 0x3;
                }
            }
            patterns[cy * cs_cols + cx] = mc_pattern_64(px);
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
    while (glyphs.size() > effective_budget) {
        std::size_t a_best = 0, b_best = 1;
        int best_d = std::numeric_limits<int>::max();
        for (std::size_t a = 0; a < glyphs.size(); ++a) {
            for (std::size_t b = a + 1; b < glyphs.size(); ++b) {
                int d = mc_dist(glyphs[a], glyphs[b]);
                if (d < best_d) {
                    best_d = d;
                    a_best = a;
                    b_best = b;
                }
            }
        }
        std::size_t keep = a_best, drop = b_best;
        if (glyph_cells[keep].size() < glyph_cells[drop].size()) std::swap(keep, drop);
        for (auto ci : glyph_cells[drop])
            cell_to_glyph[ci] = keep;
        glyph_cells[keep].insert(
            glyph_cells[keep].end(), glyph_cells[drop].begin(), glyph_cells[drop].end());
        glyphs.erase(glyphs.begin() + static_cast<std::ptrdiff_t>(drop));
        glyph_cells.erase(glyph_cells.begin() + static_cast<std::ptrdiff_t>(drop));
        for (auto& g : cell_to_glyph)
            if (g > drop) --g;
    }

    EncodeResult res;
    res.rendered = Image(W, H);  // logical raster (preview pairs the
                                 // multicolor 2:1 pixel ratio at display)
    auto unique_glyphs = std::min(glyphs.size(), std::size_t{256});
    res.bitmap.assign(unique_glyphs * 8, 0);
    for (std::size_t g = 0; g < unique_glyphs; ++g) {
        auto bytes = mc_pattern_bytes(glyphs[g]);
        for (std::size_t r = 0; r < 8; ++r)
            res.bitmap[g * 8 + r] = bytes[r];
    }
    res.cols = cs_cols;
    res.rows = cs_rows;
    res.unique_glyphs = unique_glyphs;
    res.mc1 = best_mc1;
    res.mc2 = best_mc2;
    res.screen_ram.assign(kCells, 0);
    res.color_ram.assign(kCells, 0);
    res.bg_color = bg;

    for (std::size_t i = 0; i < kCells; ++i) {
        std::uint8_t glyph_idx = static_cast<std::uint8_t>(
            std::min(cell_to_glyph[i], std::size_t{255}));
        res.screen_ram[i] = glyph_idx;
        res.color_ram[i] = best_fg[i] & 0xF;
        std::uint64_t merged = glyphs[cell_to_glyph[i]];
        auto cy = i / cs_cols;
        auto cx = i % cs_cols;
        for (std::size_t py = 0; py < kCellH; ++py) {
            for (std::size_t px = 0; px < kCellW; ++px) {
                auto x = cx * kCellW + px;
                auto y = cy * kCellH + py;
                std::size_t bit = (py * kCellW + px) * 2;
                std::uint8_t q = (merged >> bit) & 0x3;
                std::uint8_t pal_idx;
                switch (q) {
                case 0:
                    pal_idx = bg;
                    break;
                case 1:
                    pal_idx = best_mc1;
                    break;
                case 2:
                    pal_idx = best_mc2;
                    break;
                default:
                    pal_idx = best_fg[i];
                    break;
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

constexpr std::size_t kPetW = 320;
constexpr std::size_t kPetH = 200;
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
    {1.0f / 16, 2.0f / 16, 1.0f / 16},
    {2.0f / 16, 4.0f / 16, 2.0f / 16},
    {1.0f / 16, 2.0f / 16, 1.0f / 16},
}};

struct Tap {
    std::uint8_t q;
    float w;
};

// 9 taps per pixel, replicate-padded so pixels at cell edges fold
// edge taps onto boundary pixels (taps may share q values; harmless).
std::array<std::array<Tap, 9>, kPetCellN> build_petscii_taps() {
    std::array<std::array<Tap, 9>, kPetCellN> taps{};
    for (std::size_t py = 0; py < kPetCellH; ++py) {
        for (std::size_t px = 0; px < kPetCellW; ++px) {
            std::size_t p_out = py * kPetCellW + px;
            std::size_t k = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(static_cast<int>(py) + dy, 0, static_cast<int>(kPetCellH) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(
                        static_cast<int>(px) + dx, 0, static_cast<int>(kPetCellW) - 1);
                    taps[p_out][k++] = {
                        static_cast<std::uint8_t>(static_cast<std::size_t>(ny) * kPetCellW +
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
    std::uint64_t fg_mask;           // raw fg bits, 64 pixels
    std::array<float, kPetCellN> a;  // fg weight at each pixel post-blur
    float K3;                        // Σ a·(1-a)
    float K4;                        // Σ a²
    float K5;                        // Σ (1-a)²
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
        gp.K3 = 0;
        gp.K4 = 0;
        gp.K5 = 0;
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

Result<EncodeResult> encode_petscii(const Image& image_in,
                                    Palette pal,
                                    const dither::Settings& /*settings*/,
                                    Metric metric,
                                    bool graphics_only,
                                    ProgressCb on_progress) {
    if (image_in.width() != kPetW || image_in.height() != kPetH) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_petscii: expected {}x{} input, got {}x{}",
                        kPetW,
                        kPetH,
                        image_in.width(),
                        image_in.height()),
        }};
    }

    // ---- PETSCII-specific source preprocessing -------------------
    // Wide-radius unsharp mask on OKLab L. amount=0.15 was tuned by
    // a fine sweep over [0..0.5] on petsciiator's 57 examples —
    // unimodal peak: mean ΔS2 +11.29 (vs +10.82 unprocessed,
    // +10.72 at 0.25), mean ΔPSNR +1.83, clean 57 / 0 / 0 sweep
    // (the lone S2-loss `total.jpg` flips to a win). Bilateral
    // denoise was tested too — monotonic regression at every σ_r,
    // dropped.
    Image image = image_in;
    constexpr float kLocalContrast = 0.15f;
    {
        // Unsharp mask: separable 1D Gaussian low-pass (σ=8 px, half-
        // width=24) on OKLab L, then add amount × (L − L_blur) back.
        // σ=8 reaches across an 8×8 PETSCII cell, so what we boost is
        // cell-scale contrast — sub-cell texture stays untouched.
        std::vector<float> Lo(kPetW * kPetH);
        for (std::size_t y = 0; y < kPetH; ++y)
            for (std::size_t x = 0; x < kPetW; ++x)
                Lo[y * kPetW + x] = color_space::linear_to_oklab(image[x, y]).L;
        constexpr int kHalf = 24;
        std::array<float, 2 * kHalf + 1> kern;
        constexpr float sig = 8.0f;
        constexpr float k_inv = -0.5f / (sig * sig);
        float ksum = 0;
        for (int i = -kHalf; i <= kHalf; ++i) {
            kern[static_cast<std::size_t>(i + kHalf)] = std::exp(k_inv * static_cast<float>(i * i));
            ksum += kern[static_cast<std::size_t>(i + kHalf)];
        }
        for (auto& v : kern)
            v /= ksum;
        std::vector<float> tmp(kPetW * kPetH);
        for (std::size_t y = 0; y < kPetH; ++y) {
            for (std::size_t x = 0; x < kPetW; ++x) {
                float s = 0;
                for (int i = -kHalf; i <= kHalf; ++i) {
                    int xx = std::clamp(static_cast<int>(x) + i, 0, static_cast<int>(kPetW) - 1);
                    s += kern[static_cast<std::size_t>(i + kHalf)] *
                         Lo[y * kPetW + static_cast<std::size_t>(xx)];
                }
                tmp[y * kPetW + x] = s;
            }
        }
        std::vector<float> Lblur(kPetW * kPetH);
        for (std::size_t y = 0; y < kPetH; ++y) {
            for (std::size_t x = 0; x < kPetW; ++x) {
                float s = 0;
                for (int i = -kHalf; i <= kHalf; ++i) {
                    int yy = std::clamp(static_cast<int>(y) + i, 0, static_cast<int>(kPetH) - 1);
                    s += kern[static_cast<std::size_t>(i + kHalf)] *
                         tmp[static_cast<std::size_t>(yy) * kPetW + x];
                }
                Lblur[y * kPetW + x] = s;
            }
        }
        for (std::size_t y = 0; y < kPetH; ++y) {
            for (std::size_t x = 0; x < kPetW; ++x) {
                auto lab = color_space::linear_to_oklab(image[x, y]);
                lab.L = std::clamp(
                    lab.L + kLocalContrast * (lab.L - Lblur[y * kPetW + x]), 0.0f, 1.0f);
                image[x, y] = color_space::oklab_to_linear(lab);
            }
        }
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
    for (std::size_t i = 0; i < 16; ++i)
        pal_s[i] = to_sblur(pal_lin[i]);

    // Pre-bake palette dot products and norms for the closed-form
    // per-pair error formula.
    std::array<std::array<float, 16>, 16> pal_dot{};
    std::array<float, 16> pal_norm{};
    for (std::size_t i = 0; i < 16; ++i) {
        const auto& pi = pal_s[i];
        pal_norm[i] = color_space::fma_dist_sq(pi.L, pi.a, pi.b);
        for (std::size_t j = 0; j < 16; ++j) {
            const auto& pj = pal_s[j];
            pal_dot[i][j] = color_space::fma_dot3(pi.L, pj.L, pi.a, pj.a, pi.b, pj.b);
        }
    }

    static const auto taps = build_petscii_taps();
    static const auto glyph = build_glyph_precompute(taps);

    EncodeResult res;
    res.rendered = Image(kPetW, kPetH);
    res.bitmap.clear();                             // text mode: no bitmap
    res.screen_ram.assign(kPetCols * kPetRows, 0);  // 1000 bytes (char codes)
    res.color_ram.assign(kPetCols * kPetRows, 0);   // 1000 bytes (per-cell fg)
    res.bg_color = 0;

    // Outer brute-force loop: try each of 16 backgrounds globally.
    // For each bg, score every cell's best (glyph, fg) and accumulate
    // total error. Pick the bg that minimises the total.
    std::array<std::uint8_t, kPetCols * kPetRows> best_chars{};
    std::array<std::uint8_t, kPetCols * kPetRows> best_fgs{};
    std::uint8_t best_bg = 0;

    // Per-cell raw sRGB-as-3vec view (used by mse + ssim) and post-
    // blur view (used by blur metric's closed-form pair expansion).
    // K0 = Σ ‖blurred[p]‖² is the bg/fg-agnostic constant for blur.
    std::vector<std::array<color_space::OKLab, kPetCellN>> cell_raw(kPetRows * kPetCols);
    std::vector<std::array<color_space::OKLab, kPetCellN>> cell_blur(kPetRows * kPetCols);
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
                K0 += color_space::fma_dist_sq(b.L, b.a, b.b);
            }
            cell_raw[cy * kPetCols + cx] = raw;
            cell_blur[cy * kPetCols + cx] = blurred;
            cell_K0[cy * kPetCols + cx] = K0;
        }
    }

    // Pre-compute (K1, K2) per (cell, glyph), once. They depend only
    // on blurred[cell] and gp.a[glyph] — neither changes with bg/fg.
    // Without this hoist they're rebuilt 16 (bg) × 16 (fg) = 256
    // times per cell-glyph; AMDuProf showed score_pair_blur at 96 %
    // of c64-petscii CPU. Memory: ncells × 256 × 2 OKLab × 12 B ≈
    // 6 MB for a 1000-cell screen — fits in L2.
    constexpr std::size_t kNumGlyphs = 256;
    const std::size_t ncells = kPetCols * kPetRows;
    std::vector<std::array<color_space::OKLab, kNumGlyphs>> K1_cg(ncells);
    std::vector<std::array<color_space::OKLab, kNumGlyphs>> K2_cg(ncells);
    if (metric == Metric::blur) {
        // Parallelise the per-cell K1/K2 build — each cell is
        // independent.
        pipeline::parallel_for(ncells, [&](std::size_t cell_idx) {
            const auto& blurred = cell_blur[cell_idx];
            for (std::size_t g = 0; g < kNumGlyphs; ++g) {
                if (graphics_only && !petscii::is_graphic_char(static_cast<std::uint8_t>(g)))
                    continue;
                const auto& gp = glyph[g];
                color_space::OKLab K1{0, 0, 0};
                color_space::OKLab K2{0, 0, 0};
                for (std::size_t p = 0; p < kPetCellN; ++p) {
                    const float a = gp.a[p];
                    const float ma = 1.0f - a;
                    K1.L = PNG2AMIGA_FMA(blurred[p].L, a, K1.L);
                    K1.a = PNG2AMIGA_FMA(blurred[p].a, a, K1.a);
                    K1.b = PNG2AMIGA_FMA(blurred[p].b, a, K1.b);
                    K2.L = PNG2AMIGA_FMA(blurred[p].L, ma, K2.L);
                    K2.a = PNG2AMIGA_FMA(blurred[p].a, ma, K2.a);
                    K2.b = PNG2AMIGA_FMA(blurred[p].b, ma, K2.b);
                }
                K1_cg[cell_idx][g] = K1;
                K2_cg[cell_idx][g] = K2;
            }
        });
    }

    // mse path: per-pair score still computes per pixel via fg_mask
    // bit-test; we precompute fg-area and bg-area pixel-error sums
    // per (cell, glyph, palette color) so the inner (g, fg, bg)
    // call is a 32-table lookup instead of 64 per-pixel adds.
    // For each (cell, glyph, c=0..15): fg_sum[c][cell][g]  =
    //   Σ_{p: fg_mask bit set} ‖raw[p] - pal_s[c]‖²
    // and bg_sum[c][cell][g] = Σ_{p: !set} ‖raw[p] - pal_s[c]‖².
    // Total sum over a pixel for any color c is invariant of which
    // it lands in, so pix_err[c][cell][p] precomputed then split by
    // mask. Memory: 16 colors × ncells × 256 glyphs × 4 B = 4 MB
    // per side (8 MB total), still L2-friendly for typical sizes.
    std::vector<std::array<std::array<float, kNumGlyphs>, 16>> mse_fg_sum;  // [c][cell][g]
    std::vector<std::array<std::array<float, kNumGlyphs>, 16>> mse_bg_sum;
    if (metric == Metric::mse) {
        mse_fg_sum.resize(ncells);
        mse_bg_sum.resize(ncells);
        // Per (cell, c): pre-compute the 64 per-pixel ‖raw[p] -
        // pal_s[c]‖² values once. Then for each glyph just split by
        // fg_mask (popcount-based scan).
        // Chroma weight for mse: ΔL² + W·(Δa² + Δb²). 0.5 was tuned on
        // petsciiator's 57 examples — at W=1.0 (plain OKLab²) mean ΔS2
        // vs petsciiator was +5.41; at W=0.5 it climbs to +5.94. The
        // VIC-II 16-color palette is chroma-extreme (saturated
        // primaries far from each other), so cells with mid-saturation
        // source content get over-penalised on chroma mismatch and the
        // encoder picks luminance-correct but chroma-wrong colors.
        // Halving the chroma term gives luminance more say in the
        // (g, fg, bg) decision; PSNR drops (+0.7→+0.18 dB) because
        // chroma is now under-prioritised numerically, but
        // SSIMULACRA2 — the perceptually meaningful metric — is what
        // we're optimizing.
        constexpr float kChromaW = 0.5f;
        pipeline::parallel_for(ncells, [&](std::size_t cell_idx) {
            const auto& raw = cell_raw[cell_idx];
            std::array<std::array<float, kPetCellN>, 16> pix_err{};
            for (std::uint8_t c = 0; c < 16; ++c) {
                const auto& pc = pal_s[c];
                for (std::size_t p = 0; p < kPetCellN; ++p) {
                    const float dL = raw[p].L - pc.L;
                    const float da = raw[p].a - pc.a;
                    const float db = raw[p].b - pc.b;
                    pix_err[c][p] = dL * dL + kChromaW * (da * da + db * db);
                }
            }
            for (std::size_t g = 0; g < kNumGlyphs; ++g) {
                if (graphics_only && !petscii::is_graphic_char(static_cast<std::uint8_t>(g)))
                    continue;
                const std::uint64_t fg_mask = glyph[g].fg_mask;
                for (std::uint8_t c = 0; c < 16; ++c) {
                    float fg_s = 0, bg_s = 0;
                    for (std::size_t p = 0; p < kPetCellN; ++p) {
                        if ((fg_mask >> p) & 1ULL)
                            fg_s += pix_err[c][p];
                        else
                            bg_s += pix_err[c][p];
                    }
                    mse_fg_sum[cell_idx][c][g] = fg_s;
                    mse_bg_sum[cell_idx][c][g] = bg_s;
                }
            }
        });
    }

    if (on_progress) on_progress(0.0f, "petscii");
    // Per-bg trial state (collected by parallel_for, then merged).
    std::vector<float> per_bg_total(16, std::numeric_limits<float>::infinity());
    std::vector<std::array<std::uint8_t, kPetCols * kPetRows>> per_bg_chars(16);
    std::vector<std::array<std::uint8_t, kPetCols * kPetRows>> per_bg_fgs(16);
    std::atomic<std::size_t> bg_done{0};

    pipeline::parallel_for(16, [&](std::size_t bg_idx) {
        const std::uint8_t bg = static_cast<std::uint8_t>(bg_idx);
        float total = 0.0f;
        std::array<std::uint8_t, kPetCols * kPetRows> chars{};
        std::array<std::uint8_t, kPetCols * kPetRows> fgs{};
        for (std::size_t cell_idx = 0; cell_idx < ncells; ++cell_idx) {
            float K0 = cell_K0[cell_idx];

            float best_err = std::numeric_limits<float>::infinity();
            std::uint8_t best_ch = 0;
            std::uint8_t best_fg = bg;

            if (metric == Metric::blur) {
                const auto& K1_g = K1_cg[cell_idx];
                const auto& K2_g = K2_cg[cell_idx];
                for (std::size_t g = 0; g < kNumGlyphs; ++g) {
                    if (graphics_only && !petscii::is_graphic_char(static_cast<std::uint8_t>(g)))
                        continue;
                    const auto& gp = glyph[g];
                    const auto& K1 = K1_g[g];
                    const auto& K2 = K2_g[g];
                    // dot_K2_bg only depends on bg + glyph — hoist
                    // outside the fg loop.
                    const auto& pb = pal_s[bg];
                    const float dot_K2_bg = color_space::fma_dot3(
                        K2.L, pb.L, K2.a, pb.a, K2.b, pb.b);
                    const float K0_minus_2dotK2_bg = K0 - 2.0f * dot_K2_bg + gp.K5 * pal_norm[bg];
                    const float K3_x2 = 2.0f * gp.K3;
                    for (std::uint8_t fg = 0; fg < 16; ++fg) {
                        if (fg == bg) continue;
                        const auto& pf = pal_s[fg];
                        const float dot_K1 = color_space::fma_dot3(
                            K1.L, pf.L, K1.a, pf.a, K1.b, pf.b);
                        const float err = K0_minus_2dotK2_bg - 2.0f * dot_K1 +
                                          K3_x2 * pal_dot[fg][bg] + gp.K4 * pal_norm[fg];
                        if (err < best_err) {
                            best_err = err;
                            best_ch = static_cast<std::uint8_t>(g);
                            best_fg = fg;
                        }
                    }
                }
            } else {
                // mse: use the precomputed fg-area / bg-area sums.
                // err(g, fg, bg) = mse_fg_sum[cell][fg][g] +
                //                  mse_bg_sum[cell][bg][g]
                for (std::size_t g = 0; g < kNumGlyphs; ++g) {
                    if (graphics_only && !petscii::is_graphic_char(static_cast<std::uint8_t>(g)))
                        continue;
                    const float bg_s = mse_bg_sum[cell_idx][bg][g];
                    for (std::uint8_t fg = 0; fg < 16; ++fg) {
                        if (fg == bg) continue;
                        const float err = mse_fg_sum[cell_idx][fg][g] + bg_s;
                        if (err < best_err) {
                            best_err = err;
                            best_ch = static_cast<std::uint8_t>(g);
                            best_fg = fg;
                        }
                    }
                }
            }
            total += best_err;
            chars[cell_idx] = best_ch;
            fgs[cell_idx] = best_fg;
        }
        per_bg_total[bg_idx] = total;
        per_bg_chars[bg_idx] = chars;
        per_bg_fgs[bg_idx] = fgs;

        if (on_progress) {
            auto done = bg_done.fetch_add(1) + 1;
            on_progress(static_cast<float>(done) / 16.0f, "petscii");
        }
    });

    // Outer bg pick: render each candidate full image and pick the
    // highest-scoring SSIMULACRA2 (the metric the bench actually
    // measures). 16 renders × one SSIMULACRA2 each — adds ~0.4 s on
    // top of the multi-second cell brute force, but bumps mean ΔS2 vs
    // petsciiator from +5.94 (per-cell-mse-sum pick) to +6.66 on its
    // 57-example test set; wins go from 49 to 51, losses 8→5.
    {
        std::vector<Color3f> src_lin(kPetW * kPetH);
        for (std::size_t y = 0; y < kPetH; ++y)
            for (std::size_t x = 0; x < kPetW; ++x)
                src_lin[y * kPetW + x] = image[x, y];
        std::vector<Color3f> rendered(kPetW * kPetH);
        float best_score = -std::numeric_limits<float>::infinity();
        for (std::uint8_t bg = 0; bg < 16; ++bg) {
            const auto& chars = per_bg_chars[bg];
            const auto& fgs = per_bg_fgs[bg];
            const auto bg_lin = pal_lin[bg];
            for (std::size_t cy = 0; cy < kPetRows; ++cy) {
                for (std::size_t cx = 0; cx < kPetCols; ++cx) {
                    std::size_t cell_idx = cy * kPetCols + cx;
                    std::uint8_t ch = chars[cell_idx];
                    auto fg_lin = pal_lin[fgs[cell_idx]];
                    for (std::size_t py = 0; py < kPetCellH; ++py) {
                        std::uint8_t row_bits = petscii::character_rom[ch * 8 + py];
                        for (std::size_t px = 0; px < kPetCellW; ++px) {
                            bool fg_pixel = (row_bits >> (7 - px)) & 1;
                            std::size_t pi = (cy * kPetCellH + py) * kPetW + (cx * kPetCellW + px);
                            rendered[pi] = fg_pixel ? fg_lin : bg_lin;
                        }
                    }
                }
            }
            float score = ssimulacra2::compute(std::span<const Color3f>(src_lin),
                                               std::span<const Color3f>(rendered),
                                               kPetW,
                                               kPetH);
            if (score > best_score) {
                best_score = score;
                best_bg = bg;
                best_chars = per_bg_chars[bg];
                best_fgs = per_bg_fgs[bg];
            }
        }
    }

    if (on_progress) on_progress(1.0f, "done");
    res.bg_color = best_bg;
    for (std::size_t i = 0; i < kPetCols * kPetRows; ++i) {
        res.screen_ram[i] = best_chars[i];
        res.color_ram[i] = best_fgs[i];
    }

    // Render the preview by painting each cell with its chosen
    // (char, fg) over the global bg.
    for (std::size_t cy = 0; cy < kPetRows; ++cy) {
        for (std::size_t cx = 0; cx < kPetCols; ++cx) {
            std::size_t cell_idx = cy * kPetCols + cx;
            std::uint8_t ch = best_chars[cell_idx];
            std::uint8_t fg = best_fgs[cell_idx];
            for (std::size_t py = 0; py < kPetCellH; ++py) {
                std::uint8_t row_bits = petscii::character_rom[ch * 8 + py];
                for (std::size_t px = 0; px < kPetCellW; ++px) {
                    bool fg_pixel = (row_bits >> (7 - px)) & 1;
                    auto col = fg_pixel ? pal_lin[fg] : pal_lin[best_bg];
                    res.rendered[cx * kPetCellW + px, cy * kPetCellH + py] = col;
                }
            }
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// .h header writer for charset modes.
// ---------------------------------------------------------------------------

namespace {

// Convert palette entry to 12-bit OCS-style 0x0RGB hex (4 bits per
// channel via top-nibble truncation of the 8-bit sRGB value).
std::uint16_t to_c64_hex_word(const Color3f& linear) {
    auto srgb = color_space::linear_to_srgb(linear);
    auto chan = [](float v) -> std::uint8_t {
        int q = std::clamp(static_cast<int>(std::lround(v * 15.0f)), 0, 15);
        return static_cast<std::uint8_t>(q);
    };
    return static_cast<std::uint16_t>((chan(srgb.r) << 8) | (chan(srgb.g) << 4) | chan(srgb.b));
}

// Uppercase a copy of `s` for #define names.
std::string to_upper_copy(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

void write_byte_array(std::string& out,
                      std::string_view name,
                      std::span<const std::uint8_t> bytes) {
    out += "static const unsigned char ";
    out += name;
    out += "[] = {\n   ";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out += std::format(" 0x{:02X}", bytes[i]);
        if (i + 1 < bytes.size()) out += ',';
        if ((i + 1) % 16 == 0 && i + 1 < bytes.size()) out += "\n   ";
    }
    out += "\n};\n\n";
}

}  // namespace

Result<std::string> charset_header(const EncodeResult& enc,
                                   std::string_view symbol_name,
                                   bool multicolor,
                                   Palette pal) {
    if (enc.cols == 0 || enc.rows == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "charset_header requires a charset EncodeResult "
            "(cols/rows must be set)",
        }};
    }
    auto upper = to_upper_copy(symbol_name);
    std::string out;
    out.reserve(8192 + enc.bitmap.size() * 6);
    out += std::format("// Generated by png2amiga. Do not edit.\n"
                       "//   {} {}x{} cells, {} unique glyph(s)\n\n"
                       "#pragma once\n\n"
                       "#define {}_COLS         {}\n"
                       "#define {}_ROWS         {}\n"
                       "#define {}_GLYPHS       {}\n"
                       "#define {}_CELL_BYTES   8\n"
                       "#define {}_BG_COLOR     0x{:02X}\n",
                       multicolor ? "multicolor charset" : "hires charset",
                       enc.cols,
                       enc.rows,
                       enc.unique_glyphs,
                       upper,
                       enc.cols,
                       upper,
                       enc.rows,
                       upper,
                       enc.unique_glyphs,
                       upper,
                       upper,
                       enc.bg_color & 0xF);
    if (multicolor) {
        out += std::format("#define {}_MC1          0x{:02X}\n"
                           "#define {}_MC2          0x{:02X}\n",
                           upper,
                           enc.mc1 & 0xF,
                           upper,
                           enc.mc2 & 0xF);
    }
    out += '\n';

    write_byte_array(
        out, std::string(symbol_name) + "_charset", std::span<const std::uint8_t>(enc.bitmap));
    write_byte_array(
        out, std::string(symbol_name) + "_screen", std::span<const std::uint8_t>(enc.screen_ram));
    write_byte_array(
        out, std::string(symbol_name) + "_color", std::span<const std::uint8_t>(enc.color_ram));

    // Full 16-color VIC-II palette as 12-bit 0x0RGB words. The screen
    // and color bytes index into the standard VIC-II palette, but
    // emitting the resolved hex makes downstream tooling self-contained.
    auto pal_lin = palette_colors(pal);
    out += std::format("static const unsigned short {}_palette[16] = {{\n", symbol_name);
    out += "    ";
    for (std::size_t i = 0; i < 16; ++i) {
        out += std::format("0x{:04X}", to_c64_hex_word(pal_lin[i]));
        if (i + 1 < 16) out += ", ";
        if ((i + 1) % 8 == 0 && i + 1 < 16) out += "\n    ";
    }
    out += "\n};\n";
    return out;
}

}  // namespace png2amiga::c64
