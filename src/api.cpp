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
#include "palette_locks.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"

#include <stb_image.h>
#include <webp/decode.h>

#include <cstring>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <vector>

namespace png2amiga::api {

namespace {


// Load palette from inline data or file path
Result<Palette> load_user_palette(const Options& opts) {
    if (!opts.palette_data.empty())
        return palette_io::load_palette_from_memory(opts.palette_data);
    if (!opts.palette_file.empty())
        return palette_io::load_palette(opts.palette_file);
    return std::unexpected{Error{ErrorCode::unsupported_mode, "No palette"}};
}

bool has_user_palette(const Options& opts) {
    return !opts.palette_data.empty() || !opts.palette_file.empty();
}

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
    if (s == "stf-hi") return amiga::Mode::stf_hi;
    if (s == "ste-hi") return amiga::Mode::ste_hi;
    return amiga::Mode::lores;
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
    if (s == "v4x2") return dither::Method::v4x2;
    if (s == "bayer4x2") return dither::Method::bayer4x2;
    if (s == "bayer2x4") return dither::Method::bayer2x4;
    if (s == "vline2") return dither::Method::vline2;
    if (s == "vline-checker") return dither::Method::vline_checker;
    if (s == "vline4") return dither::Method::vline4;
    if (s == "vline8") return dither::Method::vline8;
    if (s == "line8") return dither::Method::line8;
    if (s == "halftone8x8") return dither::Method::halftone8x8;
    if (s == "diagonal8x8") return dither::Method::diagonal8x8;
    if (s == "spiral5x5") return dither::Method::spiral5x5;
    if (s == "hex8x8") return dither::Method::hex8x8;
    if (s == "hex5x5") return dither::Method::hex5x5;
    if (s == "blue-noise") return dither::Method::blue_noise;
    if (s == "floyd-steinberg") return dither::Method::floyd_steinberg;
    if (s == "atkinson") return dither::Method::atkinson;
    if (s == "sierra-lite") return dither::Method::sierra_lite;
    if (s == "stucki") return dither::Method::stucki;
    if (s == "jarvis") return dither::Method::jarvis;
    if (s == "ostromoukhov") return dither::Method::ostromoukhov;
    if (s == "ign") return dither::Method::ign;
    if (s == "white-noise") return dither::Method::white_noise;
    if (s == "r2") return dither::Method::r2_sequence;
    if (s == "crosshatch") return dither::Method::crosshatch;
    if (s == "radial") return dither::Method::radial;
    if (s == "value-noise") return dither::Method::value_noise;
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

// Auto-crop region selection is inlined into load_and_preprocess so the
// transparency mask can be sampled from the same region.

// Load image from memory, crop, scale, preprocess.
// If the source has alpha, computes a transparency mask at target resolution
// using the configured alpha threshold / dither method.
Result<Image> load_and_preprocess(const std::uint8_t* input_data,
                                   std::size_t input_size,
                                   const Options& options,
                                   std::size_t target_w,
                                   std::size_t target_h,
                                   std::vector<bool>* out_tmask = nullptr) {
    int w{}, h{};
    // Detect WebP (RIFF...WEBP) and dispatch to libwebp; else use stb_image.
    bool is_webp_img = input_size >= 12 &&
        std::memcmp(input_data, "RIFF", 4) == 0 &&
        std::memcmp(input_data + 8, "WEBP", 4) == 0;
    unsigned char* raw = nullptr;
    if (is_webp_img) {
        raw = WebPDecodeRGBA(input_data, input_size, &w, &h);
    } else {
        int channels{};
        raw = stbi_load_from_memory(input_data,
            static_cast<int>(input_size), &w, &h, &channels, 4);
    }
    auto free_raw = [&]() {
        if (!raw) return;
        if (is_webp_img) WebPFree(raw); else stbi_image_free(raw);
    };
    if (!raw)
        return std::unexpected{Error{ErrorCode::invalid_png, "Failed to decode image"}};

    // Reject pathological dimensions before they overflow size_t under
    // 32-bit (WASM) or exhaust memory under 64-bit.
    constexpr int kMaxDimension = 32768;
    if (w <= 0 || h <= 0 || w > kMaxDimension || h > kMaxDimension) {
        free_raw();
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Image dimensions out of range: {}x{} (max {}x{})",
                        w, h, kMaxDimension, kMaxDimension),
        }};
    }

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
    free_raw();

    // Determine the effective source crop region up front so we can sample
    // the transparency mask from the cropped region rather than the full
    // source image. crop and tmask have to agree, otherwise the mask
    // references pixels that the image no longer contains.
    std::size_t crop_x = 0, crop_y = 0;
    std::size_t crop_w = width, crop_h = height;
    bool will_crop = false;
    if (options.crop_w > 0 && options.crop_h > 0) {
        auto cx = static_cast<std::size_t>(options.crop_x);
        auto cy = static_cast<std::size_t>(options.crop_y);
        auto cw = static_cast<std::size_t>(options.crop_w);
        auto ch = static_cast<std::size_t>(options.crop_h);
        if (cx + cw > width || cy + ch > height || cw == 0 || ch == 0) {
            free_raw();
            return std::unexpected{Error{
                ErrorCode::invalid_dimensions,
                std::format("Crop region {}x{}+{}+{} exceeds image {}x{}",
                            cw, ch, cx, cy, width, height),
            }};
        }
        crop_x = cx; crop_y = cy; crop_w = cw; crop_h = ch;
        will_crop = true;
    } else if (options.crop_auto) {
        // Center-crop source to target aspect ratio
        auto target_ratio = static_cast<double>(target_w) / static_cast<double>(target_h);
        auto src_ratio = static_cast<double>(width) / static_cast<double>(height);
        if (src_ratio > target_ratio) {
            crop_h = height;
            crop_w = static_cast<std::size_t>(
                static_cast<double>(height) * target_ratio + 0.5);
        } else {
            crop_w = width;
            crop_h = static_cast<std::size_t>(
                static_cast<double>(width) / target_ratio + 0.5);
        }
        crop_x = (width - crop_w) / 2;
        crop_y = (height - crop_h) / 2;
        will_crop = (crop_w != width || crop_h != height);
    }

    // Compute transparency mask at target resolution from the (cropped)
    // source region. Sample alpha at the source pixel that maps to each
    // target pixel via the crop window, then apply threshold or ordered
    // dither.
    if (out_tmask && any_transparent) {
        out_tmask->resize(target_w * target_h);
        auto alpha_dither = parse_dither(options.alpha_dither);
        float cutoff = 0.5f + options.alpha_threshold;
        for (std::size_t y = 0; y < target_h; ++y) {
            auto sy = std::min(crop_y + y * crop_h / target_h,
                               crop_y + crop_h - 1);
            for (std::size_t x = 0; x < target_w; ++x) {
                auto sx = std::min(crop_x + x * crop_w / target_w,
                                   crop_x + crop_w - 1);
                float a = src_alpha[sy * width + sx];
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

    // Apply the crop region we already validated above.
    if (will_crop) {
        auto cropped = crop_image(image, crop_x, crop_y, crop_w, crop_h);
        if (!cropped) return std::unexpected{cropped.error()};
        image = *std::move(cropped);
    }

    if (image.width() != target_w || image.height() != target_h) {
        // Interlace needs even height. If only 1 row short, pad instead of
        // resampling the entire image (avoids blur from bicubic).
        if (image.width() == target_w && target_h == image.height() + 1) {
            Image padded(target_w, target_h);
            for (std::size_t y = 0; y < image.height(); ++y)
                for (std::size_t x = 0; x < target_w; ++x)
                    padded[x, y] = image[x, y];
            // Repeat last row
            for (std::size_t x = 0; x < target_w; ++x)
                padded[x, target_h - 1] = image[x, image.height() - 1];
            image = std::move(padded);
        } else {
            auto scaled = scale::bicubic(image, target_w, target_h);
            if (!scaled) return std::unexpected{scaled.error()};
            image = *std::move(scaled);
        }
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

    // Per-pixel palette indices, populated only for modes with a single
    // global palette (lores/hires/EHB without copper). Empty for HAM and
    // copper modes where the palette varies. Used by the PNG encoder to
    // emit a palettized PNG-8 instead of full RGB.
    std::vector<std::uint8_t> indices;

    // Copper mode
    bool copper = false;
    bool aga = false;
    std::vector<std::vector<Color3f>> scanline_palettes;
    std::vector<std::vector<copper::CopperChange>> scanline_changes;
    std::size_t copper_num_colors{};
    std::size_t changes_per_line{};
    std::size_t max_moves_per_line{};   // worst-case copper MOVEs/line for chip-RAM sizing

    // Set after construction:
    bool has_transparency = false;
    std::vector<bool> transparency_mask;
    float copper_changes{};
    float quant_error{};
    float psnr{};
};

// Round height. Only force even for interlace (fields must be equal).
std::size_t round_height(double v, bool interlace) {
    auto r = static_cast<std::size_t>(std::lround(v));
    if (interlace) return (r + 1) & ~std::size_t{1};  // round up to even
    return r;
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
    bool interlace = options.interlace || params.is_interlaced;
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
        auto h = round_height(static_cast<double>(w) * w_par / src_aspect, interlace);
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
    auto h = round_height(static_cast<double>(w) * par / src_aspect, interlace);
    // For fixed-height modes (Atari ST), clamp to screen_height
    auto mode_h = params.screen_height;
    if (mode_h > 0 && h > mode_h) h = mode_h;
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
    int peek_w{}, peek_h{};
    bool peek_is_webp = input_size >= 12 &&
        std::memcmp(input_data, "RIFF", 4) == 0 &&
        std::memcmp(input_data + 8, "WEBP", 4) == 0;
    bool peek_ok = peek_is_webp
        ? (WebPGetInfo(input_data, input_size, &peek_w, &peek_h) != 0)
        : (stbi_info_from_memory(input_data, static_cast<int>(input_size),
                                 &peek_w, &peek_h, nullptr) != 0);
    if (!peek_ok) {
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

    // Atari: center vertically in fixed-height frame if image is shorter
    auto mode_h = amiga::get_mode_params(mode).screen_height;
    if (mode_h > 0 && image->height() < mode_h) {
        auto w = image->width();
        auto h = image->height();
        Image padded(w, mode_h);
        auto y_off = (mode_h - h) / 2;
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x)
                padded[x, y + y_off] = (*image)[x, y];
        if (has_transparency) {
            std::vector<bool> new_mask(w * mode_h, true);
            for (std::size_t y = 0; y < h; ++y)
                for (std::size_t x = 0; x < w; ++x)
                    new_mask[(y + y_off) * w + x] = tmask[y * w + x];
            tmask = std::move(new_mask);
        }
        *image = std::move(padded);
    }

    auto chipset = resolve_chipset(options.chipset, mode);
    bool is_aga = (chipset == amiga::Chipset::aga);

    // --- HAM modes: use dedicated HAM encoder ---
    if (amiga::is_ham(mode)) {
        if (!options.locks.empty() || !options.pins.empty()) {
            return std::unexpected{Error{
                ErrorCode::unsupported_mode,
                "--lock-index / --pin-index-at are not supported in HAM modes "
                "(palette is dynamic per pixel)",
            }};
        }

        ham::HamOptions ham_opts;
        ham_opts.beam_width = static_cast<std::size_t>(
            std::clamp(options.ham_beam, 1, 256));

        // Wire dither settings into HAM options
        ham_opts.dither_method = parse_dither(options.dither);
        ham_opts.dither_strength = options.dither_strength;
        ham_opts.error_clamp = options.error_clamp;
        ham_opts.palette_diversity = options.palette_diversity;

        // Force transparent pixels to black BEFORE HAM encoding so the
        // encoder handles color transitions correctly at transparency edges.
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        Result<ham::HamResult> ham_result;
        if (options.copper) {
            ham_result = ham::encode_ham_copper(*image, mode, chipset, ham_opts,
                                                  compound_hires,
                                                  static_cast<std::size_t>(options.copper_changes));
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
            result.aga = is_aga;
            result.scanline_palettes = std::move(ham_result->scanline_palettes);
            result.scanline_changes = std::move(ham_result->copper_changes);
            result.changes_per_line = ham_result->changes_per_line;
            // Compute average actual changes per line
            std::size_t total_ch = 0;
            for (auto& ch : result.scanline_changes) total_ch += ch.size();
            auto h = image->height();
            result.copper_changes = h > 0
                ? static_cast<float>(total_ch) / static_cast<float>(h) : 0.0f;
            // HAM base palette is 2^(depth-2) colors; >32 means bank-switching
            auto ham_base = std::size_t{1} << (depth - 2);
            bool ham_aga_banks = is_aga && ham_base > 32;
            for (auto& ch : result.scanline_changes) {
                auto m = copper::moves_for_line(ch, is_aga, ham_aga_banks);
                if (m > result.max_moves_per_line) result.max_moves_per_line = m;
            }
        }
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        // Force transparent pixels to black in rendered preview
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) result.rendered.pixels()[i] = Color3f{0, 0, 0};
        }
        result.quant_error = ham_result->total_error;
        result.psnr = color_space::compute_psnr_blurred(
            image->pixels(), result.rendered.pixels(),
            image->width(), image->height());
        return result;
    }

    // --- EHB mode: 32 base colors + 32 half-brightness ---
    if (mode == amiga::Mode::ehb) {
        depth = 6;  // EHB is always 6 bitplanes

        // Force transparent pixels to black before encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        // --- EHB with copper: per-scanline base palette optimization ---
        if (options.copper) {
            if (!options.locks.empty() || !options.pins.empty()) {
                return std::unexpected{Error{
                    ErrorCode::unsupported_mode,
                    "--lock-index / --pin-index-at are not supported with EHB + --copper",
                }};
            }
            // Use copper encoder with depth=5 (32 base colors).
            // It generates per-scanline palette changes for the base 32.
            dither::Settings dith;
            dith.method = parse_dither(options.dither);
            dith.strength = options.dither_strength;
            dith.error_clamp = options.error_clamp;

            auto copper_result = copper::encode_copper(*image, 5, dith, chipset,
                                                       static_cast<std::size_t>(options.copper_changes),
                                                       nullptr, options.reserve_color0,
                                                       {}, options.palette_diversity);
            if (!copper_result) return std::unexpected{copper_result.error()};

            // Now re-dither each scanline against its 64-color EHB palette
            // (32 base from copper + 32 half-brite derived).
            auto w = image->width();
            auto h = image->height();
            std::vector<std::uint8_t> all_indices(w * h);
            float total_error = 0.0f;

            bool use_ordered = dither::is_ordered(dith.method) &&
                               dith.method != dither::Method::none;
            bool use_diffusion = !use_ordered &&
                                 dith.method != dither::Method::none;
            // Error buffer for diffusion (OKLab per pixel)
            std::vector<color_space::OKLab> err_buf;
            if (use_diffusion) err_buf.resize(w * h);

            for (std::size_t y = 0; y < h; ++y) {
                // Get this scanline's base palette from copper
                auto& base32 = copper_result->scanline_palettes[y];
                Palette bp;
                bp.colors.assign(base32.begin(), base32.end());
                auto ehb64 = palette::make_ehb_palette(bp.colors);

                // Dither this row against all 64 EHB colors
                auto row = image->row(y);
                std::vector<color_space::OKLab> pal_lab(ehb64.colors.size());
                for (std::size_t i = 0; i < ehb64.colors.size(); ++i)
                    pal_lab[i] = color_space::linear_to_oklab(ehb64.colors[i]);

                for (std::size_t x = 0; x < w; ++x) {
                    auto pixel_lab = color_space::linear_to_oklab(row[x]);

                    // Add accumulated error from previous rows (clamped)
                    if (!err_buf.empty()) {
                        auto& e = err_buf[y * w + x];
                        auto ec = dith.error_clamp;
                        pixel_lab.L += std::clamp(e.L, -ec, ec);
                        pixel_lab.a += std::clamp(e.a, -ec, ec);
                        pixel_lab.b += std::clamp(e.b, -ec, ec);
                    }

                    // Ordered dither: apply threshold with correct (x, y)
                    if (use_ordered) {
                        float thr = dither::ordered_threshold(dith.method, x, y);
                        pixel_lab.L += thr * dith.strength * 0.15f;
                        pixel_lab.a += thr * dith.strength * 0.03f;
                        pixel_lab.b += thr * dith.strength * 0.03f;
                    }

                    // Find nearest color
                    float best_d = std::numeric_limits<float>::max();
                    std::uint8_t best_k = 0;
                    for (std::size_t k = 0; k < pal_lab.size(); ++k) {
                        float dL = pixel_lab.L - pal_lab[k].L;
                        float da = pixel_lab.a - pal_lab[k].a;
                        float db = pixel_lab.b - pal_lab[k].b;
                        float d = dL * dL + da * da + db * db;
                        if (d < best_d) { best_d = d; best_k = static_cast<std::uint8_t>(k); }
                    }
                    all_indices[y * w + x] = best_k;
                    total_error += best_d;

                    // Error diffusion: propagate to neighbors using the
                    // kernel for the chosen method (floyd-steinberg,
                    // atkinson, sierra-lite, stucki, jarvis).
                    if (use_diffusion) {
                        auto chosen_lab = pal_lab[best_k];
                        color_space::OKLab qerr = {
                            (pixel_lab.L - chosen_lab.L) * dith.strength,
                            (pixel_lab.a - chosen_lab.a) * dith.strength,
                            (pixel_lab.b - chosen_lab.b) * dith.strength,
                        };
                        auto kernel = dither::error_diffusion_kernel(dith.method);
                        for (auto& [kdx, kdy, kw] : kernel) {
                            auto nx = static_cast<std::ptrdiff_t>(x) + kdx;
                            auto ny = static_cast<std::ptrdiff_t>(y) + kdy;
                            if (nx >= 0 && static_cast<std::size_t>(nx) < image->width() &&
                                ny >= 0 && static_cast<std::size_t>(ny) < h) {
                                auto& e = err_buf[static_cast<std::size_t>(ny) * image->width() +
                                                  static_cast<std::size_t>(nx)];
                                e.L += qerr.L * kw;
                                e.a += qerr.a * kw;
                                e.b += qerr.b * kw;
                            }
                        }
                    }
                }
            }

            // Handle transparency
            if (has_transparency) {
                for (std::size_t i = 0; i < tmask.size() && i < all_indices.size(); ++i)
                    if (tmask[i]) all_indices[i] = 0;
            }

            // Encode to 6 bitplanes
            auto planes = bitplane::encode(all_indices, w, h, 6);
            if (!planes) return std::unexpected{planes.error()};

            // Render preview using last scanline's palette (approximate)
            // Better: render per-scanline with correct palettes
            auto& last_base = copper_result->scanline_palettes[0];
            Palette preview_bp;
            preview_bp.colors.assign(last_base.begin(), last_base.end());
            auto preview_ehb = palette::make_ehb_palette(preview_bp.colors);

            // Per-scanline preview render
            Image rendered(w, h);
            for (std::size_t y = 0; y < h; ++y) {
                auto& base32 = copper_result->scanline_palettes[y];
                Palette bp;
                bp.colors.assign(base32.begin(), base32.end());
                auto ehb64 = palette::make_ehb_palette(bp.colors);
                for (std::size_t x = 0; x < w; ++x) {
                    auto idx = all_indices[y * w + x];
                    if (idx < ehb64.colors.size())
                        rendered[x, y] = ehb64.colors[idx];
                }
            }

            // Use base palette from first scanline for IFF CMAP
            auto& first_pal = copper_result->scanline_palettes[0];

            PipelineResult result;
            result.rendered = std::move(rendered);
            result.planes = *std::move(planes);
            result.palette = std::vector<Color3f>(first_pal.begin(), first_pal.end());
            result.mode = mode;
            result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
            result.interlace = options.interlace;
            result.copper = true;
            result.aga = is_aga;
            result.scanline_palettes = std::move(copper_result->scanline_palettes);
            result.scanline_changes = std::move(copper_result->scanline_changes);
            result.copper_num_colors = copper_result->num_colors;
            result.changes_per_line = copper_result->changes_per_line;
            result.max_moves_per_line = copper_result->max_moves_per_line;
            result.has_transparency = has_transparency;
            result.transparency_mask = tmask;
            result.copper_changes = copper_result->avg_changes_per_line;
            result.quant_error = total_error;
            result.psnr = color_space::compute_psnr_blurred(
                image->pixels(), result.rendered.pixels(),
                image->width(), image->height());
            return result;
        }

        // --- EHB without copper: global palette ---

        // Validate locks/pins (EHB: targets must be in 0-31, the base palette).
        if (auto v = palette_locks::validate_locks(options.locks, 32); !v)
            return std::unexpected{v.error()};
        if (auto v = palette_locks::validate_pins(options.pins, options.locks, 32,
                                                  image->width(), image->height(),
                                                  has_transparency); !v)
            return std::unexpected{v.error()};

        // Generate base colors via median-cut, or load from file.
        // With custom palette: use as-is (32 colors expected).
        // Without: reserve index 0 for transparency when needed.
        bool user_pal_ehb = has_user_palette(options);
        bool reserve_zero_ehb = !user_pal_ehb && has_transparency;
        Palette base_pal;
        std::vector<bool> base_locked(32, false);
        if (user_pal_ehb) {
            auto loaded = load_user_palette(options);
            if (!loaded) return std::unexpected{loaded.error()};
            base_pal = *std::move(loaded);
            if (base_pal.colors.size() > 32)
                base_pal.colors.resize(32);
            snap_to_chipset(base_pal, chipset, mode);
            // Apply locks on top of user palette
            for (auto& lock : options.locks) {
                auto idx = static_cast<std::size_t>(lock.index);
                if (idx < base_pal.colors.size()) {
                    base_pal.colors[idx] = palette_locks::to_color(lock, chipset, mode);
                    base_locked[idx] = true;
                }
            }
        } else {
            auto qcount = palette_locks::quant_count(32, options.locks, reserve_zero_ehb);
            auto quantized = quantize::quantize(*image, qcount,
                                                quantize_algo(chipset),
                                                options.palette_diversity);
            if (!quantized) return std::unexpected{quantized.error()};
            auto assembled = palette_locks::assemble_locked_palette(
                *quantized, options.locks, 32, reserve_zero_ehb, chipset, mode);
            base_pal = std::move(assembled.palette);
            base_locked = std::move(assembled.locked);
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

        dither::DitherResult dither_result;
        if (has_transparency) {
            std::span<const Color3f> dither_span{ehb_pal.colors.data() + 1,
                                                  ehb_pal.colors.size() - 1};
            dither_result = dither::apply(*image, dither_span, dith);
            for (auto& idx : dither_result.indices) ++idx;
            for (std::size_t i = 0; i < tmask.size() && i < dither_result.indices.size(); ++i)
                if (tmask[i]) dither_result.indices[i] = 0;
        } else {
            dither_result = dither::apply(*image, ehb_pal.colors, dith);
        }

        // Apply EHB pin-index swaps. Pins act on the BASE 32 only; half-brite
        // copies (32-63) auto-track via re-derivation. If a pin's source pixel
        // landed on a half-brite slot, error out (the user should pick a
        // different pixel or a different target).
        for (auto& pin : options.pins) {
            auto target = static_cast<std::size_t>(pin.index);
            auto pixel_offset = static_cast<std::size_t>(pin.y) * image->width() +
                                static_cast<std::size_t>(pin.x);
            if (pixel_offset >= dither_result.indices.size()) {
                return std::unexpected{Error{
                    ErrorCode::invalid_dimensions,
                    std::format("--pin-index-at {}: pixel offset out of bounds",
                                pin.index),
                }};
            }
            auto src = static_cast<std::size_t>(dither_result.indices[pixel_offset]);
            if (src >= 32) {
                return std::unexpected{Error{
                    ErrorCode::invalid_depth,
                    std::format("--pin-index-at {}: source pixel ({},{}) "
                                "dithered to half-brite slot {} (EHB pins must "
                                "land on a base color, slots 0-31)",
                                pin.index, pin.x, pin.y, src),
                }};
            }
            if (src == target) {
                base_locked[target] = true;
                continue;
            }
            if (base_locked[target]) {
                return std::unexpected{Error{
                    ErrorCode::invalid_depth,
                    std::format("--pin-index-at {} targets a locked slot",
                                pin.index),
                }};
            }
            // Swap base palette entries
            std::swap(base_pal.colors[src], base_pal.colors[target]);
            // Swap pixel indices in BOTH the base column and the half-brite column
            auto src_u8 = static_cast<std::uint8_t>(src);
            auto tgt_u8 = static_cast<std::uint8_t>(target);
            auto src_hb = static_cast<std::uint8_t>(src + 32);
            auto tgt_hb = static_cast<std::uint8_t>(target + 32);
            for (auto& idx : dither_result.indices) {
                if (idx == src_u8) idx = tgt_u8;
                else if (idx == tgt_u8) idx = src_u8;
                else if (idx == src_hb) idx = tgt_hb;
                else if (idx == tgt_hb) idx = src_hb;
            }
            base_locked[target] = true;
            // Re-derive 64-color EHB palette
            ehb_pal = palette::make_ehb_palette(base_pal.colors);
        }

        // Encode to 6 bitplanes
        auto planes = bitplane::encode(dither_result.indices,
                                       image->width(), image->height(),
                                       depth);
        if (!planes) return std::unexpected{planes.error()};

        std::vector<Color3f> full_palette(ehb_pal.colors.begin(),
                                          ehb_pal.colors.end());

        auto preview = bitplane::render(*planes, full_palette);
        if (!preview) return std::unexpected{preview.error()};

        PipelineResult result;
        result.rendered = *std::move(preview);
        result.planes = *std::move(planes);
        result.palette = std::move(full_palette);
        result.indices = std::move(dither_result.indices);
        result.mode = mode;
        result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
        result.interlace = options.interlace;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        // Force transparent pixels to black in rendered preview
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) result.rendered.pixels()[i] = Color3f{0, 0, 0};
        }
        result.quant_error = dither_result.total_error;
        result.psnr = color_space::compute_psnr_blurred(
            image->pixels(), result.rendered.pixels(),
            image->width(), image->height());
        return result;
    }

    // --- Copper palette mode ---
    if (options.copper && !amiga::is_ham(mode) && mode != amiga::Mode::ehb) {
        if (!options.pins.empty()) {
            return std::unexpected{Error{
                ErrorCode::unsupported_mode,
                "--pin-index-at is not supported with --copper",
            }};
        }
        // Force transparent pixels to black before encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        // Load user palette for copper base (if provided)
        std::vector<Color3f> copper_user_pal;
        if (has_user_palette(options)) {
            auto loaded = load_user_palette(options);
            if (loaded) {
                Palette tmp = *std::move(loaded);
                snap_to_chipset(tmp, chipset, mode);
                copper_user_pal = std::move(tmp.colors);
            }
        }
        // Build locked slot list for copper from --lock-index specs
        std::vector<std::pair<std::size_t, Color3f>> copper_locks;
        for (auto& lock : options.locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            copper_locks.emplace_back(idx,
                palette_locks::to_color(lock, chipset, mode));
        }

        auto copper_result = copper::encode_copper(*image, depth, dith, chipset,
                                                     static_cast<std::size_t>(options.copper_changes),
                                                     copper_user_pal.empty() ? nullptr : &copper_user_pal,
                                                     options.reserve_color0, copper_locks,
                                                     options.palette_diversity);
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
        result.aga = is_aga;
        result.scanline_palettes = std::move(copper_result->scanline_palettes);
        result.scanline_changes = std::move(copper_result->scanline_changes);
        result.copper_num_colors = copper_result->num_colors;
        result.changes_per_line = copper_result->changes_per_line;
        result.max_moves_per_line = copper_result->max_moves_per_line;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        // Force transparent pixels to black in rendered preview
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) result.rendered.pixels()[i] = Color3f{0, 0, 0};
        }
        result.copper_changes = copper_result->avg_changes_per_line;
        result.quant_error = copper_result->total_error;
        result.psnr = color_space::compute_psnr_blurred(
            image->pixels(), result.rendered.pixels(),
            image->width(), image->height());
        return result;
    }

    // --- Standard bitplane modes ---

    // Force transparent pixels to black before quantization/encoding
    if (has_transparency) {
        for (std::size_t i = 0; i < tmask.size(); ++i)
            if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
    }

    auto max_colors = std::size_t{1} << depth;

    // Build palette.
    // Atari mono: fixed B/W palette (no quantization needed).
    // Amiga: always reserve index 0 for black (border/background color).
    // Atari: use full palette (no border register tied to index 0).
    // Transparency: also reserves index 0 for transparent (black).
    auto is_atari = amiga::is_atari(mode);
    bool user_pal = has_user_palette(options);
    // With custom palette: use as-is, no forced black at index 0.
    // Without: reserve index 0 for black (Amiga border/background),
    // unless the user explicitly disabled it.
    auto reserve_zero = !user_pal && options.reserve_color0 &&
                        (has_transparency || !is_atari);

    // Validate locks/pins (no-op for HAM/copper paths above which return earlier).
    // Locks override the implicit reserve-zero rule when index 0 is locked.
    if (auto v = palette_locks::validate_locks(options.locks, max_colors); !v)
        return std::unexpected{v.error()};
    if (auto v = palette_locks::validate_pins(options.pins, options.locks,
                                              max_colors,
                                              image->width(), image->height(),
                                              reserve_zero); !v)
        return std::unexpected{v.error()};

    Palette pal;
    std::vector<bool> locked_mask(max_colors, false);
    if (amiga::is_atari_hi(mode)) {
        pal.colors = {Color3f{1.0f, 1.0f, 1.0f}, Color3f{0.0f, 0.0f, 0.0f}};
    } else if (user_pal) {
        auto loaded = load_user_palette(options);
        if (!loaded) return std::unexpected{loaded.error()};
        pal = *std::move(loaded);
        if (pal.colors.size() > max_colors)
            pal.colors.resize(max_colors);
        snap_to_chipset(pal, chipset, mode);
        // Apply locks on top of user palette: overwrite specified slots.
        for (auto& lock : options.locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            if (idx < pal.colors.size()) {
                pal.colors[idx] = palette_locks::to_color(lock, chipset, mode);
                locked_mask[idx] = true;
            }
        }
    } else {
        auto qcount = palette_locks::quant_count(max_colors, options.locks,
                                                 reserve_zero);
        auto quantized = quantize::quantize(*image, qcount,
                                            quantize_algo(chipset, mode),
                                            options.palette_diversity);
        if (!quantized) return std::unexpected{quantized.error()};
        if (amiga::is_stf(mode)) snap_to_chipset(*quantized, chipset, mode);
        auto assembled = palette_locks::assemble_locked_palette(
            *quantized, options.locks, max_colors, reserve_zero, chipset, mode);
        pal = std::move(assembled.palette);
        locked_mask = std::move(assembled.locked);
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

    // Dither-aware palette refinement (auto-palette only, 4 iterations)
    if (!has_user_palette(options) && dith.method != dither::Method::none) {
        auto refined = quantize::refine_with_dither(
            *image,
            Palette{"refined", {pal.colors.begin(),
                                pal.colors.begin() + static_cast<std::ptrdiff_t>(pal_size)}},
            dith, chipset, mode, 4, locked_mask);
        if (refined) {
            pal.colors = std::move(refined->colors);
            pal_size = pal.colors.size();
        }
    }

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

    // Apply pin-index swaps (post-quantization, post-dither).
    if (!options.pins.empty()) {
        auto pin_result = palette_locks::apply_pins(
            pal, dither_result.indices, locked_mask, options.pins,
            image->width(), image->height());
        if (!pin_result) return std::unexpected{pin_result.error()};
        // pal_span/pal_size still valid (palette length unchanged).
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
    result.indices = std::move(dither_result.indices);
    result.mode = mode;
    result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
    result.interlace = options.interlace;
    result.has_transparency = has_transparency;
    result.transparency_mask = tmask;
    if (has_transparency) {
        for (std::size_t i = 0; i < tmask.size(); ++i)
            if (tmask[i]) result.rendered.pixels()[i] = Color3f{0, 0, 0};
    }
    result.quant_error = dither_result.total_error;
    result.psnr = color_space::compute_psnr_blurred(
        image->pixels(), result.rendered.pixels(),
        image->width(), image->height());
    return result;
}

ConvertResult make_error(const std::string& msg) {
    ConvertResult r;
    r.error = msg;
    return r;
}

int count_unique_colors(const Image& img) {
    std::unordered_set<std::uint32_t> seen;
    for (std::size_t y = 0; y < img.height(); ++y)
        for (std::size_t x = 0; x < img.width(); ++x) {
            auto srgb = color_space::linear_to_srgb(img[x, y]).clamped();
            auto r = static_cast<std::uint32_t>(srgb.r * 255.0f + 0.5f);
            auto g = static_cast<std::uint32_t>(srgb.g * 255.0f + 0.5f);
            auto b = static_cast<std::uint32_t>(srgb.b * 255.0f + 0.5f);
            seen.insert((r << 16) | (g << 8) | b);
        }
    return static_cast<int>(seen.size());
}

ConvertResult make_result(std::vector<std::uint8_t> data, const PipelineResult& p) {
    ConvertResult r;
    r.data = std::move(data);
    r.width = static_cast<int>(p.rendered.width());
    r.height = static_cast<int>(p.rendered.height());
    r.depth = static_cast<int>(p.planes.depth);
    r.colors = static_cast<int>(p.palette.size());
    r.copperChanges = p.copper_changes;
    r.totalColors = count_unique_colors(p.rendered);
    r.planeBytes = static_cast<int>(p.planes.total_bytes());
    r.aga = p.aga;
    if (p.copper && !p.scanline_changes.empty()) {
        auto h = p.rendered.height();
        auto cpl = p.changes_per_line;
        // .raw output writes a fixed [h][cpl] grid with sentinels (4 B/entry)
        auto cop_data_bytes = h * cpl * 4;  // reg(2) + color(2) per change
        if (p.aga) cop_data_bytes *= 2;     // hi + lo per change
        r.copperBytes = static_cast<int>(cop_data_bytes);
        r.changesPerLine = static_cast<int>(cpl);
        r.maxMovesPerLine = static_cast<int>(p.max_moves_per_line);
    }
    r.quantError = p.quant_error;
    r.psnr = p.psnr;
    r.hasTransparency = p.has_transparency;
    return r;
}

} // namespace

ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    // Use palettized PNG for modes with a single global palette
    // (much smaller file). Falls back to RGB for HAM/copper which have
    // dynamic palettes per pixel/scanline.
    Result<std::vector<std::uint8_t>> png;
    if (!result->indices.empty() && !result->palette.empty()) {
        int trans = result->has_transparency ? 0 : -1;
        png = png_io::encode_palettized(
            result->indices, result->palette,
            result->rendered.width(), result->rendered.height(), trans);
    } else {
        png = png_io::encode(result->rendered);
    }
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
    ch_opts.aga = (resolve_chipset(options.chipset, result->mode) == amiga::Chipset::aga);
    ch_opts.fade_in = true;  // always enable fade-in for web/compile exports
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

    auto chipset = resolve_chipset(options.chipset, result->mode);
    bool aga = (chipset == amiga::Chipset::aga);

    // Raw format: bitplanes + palette + copper data (all big-endian)
    std::vector<std::uint8_t> raw = std::move(result->planes.data);

    // Append palette (2 bytes/color, 0x0RGB)
    for (auto& c : result->palette) {
        auto rgb12 = aga ? palette::linear_to_aga_hilo(c).hi
                         : palette::linear_to_ocs(c);
        raw.push_back(static_cast<std::uint8_t>(rgb12 >> 8));
        raw.push_back(static_cast<std::uint8_t>(rgb12 & 0xFF));
    }
    // AGA: append lo nibble palette
    if (aga) {
        for (auto& c : result->palette) {
            auto lo = palette::linear_to_aga_hilo(c).lo;
            raw.push_back(static_cast<std::uint8_t>(lo >> 8));
            raw.push_back(static_cast<std::uint8_t>(lo & 0xFF));
        }
    }

    // Append copper changes (reg:UWORD + color:UWORD per entry, per scanline)
    if (result->copper && !result->scanline_changes.empty()) {
        auto cpl = result->changes_per_line;
        for (auto& line : result->scanline_changes) {
            for (std::size_t s = 0; s < cpl; ++s) {
                // Slot is "active" if it has a real change AND (for AGA) the
                // hi pass isn't nibble-skipped. Inactive slots emit the
                // 0xFFFF sentinel — same convention as end-of-line padding.
                // A consumer that walks the table and skips 0xFFFF entries
                // honors the encoder's MOVE budget; without this, naive
                // consumers could exceed MOVE_BUDGET_PER_LINE on some lines.
                bool active = s < line.size() &&
                              !(aga && line[s].skip_hi);
                if (active) {
                    auto hi = aga ? palette::linear_to_aga_hilo(line[s].color).hi
                                  : palette::linear_to_ocs(line[s].color);
                    raw.push_back(0);
                    raw.push_back(line[s].reg);
                    raw.push_back(static_cast<std::uint8_t>(hi >> 8));
                    raw.push_back(static_cast<std::uint8_t>(hi & 0xFF));
                } else {
                    // Sentinel: unused or nibble-skipped slot
                    raw.push_back(0xFF);
                    raw.push_back(0xFF);
                    raw.push_back(0x00);
                    raw.push_back(0x00);
                }
            }
        }
        // AGA: append lo nibble copper changes
        if (aga) {
            for (auto& line : result->scanline_changes) {
                for (std::size_t s = 0; s < cpl; ++s) {
                    if (s < line.size()) {
                        auto lo = palette::linear_to_aga_hilo(line[s].color).lo;
                        raw.push_back(0);
                        raw.push_back(line[s].reg);
                        raw.push_back(static_cast<std::uint8_t>(lo >> 8));
                        raw.push_back(static_cast<std::uint8_t>(lo & 0xFF));
                    } else {
                        raw.push_back(0xFF);
                        raw.push_back(0xFF);
                        raw.push_back(0x00);
                        raw.push_back(0x00);
                    }
                }
            }
        }
    }

    return make_result(std::move(raw), *result);
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

