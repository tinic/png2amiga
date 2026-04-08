#include "api.hpp"
#include "amiga.hpp"
#include "bitplane.hpp"
#include "cheader.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "degas.hpp"
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
    if (s == "ham6" || s == "ham6-lace" || s == "ham6-hires" || s == "ham6-hires-lace")
        return amiga::Mode::ham6;
    if (s == "ham8" || s == "ham8-lace" || s == "ham8-hires" || s == "ham8-hires-lace")
        return amiga::Mode::ham8;
    if (s == "ehb" || s == "ehb-lace") return amiga::Mode::ehb;
    if (s == "stf-low") return amiga::Mode::stf_low;
    if (s == "stf-med") return amiga::Mode::stf_med;
    if (s == "ste-low") return amiga::Mode::ste_low;
    if (s == "ste-med") return amiga::Mode::ste_med;
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

quantize::Algorithm quantize_algo(amiga::Chipset chipset, amiga::Mode mode = amiga::Mode::lores) {
    // STF uses brute-force over 512 colors (same algorithm, different precision)
    // STE 12-bit = OCS 12-bit → same brute-force
    if (amiga::is_atari(mode)) return quantize::Algorithm::ocs_bruteforce;
    return chipset == amiga::Chipset::aga
        ? quantize::Algorithm::median_cut
        : quantize::Algorithm::ocs_bruteforce;
}

