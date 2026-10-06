#pragma once
#include "api.hpp"
#include "msx.hpp"
#include "color_space.hpp"
#include <fstream>
#include <iterator>
#include <print>

inline int check_msx(const char* path) {
    using namespace png2amiga;
    auto fail = [](const char* why) { std::println(stderr, "MSX: {}", why); return 1; };
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> input((std::istreambuf_iterator<char>(f)), {});
    constexpr std::array<std::uint32_t, 16> tms{
        0,0,0x21c842,0x5edc78,0x5455ed,0x7d76fc,0xd4524d,0x42ebf5,
        0xfc5554,0xff7978,0xd4c154,0xe6ce80,0x21b03b,0xc95bba,0xcccccc,0xffffff};
    // Decode CPU-visible VRAM independently of the encoder's packing code.
    auto decode = [&](const std::vector<std::uint8_t>& v, int screen, std::size_t x, std::size_t y) {
        if (screen == 2) {
            auto tile = v[0x1800 + (y / 8) * 32 + x / 8];
            auto addr = (y / 64) * 2048 + tile * 8 + y % 8;
            unsigned q = (v[addr] & (128 >> (x % 8))) ? v[0x2000+addr] >> 4 : v[0x2000+addr] & 15;
            return color_space::srgb_hex_to_linear(tms[q]);
        }
        unsigned r, g, b;
        if (screen == 8) {
            unsigned q = v[y*256+x];
            r = (q/4)%8; g = q/32;
            constexpr unsigned blue[] = {0,2,4,7};
            b = blue[q%4];
        } else {
            std::size_t addr = y * (screen == 7 ? 256 : 128);
            unsigned q = screen == 6 ? (v[addr+x/4] >> (6-2*(x%4))) & 3
                                     : (v[addr+x/2] >> (x%2 ? 0 : 4)) & 15;
            auto pal = (screen == 7 ? 0xfa80 : 0x7680) + 2*q;
            r = v[pal] >> 4; b = v[pal] & 15; g = v[pal+1];
        }
        return Color3f{color_space::srgb_to_linear(static_cast<float>(r)/7),
                       color_space::srgb_to_linear(static_cast<float>(g)/7),
                       color_space::srgb_to_linear(static_cast<float>(b)/7)};
    };
    for (const char* mode : {"msx1-screen2", "msx2-screen5", "msx2-screen6", "msx2-screen7", "msx2-screen8"}) {
        api::Options o;
        o.mode = mode; o.dither = "opt-checker";
        auto result = api::encode_state(input.data(), input.size(), o);
        if (!result.ok()) { std::println(stderr, "{}", result.error_msg); return fail("encode failed"); }
        auto& st = result.state;
        const int screen = mode[11] - '0'; // msxN-screenN
        auto file = api::convert_msx(input.data(), input.size(), o);
        auto raw = api::convert_raw(input.data(), input.size(), o);
        if (!file.error.empty() || !raw.error.empty()) return fail("export failed");
        auto size = screen == 2 ? 0x3800U : screen == 8 ? 54272U : screen == 7 ? 0xfaa0U : 0x76a0U;
        if (raw.data.size() != size || file.data.size() != size+7) return fail("wrong size");
        if (file.data[0] != 0xfe || file.data[1] || file.data[2] || file.data[5] || file.data[6] ||
            (unsigned(file.data[3]) + 256U*file.data[4]) != size-1 ||
            !std::equal(raw.data.begin(), raw.data.end(), file.data.begin()+7)) return fail("BLOAD header/payload");
        for (std::size_t y = 0; y < st.rendered.height(); ++y)
            for (std::size_t x = 0; x < st.rendered.width(); ++x)
                if (decode(raw.data, screen, x, y) != st.rendered[x,y]) return fail("VRAM/preview mismatch");
        if (screen == 2) {
            for (std::size_t i = 0x2000; i < 0x3800; ++i)
                if (!(raw.data[i] & 15) || !(raw.data[i] >> 4)) return fail("transparent SCREEN 2 ink");
            if (raw.data[0x1b00] != 208) return fail("SCREEN 2 sprite terminator");
        } else if (screen != 8) {
            for (std::size_t i = screen == 7 ? 0xfa80 : 0x7680; i < size; i += 2)
                if ((raw.data[i] & 0x88) || (raw.data[i+1] & 0xf8)) return fail("illegal RGB333 bits");
        }
        o.copper = true;
        if (api::encode_state(input.data(), input.size(), o).ok()) return fail("accepted Amiga copper");
        o.copper = false; o.width = 123; o.height = 99;
        auto resized = api::encode_state(input.data(), input.size(), o);
        if (!resized.ok() || resized.state.rendered.width() != st.rendered.width() ||
            resized.state.rendered.height() != st.rendered.height()) return fail("lost fixed screen dimensions");
        if (msx::encode(Image(123,99), st.mode, {}).has_value()) return fail("encoder accepted wrong dimensions");
        std::println("{}: native VRAM, palette and BLOAD contract passed", mode);
    }
    // Exhaustive SCREEN 8 gamut ramp: all 256 GRB codes, especially blue=2/4/7.
    Image ramp(256, 212);
    std::vector<std::uint8_t> reference(54272);
    for (std::size_t i = 0; i < reference.size(); ++i) reference[i] = static_cast<std::uint8_t>(i);
    for (std::size_t y = 0; y < 212; ++y)
        for (std::size_t x = 0; x < 256; ++x) ramp[x,y] = decode(reference,8,x,y);
    dither::Settings d; d.method = dither::Method::none;
    auto r = msx::encode(ramp, amiga::Mode::msx2_screen8, d);
    if (!r || r->vram != reference) return fail("SCREEN 8 gamut ramp");
    return 0;
}
