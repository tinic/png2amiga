#pragma once
#include "api.hpp"
#include "thomson_k7.hpp"
#include "thomson.hpp"
#include "color_space.hpp"
#include <fstream>
#include <iterator>
#include <print>

inline int check_thomson_k7(const char* path) {
    using namespace png2amiga;
    auto fail = [](const char* s) { std::println(stderr, "K7: {}", s); return 1; };
    // Independent to7.fr RGB reference: cover every hardware color, including
    // inverted pastel bits in both foreground and background attributes.
    constexpr std::array<std::uint32_t, 16> reference{
        0x000000,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff,
        0xaaaaaa,0xffaaaa,0xaaffaa,0xffffaa,0x2aaaff,0xffaaff,0xaaffff,0xffaa2a};
    auto check_rgb = [&](std::span<const std::uint8_t> frame, const Image& rendered) {
        for (std::size_t i = 0; i < 64000; ++i) {
            const unsigned attr = frame[i / 8];
            const unsigned index = (frame[8000 + i / 8] & (0x80 >> (i % 8)))
                ? ((attr >> 3) & 7) | ((~attr >> 3) & 8)
                : (attr & 7) | ((~attr >> 4) & 8);
            const auto expected = color_space::srgb_hex_to_linear(reference[index]);
            if (rendered.pixels()[i] != expected) return false;
        }
        return true;
    };
    Image swatches(320, 200);
    for (std::size_t y = 0; y < 200; ++y)
        for (std::size_t x = 0; x < 320; ++x)
            swatches[x, y] = color_space::srgb_hex_to_linear(reference[y * 16 / 200]);
    dither::Settings d;
    d.method = dither::Method::none;
    auto swatch = thomson::encode(swatches, amiga::Mode::thomson_to7_320x16, d);
    if (!swatch) return fail("swatch encoding failed");
    std::vector<std::uint8_t> frame = swatch->page_a;
    frame.insert(frame.end(), swatch->page_b.begin(), swatch->page_b.end());
    if (!check_rgb(frame, swatch->rendered))
        return fail("swatch video memory and preview RGB disagree");
    // The encoder blurs its target for cell selection; inspect the interior
    // of each band, away from transitions between different colors.
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t y = (index * 200 + 100) / 16;
        if (swatch->rendered[160, y] != swatches[160, y])
            return fail("16-color swatches differ from TO7/70 RGB reference");
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> input((std::istreambuf_iterator<char>(f)), {});
    api::Options options;
    options.mode = "thomson-to7-320x16";
    options.dither = "opt-checker";
    const auto tape = api::convert_k7(input.data(), input.size(), options);
    const auto raw = api::convert_raw(input.data(), input.size(), options);
    if (!tape.error.empty() || !raw.error.empty()) return fail("encode failed");
    const auto state = api::encode_state(input.data(), input.size(), options);
    if (!state.ok() || !check_rgb(raw.data, state.state.rendered))
        return fail("exported colors differ from preview");
    const auto& bytes = tape.data;
    std::vector<std::uint8_t> binary;
    std::size_t p = 0, blocks = 0;
    bool eof = false;
    while (p < bytes.size()) {
        const auto start = p;
        while (p < bytes.size() && bytes[p] == 255) ++p;
        if (p-start < 10 || p+5 > bytes.size() || bytes[p++] != 1 || bytes[p++] != 60)
            return fail("invalid leader");
        const auto type = bytes[p++], count = bytes[p++];
        if (p + count >= bytes.size()) return fail("truncated block");
        unsigned sum = type + count;
        for (std::size_t i = 0; i < count; ++i) sum += bytes[p+i];
        if ((sum & 255) != bytes[p+count]) return fail("checksum mismatch");
        if (!blocks) {
            if (type != 0 || count != 20 || bytes[p+11] != 2 || bytes[p+12] != 0)
                return fail("invalid binary file header");
        } else if (type == 1 && !eof) {
            binary.insert(binary.end(), bytes.data()+p, bytes.data()+p+count);
        } else if (type == 255 && !eof && count == 0) eof = true;
        else return fail("unexpected block");
        p += count + 1;
        ++blocks;
    }
    if (!eof || binary.size() != 16074 || raw.data.size() != 16000)
        return fail("missing EOF or wrong payload size");
    auto word = [&](std::size_t i) { return (std::size_t{binary[i]}<<8) | binary[i+1]; };
    const auto size = word(1), load = word(3);
    if (binary[0] != 0 || size != 16064 || load != 0x8000 ||
        binary[size+5] != 255 || word(size+6) != 0 || word(size+8) != load)
        return fail("invalid LOADM records");
    if (!std::equal(raw.data.begin(), raw.data.end(), binary.begin()+69))
        return fail("native planes changed during export");
    if (thomson::k7::encode(amiga::Mode::thomson_to7_320x16, {}) ||
        thomson::k7::encode(amiga::Mode::thomson_to8_320x16, raw.data))
        return fail("invalid input accepted");
    options.mode = "thomson-to8-320x16";
    if (api::convert_k7(input.data(),input.size(),options).error.empty())
        return fail("API accepted unsupported mode");
    std::println("K7: {} checksummed blocks, LOADM records and native planes verified", blocks);
    return 0;
}
