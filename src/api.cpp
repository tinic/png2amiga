#include "api.hpp"
#include "amiga.hpp"
#include "bitplane.hpp"
#include "cheader.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "ham.hpp"
#include "iff.hpp"
#include "palette.hpp"
#include "palette_io.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <vector>

namespace png2amiga::api {

namespace {

amiga::Mode parse_mode(const std::string& s) {
    if (s == "lores") return amiga::Mode::lores;
    if (s == "lores-lace") return amiga::Mode::lores_interlace;
    if (s == "hires") return amiga::Mode::hires;
    if (s == "hires-lace") return amiga::Mode::hires_interlace;
    if (s == "ham6") return amiga::Mode::ham6;
    if (s == "ham8") return amiga::Mode::ham8;
    if (s == "ehb") return amiga::Mode::ehb;
    return amiga::Mode::lores;
}

ham::Quality parse_ham_quality(const std::string& s) {
    if (s == "fast") return ham::Quality::fast;
    return ham::Quality::optimal;  // default to optimal
}

amiga::Chipset resolve_chipset(const std::string& s, amiga::Mode mode) {
    auto params = amiga::get_mode_params(mode);
    if (params.bitplane_depth > 6) return amiga::Chipset::aga;
    if (s == "aga") return amiga::Chipset::aga;
    return amiga::Chipset::ocs;
}

quantize::Algorithm quantize_algo(amiga::Chipset chipset) {
    return chipset == amiga::Chipset::aga
        ? quantize::Algorithm::median_cut
        : quantize::Algorithm::ocs_bruteforce;
}

void snap_to_chipset(Palette& pal, amiga::Chipset chipset) {
    if (chipset != amiga::Chipset::aga) {
        for (auto& c : pal.colors) c = palette::quantize_to_ocs(c);
    }
}

dither::Method parse_dither(const std::string& s) {
    if (s == "none") return dither::Method::none;
    if (s == "bayer2x2") return dither::Method::bayer2x2;
    if (s == "bayer4x4") return dither::Method::bayer4x4;
    if (s == "bayer8x8") return dither::Method::bayer8x8;
    if (s == "checker") return dither::Method::checker;
    if (s == "h2x4") return dither::Method::h2x4;
    if (s == "clustered-dot") return dither::Method::clustered_dot;
    if (s == "line2") return dither::Method::line2;
    if (s == "line-checker") return dither::Method::line_checker;
    if (s == "line4") return dither::Method::line4;
    if (s == "line8") return dither::Method::line8;
    if (s == "floyd-steinberg") return dither::Method::floyd_steinberg;
    if (s == "atkinson") return dither::Method::atkinson;
    if (s == "sierra-lite") return dither::Method::sierra_lite;
    if (s == "stucki") return dither::Method::stucki;
    if (s == "jarvis") return dither::Method::jarvis;
    return dither::Method::floyd_steinberg;
}

// Crop an image to a sub-region
Result<Image> crop_image(const Image& src,
                         std::size_t cx, std::size_t cy,
                         std::size_t cw, std::size_t ch) {
    if (cx + cw > src.width() || cy + ch > src.height() || cw == 0 || ch == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Crop region {}x{}+{}+{} exceeds image {}x{}",
                        cw, ch, cx, cy, src.width(), src.height()),
        }};
    }
    Image dst(cw, ch);
    for (std::size_t y = 0; y < ch; ++y) {
        for (std::size_t x = 0; x < cw; ++x) {
            dst[x, y] = src[cx + x, cy + y];
        }
    }
    return dst;
}

// Auto-crop: center-crop source to target aspect ratio
Result<Image> auto_crop_to_aspect(const Image& src,
                                  std::size_t target_w, std::size_t target_h) {
    auto src_w = src.width();
    auto src_h = src.height();

    auto target_ratio = static_cast<double>(target_w) / static_cast<double>(target_h);
    auto src_ratio = static_cast<double>(src_w) / static_cast<double>(src_h);

    std::size_t cw, ch;
    if (src_ratio > target_ratio) {
        ch = src_h;
        cw = static_cast<std::size_t>(
            static_cast<double>(src_h) * target_ratio + 0.5);
    } else {
        cw = src_w;
        ch = static_cast<std::size_t>(
            static_cast<double>(src_w) / target_ratio + 0.5);
    }

    auto cx = (src_w - cw) / 2;
    auto cy = (src_h - ch) / 2;

    return crop_image(src, cx, cy, cw, ch);
}

