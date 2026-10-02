#pragma once
#include "palette_pairs.hpp"
#include "png_io.hpp"
#include "scale.hpp"
#include <chrono>
#include <print>

// Check quality, hardware precision, locks, deterministic fallback, and exact
// re-encoding with the requested dither at every supported palette depth.
inline int check_palette_pairs(const char* input) {
    using namespace png2amiga;
    auto loaded = png_io::load(input);
    if (!loaded) return 1;
    auto source = scale::resample(*loaded, 320, 256);
    if (!source) return 1;
    for (int depth = 2; depth <= 8; ++depth) {
        const auto chipset = depth <= 5 ? amiga::Chipset::ocs : amiga::Chipset::aga;
        for (auto method :
             {dither::Method::opt_checker, dither::Method::none, dither::Method::floyd_steinberg}) {
            // A sampled, hardware-snapped seed tests fitting independently
            // of the automatic palette-generation path.
            std::vector<Color3f> pal;
            const auto count = std::size_t{1} << depth;
            for (std::size_t i = 0; i < count; ++i)
                pal.push_back(palette_pairs::snap(
                    source->pixels()[i * source->pixels().size() / count], depth > 5));
            pal[0] = {0, 0, 0};
            dither::Settings settings;
            settings.method = method;
            settings.strength = 0.7f;
            auto before = dither::apply(*source, pal, settings);
            auto render = [&](const auto& p, const auto& indices) {
                Image im(source->width(), source->height());
                for (std::size_t q = 0; q < indices.size(); ++q)
                    im.pixels()[q] = p[indices[q]];
                return im;
            };
            ssimulacra2::PrecomputedSource ref;
            ref.prepare(source->pixels(), source->width(), source->height());
            auto old_score = ssimulacra2::compute(ref, render(pal, before.indices).pixels());
            auto result = before;
            auto original = pal;
            std::vector<bool> locked(count, false);
            locked[0] = true;
            locked[1] = true;
            auto start = std::chrono::steady_clock::now();
            palette_pairs::refine(*source, pal, result, settings, locked, chipset);
            auto score = ssimulacra2::compute(ref, render(pal, result.indices).pixels());
            if (!std::isfinite(score) || score < old_score ||
                palette_pairs::key(std::span(pal).first(2), depth > 5) !=
                    palette_pairs::key(std::span(original).first(2), depth > 5) ||
                dither::apply(*source, pal, settings).indices != result.indices)
                return 1;
            for (auto c : pal) {
                auto snapped = palette_pairs::snap(c, depth > 5);
                if (c.r != snapped.r || c.g != snapped.g || c.b != snapped.b) return 1;
            }
            auto distinct = [](const auto& p, bool aga) {
                auto keys = palette_pairs::key(p, aga);
                return std::set<int>(keys.begin(), keys.end()).size();
            };
            if (distinct(pal, depth > 5) < distinct(original, depth > 5)) return 1;
            std::vector<bool> all_locked(count, true);
            auto stable = pal;
            palette_pairs::refine(*source, pal, result, settings, all_locked, chipset);
            if (palette_pairs::key(pal, depth > 5) != palette_pairs::key(stable, depth > 5))
                return 1;
            std::println(
                "depth {} method {}: S2 {:+.3f}, {:.3f}s",
                depth,
                static_cast<int>(method),
                score - old_score,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
    }
    return 0;
}
