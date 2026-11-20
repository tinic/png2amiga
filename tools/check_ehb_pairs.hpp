#pragma once
#include "palette_pairs.hpp"
#include "png_io.hpp"
#include "scale.hpp"
#include <print>

inline int check_ehb_pairs(const char* input) {
    using namespace png2amiga;
    auto loaded = png_io::load(input);
    if (!loaded) return 1;
    auto source = scale::resample(*loaded, 160, 128);
    if (!source) return 1;
    std::vector<Color3f> base;
    for (std::size_t i = 0; i < 32; ++i)
        base.push_back(
            palette::quantize_to_ocs(source->pixels()[i * source->pixels().size() / 32]));
    base[0] = {0, 0, 0};
    const auto original = palette::make_ehb_palette(base).colors;
    std::vector<bool> locks(32, false);
    locks[0] = locks[1] = true;
    for (auto method :
         {dither::Method::opt_checker, dither::Method::floyd_steinberg, dither::Method::none}) {
        dither::Settings settings;
        settings.method = method;
        settings.strength = 0.7f;
        auto encode = [&](const std::vector<Color3f>& pal) {
            dither::DitherResult out;
            out.indices.resize(source->pixels().size());
            std::vector<color_space::OKLab> lab;
            for (auto c : pal)
                lab.push_back(color_space::linear_to_oklab(c));
            out.total_error = dither::diffuse_raw_buffer(
                *source,
                settings,
                [&](const color_space::OKLab& target, std::size_t x, std::size_t y) {
                    std::size_t k = 0;
                    color_space::OKLab chosen{};
                    auto threshold = dither::pick_palette_index_with_ostro(
                        method, target, lab, x, y, settings.strength, 0, k, chosen);
                    out.indices[y * source->width() + x] = static_cast<std::uint8_t>(k);
                    return dither::PickResult{chosen, threshold};
                });
            return out;
        };
        auto colors = original;
        auto encoded = encode(colors);
        ssimulacra2::PrecomputedSource ref;
        ref.prepare(source->pixels(), source->width(), source->height());
        auto score = [&] {
            std::vector<Color3f> pixels;
            for (auto i : encoded.indices)
                pixels.push_back(colors[i]);
            return ssimulacra2::compute(ref, pixels);
        };
        float before = score();
        palette_pairs::refine_ehb(*source, colors, encoded, locks, encode);
        if (!std::isfinite(score()) || score() < before ||
            encode(colors).indices != encoded.indices)
            return 1;
        for (std::size_t i = 0; i < 32; ++i) {
            auto expected = palette::half_brite(colors[i]);
            const auto half = colors[i + 32];
            if (expected.r != half.r || expected.g != half.g || expected.b != half.b) return 1;
            if (i < 2 && palette::linear_to_ocs(colors[i]) != palette::linear_to_ocs(original[i]))
                return 1;
        }
        auto fixed = colors;
        auto fixed_indices = encoded.indices;
        std::vector<bool> all_locked(32, true);
        palette_pairs::refine_ehb(*source, colors, encoded, all_locked, encode);
        if (palette_pairs::key(colors, false) != palette_pairs::key(fixed, false) ||
            fixed_indices != encoded.indices)
            return 1;
        std::println(
            "EHB method {}: S2 {:+.3f}; locks, half-bright palette, and dither replay passed",
            static_cast<int>(method),
            score() - before);
    }
    return 0;
}