// ---------------------------------------------------------------------------
// Mask export helpers
// ---------------------------------------------------------------------------

namespace {

// Build a 1-bit B/W Image from the transparency mask.
// Default: white (1.0) = opaque pixel, black (0.0) = transparent pixel.
// With mask_invert: inverted.
Image mask_to_image(const std::vector<bool>& tmask,
                    std::size_t w, std::size_t h, bool invert) {
    Image img(w, h);
    for (std::size_t i = 0; i < w * h; ++i) {
        bool transparent = (i < tmask.size()) && tmask[i];
        bool white = invert ? transparent : !transparent;
        float v = white ? 1.0f : 0.0f;
        img.pixels()[i] = Color3f{v, v, v};
    }
    return img;
}

// Build pixel indices for 1-bitplane encoding from mask.
// Index 1 = white (opaque), index 0 = black (transparent), or inverted.
std::vector<std::uint8_t> mask_to_indices(const std::vector<bool>& tmask,
                                           std::size_t w, std::size_t h,
                                           bool invert) {
    std::vector<std::uint8_t> indices(w * h, 0);
    for (std::size_t i = 0; i < w * h; ++i) {
        bool transparent = (i < tmask.size()) && tmask[i];
        bool set = invert ? transparent : !transparent;
        indices[i] = set ? 1 : 0;
    }
    return indices;
}

ConvertResult make_mask_result(std::vector<std::uint8_t> data,
                               const PipelineResult& p) {
    ConvertResult r;
    r.data = std::move(data);
    r.width = static_cast<int>(p.rendered.width());
    r.height = static_cast<int>(p.rendered.height());
    r.depth = 1;
    r.colors = 2;
    r.hasTransparency = p.has_transparency;
    return r;
}

} // namespace

