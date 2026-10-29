#pragma once
#include "api.hpp"
#include "color_space.hpp"
#include "png_io.hpp"
#include <array>
#include <cmath>
#include <print>

inline int check_tandy() {
    using namespace png2amiga;
    auto fail = [](const char* why) { std::println(stderr, "Tandy: {}", why); return 1; };
    constexpr std::array<std::uint32_t, 16> colors{
        0x000000,0x0000AA,0x00AA00,0x00AAAA,0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
        0x555555,0x5555FF,0x55FF55,0x55FFFF,0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF};
    std::vector<Color3f> palette;
    for (auto color : colors) palette.push_back(color_space::srgb_hex_to_linear(color));
    for (const char* mode : {"tandy-160", "tandy-320", "tandy-640"}) {
        std::string name = mode;
        std::size_t width = name == "tandy-160" ? 160u : name == "tandy-320" ? 320u : 640u;
        const bool high = width == 640;
        std::size_t vram_size = width == 160 ? 16384u : 32768u;
        std::vector<std::uint8_t> indices(width * 200), expected(indices.size());
        constexpr std::array<std::uint8_t, 4> selected{1, 6, 10, 15};
        for (std::size_t y = 0; y < 200; ++y)
            for (std::size_t x = 0; x < width; ++x) {
                auto q = static_cast<std::uint8_t>((x + 5 * y + x / 7) % (high ? 4u : 16u));
                expected[y * width + x] = high ? selected[q] : q;
                indices[y * width + x] = static_cast<std::uint8_t>(15 - expected[y * width + x]);
            }
        auto reversed = palette;
        std::reverse(reversed.begin(), reversed.end());
        auto png = png_io::encode_palettized(indices, reversed, width, 200);
        if (!png) return fail("fixture encode failed");
        api::Options opts;
        opts.mode = mode;
        opts.chipset = "tandy";
        opts.native_par = true;
        opts.dither = "bayer4x4";
        auto enc = api::encode_state(png->data(), png->size(), opts);
        if (!enc.ok()) { std::println(stderr, "{}", enc.error_msg); return 1; }
        auto& state = enc.state;
        if (state.rendered.width() != width || state.rendered.height() != 200 ||
            state.raw_frame.size() != vram_size || state.palette.size() != (high ? 4u : 16u))
            return fail("native geometry, colour count or VRAM size changed");
        auto raw = api::convert_raw(png->data(), png->size(), opts);
        if (!raw.error.empty() || raw.data.size() != vram_size + 16 ||
            raw.diskBytes != static_cast<int>(vram_size + 16)) return fail("raw layout or size metadata changed");
        // Independently decode every physical pixel, including all bank
        // boundaries, last rows, nibble phases and bitplane byte pairs.
        const std::size_t banks = width == 160 ? 2u : 4u;
        const std::size_t stride = width == 160 ? 80u : 160u;
        std::vector<bool> used(vram_size, false);
        for (std::size_t y = 0; y < 200; ++y) {
            auto base = (y % banks) * 0x2000 + (y / banks) * stride;
            for (std::size_t x = 0; x < width; ++x) {
                std::uint8_t index;
                if (high) {
                    auto offset = base + 2 * (x / 8);
                    auto bit = 7 - (x % 8);
                    index = static_cast<std::uint8_t>(((raw.data[offset] >> bit) & 1) |
                                                      (((raw.data[offset + 1] >> bit) & 1) << 1));
                    used[offset] = used[offset + 1] = true;
                } else {
                    auto offset = base + x / 2;
                    index = static_cast<std::uint8_t>((raw.data[offset] >> (x % 2 ? 0 : 4)) & 15);
                    used[offset] = true;
                }
                auto hw = raw.data[vram_size + index];
                if (hw != expected[y * width + x]) return fail("VRAM/palette pixel differs from indexed source");
                auto a = state.rendered[x, y]; auto b = palette[hw];
                if (std::abs(a.r-b.r) + std::abs(a.g-b.g) + std::abs(a.b-b.b) > 1e-6f)
                    return fail("RGBI preview differs from exported VRAM");
            }
        }
        for (std::size_t i = 0; i < vram_size; ++i)
            if (!used[i] && raw.data[i]) return fail("VRAM bank padding is not zero");
        auto viewer = api::convert_viewer(png->data(), png->size(), opts);
        if (!viewer.error.empty()) return fail("DOS viewer export failed");
        std::string source(viewer.data.begin(), viewer.data.end());
        const char* bios = width == 160 ? "set_mode(0x08)" : width == 320 ? "set_mode(0x09)" : "set_mode(0x0A)";
        for (const char* required : {bios, "outb(0x3DE", "outb(0x3DA, 0)", "image_palette[16]", "blit_b800"})
            if (source.find(required) == std::string::npos) return fail("viewer mode/palette setup missing");
        auto header = api::convert_cheader(png->data(), png->size(), opts);
        if (header.error.empty()) return fail("Amiga bitplane header accepted for Tandy pixels");
        opts.cell_refine = true;
        if (api::encode_state(png->data(), png->size(), opts).ok()) return fail("glyph refinement accepted");
        opts.cell_refine = false; opts.copper = true;
        if (api::encode_state(png->data(), png->size(), opts).ok()) return fail("Amiga copper accepted");
        opts.copper = false;
        Image grey(width, 200);
        for (auto& c : grey.pixels()) c = {0.18f, 0.18f, 0.18f};
        opts.dither = "none";
        auto nearest = api::encode_state_image(grey, opts);
        opts.dither = "floyd-steinberg"; opts.dither_strength = 1; opts.error_clamp = 0.12f;
        auto dithered = api::encode_state_image(grey, opts);
        if (!nearest.ok() || !dithered.ok() || nearest.state.indices == dithered.state.indices)
            return fail("selected dithering did not reach the pixel encoder");
    }
    std::println("Tandy: native pixels, all banks, four-colour bitplanes, palettes, dither and DOS viewers verified");
    return 0;
}
