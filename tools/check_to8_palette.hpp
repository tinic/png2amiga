#pragma once
#include "api.hpp"
#include "thomson.hpp"
#include "palette_pairs.hpp"
#include "dither_tuning.hpp"
#include "png_io.hpp"
#include "scale.hpp"
#include <print>

inline int check_to8_palette(const char* input, const char* method, bool cells, float minimum) {
    using namespace png2amiga;
    auto loaded = png_io::load(input);
    if (!loaded) return 1;
    auto source = scale::resample(*loaded, 320, 200);
    if (!source) return 1;
    constexpr auto mode = amiga::Mode::thomson_to8_320x16;
    dither::Settings settings;
    settings.method = *dither::parse_method_or_null(method);
    auto defaults = dither_tuning::defaults_for(
        {mode, 5, false, false, false, amiga::Chipset::ocs, settings.method});
    settings.strength = defaults.strength;
    settings.error_clamp = defaults.error_clamp;
    auto before = thomson::encode(*source, mode, settings, {}, nullptr, cells);
    if (!before) return 1;
    api::Options opts;
    opts.mode = "thomson-to8-320x16";
    opts.dither = method;
    opts.cell_refine = cells;
    opts.dither_strength = settings.strength;
    opts.error_clamp = settings.error_clamp;
    auto result = api::encode_state_image(*source, opts);
    if (!result.ok()) return 1;
    auto& after = result.state;
    ssimulacra2::PrecomputedSource ref;
    ref.prepare(source->pixels(), 320, 200);
    float old_score = ssimulacra2::compute(ref, before->rendered.pixels());
    float new_score = ssimulacra2::compute(ref, after.rendered.pixels());
    if (!std::isfinite(new_score) || new_score < old_score || new_score < minimum - 0.001f) {
        std::println(stderr, "S2 {} < baseline {} or required {}", new_score, old_score, minimum);
        return 1;
    }
    if (after.raw_frame.size() != 16000 || after.palette.size() != 16) return 1;
    std::vector<thomson::PaletteEntry> entries;
    for (auto c : after.palette) {
        auto code = palette_pairs::thomson_code(c);
        auto exact = palette_pairs::snap(c, false, true);
        if (c.r != exact.r || c.g != exact.g || c.b != exact.b) return 1;
        entries.push_back({static_cast<std::uint8_t>(code >> 8),
                           static_cast<std::uint8_t>((code >> 4) & 15),
                           static_cast<std::uint8_t>(code & 15)});
    }
    // Decode native VRAM independently, checking every displayed pixel.
    for (std::size_t cell = 0; cell < 8000; ++cell) {
        const int a = after.raw_frame[cell];
        const int bg = (a & 7) | (((~a) >> 4) & 8), fg = ((a >> 3) & 7) | (((~a) >> 3) & 8);
        for (std::size_t x = 0; x < 8; ++x) {
            const auto index = static_cast<std::size_t>(
                (after.raw_frame[8000 + cell] & (0x80 >> x)) ? fg : bg);
            auto expected = after.palette[index], pixel = after.rendered.pixels()[cell * 8 + x];
            if (expected.r != pixel.r || expected.g != pixel.g || expected.b != pixel.b) return 1;
        }
    }
    auto replay = thomson::encode(*source, mode, settings, {}, &entries, cells);
    if (!replay) return 1;
    std::vector<std::uint8_t> raw = replay->page_a;
    raw.insert(raw.end(), replay->page_b.begin(), replay->page_b.end());
    if (raw != after.raw_frame) return 1;
    std::println("{} {} cells={}: S2 {:.3f} -> {:.3f}; native replay passed",
                 input,
                 method,
                 cells,
                 old_score,
                 new_score);
    return 0;
}
