#pragma once
#include "palette_pairs.hpp"
#include "thomson.hpp"

namespace png2amiga::thomson {
// TO8 palette fitting shared by every programmable mode: joint_refit, and for
// the bitmap modes palette_pairs::refine_with, each trial a complete native
// encode. Indices are read back from the rendered preview.
inline EncodeResult fit_palette(const Image& source,
                                EncodeResult seed,
                                amiga::Mode mode,
                                const dither::Settings& settings,
                                bool cells) {
    if (seed.palette.size() < 2) return seed;
    auto linear = [](const std::vector<PaletteEntry>& entries) {
        std::vector<Color3f> out;
        for (auto e : entries)
            out.push_back(color_space::srgb_hex_to_linear(palette::thomson_rgb_hex(e.r, e.g, e.b)));
        return out;
    };
    auto indices_of = [&](const EncodeResult& r, const std::vector<Color3f>& pal) {
        dither::DitherResult d;
        d.indices.resize(r.rendered.pixels().size());
        for (std::size_t i = 0; i < d.indices.size(); ++i) {
            const auto px = r.rendered.pixels()[i];
            std::size_t k = 0;
            while (k + 1 < pal.size() && !(pal[k] == px))
                ++k;
            d.indices[i] = static_cast<std::uint8_t>(k);
        }
        return d;
    };
    std::vector<std::pair<std::vector<int>, EncodeResult>> trials;
    auto encode = [&](const std::vector<Color3f>& pal) {
        std::vector<PaletteEntry> entries;
        for (auto c : pal) {
            const auto code = palette_pairs::thomson_code(c);
            entries.push_back({static_cast<std::uint8_t>(code >> 8),
                               static_cast<std::uint8_t>((code >> 4) & 15),
                               static_cast<std::uint8_t>(code & 15)});
        }
        auto r = thomson::encode(source, mode, settings, {}, &entries, cells);
        if (!r) return dither::DitherResult{};
        auto d = indices_of(*r, pal);
        trials.emplace_back(palette_pairs::key(pal, palette_pairs::kThomson), std::move(*r));
        return d;
    };
    auto colors = linear(seed.palette);
    const auto seed_key = palette_pairs::key(colors, palette_pairs::kThomson);
    auto output = indices_of(seed, colors);
    palette_pairs::joint_refit(source, colors, output, {}, palette_pairs::kThomson, false, encode);
    if (amiga::is_thomson_bitmap(mode))
        palette_pairs::refine_with(source, colors, output, {}, palette_pairs::kThomson, encode);
    const auto key = palette_pairs::key(colors, palette_pairs::kThomson);
    if (key == seed_key) return seed;
    for (auto& [k, r] : trials)
        if (k == key) return std::move(r);
    return seed;
}
// TO8 forme-couleur palette fitting. Every trial is a complete native encode,
// so its two-color cells, dither and optional cell refinement stay legal.
inline EncodeResult refine_palette(const Image& source,
                                   EncodeResult seed,
                                   const dither::Settings& settings,
                                   bool cells) {
    constexpr auto mode = amiga::Mode::thomson_to8_320x16;
    ssimulacra2::PrecomputedSource reference;
    reference.prepare(source.pixels(), source.width(), source.height());
    auto score = [&](const EncodeResult& frame) {
        return ssimulacra2::compute(reference, frame.rendered.pixels());
    };
    auto frame_for = [](const EncodeResult& frame) {
        palette_pairs::Frame result;
        for (auto e : frame.palette)
            result.palette.push_back(
                color_space::srgb_hex_to_linear(palette::thomson_rgb_hex(e.r, e.g, e.b)));
        result.indices.resize(64000);
        for (std::size_t c = 0; c < 8000; ++c) {
            const int attr = frame.page_a[c];
            const int bg = (attr & 7) | (((~attr) >> 4) & 8);
            const int fg = ((attr >> 3) & 7) | (((~attr) >> 3) & 8);
            for (std::size_t x = 0; x < 8; ++x)
                result.indices[c * 8 + x] = static_cast<std::uint8_t>(
                    (frame.page_b[c] & (0x80 >> x)) ? fg : bg);
        }
        return result;
    };
    auto entries_for = [](const std::vector<Color3f>& colors) {
        std::vector<PaletteEntry> entries;
        for (auto c : colors) {
            const auto code = palette_pairs::thomson_code(c);
            entries.push_back({static_cast<std::uint8_t>(code >> 8),
                               static_cast<std::uint8_t>((code >> 4) & 15),
                               static_cast<std::uint8_t>(code & 15)});
        }
        return entries;
    };
    auto search = [&](const EncodeResult& initial, bool refine_cells, bool paired) {
        auto current = initial;
        float best = score(current);
        for (int pass = 0; pass < 2; ++pass) {
            auto frame = frame_for(current);
            std::vector<palette_pairs::Proposal> candidates;
            std::set<std::vector<int>> seen;
            for (bool lab : {false, true}) {
                int added = 0;
                for (auto& p :
                     palette_pairs::proposals(source, frame, {}, lab, palette_pairs::kThomson, false, paired)) {
                    if (!seen.insert(palette_pairs::key(p.palette, palette_pairs::kThomson)).second) continue;
                    candidates.push_back(std::move(p));
                    if (++added == 4) break;
                }
            }
            auto winner = current;
            float next = best;
            for (const auto& p : candidates) {
                auto entries = entries_for(p.palette);
                auto trial = encode(source, mode, settings, {}, &entries, refine_cells);
                if (!trial) continue;
                const float candidate = score(*trial);
                if (std::isfinite(candidate) && candidate > next + 0.001f) {
                    next = candidate;
                    winner = std::move(*trial);
                }
            }
            if (next <= best) break;
            best = next;
            current = std::move(winner);
        }
        return current;
    };
    float best = score(seed);
    if (!std::isfinite(best)) return seed;
    auto winner = seed;
    auto consider = [&](EncodeResult candidate) {
        const float value = score(candidate);
        if (std::isfinite(value) && value > best) {
            best = value;
            winner = std::move(candidate);
        }
    };
    for (bool paired : {false, true})
        consider(search(seed, cells, paired));
    if (cells) {
        // Palette fitting before cell refinement can reach different optima.
        // Compare it too, retaining the original refined seed as a fallback.
        auto unrefined = encode(source, mode, settings);
        if (unrefined)
            for (bool paired : {false, true}) {
                auto candidate = search(*unrefined, false, paired);
                auto refined = encode(source, mode, settings, {}, &candidate.palette, true);
                if (refined) consider(std::move(*refined));
            }
    }
    return winner;
}
}  // namespace png2amiga::thomson
