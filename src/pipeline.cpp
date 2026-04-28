#include "pipeline.hpp"

#include "api.hpp"
#include "color_space.hpp"
#include "ham.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif

namespace png2amiga::api {
// The workhorse defined in src/api.cpp (post-anon-namespace, external
// linkage). Forward-declared here so the pipeline:: forwarder below can
// reach it without pulling api-internal types into pipeline.hpp.
Result<pipeline::PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                              std::size_t input_size,
                                              const Options& options);
}  // namespace png2amiga::api

namespace png2amiga::pipeline {

std::string derive_symbol_name(std::string_view path) {
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos) path = path.substr(slash + 1);
    auto backslash = path.rfind('\\');
    if (backslash != std::string_view::npos) path = path.substr(backslash + 1);
    auto dot = path.rfind('.');
    if (dot != std::string_view::npos) path = path.substr(0, dot);

    std::string result;
    result.reserve(path.size());
    for (auto c : path) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty()) return "image";
    if (std::isdigit(static_cast<unsigned char>(result[0])))
        result.insert(result.begin(), '_');
    return result;
}

amiga::Chipset resolve_chipset(std::optional<amiga::Chipset> requested,
                               amiga::Mode mode) {
    auto params = amiga::get_mode_params(mode);
    if (params.bitplane_depth > 6) return amiga::Chipset::aga;
    if (requested.has_value()) return *requested;
    return amiga::Chipset::ocs;
}

amiga::Chipset resolve_chipset(std::string_view requested, amiga::Mode mode) {
    std::optional<amiga::Chipset> req;
    if (requested == "aga") req = amiga::Chipset::aga;
    else if (requested == "ocs") req = amiga::Chipset::ocs;
    return resolve_chipset(req, mode);
}

void PipelineResult::finalize_psnr(const Image& src, float total_error) {
    quant_error = total_error;
    psnr = color_space::compute_psnr_blurred(
        src.pixels(), rendered.pixels(),
        src.width(), src.height());
}

// MS-SSIM: 5-scale, 11×11 Gaussian window σ=1.5 per Wang et al. 2003.
namespace {

constexpr int kSsimWin = 11;
constexpr int kSsimHalf = 5;

// Rec. 709 linear-RGB → luminance Y.
inline float linear_to_y(const Color3f& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

const std::array<float, kSsimWin>& gaussian_kernel() {
    static const std::array<float, kSsimWin> k = []() {
        std::array<float, kSsimWin> kernel{};
        constexpr float sigma = 1.5f;
        float sum = 0.0f;
        for (int i = 0; i < kSsimWin; ++i) {
            float x = static_cast<float>(i - kSsimHalf);
            kernel[static_cast<std::size_t>(i)] =
                std::exp(-0.5f * x * x / (sigma * sigma));
            sum += kernel[static_cast<std::size_t>(i)];
        }
        for (auto& v : kernel) v /= sum;
        return kernel;
    }();
    return k;
}

// Separable Gaussian blur with edge clamp. dst sized to w*h on entry.
void gaussian_blur(const std::vector<float>& in, std::vector<float>& dst,
                   std::size_t w, std::size_t h) {
    const auto& k = gaussian_kernel();
    std::vector<float> tmp(w * h);
    // Horizontal pass.
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int i = 0; i < kSsimWin; ++i) {
                int sx = static_cast<int>(x) + i - kSsimHalf;
                sx = std::clamp(sx, 0, static_cast<int>(w) - 1);
                s += k[static_cast<std::size_t>(i)] *
                     in[y * w + static_cast<std::size_t>(sx)];
            }
            tmp[y * w + x] = s;
        }
    }
    // Vertical pass.
    dst.assign(w * h, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int i = 0; i < kSsimWin; ++i) {
                int sy = static_cast<int>(y) + i - kSsimHalf;
                sy = std::clamp(sy, 0, static_cast<int>(h) - 1);
                s += k[static_cast<std::size_t>(i)] *
                     tmp[static_cast<std::size_t>(sy) * w + x];
            }
            dst[y * w + x] = s;
        }
    }
}