// Load image from memory, crop, scale, preprocess
Result<Image> load_and_preprocess(const std::uint8_t* input_data,
                                   std::size_t input_size,
                                   const Options& options,
                                   std::size_t target_w,
                                   std::size_t target_h) {
    int w{}, h{}, channels{};
    auto* raw = stbi_load_from_memory(input_data,
        static_cast<int>(input_size), &w, &h, &channels, 4);
    if (!raw)
        return std::unexpected{Error{ErrorCode::invalid_png, "Failed to decode image"}};

    auto width = static_cast<std::size_t>(w);
    auto height = static_cast<std::size_t>(h);
    auto pixel_count = width * height;

    // Check for transparency
    bool any_transparent = false;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        if (raw[i * 4 + 3] < 255) {
            any_transparent = true;
            break;
        }
    }

    std::vector<Color3f> pixels(pixel_count);
    std::vector<float> alpha;
    if (any_transparent) alpha.resize(pixel_count);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto base = i * 4;
        pixels[i] = color_space::srgb_u8_to_linear(
            raw[base], raw[base + 1], raw[base + 2]);
        if (any_transparent)
            alpha[i] = static_cast<float>(raw[base + 3]) / 255.0f;
    }
    stbi_image_free(raw);

    Image image(width, height, std::move(pixels),
                any_transparent ? std::move(alpha) : std::vector<float>{});

    // Crop (before scaling)
    if (options.crop_w > 0 && options.crop_h > 0) {
        auto cropped = crop_image(image,
            static_cast<std::size_t>(options.crop_x),
            static_cast<std::size_t>(options.crop_y),
            static_cast<std::size_t>(options.crop_w),
            static_cast<std::size_t>(options.crop_h));
        if (!cropped) return std::unexpected{cropped.error()};
        image = *std::move(cropped);
    } else if (options.crop_auto) {
        auto cropped = auto_crop_to_aspect(image, target_w, target_h);
        if (!cropped) return std::unexpected{cropped.error()};
        image = *std::move(cropped);
    }

    if (image.width() != target_w || image.height() != target_h) {
        auto scaled = scale::bicubic(image, target_w, target_h);
        if (!scaled) return std::unexpected{scaled.error()};
        image = *std::move(scaled);
    }

    preprocess::Settings pp;
    pp.gamma = options.gamma;
    pp.brightness = options.brightness;
    pp.contrast = options.contrast;
    pp.saturation = options.saturation;
    pp.hue_shift = options.hue_shift;
    pp.sharpen = options.sharpen;
    pp.black_point = options.black_point;
    pp.white_point = options.white_point;
    preprocess::apply(image, pp);

    return image;
}

struct PipelineResult {
    Image rendered;
    bitplane::BitplaneData planes;
    std::vector<Color3f> palette;
    amiga::Mode mode;
    bool interlace;

    // Copper mode: per-scanline palettes (empty if not copper)
    bool copper = false;
    std::vector<std::vector<Color3f>> scanline_palettes;
    std::size_t copper_num_colors{};

};

// Round to nearest even number (Amiga prefers even heights)
std::size_t round_even(double v) {
    auto r = static_cast<std::size_t>(std::lround(v));
    return (r + 1) & ~std::size_t{1};  // round up to even
}

// Compute target dimensions from source image size and user options.
// Requires loading the source image first to get its dimensions.
struct TargetDims { std::size_t w; std::size_t h; };