void snap_to_chipset(Palette& pal, amiga::Chipset chipset, amiga::Mode mode = amiga::Mode::lores) {
    if (amiga::is_stf(mode)) {
        for (auto& c : pal.colors) c = palette::quantize_to_stf(c);
    } else if (chipset != amiga::Chipset::aga) {
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

// Load image from memory, crop, scale, preprocess.
// If the source has alpha, computes a transparency mask at target resolution
// using the configured alpha threshold / dither method.
Result<Image> load_and_preprocess(const std::uint8_t* input_data,
                                   std::size_t input_size,
                                   const Options& options,
                                   std::size_t target_w,
                                   std::size_t target_h,
                                   std::vector<bool>* out_tmask = nullptr) {
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
    std::vector<float> src_alpha;
    if (any_transparent) src_alpha.resize(pixel_count);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto base = i * 4;
        pixels[i] = color_space::srgb_u8_to_linear(
            raw[base], raw[base + 1], raw[base + 2]);
        if (any_transparent)
            src_alpha[i] = static_cast<float>(raw[base + 3]) / 255.0f;
    }
    stbi_image_free(raw);

    // Compute transparency mask at target resolution before crop/scale
    // destroys the alpha channel. Bilinear-sample source alpha, then
    // apply threshold or ordered dither.
    if (out_tmask && any_transparent) {
        auto aw = width, ah = height;
        out_tmask->resize(target_w * target_h);
        auto alpha_dither = parse_dither(options.alpha_dither);
        float cutoff = 0.5f + options.alpha_threshold;
        for (std::size_t y = 0; y < target_h; ++y) {
            auto sy = std::min(y * ah / target_h, ah - 1);
            for (std::size_t x = 0; x < target_w; ++x) {
                auto sx = std::min(x * aw / target_w, aw - 1);
                float a = src_alpha[sy * aw + sx];
                if (alpha_dither != dither::Method::none) {
                    float thr = dither::ordered_threshold(alpha_dither, x, y);
                    (*out_tmask)[y * target_w + x] = a < (cutoff + thr * options.alpha_dither_strength);
                } else {
                    (*out_tmask)[y * target_w + x] = a < cutoff;
                }
            }
        }
    } else if (out_tmask) {
        out_tmask->clear();
    }

    Image image(width, height, std::move(pixels),
                any_transparent ? std::move(src_alpha) : std::vector<float>{});

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
    bool hires = false;
    bool interlace;

    // Copper mode
    bool copper = false;
    std::vector<std::vector<Color3f>> scanline_palettes;
    std::vector<std::vector<copper::CopperChange>> scanline_changes;
    std::size_t copper_num_colors{};
    std::size_t changes_per_line{};

    // Set after construction:
    bool has_transparency = false;
    std::vector<bool> transparency_mask;
    float copper_changes{};
    float quant_error{};
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
    // PAR from mode params; interlace doubles vertical resolution
    auto par = static_cast<double>(params.preview_scale_x)
             / static_cast<double>(params.preview_scale_y);
    if (options.interlace && !params.is_interlaced) par *= 2.0;

    bool have_w = options.width > 0;
    bool have_h = options.height > 0;

    if (have_w && have_h) {
        return {static_cast<std::size_t>(options.width),
                static_cast<std::size_t>(options.height)};
    }
    if (have_w) {
        auto w = static_cast<std::size_t>(options.width);
        // If width differs from mode default, adjust PAR for the resolution change
        // (e.g. ham6 at 640px = hires HAM, pixels are half-width)
        auto w_par = (w != mode_w && mode_w > 0)
            ? par * static_cast<double>(mode_w) / static_cast<double>(w)
            : par;
        auto h = round_even(static_cast<double>(w) * w_par / src_aspect);
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
    auto mode_h = params.screen_height;
    if (mode_h > 0) {
        return {w, mode_h};  // fixed height (Atari ST)
    }
    auto h = round_even(static_cast<double>(w) * par / src_aspect);
    return {w, h};
}

// Decompose compound mode strings (e.g. "ham6-hires-lace") into base mode
// + width/interlace overrides. Mutates a local copy of options.
Options decompose_mode_options(const Options& opts) {
    auto o = opts;
    auto& s = o.mode;
    bool has_hires = s.find("hires") != std::string::npos;
    bool has_lace = s.size() > 4 && s.find("-lace") != std::string::npos;
    // Only override if user didn't already set these
    if (has_hires && o.width == 0) o.width = 640;
    if (has_lace) o.interlace = true;
    return o;
}

Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const Options& orig_options) {
    auto options = decompose_mode_options(orig_options);
    auto mode = parse_mode(options.mode);
    bool compound_hires = orig_options.mode.find("hires") != std::string::npos;

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
    // Atari modes have fixed depth
    if (amiga::is_atari(mode))
        depth = amiga::get_mode_params(mode).bitplane_depth;

    std::vector<bool> tmask;
    auto image = load_and_preprocess(input_data, input_size, options,
                                      target_w, target_h, &tmask);
    if (!image) return std::unexpected{image.error()};
    bool has_transparency = !tmask.empty();

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

        Result<ham::HamResult> ham_result;
        if (options.copper) {
            ham_result = ham::encode_ham_copper(*image, mode, chipset, ham_opts, compound_hires);
        } else {
            ham_result = ham::encode_ham(*image, mode, chipset, ham_opts);
        }
        if (!ham_result) return std::unexpected{ham_result.error()};

        // Render preview using HAM decoder (not simple palette lookup)
        auto data_bits = ham_result->planes.depth - 2;
        Result<Image> preview;
        if (options.copper && !ham_result->scanline_palettes.empty()) {
            preview = ham::render_ham_copper(ham_result->planes,
                                            ham_result->scanline_palettes,
                                            data_bits);
        } else {
            preview = ham::render_ham(ham_result->planes,
                                     ham_result->base_palette,
                                     data_bits);
        }
        if (!preview) return std::unexpected{preview.error()};

        PipelineResult result;
        result.rendered = *std::move(preview);
        result.planes = std::move(ham_result->planes);
        result.palette = std::move(ham_result->base_palette);
        result.mode = mode;
        result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
        result.interlace = options.interlace;
        if (options.copper) {
            result.copper = true;
            result.scanline_palettes = std::move(ham_result->scanline_palettes);
            result.scanline_changes = std::move(ham_result->copper_changes);
            result.changes_per_line = ham_result->changes_per_line;
            // Compute average actual changes per line
            std::size_t total_ch = 0;
            for (auto& ch : result.scanline_changes) total_ch += ch.size();
            auto h = image->height();
            result.copper_changes = h > 0
                ? static_cast<float>(total_ch) / static_cast<float>(h) : 0.0f;
        }
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.quant_error = ham_result->total_error;
        return result;
    }

    // --- EHB mode: 32 base colors + 32 half-brightness ---
    if (mode == amiga::Mode::ehb) {
        depth = 6;  // EHB is always 6 bitplanes

        // Generate base colors via median-cut, or load from file.
        // Reserve index 0 for transparency when needed.
        auto ehb_base = has_transparency ? std::size_t{31} : std::size_t{32};
        Palette base_pal;
        if (!options.palette_file.empty()) {
            auto loaded = palette_io::load_palette(options.palette_file);
            if (!loaded) return std::unexpected{loaded.error()};
            base_pal = *std::move(loaded);
            if (base_pal.colors.size() > ehb_base)
                base_pal.colors.resize(ehb_base);
            snap_to_chipset(base_pal, chipset);
        } else {
            auto quantized = quantize::quantize(*image, ehb_base,
                                                quantize_algo(chipset));
            if (!quantized) return std::unexpected{quantized.error()};
            base_pal = *std::move(quantized);
        }
        if (has_transparency)
            base_pal.colors.insert(base_pal.colors.begin(), Color3f{0.0f, 0.0f, 0.0f});

        // Build full 64-color EHB palette (32 base + 32 half-bright)
        auto ehb_pal = palette::make_ehb_palette(base_pal.colors);

        if (options.match_range)
            preprocess::match_palette_range(*image, ehb_pal);

        // Dither against all 64 colors
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        dither::DitherResult dither_result;
        if (has_transparency) {
            // Skip index 0 during dithering
            std::span<const Color3f> dither_span{ehb_pal.colors.data() + 1,
                                                  ehb_pal.colors.size() - 1};
            dither_result = dither::apply(*image, dither_span, dith);
            for (auto& idx : dither_result.indices) ++idx;
            for (std::size_t i = 0; i < tmask.size() && i < dither_result.indices.size(); ++i)
                if (tmask[i]) dither_result.indices[i] = 0;
        } else {
            dither_result = dither::apply(*image, ehb_pal.colors, dith);
        }

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

        PipelineResult result;
        result.rendered = *std::move(preview);
        result.planes = *std::move(planes);
        result.palette = std::move(full_palette);
        result.mode = mode;
        result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
        result.interlace = options.interlace;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.quant_error = dither_result.total_error;
        return result;
    }

    // --- Copper palette mode ---
    if (options.copper && !amiga::is_ham(mode) && mode != amiga::Mode::ehb) {
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        auto copper_result = copper::encode_copper(*image, depth, dith, chipset,
                                                     false, compound_hires);
        if (!copper_result) return std::unexpected{copper_result.error()};

        auto preview = copper::render_copper(copper_result->planes,
                                             copper_result->scanline_palettes);
        if (!preview) return std::unexpected{preview.error()};

        // Use the first scanline's palette as the nominal palette
        // (for CMAP chunk in IFF; the real palettes are in COPL)
        auto& first_pal = copper_result->scanline_palettes[0];

        PipelineResult result;
        result.rendered = *std::move(preview);
        result.planes = std::move(copper_result->planes);
        result.palette = std::vector<Color3f>(first_pal.begin(), first_pal.end());
        result.mode = mode;
        result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
        result.interlace = options.interlace;
        result.copper = true;
        result.scanline_palettes = std::move(copper_result->scanline_palettes);
        result.scanline_changes = std::move(copper_result->scanline_changes);
        result.copper_num_colors = copper_result->num_colors;
        result.changes_per_line = copper_result->changes_per_line;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.copper_changes = copper_result->avg_changes_per_line;
        result.quant_error = copper_result->total_error;
        return result;
    }

    // --- Standard bitplane modes ---
    auto max_colors = std::size_t{1} << depth;

    // Build palette.
    // When transparency is present, reserve index 0 for transparent color:
    // quantize N-1 colors, then prepend black at index 0.
    auto quant_colors = has_transparency ? max_colors - 1 : max_colors;
    Palette pal;
    if (!options.palette_file.empty()) {
        auto loaded = palette_io::load_palette(options.palette_file);
        if (!loaded) return std::unexpected{loaded.error()};
        pal = *std::move(loaded);
        if (pal.colors.size() > quant_colors)
            pal.colors.resize(quant_colors);
        snap_to_chipset(pal, chipset, mode);
    } else {
        auto quantized = quantize::quantize(*image, quant_colors,
                                            quantize_algo(chipset, mode));
        if (!quantized) return std::unexpected{quantized.error()};
        pal = *std::move(quantized);
    }
    if (has_transparency) {
        pal.colors.insert(pal.colors.begin(), Color3f{0.0f, 0.0f, 0.0f});
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

    dither::DitherResult dither_result;
    if (has_transparency) {
        // Dither against colors [1..N] only (skip reserved index 0)
        std::span<const Color3f> dither_span{pal.colors.data() + 1, pal_size - 1};
        dither_result = dither::apply(*image, dither_span, dith);
        // Offset all indices by 1 (index 0 is reserved for transparency)
        for (auto& idx : dither_result.indices) ++idx;
        // Force transparent pixels to index 0
        for (std::size_t i = 0; i < tmask.size() && i < dither_result.indices.size(); ++i)
            if (tmask[i]) dither_result.indices[i] = 0;
    } else {
        dither_result = dither::apply(*image, pal_span, dith);
    }

    // Encode to bitplanes (Atari uses word-interleaved layout)
    auto bp_layout = amiga::is_atari(mode)
        ? bitplane::Layout::word_interleaved
        : bitplane::Layout::interleaved;
    auto planes = bitplane::encode(dither_result.indices,
                                   image->width(), image->height(),
                                   depth, bp_layout);
    if (!planes) return std::unexpected{planes.error()};

    // Build used palette vector
    std::vector<Color3f> used_palette(pal_span.begin(), pal_span.end());

    // Render preview
    auto preview = bitplane::render(*planes, used_palette);
    if (!preview) return std::unexpected{preview.error()};

    PipelineResult result;
    result.rendered = *std::move(preview);
    result.planes = *std::move(planes);
    result.palette = std::move(used_palette);
    result.mode = mode;
    result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
    result.interlace = options.interlace;
    result.has_transparency = has_transparency;
    result.transparency_mask = tmask;
    result.quant_error = dither_result.total_error;
    return result;
}

ConvertResult make_error(const std::string& msg) {
    ConvertResult r;
    r.error = msg;
    return r;
}

ConvertResult make_result(std::vector<std::uint8_t> data, const PipelineResult& p) {
    ConvertResult r;
    r.data = std::move(data);
    r.width = static_cast<int>(p.rendered.width());
    r.height = static_cast<int>(p.rendered.height());
    r.depth = static_cast<int>(p.planes.depth);
    r.colors = static_cast<int>(p.palette.size());
    r.copperChanges = p.copper_changes;
    r.quantError = p.quant_error;
    r.hasTransparency = p.has_transparency;
    return r;
}

} // namespace

ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    auto png = png_io::encode(result->rendered);
    if (!png) return make_error(png.error().message);

    return make_result(*std::move(png), *result);
}