float ssim_at_scale(const std::vector<float>& a,
                    const std::vector<float>& b,
                    std::size_t w, std::size_t h) {
    constexpr float K1 = 0.01f, K2 = 0.03f, L = 1.0f;
    constexpr float C1 = (K1 * L) * (K1 * L);
    constexpr float C2 = (K2 * L) * (K2 * L);

    std::vector<float> mu_a, mu_b;
    gaussian_blur(a, mu_a, w, h);
    gaussian_blur(b, mu_b, w, h);

    std::vector<float> aa(a.size()), bb(b.size()), ab(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        aa[i] = a[i] * a[i];
        bb[i] = b[i] * b[i];
        ab[i] = a[i] * b[i];
    }
    std::vector<float> mu_aa, mu_bb, mu_ab;
    gaussian_blur(aa, mu_aa, w, h);
    gaussian_blur(bb, mu_bb, w, h);
    gaussian_blur(ab, mu_ab, w, h);

    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t y = static_cast<std::size_t>(kSsimHalf);
         y + static_cast<std::size_t>(kSsimHalf) < h; ++y) {
        for (std::size_t x = static_cast<std::size_t>(kSsimHalf);
             x + static_cast<std::size_t>(kSsimHalf) < w; ++x) {
            std::size_t i = y * w + x;
            float ma = mu_a[i], mb = mu_b[i];
            float sa2 = std::max(0.0f, mu_aa[i] - ma * ma);
            float sb2 = std::max(0.0f, mu_bb[i] - mb * mb);
            float sab = mu_ab[i] - ma * mb;

            float l  = (2.0f * ma * mb + C1) / (ma * ma + mb * mb + C1);
            float cs = (2.0f * sab + C2) / (sa2 + sb2 + C2);
            sum += static_cast<double>(l * cs);
            ++count;
        }
    }
    return count == 0 ? 1.0f : static_cast<float>(sum / static_cast<double>(count));
}

void downsample_2x(const std::vector<float>& src,
                   std::size_t sw, std::size_t sh,
                   std::vector<float>& dst,
                   std::size_t& dw, std::size_t& dh) {
    dw = sw / 2;
    dh = sh / 2;
    dst.assign(dw * dh, 0.0f);
    for (std::size_t y = 0; y < dh; ++y) {
        for (std::size_t x = 0; x < dw; ++x) {
            float s = 0.25f * (
                src[(2 * y) * sw + 2 * x] +
                src[(2 * y) * sw + 2 * x + 1] +
                src[(2 * y + 1) * sw + 2 * x] +
                src[(2 * y + 1) * sw + 2 * x + 1]);
            dst[y * dw + x] = s;
        }
    }
}

}  // namespace

float compute_msssim(std::span<const Color3f> a,
                     std::span<const Color3f> b,
                     std::size_t width,
                     std::size_t height) {
    auto n = width * height;
    if (n == 0 || a.size() < n || b.size() < n) return 0.0f;

    constexpr int M = 5;
    constexpr std::array<float, M> weights{
        0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};

    std::vector<float> ya(n), yb(n);
    for (std::size_t i = 0; i < n; ++i) {
        ya[i] = std::clamp(linear_to_y(a[i]), 0.0f, 1.0f);
        yb[i] = std::clamp(linear_to_y(b[i]), 0.0f, 1.0f);
    }

    std::size_t w = width, h = height;
    std::vector<float> cur_a = std::move(ya);
    std::vector<float> cur_b = std::move(yb);

    double log_score = 0.0;
    double weight_used = 0.0;
    for (int j = 0; j < M; ++j) {
        if (w < static_cast<std::size_t>(kSsimWin) ||
            h < static_cast<std::size_t>(kSsimWin)) break;
        float ssim_j = ssim_at_scale(cur_a, cur_b, w, h);
        ssim_j = std::clamp(ssim_j, 1e-6f, 1.0f);
        log_score += static_cast<double>(weights[static_cast<std::size_t>(j)]) *
                     std::log(static_cast<double>(ssim_j));
        weight_used += static_cast<double>(weights[static_cast<std::size_t>(j)]);
        if (j + 1 < M) {
            std::vector<float> ds_a, ds_b;
            std::size_t dw, dh;
            downsample_2x(cur_a, w, h, ds_a, dw, dh);
            downsample_2x(cur_b, w, h, ds_b, dw, dh);
            cur_a = std::move(ds_a);
            cur_b = std::move(ds_b);
            w = dw; h = dh;
        }
    }
    if (weight_used <= 0.0) return 0.0f;
    // Renormalise in case some scales were skipped (very small inputs).
    log_score /= weight_used;
    return static_cast<float>(std::exp(log_score));
}