TargetDims compute_target_dims(std::size_t src_w, std::size_t src_h,
                               const Options& options, amiga::Mode mode) {
    auto params = amiga::get_mode_params(mode);
    auto mode_w = params.screen_width;
    auto src_aspect = static_cast<double>(src_w) / static_cast<double>(src_h);
    auto par = static_cast<double>(params.preview_scale_x)
             / static_cast<double>(params.preview_scale_y);

    bool have_w = options.width > 0;
    bool have_h = options.height > 0;

    if (have_w && have_h) {
        return {static_cast<std::size_t>(options.width),
                static_cast<std::size_t>(options.height)};
    }
    if (have_w) {
        auto w = static_cast<std::size_t>(options.width);
        auto h = round_even(static_cast<double>(w) * par / src_aspect);
        return {w, h};
    }
    if (have_h) {
        auto h = static_cast<std::size_t>(options.height);
        auto w = static_cast<std::size_t>(
            std::lround(static_cast<double>(h) * src_aspect / par));
        return {w, h};
    }
    // Neither: use mode default width, but don't upscale small images
    auto w = std::min(mode_w, src_w);
    auto h = round_even(static_cast<double>(w) * par / src_aspect);
    return {w, h};
}

Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const Options& options) {
    auto mode = parse_mode(options.mode);

    // We need source dimensions to compute the target size.
    // Peek at the source image dimensions first.
    int peek_w{}, peek_h{}, peek_ch{};
    if (!stbi_info_from_memory(input_data, static_cast<int>(input_size),
                                &peek_w, &peek_h, &peek_ch)) {
        return std::unexpected{Error{ErrorCode::invalid_png,
            "Failed to read image dimensions"}};
    }

    auto [target_w, target_h] = compute_target_dims(
        static_cast<std::size_t>(peek_w),
        static_cast<std::size_t>(peek_h),
        options, mode);

    auto depth = static_cast<std::size_t>(
        std::clamp(options.depth, 1, 8));

    auto image = load_and_preprocess(input_data, input_size, options,
                                      target_w, target_h);
    if (!image) return std::unexpected{image.error()};

    auto chipset = resolve_chipset(options.chipset, mode);

    // --- HAM modes: use dedicated HAM encoder ---
    if (amiga::is_ham(mode)) {

        ham::HamOptions ham_opts;
        ham_opts.quality = parse_ham_quality(options.ham_quality);
        ham_opts.beam_width = static_cast<std::size_t>(
            std::clamp(options.ham_beam, 1, 256));

        // Wire dither settings into HAM options
        ham_opts.dither_method = parse_dither(options.dither);
        ham_opts.dither_strength = options.dither_strength;
        ham_opts.error_clamp = options.error_clamp;

        auto ham_result = ham::encode_ham(*image, mode, chipset, ham_opts);
        if (!ham_result) return std::unexpected{ham_result.error()};

        // Render preview using HAM decoder (not simple palette lookup)
        auto data_bits = ham_result->planes.depth - 2;
        auto preview = ham::render_ham(ham_result->planes,
                                       ham_result->base_palette,
                                       data_bits);
        if (!preview) return std::unexpected{preview.error()};

        return PipelineResult{
            *std::move(preview),
            std::move(ham_result->planes),
            std::move(ham_result->base_palette),
            mode,
            options.interlace,
            false, {}, 0,
        };
    }

    // --- EHB mode: 32 base colors + 32 half-brightness ---
    if (mode == amiga::Mode::ehb) {
        depth = 6;  // EHB is always 6 bitplanes

        // Generate 32 base colors via median-cut, or load from file
        Palette base_pal;
        if (!options.palette_file.empty()) {
            auto loaded = palette_io::load_palette(options.palette_file);
            if (!loaded) return std::unexpected{loaded.error()};
            base_pal = *std::move(loaded);
            if (base_pal.colors.size() > 32)
                base_pal.colors.resize(32);
            snap_to_chipset(base_pal, chipset);
        } else {
            auto quantized = quantize::quantize(*image, 32,
                                                quantize_algo(chipset));
            if (!quantized) return std::unexpected{quantized.error()};
            base_pal = *std::move(quantized);
        }

        // Build full 64-color EHB palette (32 base + 32 half-bright)
        auto ehb_pal = palette::make_ehb_palette(base_pal.colors);

        if (options.match_range)
            preprocess::match_palette_range(*image, ehb_pal);

        // Dither against all 64 colors
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        auto dither_result = dither::apply(*image, ehb_pal.colors, dith);

        // Encode to 6 bitplanes
        auto planes = bitplane::encode(dither_result.indices,
                                       image->width(), image->height(),
                                       depth);
        if (!planes) return std::unexpected{planes.error()};

        // For IFF output, only the 32 base colors go in CMAP.
        // But for preview rendering, we need all 64.
        // Store all 64 in the palette — the IFF writer will trim for EHB.
        std::vector<Color3f> full_palette(ehb_pal.colors.begin(),
                                          ehb_pal.colors.end());

        auto preview = bitplane::render(*planes, full_palette);
        if (!preview) return std::unexpected{preview.error()};

        return PipelineResult{
            *std::move(preview),
            *std::move(planes),
            std::move(full_palette),
            mode,
            options.interlace,
            false, {}, 0,
        };
    }

    // --- Copper palette mode ---
    if (options.copper && !amiga::is_ham(mode) && mode != amiga::Mode::ehb) {
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        auto copper_result = copper::encode_copper(*image, depth, dith);
        if (!copper_result) return std::unexpected{copper_result.error()};

        auto preview = copper::render_copper(copper_result->planes,
                                             copper_result->scanline_palettes);
        if (!preview) return std::unexpected{preview.error()};

        // Use the first scanline's palette as the nominal palette
        // (for CMAP chunk in IFF; the real palettes are in COPL)
        auto& first_pal = copper_result->scanline_palettes[0];

        return PipelineResult{
            *std::move(preview),
            std::move(copper_result->planes),
            std::vector<Color3f>(first_pal.begin(), first_pal.end()),
            mode,
            options.interlace,
            true,  // copper
            std::move(copper_result->scanline_palettes),
            copper_result->num_colors,
        };
    }

    // --- Standard bitplane modes ---
    auto max_colors = std::size_t{1} << depth;

    // Build palette
    Palette pal;
    if (!options.palette_file.empty()) {
        auto loaded = palette_io::load_palette(options.palette_file);
        if (!loaded) return std::unexpected{loaded.error()};
        pal = *std::move(loaded);
        if (pal.colors.size() > max_colors)
            pal.colors.resize(max_colors);
        snap_to_chipset(pal, chipset);
    } else {
        auto quantized = quantize::quantize(*image, max_colors,
                                            quantize_algo(chipset));
        if (!quantized) return std::unexpected{quantized.error()};
        pal = *std::move(quantized);
    }

    if (options.match_range)
        preprocess::match_palette_range(*image, pal);

    // Apply dithering to map pixels to palette indices
    dither::Settings dith;
    dith.method = parse_dither(options.dither);
    dith.strength = options.dither_strength;
    dith.error_clamp = options.error_clamp;

    // Limit palette span to max_colors
    auto pal_size = std::min(pal.size(), max_colors);
    std::span<const Color3f> pal_span{pal.colors.data(), pal_size};

    auto dither_result = dither::apply(*image, pal_span, dith);

    // Encode to bitplanes
    auto planes = bitplane::encode(dither_result.indices,
                                   image->width(), image->height(),
                                   depth);
    if (!planes) return std::unexpected{planes.error()};

    // Build used palette vector
    std::vector<Color3f> used_palette(pal_span.begin(), pal_span.end());

    // Render preview
    auto preview = bitplane::render(*planes, used_palette);
    if (!preview) return std::unexpected{preview.error()};

    return PipelineResult{
        *std::move(preview),
        *std::move(planes),
        std::move(used_palette),
        mode,
        options.interlace,
        false, {}, 0,
    };
}

} // namespace

ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto png = png_io::encode(result->rendered);
    if (!png) return {{}, 0, 0, png.error().message};

    return {*std::move(png),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

ConvertResult convert_rgba(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto& img = result->rendered;
    auto w = img.width();
    auto h = img.height();
    std::vector<std::uint8_t> rgba(w * h * 4);

    for (std::size_t i = 0; i < w * h; ++i) {
        auto srgb = color_space::linear_to_srgb(img.pixels()[i]).clamped();
        rgba[i * 4 + 0] = static_cast<std::uint8_t>(srgb.r * 255.0f + 0.5f);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>(srgb.g * 255.0f + 0.5f);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(srgb.b * 255.0f + 0.5f);
        rgba[i * 4 + 3] = 255;
    }

    return {std::move(rgba),
            static_cast<int>(w),
            static_cast<int>(h), ""};
}

ConvertResult convert_iff(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    iff::IffOptions iff_opts;
    iff_opts.interlace = result->interlace;
    if (result->copper && !result->scanline_palettes.empty()) {
        iff_opts.scanline_palettes = &result->scanline_palettes;
    }

    auto iff_data = iff::write_ilbm(
        result->planes,
        result->palette,
        result->mode,
        iff_opts);
    if (!iff_data) return {{}, 0, 0, iff_data.error().message};

    return {*std::move(iff_data),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

ConvertResult convert_cheader(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    cheader::CHeaderOptions ch_opts;
    if (!options.symbol_name.empty()) {
        ch_opts.symbol_name = options.symbol_name;
    }
    auto header = cheader::generate(
        result->planes,
        result->palette,
        result->mode,
        ch_opts);
    if (!header) return {{}, 0, 0, header.error().message};

    // Convert string to bytes
    std::vector<std::uint8_t> bytes(header->begin(), header->end());

    return {std::move(bytes),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

ConvertResult convert_raw(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    // Return the raw interleaved bitplane bytes directly
    return {std::move(result->planes.data),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

ConvertResult convert_palette(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto pal_data = palette_io::encode_ocs_palette(result->palette);
    if (!pal_data) return {{}, 0, 0, pal_data.error().message};

    return {*std::move(pal_data),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

} // namespace png2amiga::api
