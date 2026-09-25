#include "cpc.hpp"
#include "color_space.hpp"
#include "palette.hpp"
#include "pipeline.hpp"
#include "quantize.hpp"
#include "scale.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>

namespace png2amiga::cpc {

namespace {

// Byte bit holding ink bit `b` of pixel `p` within a screen byte.
struct ModeLayout {
    std::size_t pixels_per_byte;
    std::size_t bits_per_pixel;
    std::array<std::array<std::uint8_t, 4>, 8> bit;  // [pixel][ink bit]
};

constexpr std::array<ModeLayout, 3> kLayouts = {{
    // mode 0: pen bits 0,1,2,3 at byte bits 7,3,5,1 (pixel 0) / 6,2,4,0.
    {2, 4, {{{7, 3, 5, 1}, {6, 2, 4, 0}}}},
    // mode 1: pen bits 0,1 at byte bits 7-n, 3-n.
    {4, 2, {{{7, 3}, {6, 2}, {5, 1}, {4, 0}}}},
    // mode 2: pen bit 0 at byte bit 7-n.
    {8, 1, {{{7}, {6}, {5}, {4}, {3}, {2}, {1}, {0}}}},
}};

constexpr std::size_t line_offset(std::size_t y) noexcept {
    return (y / 8) * kBytesPerLine + (y % 8) * 2048;
}

std::array<Color3f, 27> firmware_gamut() {
    std::array<Color3f, 27> g{};
    for (std::size_t f = 0; f < 27; ++f)
        g[f] = firmware_color(f);
    return g;
}

// 3×3 binomial blur of an OKLab buffer (edge-replicated).
std::vector<color_space::OKLab> blur3(const std::vector<color_space::OKLab>& in,
                                      std::size_t w,
                                      std::size_t h) {
    static constexpr std::array<float, 3> k = {0.25f, 0.5f, 0.25f};
    std::vector<color_space::OKLab> out(in.size());
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) {
            color_space::OKLab acc{0, 0, 0};
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    auto sx = static_cast<std::size_t>(
                        std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1));
                    auto sy = static_cast<std::size_t>(
                        std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1));
                    float wt = k[static_cast<std::size_t>(dx + 1)] *
                               k[static_cast<std::size_t>(dy + 1)];
                    const auto& p = in[sy * w + sx];
                    acc.L += wt * p.L;
                    acc.a += wt * p.a;
                    acc.b += wt * p.b;
                }
            out[y * w + x] = acc;
        }
    return out;
}

// Classic CPC with 2 or 4 inks: every C(27, K) ink set is tried (351 /
// 17550). Each set is scored by dithering an 80×50 proxy of the image
// with the user's dither settings (dither::apply) and comparing the
// 3×3-blurred result against the blurred proxy, plus a light per-pixel
// term. Nearest-color objectives (histogram / k-means) pick mid-tones
// here and lose the dither endpoints.
std::vector<Color3f> search_small_inkset(const Image& image,
                                         std::size_t K,
                                         const dither::Settings& settings,
                                         std::span<const Color3f> gamut) {
    constexpr std::size_t PW = 80, PH = 50;
    auto proxy = scale::resample(image, PW, PH);
    if (!proxy) return {};
    std::vector<color_space::OKLab> src(PW * PH);
    for (std::size_t i = 0; i < src.size(); ++i)
        src[i] = color_space::linear_to_oklab(proxy->pixels()[i]);
    auto src_blur = blur3(src, PW, PH);
    std::vector<color_space::OKLab> glab(gamut.size());
    for (std::size_t g = 0; g < gamut.size(); ++g)
        glab[g] = color_space::linear_to_oklab(gamut[g]);

    std::vector<std::array<std::uint8_t, 4>> combos;
    std::array<std::uint8_t, 4> c{};
    const auto G = static_cast<std::uint8_t>(gamut.size());
    auto rec = [&](auto&& self, std::size_t depth, std::uint8_t start) -> void {
        if (depth == K || depth >= c.size()) {
            combos.push_back(c);
            return;
        }
        for (std::uint8_t g = start; g < G; ++g) {
            c[depth] = g;
            self(self, depth + 1, static_cast<std::uint8_t>(g + 1));
        }
    };
    rec(rec, 0, 0);

    std::vector<float> score(combos.size());
    pipeline::parallel_for(combos.size(), [&](std::size_t ci) {
        std::vector<Color3f> pal(K);
        for (std::size_t k = 0; k < K; ++k)
            pal[k] = gamut[combos[ci][k]];
        auto d = dither::apply(*proxy, pal, settings);
        std::vector<color_space::OKLab> out(PW * PH);
        float px = 0.0f;
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = glab[combos[ci][d.indices[i]]];
            px += color_space::fma_dist_sq(
                out[i].L - src[i].L, out[i].a - src[i].a, out[i].b - src[i].b);
        }
        auto ob = blur3(out, PW, PH);
        float lf = 0.0f;
        for (std::size_t i = 0; i < ob.size(); ++i)
            lf += color_space::fma_dist_sq(
                ob[i].L - src_blur[i].L, ob[i].a - src_blur[i].a, ob[i].b - src_blur[i].b);
        score[ci] = lf + 0.1f * px;
    });
    std::size_t best = 0;
    for (std::size_t ci = 1; ci < combos.size(); ++ci)
        if (score[ci] < score[best]) best = ci;
    std::vector<Color3f> inks(K);
    for (std::size_t k = 0; k < K; ++k)
        inks[k] = gamut[combos[best][k]];
    return inks;
}

}  // namespace

