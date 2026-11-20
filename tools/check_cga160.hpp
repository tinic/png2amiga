#pragma once
#include "api.hpp"
#include "color_space.hpp"
#include "png_io.hpp"
#include <algorithm>
#include <cmath>
#include <print>

inline int check_cga160() {
    using namespace png2amiga;
    auto fail = [](const char* why) { std::println(stderr, "CGA 160x100: {}", why); return 1; };
    // Every possible left/right colour pair, with bright backgrounds,
    // asymmetric patterns, and a reversed PNG palette (not hardware indices).
    std::vector<Color3f> palette;
    constexpr std::array<std::uint32_t, 16> rgb{
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
    for (auto c : rgb) palette.push_back(color_space::srgb_hex_to_linear(c));
    std::reverse(palette.begin(), palette.end());
    std::vector<std::uint8_t> source(16000), expected(16000);
    for (std::size_t i = 0; i < source.size(); i += 2) {
        auto pair = (i / 2) % 256;
        expected[i] = static_cast<std::uint8_t>(pair / 16);
        expected[i + 1] = static_cast<std::uint8_t>(pair % 16);
        source[i] = static_cast<std::uint8_t>(15 - expected[i]);
        source[i + 1] = static_cast<std::uint8_t>(15 - expected[i + 1]);
    }
    auto png = png_io::encode_palettized(source, palette, 160, 100);
    if (!png) return fail("fixture encode failed");
    api::Options opts;
    opts.mode = "cga-160";
    // Stale UI defaults/resize fields must not change an authored pixel grid.
    opts.native_par = true;
    opts.width = 640;
    opts.height = 200;
    auto encoded = api::encode_state(png->data(), png->size(), opts);
    if (!encoded.ok()) { std::println(stderr, "{}", encoded.error_msg); return 1; }
    auto& state = encoded.state;
    if (state.rendered.width() != 160 || state.rendered.height() != 100 ||
        state.indices != expected || state.raw_frame.size() != 16000)
        return fail("indexed pixel art was changed");
    // Decode the VRAM independently: 0xDE has 4 background dots followed
    // by 4 foreground dots on both displayed scanlines of every cell.
    for (std::size_t i = 0; i < expected.size(); i += 2) {
        if (state.raw_frame[i] != 0xDE ||
            state.raw_frame[i + 1] / 16 != expected[i] ||
            state.raw_frame[i + 1] % 16 != expected[i + 1])
            return fail("VRAM half-block or attribute orientation is wrong");
        for (std::size_t j = i; j < i + 2; ++j) {
            auto a = state.rendered.pixels()[j];
            auto b = palette[15 - expected[j]];
            if (std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b) > 1e-6f)
                return fail("rendered colour differs from indexed artwork");
        }
    }
    auto raw = api::convert_raw(png->data(), png->size(), opts);
    if (!raw.error.empty() || raw.data != state.raw_frame)
        return fail("raw export differs from displayed pixel art");
    auto image = api::convert(png->data(), png->size(), opts);
    if (!image.error.empty() || image.width != 160 || image.height != 100 ||
        image.data.size() < 26 || image.data[25] != 3 || image.diskBytes != 16000 ||
        image.chipBytes != 16000)
        return fail("PNG export is not a 160x100 indexed image");
    auto viewer = api::convert_viewer(png->data(), png->size(), opts);
    if (!viewer.error.empty()) return fail("DOS viewer export failed");
    std::string text(viewer.data.begin(), viewer.data.end());
    for (const char* required : {"fixed 0xDE", "crtc_init[14]", "0x7F", "0x64", "0x70",
                                 "outb(0x3D8, 0x09)", "image_data[16000]"})
        if (text.find(required) == std::string::npos) return fail("DOS timing, blink, or VRAM setup missing");
    // Doubling the authored grid and returning to 160x100 must select
    // original pixels, without ringing or mixing neighbouring colours.
    Image doubled(320, 200);
    for (std::size_t y = 0; y < 200; ++y)
        for (std::size_t x = 0; x < 320; ++x)
            doubled[x, y] = state.rendered[x / 2, y / 2];
    auto large_png = png_io::encode(doubled);
    if (!large_png) return fail("resize fixture encode failed");
    auto resized = api::encode_state(large_png->data(), large_png->size(), opts);
    if (!resized.ok() || resized.state.indices != expected)
        return fail("pixel-art resize filtered or moved indexed pixels");
    for (const char* method : {"floyd-steinberg", "bayer4x4", "yliluoma", "dbs", "smooth"}) {
        opts.dither = method;
        auto art = api::encode_state(png->data(), png->size(), opts);
        if (!art.ok() || art.state.indices != expected)
            return fail("dithering changed exact RGBI artwork");
    }
    Image grey(160, 100);
    for (auto& pixel : grey.pixels()) pixel = {0.18f, 0.18f, 0.18f};
    opts.dither = "none";
    auto nearest = api::encode_state_image(grey, opts);
    if (!nearest.ok()) return fail("nearest-colour encode failed");
    for (const char* method : {"floyd-steinberg", "bayer4x4"}) {
        opts.dither = method;
        opts.dither_strength = 1;
        opts.error_clamp = 0.12f;
        auto dithered = api::encode_state_image(grey, opts);
        if (!dithered.ok() || dithered.state.indices == nearest.state.indices)
            return fail("selected dither is a no-op on off-palette artwork");
    }
    opts.mode = "cga-160x200";
    std::vector<std::uint8_t> tall_source(32000), tall_expected(32000);
    for (std::size_t y = 0; y < 200; ++y)
        for (std::size_t x = 0; x < 160; ++x) {
            tall_source[y * 160 + x] = source[(y / 2) * 160 + x];
            tall_expected[y * 160 + x] = expected[(y / 2) * 160 + x];
        }
    auto tall_png = png_io::encode_palettized(tall_source, palette, 160, 200);
    if (!tall_png) return fail("160x200 fixture encode failed");
    auto tall = api::encode_state(tall_png->data(), tall_png->size(), opts);
    if (!tall.ok() || tall.state.rendered.width() != 160 || tall.state.rendered.height() != 200 ||
        tall.state.indices != tall_expected || tall.state.raw_frame.size() != 32000)
        return fail("160x200 pixel art or cell layout changed");
    for (std::size_t i = 0; i < 32000; i += 2)
        if (tall.state.raw_frame[i] != 0xDE || tall.state.raw_frame[i + 1] !=
            ((tall_expected[i] << 4) | tall_expected[i + 1]))
            return fail("160x200 half-block packing changed");
    auto tall_raw = api::convert_raw(tall_png->data(), tall_png->size(), opts);
    if (!tall_raw.error.empty() || tall_raw.data != tall.state.raw_frame || tall_raw.diskBytes != 32000)
        return fail("160x200 raw export changed");
    auto tall_viewer = api::convert_viewer(tall_png->data(), tall_png->size(), opts);
    if (tall_viewer.error.find("32000") == std::string::npos || !tall_viewer.data.empty())
        return fail("160x200 viewer did not reject the CGA VRAM overflow");
    opts.mode = "cga-160";
    opts.cell_refine = true;
    if (api::encode_state(png->data(), png->size(), opts).ok())
        return fail("glyph refinement accepted in a direct-pixel mode");
    std::println("CGA 160x100: all 256 RGBI pairs preserved, raw/indexed PNG/DOS exports verified");
    return 0;
}