ConvertResult convert_mask(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    if (!result->has_transparency) {
        ConvertResult r;
        r.width = static_cast<int>(result->rendered.width());
        r.height = static_cast<int>(result->rendered.height());
        r.hasTransparency = false;
        r.error = "No transparency in source image";
        return r;
    }

    auto mask_img = mask_to_image(result->transparency_mask,
                                  result->rendered.width(),
                                  result->rendered.height(),
                                  options.mask_invert);

    auto png = png_io::encode(mask_img);
    if (!png) return make_error(png.error().message);

    return make_mask_result(*std::move(png), *result);
}

ConvertResult convert_mask_raw(const std::uint8_t* input_data,
                               std::size_t input_size,
                               const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    if (!result->has_transparency) {
        ConvertResult r;
        r.width = static_cast<int>(result->rendered.width());
        r.height = static_cast<int>(result->rendered.height());
        r.hasTransparency = false;
        r.error = "No transparency in source image";
        return r;
    }

    auto indices = mask_to_indices(result->transparency_mask,
                                   result->rendered.width(),
                                   result->rendered.height(),
                                   options.mask_invert);

    auto planes = bitplane::encode(indices,
                                   result->rendered.width(),
                                   result->rendered.height(),
                                   1); // 1 bitplane
    if (!planes) return make_error(planes.error().message);

    return make_mask_result(std::move(planes->data), *result);
}