Image jitter_image(const Image& source, std::uint32_t seed,
                   float amplitude) {
    Image j(source.width(), source.height());
    const float amp = amplitude / 255.0f;
    for (std::size_t y = 0; y < source.height(); ++y) {
        for (std::size_t x = 0; x < source.width(); ++x) {
            auto p = source[x, y];
            // Per-pixel hash → ±amplitude/255 nudge per channel.
            auto h32 = seed * 2654435761u
                     + static_cast<std::uint32_t>(y) * 0x9E3779B9u
                     + static_cast<std::uint32_t>(x) * 0x85EBCA6Bu;
            auto nudge = [&](unsigned shift) {
                return (static_cast<float>((h32 >> shift) & 0xFFu) /
                        255.0f - 0.5f) * amp;
            };
            j[x, y] = Color3f{
                std::clamp(p.r + nudge(0),  0.0f, 1.0f),
                std::clamp(p.g + nudge(8),  0.0f, 1.0f),
                std::clamp(p.b + nudge(16), 0.0f, 1.0f),
            };
        }
    }
    return j;
}

void parallel_for(std::size_t n,
                  std::function<void(std::size_t)> body) {
    if (n == 0) return;
#ifndef __EMSCRIPTEN__
    auto n_threads = std::max<unsigned>(1,
        std::thread::hardware_concurrency());
    n_threads = std::min(n_threads, static_cast<unsigned>(n));
    std::atomic<std::size_t> next{0};
    auto worker = [&]() {
        while (true) {
            auto i = next.fetch_add(1);
            if (i >= n) break;
            body(i);
        }
    };
    std::vector<std::jthread> threads;
    threads.reserve(n_threads);
    for (unsigned t = 0; t < n_threads; ++t)
        threads.emplace_back(worker);
    threads.clear();  // join on destruction
#else
    for (std::size_t i = 0; i < n; ++i) body(i);
#endif
}

Result<Image> render_preview(
    const bitplane::BitplaneData& planes,
    std::span<const Color3f> base_palette,
    bool is_ham,
    bool is_lace,
    amiga::Chipset chipset,
    const std::vector<std::vector<Color3f>>* scanline_palettes,
    std::size_t cap_changes_per_line) {
    bool has_scanline_pal = scanline_palettes && !scanline_palettes->empty();
    if (is_ham) {
        auto data_bits = planes.depth - 2;
        if (has_scanline_pal) {
            return ham::render_ham_copper(planes, *scanline_palettes, data_bits);
        }
        return ham::render_ham(planes, base_palette, data_bits);
    }
    if (has_scanline_pal) {
        return copper::render_copper_capped(
            planes, *scanline_palettes, base_palette,
            cap_changes_per_line, is_lace, chipset);
    }
    return bitplane::render(planes, base_palette);
}

Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const api::Options& options) {
    return api::run_pipeline(input_data, input_size, options);
}

cheader::CHeaderOptions make_ch_opts(const ChOptsBase& base) {
    cheader::CHeaderOptions ch;
    ch.symbol_name = base.symbol_override.empty()
        ? derive_symbol_name(base.output_path)
        : std::string(base.symbol_override);
    ch.hires = base.hires;
    ch.interlace = base.interlace;
    ch.aga = base.aga;
    ch.fade_in = base.fade_in;
    ch.dpf = base.dpf;
    ch.interleaved = base.interleaved;
    return ch;
}

}  // namespace png2amiga::pipeline