ConvertResult convert_rgba(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    auto& img = result->rendered;
    auto w = img.width();
    auto h = img.height();
    std::vector<std::uint8_t> rgba(w * h * 4);

    auto& tmask = result->transparency_mask;

    for (std::size_t i = 0; i < w * h; ++i) {
        auto srgb = color_space::linear_to_srgb(img.pixels()[i]).clamped();
        rgba[i * 4 + 0] = static_cast<std::uint8_t>(srgb.r * 255.0f + 0.5f);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>(srgb.g * 255.0f + 0.5f);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(srgb.b * 255.0f + 0.5f);
        rgba[i * 4 + 3] = (i < tmask.size() && tmask[i]) ? 0 : 255;
    }

    return make_result(std::move(rgba), *result);
}

ConvertResult convert_iff(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    iff::IffOptions iff_opts;
    iff_opts.interlace = result->interlace;
    if (result->copper && !result->scanline_palettes.empty()) {
        iff_opts.scanline_palettes = &result->scanline_palettes;
    }

    auto iff_data = iff::write_ilbm(
        result->planes, result->palette, result->mode, iff_opts);
    if (!iff_data) return make_error(iff_data.error().message);

    return make_result(*std::move(iff_data), *result);
}

ConvertResult convert_degas(const std::uint8_t* input_data,
                            std::size_t input_size,
                            const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    auto degas_data = degas::encode(
        result->planes, result->palette, result->mode);
    if (!degas_data) return make_error(degas_data.error().message);

    return make_result(*std::move(degas_data), *result);
}