Color3f firmware_color(std::size_t f) noexcept {
    return color_space::srgb_u8_to_linear(kLevels[(f / 3) % 3], kLevels[f / 9], kLevels[f % 3]);
}

std::vector<std::uint8_t> pack_screen(int screen_mode,
                                      std::span<const std::uint8_t> indices,
                                      std::size_t width) {
    const auto& L = kLayouts[static_cast<std::size_t>(screen_mode)];
    std::vector<std::uint8_t> screen(kScreenBytes, 0);
    for (std::size_t y = 0; y < kLines; ++y) {
        for (std::size_t xb = 0; xb < kBytesPerLine; ++xb) {
            std::uint8_t byte = 0;
            for (std::size_t p = 0; p < L.pixels_per_byte; ++p) {
                std::size_t x = xb * L.pixels_per_byte + p;
                std::uint8_t ink = (x < width) ? indices[y * width + x] : 0;
                for (std::size_t b = 0; b < L.bits_per_pixel; ++b)
                    byte = static_cast<std::uint8_t>(byte | (((ink >> b) & 1) << L.bit[p][b]));
            }
            screen[line_offset(y) + xb] = byte;
        }
    }
    return screen;
}

std::vector<std::uint8_t> unpack_screen(int screen_mode, std::span<const std::uint8_t> screen) {
    const auto& L = kLayouts[static_cast<std::size_t>(screen_mode)];
    const std::size_t width = kBytesPerLine * L.pixels_per_byte;
    std::vector<std::uint8_t> idx(width * kLines, 0);
    for (std::size_t y = 0; y < kLines; ++y)
        for (std::size_t xb = 0; xb < kBytesPerLine; ++xb) {
            std::uint8_t byte = screen[line_offset(y) + xb];
            for (std::size_t p = 0; p < L.pixels_per_byte; ++p) {
                std::uint8_t ink = 0;
                for (std::size_t b = 0; b < L.bits_per_pixel; ++b)
                    ink = static_cast<std::uint8_t>(ink | (((byte >> L.bit[p][b]) & 1) << b));
                idx[y * width + xb * L.pixels_per_byte + p] = ink;
            }
        }
    return idx;
}

std::vector<std::uint8_t> firmware_numbers(std::span<const Color3f> inks) {
    static const auto gamut = firmware_gamut();
    std::vector<std::uint8_t> out;
    for (auto& c : inks) {
        std::size_t best = 0;
        float best_d = std::numeric_limits<float>::max();
        for (std::size_t f = 0; f < gamut.size(); ++f) {
            float d = color_space::perceptual_distance_sq(c, gamut[f]);
            if (d < best_d) {
                best_d = d;
                best = f;
            }
        }
        out.push_back(static_cast<std::uint8_t>(best));
    }
    return out;
}

std::vector<std::uint16_t> plus_words(std::span<const Color3f> inks) {
    std::vector<std::uint16_t> out;
    for (auto& c : inks) {
        auto rgb = palette::linear_to_ocs(c);  // 0x0RGB
        unsigned r = (rgb >> 8) & 0xF, g = (rgb >> 4) & 0xF, b = rgb & 0xF;
        out.push_back(static_cast<std::uint16_t>((g << 8) | (r << 4) | b));
    }
    return out;
}