ConvertResult convert_mask_iff(const std::uint8_t* input_data,
                               std::size_t input_size,
                               const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    if (!result->has_transparency) {
        ConvertResult r;
        r.width = static_cast<int>(result->rendered.width());
        r.height = static_cast<int>(result->rendered.height());
        r.hasTransparency = false;
        r.error = "No transparency in source image";
        return r;
    }

    auto indices = mask_to_indices(result->transparency_mask,
                                   result->rendered.width(),
                                   result->rendered.height(),
                                   options.mask_invert);

    auto planes = bitplane::encode(indices,
                                   result->rendered.width(),
                                   result->rendered.height(),
                                   1);
    if (!planes) return make_error(planes.error().message);

    // B/W palette: index 0 = black, index 1 = white
    std::vector<Color3f> mask_palette = {
        Color3f{0.0f, 0.0f, 0.0f},
        Color3f{1.0f, 1.0f, 1.0f},
    };

    iff::IffOptions iff_opts;
    iff_opts.interlace = result->interlace;

    auto iff_data = iff::write_ilbm(*planes, mask_palette,
                                    amiga::Mode::lores, iff_opts);
    if (!iff_data) return make_error(iff_data.error().message);

    return make_mask_result(*std::move(iff_data), *result);
}

} // namespace png2amiga::api
