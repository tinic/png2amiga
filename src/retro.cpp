#include "retro.hpp"
#include "atari/spectrum_runtime.hpp"
#include "palette.hpp"
#include "quantize.hpp"
#include "pipeline.hpp"
#include <array>
#include <cmath>
#include <format>
#include <limits>
namespace png2amiga::retro {
namespace {
using color_space::OKLab;
void put16(std::vector<std::uint8_t>& b, std::size_t p, unsigned v) {
    b[p] = static_cast<std::uint8_t>(v >> 8);
    b[p + 1] = static_cast<std::uint8_t>(v);
}
void put32(std::vector<std::uint8_t>& b, std::size_t p, unsigned v) {
    put16(b, p, v >> 16);
    put16(b, p + 2, v);
}
Color3f snap(Color3f c, bool ste) {
    return ste ? palette::ocs_to_linear(palette::linear_to_ocs(c)) : palette::quantize_to_stf(c);
}
unsigned hardware_word(Color3f c, bool ste) {
    if (!ste) return palette::linear_to_stf(c);
    unsigned w = palette::linear_to_ocs(c);
    // STE channels store the least significant bit in bit 3 of each nibble.
    return ((w & 0xeee) >> 1) | ((w & 0x111) << 3);
}
EncodeResult zx(const Image& image, const dither::Settings& settings) {
    constexpr std::size_t W = 256, H = 192;
    EncodeResult out;
    out.rendered = Image(W, H);
    out.bytes.assign(6912, 0);
    std::array<OKLab, 16> lab{};
    for (unsigned i = 0; i < 16; ++i) {
        std::uint8_t v = i & 8 ? 255 : 205;
        auto c = color_space::srgb_u8_to_linear(i & 2 ? v : 0, i & 4 ? v : 0, i & 1 ? v : 0);
        out.palette.push_back(c);
        lab[i] = color_space::linear_to_oklab(c);
    }
    std::vector<OKLab> src(W * H), target(W * H);
    for (std::size_t i = 0; i < src.size(); ++i)
        src[i] = color_space::linear_to_oklab(image.pixels()[i]);
    bool mixing = settings.method != dither::Method::none;
    for (int y = 0; y < static_cast<int>(H); ++y)
        for (int x = 0; x < static_cast<int>(W); ++x) {
            OKLab t{0, 0, 0};
            if (mixing) {
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto s = src[static_cast<std::size_t>(std::clamp(y + dy, 0, 191) * 256 +
                                                              std::clamp(x + dx, 0, 255))];
                        float w = static_cast<float>((dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1)) / 16;
                        t.L += s.L * w;
                        t.a += s.a * w;
                        t.b += s.b * w;
                    }
            } else
                t = src[static_cast<std::size_t>(y * 256 + x)];
            target[static_cast<std::size_t>(y * 256 + x)] = t;
        }
    std::array<std::array<unsigned, 2>, 768> pairs{};
    pipeline::parallel_for(768, [&](std::size_t cell) {
        float best = std::numeric_limits<float>::infinity();
        for (unsigned bright = 0; bright < 16; bright += 8)
            for (unsigned a = 0; a < 8; ++a)
                for (unsigned b = a; b < 8; ++b) {
                    float score = 0;
                    for (std::size_t y = 0; y < 8; ++y)
                        for (std::size_t x = 0; x < 8; ++x) {
                            auto t = target[(cell / 32 * 8 + y) * W + cell % 32 * 8 + x];
                            score += mixing
                                         ? color_space::mix_segment_score(
                                               t, lab[bright + a], lab[bright + b], 0.09375f)
                                         : std::min(color_space::fma_dist_sq(t, lab[bright + a]),
                                                    color_space::fma_dist_sq(t, lab[bright + b]));
                        }
                    if (score < best) {
                        best = score;
                        pairs[cell] = {bright + a, bright + b};
                    }
                }
    });
    auto pick = [&](const OKLab& t, std::size_t x, std::size_t y) -> dither::PickResult {
        auto p = pairs[y / 8 * 32 + x / 8];
        std::array<OKLab, 2> cp{lab[p[0]], lab[p[1]]};
        std::size_t q = 0;
        OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, t, cp, x, y, settings.strength, 0, q, chosen);
        auto addr = ((y & 0xc0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | (x >> 3);
        if (q) out.bytes[addr] |= static_cast<std::uint8_t>(0x80 >> (x & 7));
        out.rendered[x, y] = out.palette[p[q]];
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);
    for (std::size_t c = 0; c < 768; ++c) {
        auto p = pairs[c];
        out.bytes[6144 + c] = static_cast<std::uint8_t>(((p[0] & 8) << 3) | ((p[0] & 7) << 3) |
                                                        (p[1] & 7));
    }
    return out;
}
EncodeResult spectrum(const Image& image, bool ste, const dither::Settings& settings) {
    constexpr std::size_t W = 320, H = 199;
    EncodeResult out;
    out.rendered = Image(W, H);
    out.bytes.assign(51104, 0);
    std::vector<std::array<Color3f, 48>> colors(H);
    std::vector<std::array<OKLab, 48>> labs(H);
    // Constrained Lloyd fitting: each pixel can choose only the 16 registers
    // actually live at its horizontal position. Reserved 0/15 stay black.
    pipeline::parallel_for(H, [&](std::size_t y) {
        Image row(W, 1);
        std::array<OKLab, W> target{};
        for (std::size_t x = 0; x < W; ++x) {
            row[x, 0] = image[x, y];
            target[x] = color_space::linear_to_oklab(image[x, y]);
        }
        auto seed = quantize::quantize(row, 14);
        for (std::size_t k = 0; k < 48; ++k) {
            colors[y][k] = (k % 16 == 0 || k % 16 == 15)
                               ? Color3f{}
                               : snap(seed->colors[(k % 16 - 1) % seed->colors.size()], ste);
        }
        for (int pass = 0; pass < 12; ++pass) {
            for (std::size_t k = 0; k < 48; ++k)
                labs[y][k] = color_space::linear_to_oklab(colors[y][k]);
            std::array<OKLab, 48> sums{};
            std::array<unsigned, 48> count{};
            for (std::size_t x = 0; x < W; ++x) {
                float best = std::numeric_limits<float>::infinity();
                std::size_t index = 0;
                for (std::size_t c = 0; c < 16; ++c) {
                    auto k = spectrum_slot(x, c);
                    float e = color_space::fma_dist_sq(target[x], labs[y][k]);
                    if (e < best) {
                        best = e;
                        index = k;
                    }
                }
                auto& s = sums[index];
                s.L += target[x].L;
                s.a += target[x].a;
                s.b += target[x].b;
                ++count[index];
            }
            bool changed = false;
            for (std::size_t k = 0; k < 48; ++k)
                if (count[k] && k % 16 != 0 && k % 16 != 15) {
                    float n = static_cast<float>(count[k]);
                    auto s = sums[k];
                    auto c = snap(color_space::oklab_to_linear({s.L / n, s.a / n, s.b / n}), ste);
                    changed |= c != colors[y][k];
                    colors[y][k] = c;
                }
            if (!changed) break;
        }
        for (std::size_t k = 0; k < 48; ++k) {
            labs[y][k] = color_space::linear_to_oklab(colors[y][k]);
            put16(out.bytes, 32000 + (y * 48 + k) * 2, hardware_word(colors[y][k], ste));
        }
    });
    auto pick = [&](const OKLab& t, std::size_t x, std::size_t y) -> dither::PickResult {
        std::array<OKLab, 16> cp{};
        for (std::size_t c = 0; c < 16; ++c)
            cp[c] = labs[y][spectrum_slot(x, c)];
        std::size_t q = 0;
        OKLab chosen{};
        float thr = dither::pick_palette_index_with_ostro(
            settings.method, t, cp, x, y, settings.strength, 0, q, chosen);
        out.rendered[x, y] = colors[y][spectrum_slot(x, q)];
        // Blank physical row 0; four interleaved big-endian plane words.
        auto base = (y + 1) * 160 + (x / 16) * 8 + (x % 16) / 8;
        for (std::size_t p = 0; p < 4; ++p)
            if (q & (std::size_t{1} << p))
                out.bytes[base + p * 2] |= static_cast<std::uint8_t>(0x80 >> (x % 8));
        return {chosen, thr};
    };
    (void)dither::diffuse_raw_buffer(image, settings, pick);
    for (auto& row : colors)
        out.palette.insert(out.palette.end(), row.begin(), row.end());
    return out;
}
}  // namespace
Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings) {
    auto p = amiga::get_mode_params(mode);
    if (!amiga::is_retro_raster(mode) || image.width() != p.screen_width ||
        image.height() != p.screen_height)
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                     "ZX/Atari Spectrum requires native screen dimensions"}};
    auto out = mode == amiga::Mode::zx_spectrum
                   ? zx(image, settings)
                   : spectrum(image, mode == amiga::Mode::ste_spectrum4096, settings);
    for (std::size_t i = 0; i < image.pixels().size(); ++i)
        out.total_error += color_space::perceptual_distance_sq(image.pixels()[i],
                                                               out.rendered.pixels()[i]);
    return out;
}
Result<std::vector<std::uint8_t>> executable(amiga::Mode mode,
                                             std::span<const std::uint8_t> bytes) {
    if (!amiga::is_atari_spectrum(mode) || bytes.size() != 51104)
        return std::unexpected{Error{ErrorCode::unsupported_mode,
                                     "TOS executable requires Atari Spectrum screen data"}};
    std::vector<std::uint8_t> out(28, 0);
    put16(out, 0, 0x601a);
    put32(out, 2, static_cast<unsigned>(sizeof(spectrum_stub) + bytes.size()));
    put32(out, 10, 33024);
    put16(out, 26, 1);  // absolute, position-independent; no relocations
    out.insert(out.end(), std::begin(spectrum_stub), std::end(spectrum_stub));
    out.back() = mode == amiga::Mode::ste_spectrum4096 ? 1 : 0;
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}
Result<std::string> viewer_source(amiga::Mode mode, std::span<const std::uint8_t> bytes) {
    if (!amiga::is_atari_spectrum(mode) || bytes.size() != 51104)
        return std::unexpected{Error{ErrorCode::unsupported_mode,
                                     "Viewer source requires Atari Spectrum screen data"}};
    std::string source(spectrum_assembly);
    if (mode == amiga::Mode::ste_spectrum4096)
        source.replace(source.find("required_ste:\n    .word 0"),
                       std::string_view("required_ste:\n    .word 0").size(),
                       "required_ste:\n    .word 1");
    std::string out =
        "// Atari TOS executable source, GNU m68k C++. Build: ./build-atari.sh picture.cpp\n// PAL "
        "8 MHz ST/STE; Spectrum 4096 requires STE. Space/Escape exits.\nasm(R\"p2a(\n" +
        source;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i % 16 == 0)
            out += "    .byte ";
        else
            out += ',';
        out += std::format("0x{:02x}", bytes[i]);
        if (i % 16 == 15 || i + 1 == bytes.size()) out += '\n';
    }
    out += ")p2a\");\n";
    return out;
}
}  // namespace png2amiga::retro