ConvertResult convert_cheader(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    cheader::CHeaderOptions ch_opts;
    if (!options.symbol_name.empty())
        ch_opts.symbol_name = options.symbol_name;
    auto header = cheader::generate(
        result->planes, result->palette, result->mode, ch_opts);
    if (!header) return make_error(header.error().message);

    std::vector<std::uint8_t> bytes(header->begin(), header->end());
    return make_result(std::move(bytes), *result);
}

ConvertResult convert_viewer(const std::uint8_t* input_data,
                             std::size_t input_size,
                             const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    // Pad or crop bitplanes to display width for correct hardware row stride.
    auto mode = result->mode;
    auto display_w = result->hires
        ? std::size_t{640} : amiga::default_width(mode);
    auto& planes = result->planes;
    if (planes.width != display_w) {
        auto old_bpr = planes.bytes_per_row;
        auto new_bpr = ((display_w + 15) / 16) * 2;  // word-aligned
        auto depth = planes.depth;
        auto height = planes.height;
        std::vector<std::uint8_t> padded(depth * height * new_bpr, 0);
        auto copy_bpr = std::min(old_bpr, new_bpr);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t p = 0; p < depth; ++p) {
                auto src_off = planes.plane_row_offset(p, y);
                std::size_t dst_off;
                if (planes.layout == bitplane::Layout::interleaved)
                    dst_off = y * depth * new_bpr + p * new_bpr;
                else
                    dst_off = p * height * new_bpr + y * new_bpr;
                std::copy_n(planes.data.data() + src_off, copy_bpr,
                            padded.data() + dst_off);
            }
        }
        planes.data = std::move(padded);
        planes.width = display_w;
        planes.bytes_per_row = new_bpr;
    }

    cheader::CHeaderOptions ch_opts;
    if (!options.symbol_name.empty())
        ch_opts.symbol_name = options.symbol_name;
    ch_opts.hires = result->hires;
    ch_opts.interlace = result->interlace;
    if (result->copper && !result->scanline_changes.empty()) {
        ch_opts.copper_changes = &result->scanline_changes;
        ch_opts.copper_changes_per_line = result->changes_per_line;
    }
    auto viewer = cheader::generate_viewer(
        planes, result->palette, result->mode, ch_opts);
    if (!viewer) return make_error(viewer.error().message);

    std::vector<std::uint8_t> bytes(viewer->begin(), viewer->end());
    return make_result(std::move(bytes), *result);
}

ConvertResult convert_raw(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    return make_result(std::move(result->planes.data), *result);
}

ConvertResult convert_palette(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    auto pal_data = palette_io::encode_ocs_palette(result->palette);
    if (!pal_data) return make_error(pal_data.error().message);

    return make_result(*std::move(pal_data), *result);
}

} // namespace png2amiga::api
