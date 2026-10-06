#pragma once
#include "api.hpp"
#include "retro.hpp"
#include "palette.hpp"
#include <fstream>
#include <iterator>
#include <print>
inline int check_retro(const char* path) {
    using namespace png2amiga;
    auto fail = [](const char* why) {
        std::println(stderr, "Spectrum: {}", why);
        return 1;
    };
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> input((std::istreambuf_iterator<char>(f)), {});
    for (const char* mode : {"zx-spectrum", "stf-spectrum512", "ste-spectrum4096"}) {
        for (const char* method : {"none", "opt-checker", "floyd-steinberg"}) {
            api::Options o;
            o.mode = mode;
            o.dither = method;
            auto result = api::encode_state(input.data(), input.size(), o);
            if (!result.ok()) {
                std::println(stderr, "{}", result.error_msg);
                return fail("encode failed");
            }
            auto& st = result.state;
            auto& b = st.raw_frame;
            bool zx = o.mode == "zx-spectrum", ste = o.mode == "ste-spectrum4096";
            if (b.size() != (zx ? 6912 : 51104)) return fail("native size");
            if (!zx) {
                for (std::size_t i = 0; i < 160; ++i)
                    if (b[i]) return fail("first row not blank");
                for (std::size_t i = 32000; i < b.size(); i += 2) {
                    unsigned word = 256U * b[i] + b[i + 1];
                    if (word & (ste ? 0xf000 : 0xf888)) return fail("invalid palette bits");
                    if (((i - 32000) / 2 % 16 == 0 || (i - 32000) / 2 % 16 == 15) && word)
                        return fail("reserved registers");
                }
            }
            for (std::size_t y = 0; y < st.rendered.height(); ++y)
                for (std::size_t x = 0; x < st.rendered.width(); ++x) {
                    Color3f expected;
                    if (zx) {
                        auto a = b[6144 + y / 8 * 32 + x / 8];
                        if (a & 128) return fail("FLASH enabled");
                        auto address = (y / 64) * 2048 + (y % 8) * 256 + (y % 64 / 8) * 32 + x / 8;
                        bool ink = (b[address] >> (7 - x % 8)) & 1;
                        unsigned c = ink ? a & 7 : (a >> 3) & 7;
                        std::uint8_t v = a & 64 ? 255 : 205;
                        expected = color_space::srgb_u8_to_linear(
                            c & 2 ? v : 0, c & 4 ? v : 0, c & 1 ? v : 0);
                    } else {
                        unsigned reg = 0;
                        for (std::size_t plane = 0; plane < 4; ++plane) {
                            auto addr = (y + 1) * 160 + (x / 16) * 8 + plane * 2;
                            unsigned word = 256U * b[addr] + b[addr + 1];
                            reg |= ((word >> (15 - x % 16)) & 1) << plane;
                        }
                        int cut = 10 * static_cast<int>(reg) + (reg % 2 ? -5 : 1);
                        unsigned bank = static_cast<int>(x) < cut         ? 0
                                        : static_cast<int>(x) < cut + 160 ? 1
                                                                          : 2;
                        auto addr = 32000 + (y * 48 + bank * 16 + reg) * 2;
                        auto word = static_cast<std::uint16_t>(256U * b[addr] + b[addr + 1]);
                        if (ste)
                            expected = palette::ocs_to_linear(static_cast<std::uint16_t>(
                                ((word & 0x777) << 1) | ((word & 0x888) >> 3)));
                        else
                            expected = palette::stf_to_linear(word);
                    }
                    if (expected != st.rendered[x, y])
                        return fail("independent decode/preview mismatch");
                }
            auto raw = api::convert_raw(input.data(), input.size(), o);
            if (!raw.error.empty() || raw.data != b) return fail("raw export mismatch");
            if (zx) {
                auto scr = api::convert_scr(input.data(), input.size(), o);
                if (!scr.error.empty() || scr.data != b) return fail("SCR mismatch");
                if (api::convert_prg(input.data(), input.size(), o).error.empty())
                    return fail("ZX accepted TOS executable");
            } else {
                auto exe = api::convert_prg(input.data(), input.size(), o);
                if (!exe.error.empty() || exe.data.size() <= b.size() + 28 || exe.data[0] != 0x60 ||
                    exe.data[1] != 0x1a || exe.data[27] != 1)
                    return fail("TOS header");
                if (!std::equal(
                        b.begin(), b.end(), exe.data.end() - static_cast<std::ptrdiff_t>(b.size())))
                    return fail("TOS payload");
                if (exe.data[exe.data.size() - b.size() - 1] != (ste ? 1 : 0))
                    return fail("STE gate");
                auto source = api::convert_viewer(input.data(), input.size(), o);
                if (!source.error.empty() || source.data.empty()) return fail("source export");
            }
            o.copper = true;
            if (api::encode_state(input.data(), input.size(), o).ok())
                return fail("accepted Amiga copper");
            o.copper = false;
            o.width = 123;
            o.height = 99;
            auto resized = api::encode_state(input.data(), input.size(), o);
            if (!resized.ok() || resized.state.rendered.width() != st.rendered.width() ||
                resized.state.rendered.height() != st.rendered.height())
                return fail("fixed dimensions");
            if (retro::encode(Image(123, 99), st.mode, {}))
                return fail("encoder accepted invalid dimensions");
            std::println("{} / {}: native decode and exports passed", mode, method);
        }
    }
    return 0;
}
