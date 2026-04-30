#include "c64.hpp"

#include "palette.hpp"

#include <array>
#include <format>
#include <limits>

namespace png2amiga::c64 {

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

std::span<const Color3f, 16> palette_colors(Palette p) {
    return std::span<const Color3f, 16>(palette_linear(p));
}

Result<EncodeResult> encode_multicolor(const Image& image, Palette pal,
                                        const dither::Settings& settings) {
    constexpr std::size_t W = kCols * kCellW;  // 160
    constexpr std::size_t H = kRows * kCellH;  // 200

    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("c64::encode_multicolor: expected {}x{} input, got {}x{}",
                        W, H, image.width(), image.height()),
        }};
    }

    const auto& pal_lin  = palette_linear(pal);
    const auto& pal_lab  = palette_oklab(pal);

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

    // Pass 1: per-cell brute-force quad pick on the *undithered* source.
    // Stores the chosen (bg, c1, c2, c3) for each cell; dither runs on
    // top of these palettes in pass 2.
    std::vector<std::array<std::uint8_t, 4>> cell_quad(kRows * kCols);
    std::array<color_space::OKLab, kCellW * kCellH> cell_lab{};
    std::array<std::uint8_t, kCellW * kCellH> pix_idx{};
    for (std::size_t cy = 0; cy < kRows; ++cy) {
        for (std::size_t cx = 0; cx < kCols; ++cx) {
            for (std::size_t py = 0; py < kCellH; ++py) {
                for (std::size_t px = 0; px < kCellW; ++px) {
                    cell_lab[py * kCellW + px] =
                        src_lab[(cy * kCellH + py) * W + (cx * kCellW + px)];
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
                            std::array<std::uint8_t, 4> quad{bg, i, j, k};
                            float err = cell_error_for_quad(
                                cell_lab, quad, pal_lab, &pix_idx);
                            if (err < best_err) {
                                best_err  = err;
                                best_quad = quad;
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
                                   const dither::Settings& settings) {
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

    EncodeResult res;
    res.rendered = Image(W, H);
    res.bitmap.assign(kHiRows * kHiCols * kHiCellH, 0);  // 8000 bytes
    res.screen_ram.assign(kHiRows * kHiCols, 0);          // 1000 bytes
    // color_ram unused for hires — left empty.
    res.bg_color = 0;

    // Pass 1: per-cell pair pick. C(16, 2) = 120 pairs.
    std::vector<std::array<std::uint8_t, 2>> cell_pair(kHiRows * kHiCols);
    std::array<color_space::OKLab, kHiCellW * kHiCellH> cell_lab{};
    std::array<std::uint8_t, kHiCellW * kHiCellH> pix_idx{};
    for (std::size_t cy = 0; cy < kHiRows; ++cy) {
        for (std::size_t cx = 0; cx < kHiCols; ++cx) {
            for (std::size_t py = 0; py < kHiCellH; ++py) {
                for (std::size_t px = 0; px < kHiCellW; ++px) {
                    cell_lab[py * kHiCellW + px] =
                        src_lab[(cy * kHiCellH + py) * W + (cx * kHiCellW + px)];
                }
            }
            float best_err = std::numeric_limits<float>::infinity();
            std::array<std::uint8_t, 2> best_pair{0, 0};
            for (std::uint8_t i = 0; i < 16; ++i) {
                for (std::uint8_t j = static_cast<std::uint8_t>(i + 1);
                     j < 16; ++j) {
                    std::array<std::uint8_t, 2> pair{i, j};
                    float err = cell_error_for_pair(
                        cell_lab, pair, pal_lab, &pix_idx);
                    if (err < best_err) {
                        best_err  = err;
                        best_pair = pair;
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

}  // namespace png2amiga::c64