std::vector<std::uint8_t> pal_bytes(amiga::Mode mode, std::span<const Color3f> inks) {
    std::vector<std::uint8_t> out;
    if (amiga::is_cpc_plus(mode)) {
        for (auto w : plus_words(inks)) {
            out.push_back(static_cast<std::uint8_t>(w & 0xFF));
            out.push_back(static_cast<std::uint8_t>(w >> 8));
        }
    } else {
        for (auto f : firmware_numbers(inks))
            out.push_back(static_cast<std::uint8_t>(0x40 | kFirmwareToHardware[f]));
    }
    return out;
}

std::array<std::uint8_t, kAmsdosHeaderBytes> amsdos_header(std::string_view name,
                                                           std::uint16_t load_address,
                                                           std::uint16_t length) {
    std::array<std::uint8_t, kAmsdosHeaderBytes> h{};
    // 0x01-0x0B: 8-char name + 3-char extension, upper case, space padded.
    for (std::size_t i = 1; i <= 11; ++i)
        h[i] = ' ';
    auto dot = name.rfind('.');
    auto stem = name.substr(0, dot);
    auto ext = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
    for (std::size_t i = 0; i < std::min<std::size_t>(8, stem.size()); ++i)
        h[1 + i] = static_cast<std::uint8_t>(std::toupper(static_cast<unsigned char>(stem[i])));
    for (std::size_t i = 0; i < std::min<std::size_t>(3, ext.size()); ++i)
        h[9 + i] = static_cast<std::uint8_t>(std::toupper(static_cast<unsigned char>(ext[i])));
    h[0x12] = 2;  // file type: binary
    h[0x15] = static_cast<std::uint8_t>(load_address & 0xFF);
    h[0x16] = static_cast<std::uint8_t>(load_address >> 8);
    h[0x18] = static_cast<std::uint8_t>(length & 0xFF);  // logical length
    h[0x19] = static_cast<std::uint8_t>(length >> 8);
    h[0x40] = static_cast<std::uint8_t>(length & 0xFF);  // real length (24-bit)
    h[0x41] = static_cast<std::uint8_t>(length >> 8);
    unsigned sum = 0;
    for (std::size_t i = 0; i <= 66; ++i)
        sum += h[i];
    h[0x43] = static_cast<std::uint8_t>(sum & 0xFF);
    h[0x44] = static_cast<std::uint8_t>((sum >> 8) & 0xFF);
    return h;
}

Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings) {
    if (!amiga::is_cpc(mode))
        return std::unexpected{Error{ErrorCode::unsupported_mode, "cpc::encode: not a CPC mode"}};
    const auto params = amiga::get_mode_params(mode);
    const std::size_t W = params.screen_width, H = params.screen_height;
    if (image.width() != W || image.height() != H) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("cpc: expected {}x{}, got {}x{}", W, H, image.width(), image.height())}};
    }
    const std::size_t K = params.max_colors;
    const bool plus = amiga::is_cpc_plus(mode);

    // Inks: K distinct entries of the 27-color firmware gamut (histogram
    // picker, as for EGA's 64), or K of the 4096-color RGB444 gamut (the
    // OCS gamut — same brute-force quantizer as OCS lores).
    std::vector<Color3f> inks;
    if (plus) {
        auto pal = quantize::quantize(image, K, quantize::Algorithm::ocs_bruteforce);
        if (!pal) return std::unexpected{pal.error()};
        inks = pal->colors;
        for (auto& c : inks)
            c = palette::quantize_to_ocs(c);
    } else {
        static const auto gamut = firmware_gamut();
        inks = (K <= 4) ? search_small_inkset(image, K, settings, gamut)
                        : quantize::gamut_histogram(image, K, gamut).colors;
    }
    if (inks.size() > K) inks.resize(K);
    while (inks.size() < K)
        inks.push_back(inks.empty() ? Color3f{0, 0, 0} : inks.front());

    auto dres = dither::apply(image, inks, settings);

    EncodeResult res;
    const int smode = amiga::cpc_screen_mode(mode);
    res.screen = pack_screen(smode, dres.indices, W);
    res.inks = inks;
    if (plus)
        res.plus_grb = plus_words(inks);
    else
        res.firmware = firmware_numbers(inks);
    // Preview = the screen bytes decoded through the ink table.
    auto idx = unpack_screen(smode, res.screen);
    res.rendered = Image(W, H);
    float err = 0.0f;
    for (std::size_t i = 0; i < W * H; ++i) {
        res.rendered.pixels()[i] = inks[idx[i]];
        err += color_space::perceptual_distance_sq(image.pixels()[i], res.rendered.pixels()[i]);
    }
    res.total_error = err;
    return res;
}

}  // namespace png2amiga::cpc
