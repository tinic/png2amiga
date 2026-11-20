#pragma once
#include "c64.hpp"
#include "cell_graph_refine.hpp"
#include <print>

// Regression contract: the final bitmap is exactly what the selected
// ordered quantizer produces from its final legal palette, not an
// unconstrained pattern that merely scores better after blur.
inline int check_ordered_refine() {
    using namespace png2amiga;
    using amiga::Mode;
    std::size_t improved = 0;
    for (auto method : {dither::Method::opt_checker,
                        dither::Method::opt_line,
                        dither::Method::bayer4x4,
                        dither::Method::none}) {
        dither::Settings settings;
        settings.method = method;
        settings.strength = 0.8f;
        for (auto mode : {Mode::c64_hires, Mode::c64_multicolor, Mode::c64_fli, Mode::c64_afli}) {
            const bool mc = amiga::is_c64_multicolor(mode);
            const bool sliced = mode == Mode::c64_fli || mode == Mode::c64_afli;
            const std::size_t cw = mc ? 4 : 8, w = cw * 40;
            Image source(w, 200);
            for (std::size_t y = 0; y < 200; ++y)
                for (std::size_t x = 0; x < w; ++x) {
                    const auto r = static_cast<std::uint8_t>((x * 255 / w + y / 3) % 256);
                    const auto g = static_cast<std::uint8_t>((y * 255 / 200 + x / 4) % 256);
                    const auto b = static_cast<std::uint8_t>((x / 16 * 31 + y / 24 * 23) % 256);
                    source[x, y] = color_space::srgb_u8_to_linear(r, g, b);
                }
            const auto pal = c64::Palette::colodore;
            auto enc = mode == Mode::c64_hires ? c64::encode_hires(source, pal, settings)
                       : mode == Mode::c64_multicolor
                           ? c64::encode_multicolor(source, pal, settings)
                       : mode == Mode::c64_fli ? c64::encode_fli(source, pal, settings)
                                               : c64::encode_afli(source, pal, settings);
            if (!enc) return 1;
            auto before = enc->bitmap;
            c64::refine_cells(source, *enc, mode, pal, false, settings);
            if (enc->bitmap != before) ++improved;
            auto colors = c64::palette_colors(pal);
            std::array<color_space::OKLab, 16> lab{};
            for (std::size_t c = 0; c < 16; ++c)
                lab[c] = color_space::linear_to_oklab(colors[c]);
            std::size_t mismatches = 0;
            dither::diffuse_raw_buffer(
                source,
                settings,
                [&](const color_space::OKLab& target, std::size_t x, std::size_t y) {
                    const auto cell = (y / 8) * 40 + x / cw;
                    const auto attr = enc->screen_ram[(sliced ? (y % 8) * 1000 : 0) + cell];
                    const std::array<std::uint8_t, 4> slots =
                        mc ? std::array<std::uint8_t, 4>{enc->bg_color,
                                                         static_cast<std::uint8_t>(attr >> 4),
                                                         static_cast<std::uint8_t>(attr & 15),
                                                         enc->color_ram[cell]}
                           : std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(attr & 15),
                                                         static_cast<std::uint8_t>(attr >> 4),
                                                         0,
                                                         0};
                    std::array<color_space::OKLab, 4> local{};
                    for (std::size_t q = 0; q < (mc ? 4u : 2u); ++q)
                        local[q] = lab[slots[q]];
                    std::size_t q = 0;
                    color_space::OKLab chosen{};
                    dither::pick_palette_index_with_ostro(method,
                                                          target,
                                                          {local.data(), mc ? 4u : 2u},
                                                          x,
                                                          y,
                                                          settings.strength,
                                                          0,
                                                          q,
                                                          chosen);
                    const auto bits = enc->bitmap[cell * 8 + y % 8];
                    const auto actual = (bits >> ((cw - 1 - x % cw) * (mc ? 2 : 1))) & (mc ? 3 : 1);
                    const auto want = sliced && x < 3 * cw ? 15 : slots[q];
                    if (slots[static_cast<std::size_t>(actual)] != want) ++mismatches;
                    return dither::PickResult{chosen, 0.5f};
                });
            if (mismatches) {
                std::println(stderr,
                             "Ordered bitmap contract failed: mode {}, method {}, {} pixels",
                             static_cast<int>(mode),
                             static_cast<int>(method),
                             mismatches);
                return 1;
            }
        }
    }
    if (improved == 0) {
        std::println(stderr, "Ordered refinement did not exercise any bitmap updates");
        return 1;
    }
    // Writable glyphs are shared: keep the actual bit patterns and references
    // unchanged, even when local/global color registers improve.
    for (bool mc : {false, true}) {
        Image source(mc ? 32 : 64, 16);
        for (std::size_t p = 0; p < source.pixels().size(); ++p)
            source.pixels()[p] = color_space::srgb_u8_to_linear(static_cast<std::uint8_t>(p * 7),
                                                                static_cast<std::uint8_t>(p * 3),
                                                                static_cast<std::uint8_t>(p * 13));
        dither::Settings settings;
        settings.method = dither::Method::opt_checker;
        auto enc = mc ? c64::encode_charset_multicolor(
                            source, c64::Palette::colodore, settings, c64::Metric::mse, 8, 2)
                      : c64::encode_charset_hires(
                            source, c64::Palette::colodore, settings, c64::Metric::mse, 8, 2);
        if (!enc) return 1;
        const auto bitmap = enc->bitmap, screen = enc->screen_ram;
        c64::refine_cells(source,
                          *enc,
                          mc ? Mode::c64_charset_multicolor : Mode::c64_charset_hires,
                          c64::Palette::colodore,
                          false,
                          settings);
        if (enc->bitmap != bitmap || enc->screen_ram != screen || enc->unique_glyphs > 6) return 1;
    }
    std::println("Ordered bitmap regeneration and shared charset patterns passed ({} updates)",
                 improved);
    return 0;
}
