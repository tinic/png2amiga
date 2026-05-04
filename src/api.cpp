#include "api.hpp"
#include "amiga.hpp"
#include "bitplane.hpp"
#include "cga_text.hpp"
#include "cheader.hpp"
#include "cheader_dos_c.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "degas.hpp"
#include "dither.hpp"
#include "dither_tuning.hpp"
#include "ham.hpp"
#include "strips.hpp"
#include "iff.hpp"
#include "console_color.hpp"
#include "cheader_genesis.hpp"
#include "genesis.hpp"
#include "c64.hpp"
#include "c64_prg.hpp"
#include "snes_io.hpp"
#include "palette.hpp"
#include "palette_io.hpp"
#include "palette_locks.hpp"
#include "pipeline.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"

#include <stb_image.h>
#include <webp/decode.h>

#include <cstring>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
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
    // IBM PC planar DOS modes. Web UI exposes only the 4-bitplane planar
    // variants (no chunky VGA 13h / Mode X / Mode Y, no CGA composite, no
    // glyph-matched text modes) — those require dispatch paths that aren't
    // wired through api.cpp yet.
    if (s == "ega-320") return amiga::Mode::ega_320;
    if (s == "ega-640") return amiga::Mode::ega_640;
    if (s == "ega-hi")  return amiga::Mode::ega_hi;
    if (s == "vga-10h") return amiga::Mode::vga_10h;
    if (s == "vga-12h") return amiga::Mode::vga_12h;
    if (s == "vga-13h")   return amiga::Mode::vga_13h;
    if (s == "cga-320") return amiga::Mode::cga_320;
    if (s == "cga-640") return amiga::Mode::cga_640;
    if (s == "cga-composite")   return amiga::Mode::cga_composite;
    if (s == "cga-text80x100")  return amiga::Mode::cga_text80x100;
    if (s == "snes-mode7-256")    return amiga::Mode::snes_mode7_256;
    if (s == "snes-mode7-direct") return amiga::Mode::snes_mode7_direct;
    if (s == "genesis-h32")       return amiga::Mode::genesis_h32;
    if (s == "genesis-h40")       return amiga::Mode::genesis_h40;
    if (s == "genesis-h32-sh")    return amiga::Mode::genesis_h32_sh;
    if (s == "genesis-h40-sh")    return amiga::Mode::genesis_h40_sh;
    if (s == "c64-hires")         return amiga::Mode::c64_hires;
    if (s == "c64-multicolor")    return amiga::Mode::c64_multicolor;
    if (s == "c64-fli")           return amiga::Mode::c64_fli;
    if (s == "c64-afli")          return amiga::Mode::c64_afli;
    if (s == "c64-petscii")       return amiga::Mode::c64_petscii;
    if (s == "c64-charset-hires") return amiga::Mode::c64_charset_hires;
    if (s == "c64-charset-multicolor")
        return amiga::Mode::c64_charset_multicolor;
    return amiga::Mode::lores;
}

using pipeline::resolve_chipset;

quantize::Algorithm quantize_algo(amiga::Chipset chipset, amiga::Mode mode = amiga::Mode::lores) {
    // STF uses brute-force over 512 colors (same algorithm, different precision)
    // STE 12-bit = OCS 12-bit → same brute-force
    if (amiga::is_atari(mode)) return quantize::Algorithm::ocs_bruteforce;
    // DOS modes: median-cut in continuous RGB, then snap to the target gamut
    // (EGA-64, VGA 18-bit, or fixed CGA palette). EGA gets a dedicated
    // histogram path in run_pipeline; the median-cut choice here is a
    // fallback and matches how the CLI path in main.cpp handles VGA.
    if (amiga::is_ega(mode) || amiga::is_vga(mode) || amiga::is_cga(mode))
        return quantize::Algorithm::median_cut;
    return chipset == amiga::Chipset::aga
        ? quantize::Algorithm::median_cut
        : quantize::Algorithm::ocs_bruteforce;
}

void snap_to_chipset(Palette& pal, amiga::Chipset chipset, amiga::Mode mode = amiga::Mode::lores) {
    if (amiga::is_stf(mode)) {
        for (auto& c : pal.colors) c = palette::quantize_to_stf(c);
    } else if (amiga::is_ega(mode)) {
        for (auto& c : pal.colors) c = palette::quantize_to_ega(c);
    } else if (amiga::is_vga(mode)) {
        for (auto& c : pal.colors) c = palette::quantize_to_vga(c);
    } else if (chipset != amiga::Chipset::aga) {
        for (auto& c : pal.colors) c = palette::quantize_to_ocs(c);
    }
}

dither::Method parse_dither(const std::string& s) {
    if (s == "none") return dither::Method::none;
    if (s == "bayer2x2") return dither::Method::bayer2x2;
    if (s == "bayer4x4") return dither::Method::bayer4x4;
    if (s == "bayer8x8") return dither::Method::bayer8x8;
    if (s == "bayer3x3") return dither::Method::bayer3x3;
    if (s == "bayer5x5") return dither::Method::bayer5x5;
    if (s == "bayer6x6") return dither::Method::bayer6x6;
    if (s == "bayer7x7") return dither::Method::bayer7x7;
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
    if (s == "void-cluster") return dither::Method::void_cluster;
    if (s == "cluster-noise") return dither::Method::cluster_noise;
    if (s == "fractal16") return dither::Method::fractal16;
    if (s == "floyd-steinberg") return dither::Method::floyd_steinberg;
    if (s == "atkinson") return dither::Method::atkinson;
    if (s == "sierra-lite") return dither::Method::sierra_lite;
    if (s == "stucki") return dither::Method::stucki;
    if (s == "jarvis") return dither::Method::jarvis;
    if (s == "dbs") return dither::Method::dbs;
    if (s == "gilbert") return dither::Method::gilbert;
    if (s == "riemersma") return dither::Method::riemersma;
    if (s == "structure-fs") return dither::Method::structure_fs;
    if (s == "contrast-fs") return dither::Method::contrast_fs;
    if (s == "zhoufang") return dither::Method::zhoufang;
    if (s == "yliluoma") return dither::Method::yliluoma;
    if (s == "yliluoma2") return dither::Method::yliluoma2;
    if (s == "opt-checker") return dither::Method::opt_checker;
    if (s == "knoll") return dither::Method::knoll;
    if (s == "tri-tone") return dither::Method::tri_tone;
    if (s == "yliluoma1") return dither::Method::yliluoma1;
    if (s == "opt-line") return dither::Method::opt_line;
    if (s == "opt-line-checker") return dither::Method::opt_line_checker;
    if (s == "aseprite-old") return dither::Method::aseprite_old;
    if (s == "libcaca3") return dither::Method::libcaca_3x3;
    if (s == "libcaca6") return dither::Method::libcaca_6x6;
    if (s == "pegasus") return dither::Method::pegasus_8x8;
    if (s == "cranley-bayer") return dither::Method::cranley_bayer;
    if (s == "quasicrystal") return dither::Method::quasicrystal;
    if (s == "truchet") return dither::Method::truchet;
    if (s == "ign") return dither::Method::ign;
    if (s == "ign-tri") return dither::Method::ign_triangle;
    if (s == "white-noise") return dither::Method::white_noise;
    if (s == "r2") return dither::Method::r2_sequence;
    if (s == "r2-tri") return dither::Method::r2_triangle;
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
    // 32-bit (WASM) or exhaust memory under 64-bit. The per-axis cap is
    // relaxed enough for batch-mode atlases (N frames stitched
    // horizontally → very wide but short); the pixel-count cap protects
    // against true memory blowups.
    constexpr int kMaxDimension = 131072;
    constexpr long long kMaxPixels = 256LL * 1024 * 1024;  // 256 Mpx → 1 GiB RGBA
    if (w <= 0 || h <= 0 || w > kMaxDimension || h > kMaxDimension ||
        static_cast<long long>(w) * h > kMaxPixels) {
        free_raw();
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Image dimensions out of range: {}x{} "
                        "(per-axis max {}, total max {} pixels)",
                        w, h, kMaxDimension, kMaxPixels),
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
        if (cx + cw > width || cy + ch > height) {
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
                // Fully transparent (alpha == 0) and fully opaque
                // (alpha == 1) pixels are locked regardless of
                // cutoff/dither — dither should only ever shape the
                // partial-alpha boundary, never punch holes into pixels
                // the artist explicitly marked visible or invisible.
                if (a == 0.0f) {
                    (*out_tmask)[y * target_w + x] = true;
                } else if (a == 1.0f) {
                    (*out_tmask)[y * target_w + x] = false;
                } else if (alpha_dither != dither::Method::none) {
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
            auto scaled = scale::resample(image, target_w, target_h);
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

using pipeline::PipelineResult;

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
    // Use the authoritative float PAR from ModeParams. The integer
    // preview_scale_x/y fields are display-only upscaling factors that
    // approximate PAR as a ratio of small integers (1:1, 1:2, 2:1). For
    // DOS modes the real PAR is non-integer (e.g. EGA 640×200 = 0.417,
    // VGA 13h = 0.833, EGA-hi = 0.73) — the integer approximation was
    // off by up to 20% and produced wrong letterbox heights.
    auto par = static_cast<double>(params.par);
    bool interlace = options.interlace || params.is_interlaced;
    if (options.interlace && !params.is_interlaced) par *= 2.0;

    bool have_w = options.width > 0;
    bool have_h = options.height > 0;

    // Fixed-buffer modes have hardware-locked screen dimensions; if the
    // caller didn't ask for native-par, ALWAYS fit to mode_w × screen_h
    // regardless of any user-supplied width/height. (Otherwise the
    // encoder rejects mismatched input — c64::encode_multicolor wants
    // exactly 160×200, etc.)
    //
    // Exception: c64 charset modes (hires + multicolor) accept arbitrary
    // dimensions, padded up to the next multiple of cell size. Lets
    // demos use very large source bitmaps with shared charsets across
    // many screens. When the user passes --width / --height the
    // encoder receives those (rounded up to 8×8 / 4×8); otherwise the
    // fixed-buf default still applies.
    bool is_fixed_buf =
        amiga::is_atari(mode) || amiga::is_vga(mode) || amiga::is_ega(mode) ||
        amiga::is_cga(mode)   || amiga::is_cga_text(mode) ||
        amiga::is_snes(mode)  || amiga::is_genesis(mode) || amiga::is_c64(mode);
    // Tile-based platforms with freeform sizing — Genesis (8×8 cells)
    // and SNES Mode 7 (8×8 cells) use the same 1:1 source-pixel
    // convention as c64-charset-hires. No multicolor halving.
    bool tile_8x8_freeform =
        (amiga::is_genesis(mode) || amiga::is_snes(mode))
        && (have_w || have_h);
    if (tile_8x8_freeform) {
        constexpr std::size_t kTileSide = 8;
        auto round_up = [](std::size_t v, std::size_t s) {
            return ((v + s - 1) / s) * s;
        };
        std::size_t tw = have_w
            ? round_up(static_cast<std::size_t>(options.width), kTileSide)
            : 0;
        std::size_t th = have_h
            ? round_up(static_cast<std::size_t>(options.height), kTileSide)
            : 0;
        if (!have_w) {
            auto wf = static_cast<double>(th) * src_aspect;
            tw = round_up(static_cast<std::size_t>(std::lround(wf)), kTileSide);
        }
        if (!have_h) {
            auto hf = static_cast<double>(tw) / src_aspect;
            th = round_up(static_cast<std::size_t>(std::lround(hf)), kTileSide);
        }
        return {tw, th};
    }

    // cga-text80x100 freeform: --width / --height treats source as
    // square-pixel and the cell grid is 8 × cell_h_src (cell_h_src=2 for
    // canonical 200-line buffer, =4 for any other size). Preserve source
    // aspect (target_h = target_w / src_aspect) — same rule the CLI's
    // main.cpp applies. Pad up to 8×4 alignment so the encoder accepts.
    bool cga_text_freeform =
        amiga::is_cga_text(mode) && (have_w || have_h);
    if (cga_text_freeform) {
        constexpr std::size_t kCellW = 8;
        constexpr std::size_t kCellH = 4;
        auto round_up = [](std::size_t v, std::size_t s) {
            return ((v + s - 1) / s) * s;
        };
        std::size_t tw = have_w
            ? static_cast<std::size_t>(options.width)
            : 0;
        std::size_t th = have_h
            ? static_cast<std::size_t>(options.height)
            : 0;
        if (!have_w) {
            tw = static_cast<std::size_t>(std::lround(
                static_cast<double>(th) * src_aspect));
        }
        if (!have_h) {
            th = static_cast<std::size_t>(std::lround(
                static_cast<double>(tw) / src_aspect));
        }
        if (tw == 0) tw = kCellW;
        if (th == 0) th = kCellH;
        return {round_up(tw, kCellW), round_up(th, kCellH)};
    }

    bool charset_freeform =
        amiga::is_c64_charset(mode) && (have_w || have_h);
    if (charset_freeform) {
        // Designer convention: --width / --height refer to *source*
        // pixels, treated 1:1. For charset-hires every 8×8 source block
        // becomes one 8×8 cell, so the encoder receives the user dims
        // verbatim. For charset-multicolor every 8×8 source block
        // becomes one 4×8 multicolor cell — round source dims up to
        // multiples of 8, then halve the width when handing the buffer
        // to the encoder (multicolor logical pixels are 2× wider).
        bool mc = (mode == amiga::Mode::c64_charset_multicolor);
        constexpr std::size_t kSrcStep = 8;
        auto round_up = [](std::size_t v, std::size_t s) {
            return ((v + s - 1) / s) * s;
        };
        std::size_t sw = have_w
            ? round_up(static_cast<std::size_t>(options.width), kSrcStep)
            : 0;
        std::size_t sh = have_h
            ? round_up(static_cast<std::size_t>(options.height), kSrcStep)
            : 0;
        if (!have_w) {
            auto wf = static_cast<double>(sh) * src_aspect;
            sw = round_up(static_cast<std::size_t>(std::lround(wf)), kSrcStep);
        }
        if (!have_h) {
            auto hf = static_cast<double>(sw) / src_aspect;
            sh = round_up(static_cast<std::size_t>(std::lround(hf)), kSrcStep);
        }
        return {mc ? (sw / 2) : sw, sh};
    }
    if (is_fixed_buf && !options.native_par && params.screen_height > 0) {
        return {mode_w, params.screen_height};
    }

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
    // For Amiga hires, always use the full mode width.
    if (params.is_hires) w = mode_w;
    auto h = round_height(static_cast<double>(w) * par / src_aspect, interlace);
    // Fixed-buffer modes (Atari, VGA, EGA, CGA): by default stretch-fit the
    // source to the full hardware buffer. With --native-par, preserve source
    // aspect by reducing target_h (letterbox) or target_w (pillarbox).
    auto mode_h = params.screen_height;
    if (mode_h > 0) {
        bool is_fixed_buffer = amiga::is_atari(mode) || amiga::is_vga(mode) ||
                               amiga::is_ega(mode)   || amiga::is_cga(mode) ||
                               amiga::is_cga_text(mode) || amiga::is_snes(mode) ||
                               amiga::is_genesis(mode) || amiga::is_c64(mode);
        if (is_fixed_buffer && !options.native_par) {
            h = mode_h;  // stretch to fill
        } else if (h > mode_h) {
            h = mode_h;
            w = static_cast<std::size_t>(std::lround(
                static_cast<double>(h) * src_aspect / par));
            if (w > params.screen_width) w = params.screen_width;
        }
    }
    return {w, h};
}

// Decompose compound mode strings (e.g. "ham6-hires-lace") into base mode
// + width/interlace overrides. Mutates a local copy of options.
Options decompose_mode_options(const Options& opts) {
    auto o = opts;
    auto& s = o.mode;
    // The "hires" / "lace" suffixes are Amiga compound-mode markers
    // (ham6-hires, lores-lace, etc.). Other chipsets use mode strings
    // that may *contain* the substring (c64-hires, vga-13h… though
    // none of those collide today besides c64). Anchor by chipset
    // prefix so non-Amiga modes don't get accidentally rewritten.
    bool is_amiga_compound = s.starts_with("ham") || s.starts_with("lores") ||
                              s.starts_with("hires") || s.starts_with("ehb");
    bool has_hires = is_amiga_compound &&
                     s.find("hires") != std::string::npos;
    bool has_lace = is_amiga_compound &&
                    s.size() > 4 && s.find("-lace") != std::string::npos;
    // Only override if user didn't already set these
    if (has_hires && o.width == 0) o.width = 640;
    if (has_lace) o.interlace = true;
    return o;
}

// --- Seamless-tile helpers ----------------------------------------------
//
// `--tile`: replicate a W x H image into a 3W x 3H grid of identical
// copies, run the full pipeline (quantize / dither / encode) over the
// big buffer, then crop the centre W x H back out for export. Running
// error diffusion across the source-period-3W width lets the dither
// converge to a W-periodic pattern; the centre tile inherits that
// pattern, and so its right edge dither matches its left edge dither
// when the user actually tiles it. The outer 8 cells are scratch and
// get discarded.
//
// Allowed only on freeform indexed bitmap modes (lores / hires / EHB
// ± interlace ± dpf). Rejected for HAM, sliced palette, strips, and
// every fixed-size or tile-coded mode (Atari, VGA, EGA, CGA, SNES
// Mode 7, Genesis, C64). The gate lives at the top of run_pipeline.

static Image tile_replicate_3x3(const Image& src) {
    auto w = src.width();
    auto h = src.height();
    Image out(w * 3, h * 3);
    for (std::size_t ty = 0; ty < 3; ++ty) {
        for (std::size_t tx = 0; tx < 3; ++tx) {
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    out[tx * w + x, ty * h + y] = src[x, y];
                }
            }
        }
    }
    return out;
}

static std::vector<bool> tile_replicate_mask_3x3(const std::vector<bool>& src,
                                                  std::size_t w, std::size_t h) {
    if (src.empty()) return {};
    std::vector<bool> out(w * 3 * h * 3);
    for (std::size_t ty = 0; ty < 3; ++ty) {
        for (std::size_t tx = 0; tx < 3; ++tx) {
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    out[(ty * h + y) * (w * 3) + (tx * w + x)] = src[y * w + x];
                }
            }
        }
    }
    return out;
}

static Image tile_crop_centre_image(const Image& tiled,
                                     std::size_t w, std::size_t h) {
    Image out(w, h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            out[x, y] = tiled[x + w, y + h];
        }
    }
    return out;
}

static std::vector<std::uint8_t> tile_crop_centre_indices(
        const std::vector<std::uint8_t>& tiled,
        std::size_t w, std::size_t h) {
    std::vector<std::uint8_t> out(w * h);
    auto tw = w * 3;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            out[y * w + x] = tiled[(y + h) * tw + (x + w)];
        }
    }
    return out;
}

static std::vector<bool> tile_crop_centre_mask(const std::vector<bool>& tiled,
                                                std::size_t w, std::size_t h) {
    if (tiled.empty()) return {};
    std::vector<bool> out(w * h);
    auto tw = w * 3;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            out[y * w + x] = tiled[(y + h) * tw + (x + w)];
        }
    }
    return out;
}

// Apply the centre crop to a PipelineResult that was encoded from a
// 3W x 3H replicate. Re-encodes bitplanes from the cropped indices so
// the .iff/.h/.cpp output paths see the right dimensions and bitplane
// data without any extra knowledge of the tile mode.
static Result<void> tile_crop_result(PipelineResult& r,
                                      std::size_t w, std::size_t h,
                                      std::size_t depth,
                                      bitplane::Layout layout) {
    r.indices = tile_crop_centre_indices(r.indices, w, h);
    r.rendered = tile_crop_centre_image(r.rendered, w, h);
    if (!r.transparency_mask.empty())
        r.transparency_mask = tile_crop_centre_mask(r.transparency_mask, w, h);
    auto planes = bitplane::encode(r.indices, w, h, depth, layout);
    if (!planes) return std::unexpected{planes.error()};
    r.planes = *std::move(planes);
    return {};
}

}  // close anon namespace so run_pipeline gets external linkage and can
   // be reached from pipeline.cpp via the api:: forwarder.

Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const Options& orig_options) {
    auto options = decompose_mode_options(orig_options);
    auto mode = parse_mode(options.mode);
    bool compound_hires =
        (orig_options.mode.starts_with("ham") ||
         orig_options.mode.starts_with("lores") ||
         orig_options.mode.starts_with("hires") ||
         orig_options.mode.starts_with("ehb")) &&
        orig_options.mode.find("hires") != std::string::npos;

    // Reject dither methods that don't apply to the chosen mode rather
    // than silently fall through to a degraded encode. HAM has no
    // discrete palette per pixel — the encoder picks SET/MODIFY ops
    // dynamically — so palette-pair / palette-index methods (yliluoma
    // family, dbs) silently degenerate. SNES Mode 7
    // Direct has no palette table at all, same problem for the
    // yliluoma family. Web frontend has matching auto-fallback gates
    // (see Converter.vue HAM_INCOMPATIBLE_DITHERS); CLI errors out
    // because scripted callers picked the dither for a reason.
    if (auto dm = parse_dither(options.dither);
        dither::needs_discrete_palette(dm)) {
        if (amiga::is_ham(mode)) {
            return std::unexpected{Error{
                ErrorCode::unsupported_mode,
                "Dither '" + options.dither + "' needs a discrete palette "
                "and silently degenerates in HAM modes (no per-pixel "
                "palette index — encoder picks SET/MODIFY ops). Use "
                "atkinson, floyd-steinberg, sierra-lite, jarvis, "
                "stucki, or an ordered method.",
            }};
        }
        if (amiga::is_snes_direct(mode) && dither::is_yliluoma(dm)) {
            return std::unexpected{Error{
                ErrorCode::unsupported_mode,
                "Dither '" + options.dither + "' is palette-aware but "
                "Mode 7 Direct has no palette table. Use a non-yliluoma "
                "method (atkinson, floyd-steinberg, ordered).",
            }};
        }
    }

    // --reserve-range gating. Modes where reserves can't yet be honoured
    // (HAM dynamic palette, copper per-line palettes, dual-playfield split,
    // multi-palette tile modes, fixed hardware palettes) reject early with
    // a clear message rather than silently dropping the spec.
    if (!options.reserves.empty()) {
        auto reject = [&](std::string_view why) -> Result<PipelineResult> {
            return std::unexpected{Error{
                ErrorCode::unsupported_mode,
                std::string("--reserve-range: ") + std::string(why),
            }};
        };
        if (amiga::is_ham(mode))
            return reject("not supported in HAM modes (palette is dynamic — "
                          "the modify ops produce arbitrary colours, no "
                          "fixed slot to reserve)");
        if (options.dual_playfield)
            return reject("not supported with --dpf (two split palettes; "
                          "specify which playfield is needed)");
        if (amiga::is_genesis(mode))
            return reject("not supported in Genesis modes (4 separate "
                          "16-colour palette lines; reserve target ambiguous)");
        if (amiga::is_snes(mode))
            return reject("not supported in SNES Mode 7 yet "
                          "(encoder is opaque to the reserve overlay)");
        if (amiga::is_c64(mode))
            return reject("not supported in C64 modes (VIC-II palette is "
                          "fixed in hardware; --lock-color0 covers the "
                          "common 'pin background' use-case)");
        if (amiga::is_cga(mode) || amiga::is_cga_text(mode))
            return reject("not supported in CGA modes (palette is "
                          "hardware-fixed)");
    }

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
    // DOS + SNES + Genesis modes: depth also fixed by the hardware buffer.
    if (amiga::is_vga(mode) || amiga::is_ega(mode) || amiga::is_cga(mode) ||
        amiga::is_snes(mode) || amiga::is_genesis(mode))
        depth = amiga::get_mode_params(mode).bitplane_depth;

    // Dual-playfield: encode the image into PF2 only with a constrained
    // sub-depth (3 OCS / 4 AGA). The remaining planes (PF1 = foreground)
    // are zeroed and emitted as part of the final 6 / 8-plane output.
    bool use_dpf = options.dual_playfield &&
                   !amiga::is_ham(mode) && mode != amiga::Mode::ehb &&
                   !amiga::is_atari(mode) && !amiga::is_vga(mode) &&
                   !amiga::is_ega(mode) && !amiga::is_cga(mode);
    if (use_dpf) {
        auto cs = (options.chipset == "aga") ? amiga::Chipset::aga
                                             : amiga::Chipset::ocs;
        depth = (cs == amiga::Chipset::aga) ? 4 : 3;
    }

    // --- Seamless-tile gate ----------------------------------------------
    // Allowed only on freeform indexed bitmap modes (lores / hires / EHB
    // ± interlace ± dpf). Reject HAM, sliced palette (--copper), strips
    // (--scap), and every fixed-size or tile-coded mode here so the
    // downstream branches don't need to know about tile mode.
    bool tile_active = false;
    std::size_t tile_w = 0, tile_h = 0;
    if (options.tile) {
        bool tile_mode_ok =
            mode == amiga::Mode::lores ||
            mode == amiga::Mode::lores_interlace ||
            mode == amiga::Mode::hires ||
            mode == amiga::Mode::hires_interlace ||
            mode == amiga::Mode::ehb;
        if (!tile_mode_ok) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "--tile is only supported on freeform indexed bitmap modes "
                "(lores, hires, EHB; with optional --interlace and --dpf). "
                "HAM, fixed-size, and tile-coded modes are not eligible."}};
        }
        if (amiga::is_ham(mode)) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "--tile is not supported with HAM modes"}};
        }
        if (options.copper || options.scap) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "--tile is not yet supported with sliced palette (--copper) "
                "or strip palette (--scap). Drop those flags or drop --tile."}};
        }
        tile_active = true;
        tile_w = target_w;
        tile_h = target_h;
    }

    std::vector<bool> tmask;
    auto image = load_and_preprocess(input_data, input_size, options,
                                      target_w, target_h, &tmask);
    if (!image) return std::unexpected{image.error()};
    bool has_transparency = !tmask.empty();

    // Tile pre-processing: replicate the loaded image (and the alpha
    // mask, if any) into a 3x3 grid. The rest of the pipeline runs on
    // the 3W x 3H buffer; the centre crop happens at the per-branch
    // return points below.
    if (tile_active) {
        *image = tile_replicate_3x3(*image);
        if (has_transparency)
            tmask = tile_replicate_mask_3x3(tmask, tile_w, tile_h);
    }

    // Fixed-buffer modes (Atari, VGA, EGA, CGA): center the image in the full
    // hardware frame when either dimension is smaller than the buffer.
    // Atari only uses vertical padding in practice; DOS modes with
    // native_par may need pillarbox (horizontal) padding too.
    auto mparams = amiga::get_mode_params(mode);
    auto mode_h = mparams.screen_height;
    auto mode_w_fixed = mparams.screen_width;
    bool is_fixed_buffer = amiga::is_atari(mode) || amiga::is_vga(mode) ||
                           amiga::is_ega(mode)   || amiga::is_cga(mode) ||
                           amiga::is_cga_text(mode) || amiga::is_snes(mode) ||
                               amiga::is_genesis(mode);
    // cga-text accepts arbitrary multiples of 8×2 in freeform (--width
    // / --height set). Don't center-pad freeform input up to the
    // canonical 640×200 buffer — that would silently turn a 200×400
    // target back into 640×200 and the encoder would produce 80×100
    // cells instead of the user-typed 25×100.
    bool cga_text_freeform_skip_pad =
        amiga::is_cga_text(mode) && (options.width > 0 || options.height > 0);
    if (mode_h > 0 && is_fixed_buffer && !cga_text_freeform_skip_pad &&
        (image->height() < mode_h || image->width() < mode_w_fixed)) {
        auto w = image->width();
        auto h = image->height();
        auto fw = std::max(w, mode_w_fixed);
        auto fh = std::max(h, mode_h);
        Image padded(fw, fh);
        auto x_off = (fw - w) / 2;
        auto y_off = (fh - h) / 2;
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x)
                padded[x + x_off, y + y_off] = (*image)[x, y];
        if (has_transparency) {
            std::vector<bool> new_mask(fw * fh, true);
            for (std::size_t y = 0; y < h; ++y)
                for (std::size_t x = 0; x < w; ++x)
                    new_mask[(y + y_off) * fw + (x + x_off)] = tmask[y * w + x];
            tmask = std::move(new_mask);
        }
        *image = std::move(padded);
    }

    auto chipset = resolve_chipset(options.chipset, mode);
    bool is_aga = (chipset == amiga::Chipset::aga);

    // ---- IBM PC DOS: glyph-matched text modes ----
    // Pre-dither to the 16-entry CGA master palette (so the glyph matcher
    // sees discrete candidate colors at sub-cell resolution), then call
    // cga_text::encode to pick (char, fg, bg) per cell. Rendered preview
    // comes back from cga_text::render. Raw output is char+attr pairs.
    if (amiga::is_cga_text(mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        // Freeform = user passed --width / --height (web Resize override
        // path). Halve the image vertically (averaging row pairs) so
        // square-pixel source lands in the encoder's hardware-pixel
        // space (1 source row = 1 hw scanline). Default canonical
        // 640×200 input is already at hw dims — no halve. Mirrors
        // main.cpp's freeform halve so CLI and web produce identical
        // cell counts.
        bool cga_freeform = options.width > 0 || options.height > 0;
        if (cga_freeform) {
            if ((image->height() % 2) != 0) {
                Image padded(image->width(), image->height() + 1);
                for (std::size_t y = 0; y < image->height(); ++y)
                    for (std::size_t x = 0; x < image->width(); ++x)
                        padded[x, y] = (*image)[x, y];
                *image = std::move(padded);
            }
            std::size_t halved_h = image->height() / 2;
            Image halved(image->width(), halved_h);
            for (std::size_t y = 0; y < halved_h; ++y) {
                for (std::size_t x = 0; x < image->width(); ++x) {
                    auto a = (*image)[x, y * 2];
                    auto b = (*image)[x, y * 2 + 1];
                    halved[x, y] = {(a.r + b.r) * 0.5f,
                                    (a.g + b.g) * 0.5f,
                                    (a.b + b.b) * 0.5f};
                }
            }
            *image = std::move(halved);
        }
        // Pad to encoder's 8×2 cell grid.
        {
            constexpr std::size_t cw = 8;
            constexpr std::size_t ch = 2;
            std::size_t pw = ((image->width()  + cw - 1) / cw) * cw;
            std::size_t ph = ((image->height() + ch - 1) / ch) * ch;
            if (pw != image->width() || ph != image->height()) {
                Image padded(pw, ph);
                for (std::size_t y = 0; y < image->height(); ++y)
                    for (std::size_t x = 0; x < image->width(); ++x)
                        padded[x, y] = (*image)[x, y];
                *image = std::move(padded);
            }
        }
        // Build fg/bg candidate palette: CGA text = fixed IRGB master,
        // EGA text = image-adaptive 16-of-64 via the EGA histogram quantizer
        // (same mechanism as EGA graphics modes). The 16 colors correspond
        // to the ATC palette register load-up on real EGA hardware.
        std::vector<Color3f> text_pal;
        text_pal.reserve(16);
        // All text-graphics modes use kCgaHw (see main.cpp).
        for (std::size_t i = 0; i < 16; ++i) {
            text_pal.push_back(
                color_space::srgb_hex_to_linear(palette::kCgaHw[i]));
        }
        // Resolve the per-cell metric. `blur` and `pca` need the
        // continuous source (pre-dither would destroy the precision
        // their inner loops rely on); only `mse` benefits from a
        // pre-dithered input.
        auto cga_metric =
            options.cga_text_metric == "mse" ? cga_text::Metric::mse
                                             : cga_text::Metric::blur;
        auto dith_method = parse_dither(options.dither);
        Image dithered(image->width(), image->height());
        if (cga_metric != cga_text::Metric::mse ||
            dith_method == dither::Method::none) {
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x)
                    dithered[x, y] = (*image)[x, y];
        } else {
            auto dith_result = dither::apply(*image, text_pal, {
                .method = dith_method,
                .strength = options.dither_strength,
                .error_clamp = options.error_clamp,
                .serpentine = true,
            });
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x)
                    dithered[x, y] = text_pal[dith_result.indices[y * image->width() + x]];
        }
        auto res = cga_text::encode(dithered, mode, {}, text_pal, -1,
                                    cga_metric, options.on_progress);
        if (!res) return std::unexpected{res.error()};
        auto preview = cga_text::render(*res);
        // No post-double here: result.rendered stays at hardware-pixel
        // dims (cols*8 × rows*2). The display layer un-halves freeform
        // via sy=2 in paintPreviewCanvas (web) / scale_for_display
        // (CLI). Doubling here would compound with that and produce a
        // 2× overstretch.

        PipelineResult result;
        result.rendered = std::move(preview);
        result.palette = text_pal;
        result.planes.depth = 4;  // conceptually 4-bit attr byte
        result.mode = mode;
        result.hires = amiga::get_mode_params(mode).is_hires;
        result.interlace = false;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.finalize_psnr(*image, res->total_error);
        result.raw_frame = std::move(res->data);
        result.text_scanline_offset = res->scanline_offset;
        result.text_cell_height =
            static_cast<std::uint8_t>(res->cell_height_scanlines);
        // Surface the cell grid via the genesis-cells field so the web
        // result string can show "cells: 80×100" same way the CLI's
        // Encoded line does. cols = width/8 always; rows is derived as
        // genesis_total_cells / cols on the consumer side.
        result.genesis_total_cells = res->cols * res->rows;
        // Note: convert_raw appends the 16-byte IrgbIRGB ATC palette
        // onto .raw output for EGA text modes — kept out of raw_frame
        // here so convert_viewer can hand only the char+attr bytes to
        // the DJGPP viewer generator.
        return result;
    }

    // ---- IBM PC CGA composite (160x200 effective, 16 NTSC-artifact colors) ----
    // The CGA composite palette is fixed (16 colors derived from the 4-bit
    // 2bpp pixel-pair pattern). We quantize to that palette and render via
    // palette lookup. The raw .raw output packs pattern nibbles into the
    // banked 16KB CGAPIC frame.
    if (mode == amiga::Mode::cga_composite) {
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        auto pal16 = palette::cga_composite_palette();
        std::vector<Color3f> pal_vec(pal16.begin(), pal16.end());
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;
        auto dith_result = dither::apply(*image, pal_vec, dith);

        Image rendered(image->width(), image->height());
        for (std::size_t i = 0; i < dith_result.indices.size(); ++i)
            rendered.pixels()[i] = pal_vec[dith_result.indices[i]];

        // Pack into banked CGAPIC layout: 16KB total, even rows in 0x0000
        // bank, odd rows in 0x2000 bank. Each byte holds 2 composite pixels
        // (two 4-bit patterns).
        auto w = image->width(), h = image->height();
        std::vector<std::uint8_t> raw(16384, 0);
        auto row_bytes = 320u / 4u;  // 80 bytes per row (2bpp, 4 px/byte)
        for (std::size_t y = 0; y < h; ++y) {
            auto bank = (y & 1) ? 0x2000u : 0x0000u;
            auto row_off = bank + (y >> 1) * row_bytes;
            for (std::size_t bx = 0; bx < row_bytes; ++bx) {
                auto p0 = palette::cga_composite_pattern(
                    dith_result.indices[y * w + bx * 2]);
                auto p1 = palette::cga_composite_pattern(
                    dith_result.indices[y * w + bx * 2 + 1]);
                raw[row_off + bx] = static_cast<std::uint8_t>((p0 << 4) | p1);
            }
        }

        PipelineResult result;
        result.rendered = std::move(rendered);
        result.palette = std::move(pal_vec);
        result.indices = std::move(dith_result.indices);
        result.planes.depth = 2;  // 2bpp packed
        result.mode = mode;
        result.hires = false;
        result.interlace = false;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.finalize_psnr(*image, dith_result.total_error);
        result.raw_frame = std::move(raw);
        return result;
    }

    // ---- IBM PC VGA 256-color chunky modes (13h, Mode X, Mode Y) ----
    // 8bpp indices + 256 entries of 18-bit DAC palette. For Mode X / Mode Y
    // raw_frame holds the 4 column-interleaved planes back-to-back; for
    // Mode 13h it's the straight row-major byte array.
    if (amiga::is_vga(mode) && amiga::is_chunky(mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        auto max_colors = std::size_t{1} << depth;  // 256 for chunky VGA
        auto quantized = quantize::quantize(*image, max_colors,
                                             quantize::Algorithm::median_cut,
                                             options.palette_diversity);
        if (!quantized) return std::unexpected{quantized.error()};
        for (auto& c : quantized->colors) c = palette::quantize_to_vga(c);
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;
        auto dith_result = dither::apply(*image, quantized->colors, dith);

        auto w = image->width(), h = image->height();
        Image rendered(w, h);
        for (std::size_t i = 0; i < dith_result.indices.size(); ++i)
            rendered.pixels()[i] = quantized->colors[dith_result.indices[i]];

        std::vector<std::uint8_t> raw;
        if (mode == amiga::Mode::vga_13h) {
            raw = dith_result.indices;
        } else {
            // Mode X / Mode Y: 4 column-interleaved planes.
            auto plane_w = w / 4;
            raw.reserve(plane_w * h * 4);
            for (std::size_t p = 0; p < 4; ++p) {
                for (std::size_t y = 0; y < h; ++y) {
                    for (std::size_t bx = 0; bx < plane_w; ++bx) {
                        raw.push_back(dith_result.indices[y * w + bx * 4 + p]);
                    }
                }
            }
        }

        PipelineResult result;
        result.rendered = std::move(rendered);
        result.palette = quantized->colors;
        result.indices = std::move(dith_result.indices);
        result.planes.depth = 8;  // 8bpp chunky
        result.mode = mode;
        result.hires = false;
        result.interlace = false;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.finalize_psnr(*image, dith_result.total_error);
        result.raw_frame = std::move(raw);
        return result;
    }

    // --- C64 / VIC-II ---
    // Brute-force per-cell quantization (4 colours per 4×8 cell,
    // 1 shared bg + 3 fg). Image arrives at 160×200 from the
    // pipeline's resize stage; we render back to 160×200 and let
    // preview_scale do the 2:1 hardware doubling. No dither pass
    // for the proof-of-fit; future work will route through
    // diffuse_raw_buffer with per-cell palette callback.
    if (amiga::is_c64(mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        // Pad to the mode's screen buffer. native_par may have produced
        // a smaller letterboxed image — pad with black (matching SNES /
        // Genesis), and pad tmask in lock-step (border = transparent
        // since it's outside the source region).
        //
        // Charset modes are flexible: if the image's current dims are
        // already a valid multiple of the cell size, leave them alone
        // (the user passed --width / --height for a freeform charmap).
        // Otherwise pad to the mode default.
        auto cparams = amiga::get_mode_params(mode);
        std::size_t kCW = cparams.screen_width;
        std::size_t kCH = cparams.screen_height;
        if (amiga::is_c64_charset(mode)) {
            std::size_t tw_step =
                (mode == amiga::Mode::c64_charset_multicolor) ? 4 : 8;
            std::size_t th_step = 8;
            if (image->width()  != 0 && (image->width()  % tw_step) == 0
                && image->height() != 0 && (image->height() % th_step) == 0) {
                kCW = image->width();
                kCH = image->height();
            }
        }
        if (image->width() != kCW || image->height() != kCH) {
            std::size_t old_w = image->width(), old_h = image->height();
            std::size_t ox = (kCW > old_w) ? (kCW - old_w) / 2 : 0;
            std::size_t oy = (kCH > old_h) ? (kCH - old_h) / 2 : 0;
            std::size_t cw = std::min(old_w, kCW);
            std::size_t ch = std::min(old_h, kCH);
            Image padded(kCW, kCH);
            for (std::size_t y = 0; y < ch; ++y)
                for (std::size_t x = 0; x < cw; ++x)
                    padded[ox + x, oy + y] = (*image)[x, y];
            *image = std::move(padded);
            if (has_transparency && tmask.size() == old_w * old_h) {
                std::vector<bool> new_mask(kCW * kCH, true);
                for (std::size_t y = 0; y < ch; ++y)
                    for (std::size_t x = 0; x < cw; ++x)
                        new_mask[(oy + y) * kCW + (ox + x)] =
                            tmask[y * old_w + x];
                tmask = std::move(new_mask);
            }
        }
        auto pal_choice = c64::parse_palette(options.c64_palette);
        auto metric     = c64::parse_metric(options.c64_metric);
        // match_range: stretch the source's OKLab extent to span the
        // chosen VIC-II palette's extent. Same shape as the EHB /
        // standard-mode preprocessor — pulls highlights / shadows
        // into the palette's reachable range so quantisation has
        // headroom on both ends. Off by default.
        if (options.match_range) {
            auto pal_span = c64::palette_colors(pal_choice);
            Palette c64_pal;
            c64_pal.name = "c64";
            c64_pal.colors.assign(pal_span.begin(), pal_span.end());
            preprocess::match_palette_range(*image, c64_pal);
        }
        dither::Settings dith;
        dith.method      = parse_dither(options.dither);
        dith.strength    = options.dither_strength;
        dith.error_clamp = options.error_clamp;
        dith.serpentine  = true;
        Result<c64::EncodeResult> enc = [&] {
            switch (mode) {
            case amiga::Mode::c64_hires:
                return c64::encode_hires(*image, pal_choice, dith, metric);
            case amiga::Mode::c64_fli:
                return c64::encode_fli(*image, pal_choice, dith, metric);
            case amiga::Mode::c64_afli:
                return c64::encode_afli(*image, pal_choice, dith, metric);
            case amiga::Mode::c64_petscii:
                return c64::encode_petscii(*image, pal_choice, dith, metric,
                                            options.c64_petscii_graphics_only,
                                            options.on_progress);
            case amiga::Mode::c64_charset_hires: {
                std::size_t budget = options.tile_budget == 0
                    ? std::size_t{256} : options.tile_budget;
                return c64::encode_charset_hires(
                    *image, pal_choice, dith, metric,
                    budget, options.tile_reserve);
            }
            case amiga::Mode::c64_charset_multicolor: {
                std::size_t budget = options.tile_budget == 0
                    ? std::size_t{256} : options.tile_budget;
                return c64::encode_charset_multicolor(
                    *image, pal_choice, dith, metric,
                    budget, options.tile_reserve);
            }
            default:
                return c64::encode_multicolor(*image, pal_choice, dith, metric);
            }
        }();
        if (!enc) return std::unexpected{enc.error()};

        PipelineResult result;
        result.rendered = std::move(enc->rendered);
        auto pal_span = c64::palette_colors(pal_choice);
        result.palette.assign(pal_span.begin(), pal_span.end());
        result.indices.clear();
        result.planes.depth = amiga::is_c64_multicolor(mode) ? 2 : 1;
        result.mode = mode;
        result.hires = false;
        result.interlace = false;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        // c64::EncodeResult doesn't expose a total_error (the per-cell
        // brute force scores cells in isolation under either mse or blur
        // metric — neither is an image-wide sum we want to surface).
        // Reconstruct one as the OKLab² sum between source and rendered,
        // matching the convention of every other path's total_error.
        float te_c64 = 0.0f;
        {
            auto src_px = image->pixels();
            auto ren_px = result.rendered.pixels();
            std::size_t n = std::min(src_px.size(), ren_px.size());
            for (std::size_t i = 0; i < n; ++i)
                te_c64 += color_space::perceptual_distance_sq(
                    src_px[i], ren_px[i]);
        }
        result.finalize_psnr(*image, te_c64);
        // Pack bitmap + screen + color RAM into raw_frame for downstream
        // writers. Order: bitmap + screen + color. Sizes vary by mode
        // (see c64_prg.cpp for the per-mode split).
        std::vector<std::uint8_t> raw;
        raw.reserve(enc->bitmap.size() + enc->screen_ram.size() +
                    enc->color_ram.size());
        raw.insert(raw.end(), enc->bitmap.begin(), enc->bitmap.end());
        raw.insert(raw.end(), enc->screen_ram.begin(), enc->screen_ram.end());
        raw.insert(raw.end(), enc->color_ram.begin(), enc->color_ram.end());
        result.raw_frame = std::move(raw);
        result.c64_bg_color = enc->bg_color;
        result.c64_cols = enc->cols;
        result.c64_rows = enc->rows;
        result.c64_unique_glyphs = enc->unique_glyphs;
        result.c64_mc1 = enc->mc1;
        result.c64_mc2 = enc->mc2;
        // Re-use the Genesis tile-stats fields for the web status line:
        // semantically identical (unique tiles, total cells, bytes/tile).
        // Bytes/tile = 8 for c64 8-byte glyph patterns.
        if (enc->cols > 0 && enc->rows > 0) {
            result.genesis_unique_tiles = enc->unique_glyphs;
            result.genesis_total_cells = enc->cols * enc->rows;
            result.tile_data_bytes = enc->unique_glyphs * 8;
        }
        return result;
    }

    // --- SNES Mode 7 (256-palette and Direct Color) ---
    // The full quantise → dither → pack pipeline lives inside
    // snes_io::encode_snes_mode7. The chunky intermediate (palette
    // indices for 256, BBGGGRRR pixel bytes for Direct) is internal
    // to that function — no pre-pack state escapes here.
    if (amiga::is_snes(mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        auto w = image->width(), h = image->height();
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        bool direct_color = (mode == amiga::Mode::snes_mode7_direct);
        auto enc = snes_io::encode_snes_mode7(
            *image, direct_color, dith, options.palette_diversity,
            options.on_progress);
        if (!enc) return std::unexpected{enc.error()};

        PipelineResult result;
        result.rendered = std::move(enc->rendered);
        result.palette = std::move(enc->palette);
        result.indices.clear();  // not meaningful for the packed format
        result.planes.depth = 8;
        result.mode = mode;
        result.hires = false;
        result.interlace = false;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.finalize_psnr(*image, enc->quant_error);
        result.raw_frame = std::move(enc->packed_bytes);
        result.genesis_unique_tiles = enc->unique_after_merge;
        result.genesis_total_cells = (w / 8) * (h / 8);
        result.tile_data_bytes = enc->unique_after_merge * 64;
        return result;
    }

    // --- Sega Genesis / Mega Drive (tile-bitmap title art) ---
    // 8×8 4bpp tiles + 4 palette lines × 16 BGR333 entries. Each tile is
    // assigned to one palette line via OKLab k-means; pixels are then
    // re-quantised against the assigned per-tile palette through the
    // central dither::diffuse_raw_buffer driver — the same shape as the
    // strips per-strip path, just at 8×8 granularity.
    if (amiga::is_genesis(mode)) {
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        auto w = image->width();
        auto h = image->height();
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        // 1. BGR333-snap the source so clustering scores against the
        //    palette grid the hardware actually exposes.
        Image snapped(w, h);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x)
                snapped[x, y] = console_color::bgr333_quantize((*image)[x, y]);

        // 2. Cluster tiles → 4 palette lines (+ per-tile shadow decision
        //    for S/H modes).
        bool sh_mode = amiga::is_genesis_sh(mode);
        auto gres = sh_mode
            ? genesis::cluster_tiles_into_palettes_sh(
                  snapped, static_cast<float>(options.palette_diversity))
            : genesis::cluster_tiles_into_palettes(
                  snapped, static_cast<float>(options.palette_diversity));
        auto tiles_x = (w + genesis::kTileSide - 1) / genesis::kTileSide;

        // 3. Pre-convert each palette line to OKLab so the picker is cheap.
        //    For S/H modes also pre-compute the shadowed-palette views so
        //    shadow tiles' picker scores against the correct effective
        //    colour set.
        std::array<std::vector<color_space::OKLab>, genesis::kPaletteCount>
            palette_lab, shadow_lab;
        std::array<std::vector<Color3f>, genesis::kPaletteCount>
            shadow_lines;
        for (std::size_t k = 0; k < genesis::kPaletteCount; ++k) {
            palette_lab[k].resize(genesis::kColorsPerPalette);
            for (std::size_t i = 0; i < genesis::kColorsPerPalette; ++i)
                palette_lab[k][i] = color_space::linear_to_oklab(
                    gres.palette_lines[k][i]);
            if (sh_mode) {
                shadow_lines[k] = genesis::shadow_palette_line(
                    gres.palette_lines[k]);
                shadow_lab[k].resize(genesis::kColorsPerPalette);
                for (std::size_t i = 0; i < genesis::kColorsPerPalette; ++i)
                    shadow_lab[k][i] = color_space::linear_to_oklab(
                        shadow_lines[k][i]);
            }
        }

        // 4. Per-pixel re-quantise via per-tile mirrored 3×3 ED.
        //
        // Build a 3W×3H buffer per 8×8 tile where the centre block is
        // the source tile and the 8 surrounding blocks are mirror
        // reflections (h-flip on left/right, v-flip on top/bottom,
        // both on corners). Run ED on the full 24×24, take the centre
        // 8×8. Identical source tiles produce identical mirrored
        // buffers → identical post-ED patterns → cleaner dedup. Same
        // approach as c64 charset (commit 7236237).
        Image rendered(w, h);
        std::vector<std::uint8_t>& pixel_index = gres.pixel_index;
        const std::vector<std::uint8_t>& tile_pal = gres.tile_palette;
        const std::vector<std::uint8_t>& tile_shadow = gres.tile_shadow;
        constexpr std::size_t kTS = genesis::kTileSide;       // 8
        constexpr std::size_t k3T = 3 * kTS;                  // 24
        Image block(k3T, k3T);
        std::vector<std::uint8_t> block_idx(k3T * k3T, 0);
        std::vector<color_space::OKLab> block_chosen(k3T * k3T);
        auto tiles_y = (h + kTS - 1) / kTS;
        float te = 0.0f;
        for (std::size_t ty = 0; ty < tiles_y; ++ty) {
            for (std::size_t tx = 0; tx < tiles_x; ++tx) {
                // Mirror-fill the 3W×3H block.
                for (std::size_t by = 0; by < 3; ++by) {
                    for (std::size_t bx = 0; bx < 3; ++bx) {
                        for (std::size_t ly = 0; ly < kTS; ++ly) {
                            std::size_t sy_local =
                                (by == 1) ? ly : (kTS - 1 - ly);
                            for (std::size_t lx = 0; lx < kTS; ++lx) {
                                std::size_t sx_local =
                                    (bx == 1) ? lx : (kTS - 1 - lx);
                                std::size_t sx = std::min(
                                    tx * kTS + sx_local, w - 1);
                                std::size_t sy = std::min(
                                    ty * kTS + sy_local, h - 1);
                                block[bx * kTS + lx, by * kTS + ly] =
                                    (*image)[sx, sy];
                            }
                        }
                    }
                }
                std::size_t cell = ty * tiles_x + tx;
                std::uint8_t pal = tile_pal[cell];
                bool shadowed = sh_mode && tile_shadow[cell] != 0;
                auto& pl = shadowed ? shadow_lab[pal] : palette_lab[pal];
                std::span<const color_space::OKLab> pl_span(pl.data(),
                                                             pl.size());
                std::fill(block_idx.begin(), block_idx.end(), 0);
                te += dither::diffuse_raw_buffer(
                    block, dith,
                    [&](const color_space::OKLab& target,
                        std::size_t bx, std::size_t by) -> dither::PickResult {
                        std::size_t k = 1;
                        color_space::OKLab chosen{};
                        float thr = dither::pick_palette_index_with_ostro(
                            dith.method, target, pl_span, bx, by,
                            dith.strength, /*k_min=*/1, k, chosen);
                        std::size_t bi = by * k3T + bx;
                        block_idx[bi] = static_cast<std::uint8_t>(k);
                        block_chosen[bi] = chosen;
                        return {chosen, thr};
                    });
                // Copy centre 8×8 back to the global buffers.
                for (std::size_t ly = 0; ly < kTS; ++ly) {
                    for (std::size_t lx = 0; lx < kTS; ++lx) {
                        auto bi = (kTS + ly) * k3T + (kTS + lx);
                        auto x = tx * kTS + lx;
                        auto y = ty * kTS + ly;
                        if (x >= w || y >= h) continue;
                        std::uint8_t k = block_idx[bi];
                        pixel_index[y * w + x] = k;
                        rendered[x, y] = shadowed
                            ? shadow_lines[pal][k]
                            : gres.palette_lines[pal][k];
                    }
                }
            }
        }

        gres.preview = rendered;
        gres.total_error = te;

        // 5. Tile-dedup with H/V/H+V flip detection. Two cells with the
        //    same pixel-index pattern share VRAM bytes; per-cell palette
        //    + flips live in the tilemap. Cuts VRAM by ~30-70% on title
        //    art with repeated regions (skies, gradients, decorations).
        auto dedup = genesis::dedup_tiles(
            gres.pixel_index, gres.tile_palette, w, h);
        auto total_cells = tiles_x * tiles_y;

        // Build the three byte streams: tile bytes, tilemap cells (u16),
        // palette CRAM words (u16). The single .bin stream concatenates
        // all three; the SGDK C-header generator wants them separately.
        std::vector<std::uint8_t>  tile_bytes;
        std::vector<std::uint16_t> tilemap_cells;
        std::vector<std::uint16_t> palette_words;
        tile_bytes.reserve(dedup.tiles.size() * 32);
        tilemap_cells.reserve(total_cells);
        palette_words.reserve(genesis::kPaletteCount *
                              genesis::kColorsPerPalette);
        for (auto& bytes : dedup.tiles)
            tile_bytes.insert(tile_bytes.end(), bytes.begin(), bytes.end());
        // Tilemap encoding: for S/H modes the priority bit doubles as
        // the shadow flag — HIGH priority = render normally, LOW priority
        // = render shadowed (when VDP S/H register is set). For non-S/H
        // modes priority stays low; the user can OR in plane-priority
        // semantics in their own code if needed.
        for (std::size_t i = 0; i < dedup.tilemap.size(); ++i) {
            auto& cell = dedup.tilemap[i];
            bool priority = false;
            if (sh_mode) {
                // tile_shadow[i] == 0 → render normally → priority HIGH.
                // tile_shadow[i] == 1 → render shadowed → priority LOW.
                priority = (tile_shadow[i] == 0);
            }
            tilemap_cells.push_back(genesis::encode_tilemap_cell(
                cell.tile_index, cell.palette_line,
                cell.h_flip, cell.v_flip, priority));
        }
        for (std::size_t k = 0; k < genesis::kPaletteCount; ++k) {
            for (std::size_t i = 0; i < genesis::kColorsPerPalette; ++i) {
                palette_words.push_back(
                    console_color::to_bgr333_word(gres.palette_lines[k][i]));
            }
        }
        std::vector<std::uint8_t> raw;
        raw.reserve(tile_bytes.size() + tilemap_cells.size() * 2 +
                    palette_words.size() * 2);
        raw.insert(raw.end(), tile_bytes.begin(), tile_bytes.end());
        for (auto w16 : tilemap_cells) {
            raw.push_back(static_cast<std::uint8_t>(w16 >> 8));
            raw.push_back(static_cast<std::uint8_t>(w16 & 0xFF));
        }
        for (auto w16 : palette_words) {
            raw.push_back(static_cast<std::uint8_t>(w16 >> 8));
            raw.push_back(static_cast<std::uint8_t>(w16 & 0xFF));
        }

        // Flatten the palette lines into a single 64-entry vector for the
        // result — index = pal*16 + entry.
        std::vector<Color3f> flat_palette;
        flat_palette.reserve(genesis::kPaletteCount *
                              genesis::kColorsPerPalette);
        for (std::size_t k = 0; k < genesis::kPaletteCount; ++k)
            for (auto& c : gres.palette_lines[k])
                flat_palette.push_back(c);

        PipelineResult result;
        result.rendered = std::move(rendered);
        result.palette = std::move(flat_palette);
        result.indices = std::move(gres.pixel_index);
        result.planes.depth = 4;
        result.mode = mode;
        result.hires = false;
        result.interlace = false;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.finalize_psnr(*image, te);
        result.raw_frame = std::move(raw);
        result.genesis_unique_tiles = dedup.tiles.size();
        result.tile_data_bytes = dedup.tiles.size() * 32;  // 4bpp 8×8
        result.genesis_total_cells = total_cells;
        result.genesis_tile_bytes = std::move(tile_bytes);
        result.genesis_tilemap_cells = std::move(tilemap_cells);
        result.genesis_palette_words = std::move(palette_words);
        return result;
    }

    // --- HAM modes: use dedicated HAM encoder ---
    // HAM6+strips: defer to the strips branch downstream (skip the
    // standard HAM dispatch). HAM6 has the same 6-plane DMA pattern
    // as EHB so kStrips6bplEhb transfers; the strips branch routes to
    // strips::encode_strips_ham6_ocs.
    if (amiga::is_ham(mode) && !(options.scap && mode == amiga::Mode::ham6)) {
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

        // Wire dither settings into HAM options. DBS doesn't apply
        // (HAM uses ops, not palette indices) — silently fall back to
        // FS so users picking DBS still get a sensible result.
        ham_opts.dither_method = parse_dither(options.dither);
        if (ham_opts.dither_method == dither::Method::dbs) {
            ham_opts.dither_method = dither::Method::floyd_steinberg;
        }
        ham_opts.dither_strength = options.dither_strength;
        ham_opts.error_clamp = options.error_clamp;
        ham_opts.palette_diversity = options.palette_diversity;
        ham_opts.quantizer = options.quantizer;
        ham_opts.triple_beam = static_cast<std::size_t>(
            std::clamp(options.ham_triple, 0, 256));
        ham_opts.greedy = options.ham_fast;
        ham_opts.metric = (options.ham_metric == "oklab2")
            ? ham::HamMetric::oklab2 : ham::HamMetric::srgb_mse;
        // best only applies to HAM6 and HAM8 — HAM4/5/7 are skipped
        // because their tiny base palettes (4/8/32 colors) don't benefit
        // from the refined planner enough to justify the cost.
        auto ham_params = amiga::get_mode_params(mode);
        bool ham_eligible_for_best =
            (ham_params.bitplane_depth == 6 ||
             ham_params.bitplane_depth == 8);
        ham_opts.best = options.best && ham_eligible_for_best;
        ham_opts.on_progress = options.on_progress;
        ham_opts.skip_initial_swap_rows = options.interlace ? 2 : 0;

        // --palette: load and feed as the HAM base palette. Skips the
        // auto-quantizer + refinement; user takes responsibility for
        // palette quality. For OCS, palette_io snaps to 12-bit on load.
        if (has_user_palette(options)) {
            auto loaded = load_user_palette(options);
            if (!loaded) return std::unexpected{loaded.error()};
            ham_opts.external_palette = std::move(loaded->colors);
        }

        // Force transparent pixels to black BEFORE HAM encoding so the
        // encoder handles color transitions correctly at transparency edges.
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        Result<ham::HamResult> ham_result;
        std::optional<Image> swept_preview;
        if (options.copper) {
            ham_result = ham::encode_ham_copper(*image, mode, chipset, ham_opts,
                                                  compound_hires,
                                                  static_cast<std::size_t>(options.copper_changes));
        } else if (options.best && ham_eligible_for_best) {
            // Plain HAM6/HAM8 + --best: same multi-restart shape as sliced /
            // strips. Each trial encodes a jittered source under different
            // (dither_strength × diversity) and is ranked by best_metric
            // against the unjittered original. 8 jitter seeds matches
            // plain sliced's setting; HAM8 uses amplitude 0.4 (its 64-colour
            // base + 8-bit modify channels are more sensitive to large
            // pre-image perturbations — AGA shimmer).
            struct HamTrial {
                ham::HamResult result;
                Image rendered;
            };
            dither::Settings dith;
            dith.method = parse_dither(options.dither);
            dith.strength = options.dither_strength;
            dith.error_clamp = options.error_clamp;
            auto encode_once = [&](const Image& img,
                                   const dither::Settings& d,
                                   int div) -> Result<HamTrial> {
                auto trial_opts = ham_opts;
                trial_opts.dither_strength = d.strength;
                trial_opts.error_clamp = d.error_clamp;
                trial_opts.palette_diversity = div;
                trial_opts.best = false;  // best replaced by the sweep
                // Silence per-trial progress: best_sweep parallelises trials,
                // and N workers all firing options.on_progress = chaos. The
                // sweep-level reporter (passed below into best_sweep) is the
                // single clean stream the user sees.
                trial_opts.on_progress = nullptr;
                auto r = ham::encode_ham(img, mode, chipset, trial_opts);
                if (!r) return std::unexpected{r.error()};
                auto p = pipeline::render_preview(
                    r->planes, r->base_palette, /*is_ham=*/true,
                    options.interlace, chipset, nullptr);
                if (!p) return std::unexpected{p.error()};
                return HamTrial{*std::move(r), *std::move(p)};
            };
            auto best_metric = pipeline::parse_best_metric(options.best_metric);
            float amp = (ham_params.bitplane_depth == 8) ? 0.4f : 1.0f;
            auto winner = pipeline::best_sweep<HamTrial>(
                *image, dith, options.palette_diversity,
                /*jitter_count=*/8,
                encode_once,
                [](const HamTrial& t) -> const Image& { return t.rendered; },
                options.on_progress, amp, best_metric);
            if (!winner) {
                return std::unexpected{Error{
                    ErrorCode::unsupported_mode,
                    "HAM --best sweep produced no result"}};
            }
            ham_result = std::move(winner->result);
            swept_preview = std::move(winner->rendered);
        } else {
            ham_result = ham::encode_ham(*image, mode, chipset, ham_opts);
        }
        if (!ham_result) return std::unexpected{ham_result.error()};

        // Render preview using HAM decoder (not simple palette lookup)
        Result<Image> preview;
        if (swept_preview) {
            preview = std::move(*swept_preview);
        } else {
            preview = pipeline::render_preview(
                ham_result->planes, ham_result->base_palette,
                /*is_ham=*/true, options.interlace, chipset,
                (options.copper && !ham_result->scanline_palettes.empty())
                    ? &ham_result->scanline_palettes : nullptr);
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
        result.finalize_psnr(*image, ham_result->total_error);
        return result;
    }

    // --- EHB mode: 32 base colors + 32 half-brightness ---
    // strips+EHB has its own dedicated encoder downstream — let it through.
    if (mode == amiga::Mode::ehb && !options.scap) {
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
            dither::Settings dith;
            dith.method = parse_dither(options.dither);
            dith.strength = options.dither_strength;
            dith.error_clamp = options.error_clamp;

            std::size_t skip_initial = options.interlace ? 2 : 0;

            // Single-pass EHB+sliced encode: depth-5 copper (32 base colors)
            // → per-row 64-entry EHB palette (base + hardware halfbrites)
            // → re-dither against per-row palette → 6-plane bitmap +
            // preview. Factored so best below replays it under a
            // parallel jitter sweep without duplicating the body.
            struct EhbSlicedTrial {
                copper::CopperResult copper_result;
                bitplane::BitplaneData planes;
                Image rendered;
                float total_error;
            };
            auto encode_once = [&](const Image& img,
                                   const dither::Settings& d,
                                   int diversity) -> Result<EhbSlicedTrial> {
                // Pre-build the global base palette ourselves with
                // PNN + pair-aware refinement (and 1-opt on --best),
                // then hand it to encode_copper as the line-0 seed.
                // sliced still evolves from there per-line via its own
                // copper-budget-aware planner — but the seed is the
                // EHB-aware best palette we can build offline.
                Palette seed_pal;
                {
                    auto q = quantize::quantize(img, 32,
                                                quantize::Algorithm::pnn,
                                                diversity);
                    if (!q) return std::unexpected{q.error()};
                    seed_pal = std::move(*q);
                    snap_to_chipset(seed_pal, chipset, mode);
                    while (seed_pal.colors.size() < 32)
                        seed_pal.colors.emplace_back(0, 0, 0);
                    palette::refine_ehb_base_palette(
                        std::span<Color3f>(seed_pal.colors.data(), 32),
                        img.pixels(),
                        /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
                    if (options.best) {
                        // Silence per-trial progress: best_sweep parallelises
                        // trials, and N workers all firing options.on_progress
                        // garbles the output. The sweep-level reporter passed
                        // into best_sweep below is the single clean stream.
                        palette::extra_ehb_optimization(
                            std::span<Color3f>(seed_pal.colors.data(), 32),
                            img.pixels(),
                            /*snap_to_ocs=*/chipset != amiga::Chipset::aga,
                            /*max_passes=*/2,
                            [](std::size_t n,
                               std::function<void(std::size_t)> f) {
                                pipeline::parallel_for(n, std::move(f));
                            },
                            /*on_progress=*/{});
                    }
                }
                auto cr = copper::encode_copper(
                    img, 5, d, chipset,
                    static_cast<std::size_t>(options.copper_changes),
                    &seed_pal.colors, options.lock_color0,
                    {}, diversity,
                    skip_initial, options.interlace,
                    /*is_ehb=*/true,
                    /*on_progress=*/{},
                    // Forward the sentinel when the CLI flag wasn't set
                    // so encode_copper picks its depth/is_ehb-aware default.
                    options.sliced_spread_radius >= 0
                        ? static_cast<std::size_t>(options.sliced_spread_radius)
                        : std::numeric_limits<std::size_t>::max(),
                    options.sliced_spread_decay >= 0.0f
                        ? options.sliced_spread_decay : -1.0f);
                if (!cr) return std::unexpected{cr.error()};

                auto w = img.width();
                auto h = img.height();
                std::vector<std::uint8_t> all_indices(w * h);

                std::vector<std::vector<color_space::OKLab>> pal_lab_per_row(h);
                for (std::size_t y = 0; y < h; ++y) {
                    auto& base32 = cr->scanline_palettes[y];
                    while (base32.size() < 32) base32.emplace_back(0, 0, 0);
                    // Pair-aware refinement on the row's base palette
                    // using only that row's pixels — same idea as plain
                    // EHB but per-line, so each scanline's 32 base
                    // colours are jointly optimal under the half-brite
                    // pairing for the colours actually appearing on
                    // that row.
                    palette::refine_ehb_base_palette(
                        std::span<Color3f>(base32.data(), 32),
                        std::span<const Color3f>(
                            img.pixels().data() + y * w, w),
                        /*snap_to_ocs=*/chipset != amiga::Chipset::aga,
                        /*max_iters=*/4);
                    Palette bp;
                    bp.colors.assign(base32.begin(), base32.end());
                    auto ehb64 = palette::make_ehb_palette(bp.colors);
                    // Update copper-result palette so downstream
                    // consumers (preview render, IFF CMAP) see the
                    // refined values rather than the pre-refinement
                    // copy.
                    base32.assign(bp.colors.begin(), bp.colors.end());
                    pal_lab_per_row[y].resize(ehb64.colors.size());
                    for (std::size_t i = 0; i < ehb64.colors.size(); ++i)
                        pal_lab_per_row[y][i] =
                            color_space::linear_to_oklab(ehb64.colors[i]);
                }

                float total_err = dither::diffuse_raw_buffer(
                    img, d,
                    [&](const color_space::OKLab& target,
                        std::size_t x, std::size_t y) -> dither::PickResult {
                        auto& pal_lab = pal_lab_per_row[y];
                        std::size_t k = 0;
                        color_space::OKLab chosen{};
                        float thr = dither::pick_palette_index_with_ostro(
                            d.method, target, pal_lab, x, y,
                            d.strength, /*k_min=*/0, k, chosen);
                        all_indices[y * w + x] = static_cast<std::uint8_t>(k);
                        return {chosen, thr};
                    });

                if (d.method == dither::Method::dbs) {
                    dither::apply_dbs_post_pass(
                        img, all_indices,
                        [&](std::size_t /*x*/, std::size_t y)
                            -> std::span<const color_space::OKLab> {
                            return pal_lab_per_row[y];
                        });
                }

                if (has_transparency) {
                    for (std::size_t i = 0; i < tmask.size() && i < all_indices.size(); ++i)
                        if (tmask[i]) all_indices[i] = 0;
                }

                auto planes = bitplane::encode(all_indices, w, h, 6);
                if (!planes) return std::unexpected{planes.error()};

                Image rendered(w, h);
                for (std::size_t y = 0; y < h; ++y) {
                    auto& base32 = cr->scanline_palettes[y];
                    Palette bp;
                    bp.colors.assign(base32.begin(), base32.end());
                    auto ehb64 = palette::make_ehb_palette(bp.colors);
                    for (std::size_t x = 0; x < w; ++x) {
                        auto idx = all_indices[y * w + x];
                        if (idx < ehb64.colors.size())
                            rendered[x, y] = ehb64.colors[idx];
                    }
                }

                return EhbSlicedTrial{
                    *std::move(cr),
                    *std::move(planes),
                    std::move(rendered),
                    total_err,
                };
            };

            std::optional<EhbSlicedTrial> winner;
            if (options.best) {
                // Same sweep shape as plain sliced and strips EHB: 8 jitter
                // seeds × 5 strengths × 4 diversities + 1 baseline.
                // 32-colour base palette → shallower median-cut basins,
                // amplitude 1.0 (AGA-only weakening doesn't apply here
                // since EHB is OCS-bound).
                auto sliced_metric = pipeline::parse_best_metric(options.best_metric);
                winner = pipeline::best_sweep<EhbSlicedTrial>(
                    *image, dith, options.palette_diversity,
                    /*jitter_count=*/8,
                    [&](const Image& jittered_in,
                        const dither::Settings& d, int div)
                            -> Result<EhbSlicedTrial> {
                        return encode_once(jittered_in, d, div);
                    },
                    [](const EhbSlicedTrial& t) -> const Image& {
                        return t.rendered;
                    },
                    options.on_progress,
                    /*jitter_amp=*/1.0f,
                    sliced_metric);
            }
            if (!winner.has_value()) {
                auto r = encode_once(*image, dith,
                                     options.palette_diversity);
                if (!r) return std::unexpected{r.error()};
                winner = std::move(*r);
                if (options.on_progress) options.on_progress(1.0f, "done");
            }

            auto& first_pal = winner->copper_result.scanline_palettes[0];
            PipelineResult result;
            result.rendered = std::move(winner->rendered);
            result.planes = std::move(winner->planes);
            result.palette = std::vector<Color3f>(first_pal.begin(), first_pal.end());
            result.mode = mode;
            result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
            result.interlace = options.interlace;
            result.copper = true;
            result.aga = is_aga;
            result.scanline_palettes = std::move(winner->copper_result.scanline_palettes);
            result.scanline_changes = std::move(winner->copper_result.scanline_changes);
            result.copper_num_colors = winner->copper_result.num_colors;
            result.changes_per_line = winner->copper_result.changes_per_line;
            result.max_moves_per_line = winner->copper_result.max_moves_per_line;
            result.has_transparency = has_transparency;
            result.transparency_mask = tmask;
            result.copper_changes = winner->copper_result.avg_changes_per_line;
            result.finalize_psnr(*image, winner->total_error);
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

        // Plain EHB --best: multi-restart sweep over (dither_strength ×
        // diversity × pre-image jitter), ranked by best_metric. Only
        // engaged on the simple case (no user palette, no locks/pins,
        // no transparency, no pre-existing pin swaps to replay) — the
        // joint state otherwise gets unwieldy. Same shape and trial
        // count as plain sliced / strips EHB.
        bool ehb_can_sweep = options.best
                          && !has_user_palette(options)
                          && options.locks.empty()
                          && options.pins.empty()
                          && !has_transparency;
        if (ehb_can_sweep) {
            struct EhbPlainTrial {
                Palette base_pal;
                Palette ehb_pal;
                bitplane::BitplaneData planes;
                std::vector<std::uint8_t> indices;
                Image rendered;
                float total_error;
            };
            dither::Settings base_dith;
            base_dith.method = parse_dither(options.dither);
            base_dith.strength = options.dither_strength;
            base_dith.error_clamp = options.error_clamp;
            auto encode_once = [&](const Image& img,
                                   const dither::Settings& d,
                                   int diversity) -> Result<EhbPlainTrial> {
                // Pair-aware EHB seeding: quantise the *union* of the
                // source pixels and their sRGB-doubled darks. A picked
                // colour either matches a bright pixel directly OR is
                // 2x a dark pixel (whose half-brite copy will then
                // land exactly on that dark pixel). Plain median-cut
                // / OCS-brute on the source alone tends to cluster in
                // the midtone where pixel density is highest, leaving
                // the half-brite half unused on dark detail.
                Image enriched(img.width(), img.height() * 2);
                for (std::size_t y = 0; y < img.height(); ++y) {
                    for (std::size_t x = 0; x < img.width(); ++x) {
                        enriched[x, y] = img[x, y];
                    }
                }
                for (std::size_t y = 0; y < img.height(); ++y) {
                    for (std::size_t x = 0; x < img.width(); ++x) {
                        auto p = img[x, y];
                        auto s = color_space::linear_to_srgb(p).clamped();
                        Color3f doubled_srgb{
                            std::clamp(s.r * 2.0f, 0.0f, 1.0f),
                            std::clamp(s.g * 2.0f, 0.0f, 1.0f),
                            std::clamp(s.b * 2.0f, 0.0f, 1.0f),
                        };
                        enriched[x, img.height() + y] =
                            color_space::srgb_to_linear(doubled_srgb);
                    }
                }
                // PNN (Pairwise Nearest Neighbor agglomerative
                // clustering) on the enriched source produces a
                // better 32-base seed than median-cut / OCS-brute-
                // force on most images — fewer wasted slots in the
                // dominant cluster, cleaner spread across the gamut.
                auto quantized = quantize::quantize(
                    enriched, 32, quantize::Algorithm::pnn, diversity);
                if (!quantized) return std::unexpected{quantized.error()};
                Palette bp = std::move(*quantized);
                snap_to_chipset(bp, chipset, mode);
                // Pair-aware refinement: jointly optimise the 32 base
                // colours under the hardware-tied half-brite pairing.
                while (bp.colors.size() < 32) bp.colors.emplace_back(0, 0, 0);
                palette::refine_ehb_base_palette(
                    std::span<Color3f>(bp.colors.data(), 32),
                    img.pixels(),
                    /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
                auto ehbp = palette::make_ehb_palette(bp.colors);
                auto dr = dither::apply(img, ehbp.colors, d);
                auto pl = bitplane::encode(dr.indices,
                                            img.width(), img.height(), 6);
                if (!pl) return std::unexpected{pl.error()};
                std::vector<Color3f> full_pal(ehbp.colors.begin(),
                                              ehbp.colors.end());
                auto pv = pipeline::render_preview(
                    *pl, full_pal, /*is_ham=*/false,
                    options.interlace, chipset);
                if (!pv) return std::unexpected{pv.error()};
                return EhbPlainTrial{
                    std::move(bp), std::move(ehbp), *std::move(pl),
                    std::move(dr.indices), *std::move(pv), dr.total_error,
                };
            };
            auto bm = pipeline::parse_best_metric(options.best_metric);
            auto winner = pipeline::best_sweep<EhbPlainTrial>(
                *image, base_dith, options.palette_diversity,
                /*jitter_count=*/8,
                encode_once,
                [](const EhbPlainTrial& t) -> const Image& { return t.rendered; },
                options.on_progress, /*jitter_amplitude=*/1.0f, bm);
            if (!winner) {
                return std::unexpected{Error{
                    ErrorCode::unsupported_mode,
                    "EHB --best sweep produced no result"}};
            }
            // Post-sweep 1-opt local search on the winning palette —
            // takes the multi-restart's best basin and tightens it
            // further. Non-best gets the same step earlier in the
            // pipeline; here we run it once on the sweep winner.
            {
                std::vector<Color3f> base32(
                    winner->base_pal.colors.begin(),
                    winner->base_pal.colors.begin() + 32);
                palette::extra_ehb_optimization(
                    std::span<Color3f>(base32.data(), 32),
                    image->pixels(),
                    /*snap_to_ocs=*/chipset != amiga::Chipset::aga,
                    /*max_passes=*/2,
                    [](std::size_t n,
                       std::function<void(std::size_t)> f) {
                        pipeline::parallel_for(n, std::move(f));
                    },
                    options.on_progress);
                winner->base_pal.colors = std::move(base32);
                winner->ehb_pal = palette::make_ehb_palette(
                    winner->base_pal.colors);
                // Re-dither + re-bitplane + re-render with the refined
                // palette so the result indices/preview reflect it.
                dither::Settings post_dith = base_dith;
                auto dr = dither::apply(*image, winner->ehb_pal.colors,
                                         post_dith);
                auto pl = bitplane::encode(
                    dr.indices, image->width(), image->height(), 6);
                if (pl) {
                    auto pv = pipeline::render_preview(
                        *pl, std::vector<Color3f>(
                            winner->ehb_pal.colors.begin(),
                            winner->ehb_pal.colors.end()),
                        /*is_ham=*/false, options.interlace, chipset);
                    if (pv) {
                        winner->planes = *std::move(pl);
                        winner->indices = std::move(dr.indices);
                        winner->rendered = *std::move(pv);
                        winner->total_error = dr.total_error;
                    }
                }
                if (options.on_progress)
                    options.on_progress(1.0f, "done");
            }
            std::vector<Color3f> full_palette(winner->ehb_pal.colors.begin(),
                                              winner->ehb_pal.colors.end());
            PipelineResult result;
            result.rendered = std::move(winner->rendered);
            result.planes = std::move(winner->planes);
            result.palette = std::move(full_palette);
            result.indices = std::move(winner->indices);
            result.mode = mode;
            result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
            result.interlace = options.interlace;
            result.has_transparency = has_transparency;
            result.transparency_mask = tmask;
            result.finalize_psnr(*image, winner->total_error);
            return result;
        }

        // Generate base colors via median-cut, or load from file.
        // With custom palette: use as-is (32 colors expected).
        // Without: reserve index 0 for transparency when needed.
        bool user_pal_ehb = has_user_palette(options);
        bool lock_zero_ehb = !user_pal_ehb && has_transparency;
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
            auto reserves_in_pal_ehb = palette_locks::validate_reserves(
                options.reserves, options.locks, 32, lock_zero_ehb);
            if (!reserves_in_pal_ehb)
                return std::unexpected{reserves_in_pal_ehb.error()};
            std::size_t reserves_ehb = *reserves_in_pal_ehb;
            auto qcount = palette_locks::quant_count(32, options.locks, lock_zero_ehb);
            if (qcount > reserves_ehb) qcount -= reserves_ehb;
            else                       qcount = 1;
            auto quantized = quantize::quantize(*image, qcount,
                                                quantize::Algorithm::pnn,
                                                options.palette_diversity);
            if (!quantized) return std::unexpected{quantized.error()};
            auto assembled = palette_locks::assemble_locked_palette(
                *quantized, options.locks, 32, lock_zero_ehb, chipset, mode);
            base_pal = std::move(assembled.palette);
            base_locked = std::move(assembled.locked);
            for (auto& r : options.reserves) {
                auto i = static_cast<std::size_t>(r.index);
                if (r.index >= 0 && i < 32) {
                    base_pal.colors[i] = palette_locks::to_color(
                        LockSpec{r.index, r.r, r.g, r.b}, chipset, mode);
                    base_locked[i] = true;
                }
            }
        }

        // Pair-aware refinement: jointly optimise the 32 base colours
        // under the hardware-tied half-brite pairing. Skipped when the
        // user supplied an external palette or any base slots are
        // locked — the refinement isn't lock-aware yet.
        bool any_locked = false;
        for (auto v : base_locked) any_locked = any_locked || v;
        if (!user_pal_ehb && !any_locked && !has_transparency
                && base_pal.colors.size() >= 32) {
            palette::refine_ehb_base_palette(
                std::span<Color3f>(base_pal.colors.data(), 32),
                image->pixels(),
                /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
        }
        // 1-opt local search is --best-only; plain EHB stays at
        // PNN + pair-refine for the fast (~0.1s) baseline path.

        // Build full 64-color EHB palette (32 base + 32 half-bright)
        auto ehb_pal = palette::make_ehb_palette(base_pal.colors);

        if (options.match_range)
            preprocess::match_palette_range(*image, ehb_pal);

        // Dither against all 64 colors
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;

        // Build candidate set for dithering: reserved base slots and their
        // half-brite copies are excluded so image pixels can't land on them.
        // (Locked slots remain candidates — image pixels CAN choose them.)
        std::vector<bool> ehb_blocked(64, false);
        for (auto& r : options.reserves) {
            auto i = static_cast<std::size_t>(r.index);
            if (r.index >= 0 && i < 32) {
                ehb_blocked[i] = true;
                ehb_blocked[i + 32] = true;
            }
        }

        dither::DitherResult dither_result;
        if (has_transparency) {
            // Build candidate palette skipping slot 0 (transparent) AND
            // any reserved base/half-brite slots.
            std::vector<Color3f> cand;
            std::vector<std::uint8_t> cand_to_full;
            cand.reserve(63);
            cand_to_full.reserve(63);
            for (std::size_t i = 1; i < 64; ++i) {
                if (ehb_blocked[i]) continue;
                cand.push_back(ehb_pal.colors[i]);
                cand_to_full.push_back(static_cast<std::uint8_t>(i));
            }
            dither_result = dither::apply(*image, cand, dith);
            for (auto& idx : dither_result.indices)
                idx = cand_to_full[idx];
            for (std::size_t i = 0; i < tmask.size() && i < dither_result.indices.size(); ++i)
                if (tmask[i]) dither_result.indices[i] = 0;
        } else {
            // diffuse_raw_buffer + pick_palette_index_with_ostro path —
            // same dither used by EHB+sliced / strips+EHB. Bisect against
            // dither::apply on a fixed 64-colour EHB palette showed it
            // produces +28 SSIMULACRA2 for free (encode_copper with 0
            // changes/line measured 63.76 vs plain EHB's apply path
            // 35.79 on the same palette). The diffuse_raw_buffer path
            // pairs with pick_palette_index_with_ostro's
            // second-nearest tracking which the legacy `apply` route
            // doesn't expose.
            std::vector<color_space::OKLab> pal_lab;
            std::vector<std::uint8_t> cand_to_full;
            pal_lab.reserve(64);
            cand_to_full.reserve(64);
            for (std::size_t i = 0; i < ehb_pal.colors.size(); ++i) {
                if (ehb_blocked[i]) continue;
                pal_lab.push_back(color_space::linear_to_oklab(ehb_pal.colors[i]));
                cand_to_full.push_back(static_cast<std::uint8_t>(i));
            }
            auto w = image->width();
            std::vector<std::uint8_t> indices(w * image->height(), 0);
            float total_err = dither::diffuse_raw_buffer(
                *image, dith,
                [&](const color_space::OKLab& target,
                    std::size_t x, std::size_t y) -> dither::PickResult {
                    std::size_t k = 0;
                    color_space::OKLab chosen{};
                    float thr = dither::pick_palette_index_with_ostro(
                        dith.method, target, pal_lab, x, y,
                        dith.strength, /*k_min=*/0, k, chosen);
                    indices[y * w + x] = cand_to_full[k];
                    return {chosen, thr};
                });
            if (dith.method == dither::Method::dbs) {
                // DBS post-pass also needs the filtered candidate set so
                // it doesn't swap pixels back onto reserved slots.
                std::vector<std::uint8_t> cand_indices(indices.size());
                std::vector<std::uint8_t> full_to_cand(64, 255);
                for (std::size_t k = 0; k < cand_to_full.size(); ++k)
                    full_to_cand[cand_to_full[k]] = static_cast<std::uint8_t>(k);
                for (std::size_t i = 0; i < indices.size(); ++i)
                    cand_indices[i] = full_to_cand[indices[i]];
                dither::apply_dbs_post_pass(
                    *image, cand_indices,
                    [&](std::size_t /*x*/, std::size_t /*y*/)
                        -> std::span<const color_space::OKLab> {
                        return pal_lab;
                    });
                for (std::size_t i = 0; i < indices.size(); ++i)
                    indices[i] = cand_to_full[cand_indices[i]];
            }
            dither_result.indices = std::move(indices);
            dither_result.total_error = total_err;
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

        auto preview = pipeline::render_preview(
            *planes, full_palette,
            /*is_ham=*/false, options.interlace, chipset);
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
        result.finalize_psnr(*image, dither_result.total_error);
        if (tile_active) {
            auto cropped = tile_crop_result(result, tile_w, tile_h, depth,
                                            bitplane::Layout::interleaved);
            if (!cropped) return std::unexpected{cropped.error()};
        }
        return result;
    }

    // --- Copper palette mode ---
    // strips is an extension to sliced — when both are set, the strips encoder
    // owns the per-line copper stream (and adds mid-line MOVEs on top),
    // so skip the sliced branch entirely.
    if (options.copper && !options.scap &&
        !amiga::is_ham(mode) && mode != amiga::Mode::ehb) {
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
        // Build locked slot list for copper from --lock-index specs.
        // --reserve-range entries also feed `locked` so the copper
        // never re-programs them — but their indices ALSO flow into
        // `sliced_excluded` so the dither pass treats them as untouchable.
        std::vector<std::pair<std::size_t, Color3f>> copper_locks;
        for (auto& lock : options.locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            copper_locks.emplace_back(idx,
                palette_locks::to_color(lock, chipset, mode));
        }
        // Validate reserves against the sliced base palette size (1<<depth).
        auto sliced_max_colors = std::size_t{1} << depth;
        auto reserves_in_cap = palette_locks::validate_reserves(
            options.reserves, options.locks, sliced_max_colors,
            options.lock_color0);
        if (!reserves_in_cap) return std::unexpected{reserves_in_cap.error()};
        std::vector<std::size_t> sliced_excluded;
        for (auto& r : options.reserves) {
            auto i = static_cast<std::size_t>(r.index);
            if (r.index >= 0 && i < sliced_max_colors) {
                copper_locks.emplace_back(i, palette_locks::to_color(
                    LockSpec{r.index, r.r, r.g, r.b}, chipset, mode));
                sliced_excluded.push_back(i);
            }
        }

        std::size_t skip_initial_lace = options.interlace ? 2 : 0;

        // Single-pass encoder, factored out so best below can replay
        // it under a parallel jitter sweep without duplicating the
        // argument list.
        // Forward sentinel when CLI flag absent → encode_copper picks
        // its depth/is_ehb-aware default.
        auto spread_r = options.sliced_spread_radius >= 0
            ? static_cast<std::size_t>(options.sliced_spread_radius)
            : std::numeric_limits<std::size_t>::max();
        auto spread_d = options.sliced_spread_decay >= 0.0f
            ? options.sliced_spread_decay : -1.0f;
        auto encode_once = [&](const Image& img,
                               const dither::Settings& d, int diversity) {
            return copper::encode_copper(
                img, depth, d, chipset,
                static_cast<std::size_t>(options.copper_changes),
                copper_user_pal.empty() ? nullptr : &copper_user_pal,
                options.lock_color0, copper_locks,
                diversity,
                skip_initial_lace, options.interlace,
                /*is_ehb=*/false,
                /*on_progress=*/{},
                spread_r, spread_d,
                sliced_excluded);
        };

        Result<copper::CopperResult> copper_result;
        if (options.best) {
            // Plain sliced: 8 jitter seeds. Same shape as strips EHB —
            // 16-colour (or wider) palette so the median-cut basin is
            // less acute than DPF's 8-colour PF2; 8 seeds × 5×4 = 161
            // trials is the sweet spot.
            //
            // copper::encode_copper returns the rendered preview via
            // copper::render_copper_capped on (planes, scanline_palettes).
            // We pre-render here per trial so best_sweep can rank by
            // PSNR.
            struct CapTrial {
                copper::CopperResult result;
                Image rendered;
            };
            float jitter_amp = (chipset == amiga::Chipset::aga)
                ? 0.4f : 1.0f;
            auto sliced_metric = pipeline::parse_best_metric(options.best_metric);
            auto best = pipeline::best_sweep<CapTrial>(
                *image, dith, options.palette_diversity,
                /*jitter_count=*/8,
                [&](const Image& jittered_in,
                    const dither::Settings& d, int div) -> Result<CapTrial> {
                    auto enc = encode_once(jittered_in, d, div);
                    if (!enc) return std::unexpected{enc.error()};
                    auto preview = pipeline::render_preview(
                        enc->planes, enc->base_palette,
                        /*is_ham=*/false, options.interlace, chipset,
                        &enc->scanline_palettes,
                        enc->changes_per_line);
                    if (!preview) return std::unexpected{preview.error()};
                    return CapTrial{*std::move(enc), *std::move(preview)};
                },
                [](const CapTrial& t) -> const Image& { return t.rendered; },
                options.on_progress,
                jitter_amp,
                sliced_metric);
            if (best.has_value()) {
                copper_result = std::move(best->result);
            } else {
                copper_result = encode_once(*image, dith,
                                            options.palette_diversity);
            }
        } else {
            copper_result = encode_once(*image, dith,
                                        options.palette_diversity);
            if (options.on_progress) options.on_progress(1.0f, "done");
        }
        if (!copper_result) return std::unexpected{copper_result.error()};

        // Render preview BEFORE any DPF expansion: render_copper builds a
        // combined palette index from all planes which would land on
        // non-contiguous slots once PF1 zeros are interleaved. Capped
        // version replays the same top-K-by-distance diff clipping the
        // cheader emitter does, so the preview matches what a generated
        // viewer will actually display on hardware.
        auto preview = pipeline::render_preview(
            copper_result->planes, copper_result->base_palette,
            /*is_ham=*/false, options.interlace, chipset,
            &copper_result->scanline_palettes,
            copper_result->changes_per_line);
        if (!preview) return std::unexpected{preview.error()};

        // Dual-playfield expansion (copper path): same as standard branch —
        // expand to 2N planes with PF1 zeroed, prepend pf2_base zero
        // entries to each palette, and shift each copper write target
        // register up by pf2_base so writes land in the upper registers.
        if (use_dpf) {
            auto expanded = bitplane::expand_to_dpf_pf2(copper_result->planes);
            if (!expanded) return std::unexpected{expanded.error()};
            copper_result->planes = *std::move(expanded);
            auto pf2_base = std::size_t{1} << (copper_result->planes.depth / 2);
            auto shift_palette = [&](std::vector<Color3f>& p) {
                std::vector<Color3f> shifted(pf2_base, Color3f{0, 0, 0});
                shifted.insert(shifted.end(), p.begin(), p.end());
                p = std::move(shifted);
            };
            for (auto& pal : copper_result->scanline_palettes)
                shift_palette(pal);
            for (auto& line : copper_result->scanline_changes)
                for (auto& ch : line)
                    ch.reg = static_cast<std::uint8_t>(ch.reg + pf2_base);
            copper_result->num_colors += pf2_base;
        }

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
        result.dpf = use_dpf;
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
        result.finalize_psnr(*image, copper_result->total_error);
        return result;
    }

    // --- strips (mid-line palette swaps) ---
    // Two variants:
    //   * DPF: OCS lores DPF (depth=3), 8 PF2 colours. cpp export ready.
    //   * EHB: OCS EHB (6bpp, 32 base + 32 hardware half-brites). No
    //     cpp export wired yet — the encoder produces planes + preview.
    if (options.scap) {
        bool strips_ehb = mode == amiga::Mode::ehb;
        bool strips_dpf = options.dual_playfield && mode == amiga::Mode::lores;
        bool strips_ham6 = mode == amiga::Mode::ham6;
        if (chipset != amiga::Chipset::ocs || options.interlace ||
            (!strips_ehb && !strips_dpf && !strips_ham6)) {
            return std::unexpected{Error{
                ErrorCode::unsupported_mode,
                "Strips requires OCS (no interlace) with --dpf lores depth=3, "
                "--mode ehb, or --mode ham6",
            }};
        }
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }
        dither::Settings strips_dith;
        strips_dith.method = parse_dither(options.dither);
        strips_dith.strength = options.dither_strength;
        strips_dith.error_clamp = options.error_clamp;

        std::vector<Color3f> strips_user_pal;
        if (has_user_palette(options)) {
            auto loaded = load_user_palette(options);
            if (!loaded) return std::unexpected{loaded.error()};
            strips_user_pal = std::move(loaded->colors);
        }
        std::span<const Color3f> strips_user_pal_span(strips_user_pal);

        Result<strips::ScapResult> strips_res =
            strips_ham6
            ? strips::encode_strips_ham6_ocs(
                *image,
                static_cast<int>(image->width()),
                static_cast<int>(image->height()),
                options.lock_color0,
                strips_dith,
                static_cast<std::size_t>(options.copper_changes),
                options.palette_diversity,
                options.on_progress,
                options.sliced_spread_radius,
                options.sliced_spread_decay,
                options.best,
                options.best_metric,
                strips_user_pal_span,
                (options.ham_metric == "srgb-mse")
                    ? ham::HamMetric::srgb_mse
                    : ham::HamMetric::oklab2)
            : strips_ehb
            ? [&] {
                // strips-EHB reserves: build (idx, color) pairs for the
                // 32-base palette. encode_strips_ehb_ocs forwards them to
                // encode_copper as locked + dither-excluded, AND blocks
                // the mid-line strips swap planner from targeting them.
                std::vector<std::pair<std::size_t, Color3f>> strips_ehb_reserves;
                for (auto& r : options.reserves) {
                    auto i = static_cast<std::size_t>(r.index);
                    if (r.index >= 0 && i < 32) {
                        strips_ehb_reserves.emplace_back(i,
                            palette_locks::to_color(
                                LockSpec{r.index, r.r, r.g, r.b},
                                chipset, mode));
                    }
                }
                return strips::encode_strips_ehb_ocs(
                    *image,
                    static_cast<int>(image->width()),
                    static_cast<int>(image->height()),
                    options.lock_color0,
                    strips_dith,
                    static_cast<std::size_t>(options.copper_changes),
                    options.palette_diversity,
                    options.strips_debug,
                    options.on_progress,
                    options.best,
                    options.best_metric,
                    options.sliced_spread_radius,
                    options.sliced_spread_decay,
                    strips_user_pal_span,
                    strips_ehb_reserves);
            }()
            : strips::encode_strips_dpf_ocs(
                *image,
                static_cast<int>(image->width()),
                static_cast<int>(image->height()),
                options.lock_color0,
                strips_dith,
                options.strips_debug,
                static_cast<std::size_t>(options.copper_changes),
                options.palette_diversity,
                options.on_progress,
                options.best,
                options.best_metric,
                options.sliced_spread_radius,
                options.sliced_spread_decay,
                strips_user_pal_span);
        if (!strips_res) return std::unexpected{strips_res.error()};

        PipelineResult result;
        result.rendered = std::move(strips_res->rendered);
        result.planes = std::move(strips_res->planes);
        result.palette = std::move(strips_res->palette);
        result.mode = mode;
        result.hires = false;
        result.interlace = false;
        result.dpf = options.dual_playfield;
        result.scap = true;
        // The cheader emitter writes COLORxx MOVEs for any reg in 0..31
        // (`0x0180 + reg*2`), so it handles both DPF (regs 8-15) and EHB
        // (regs 0-31) variants of the strips copper list identically.
        result.strips_line_moves = std::move(strips_res->line_moves);
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        result.copper_changes = strips_res->avg_changes_per_line;
        result.max_moves_per_line = strips_res->max_moves_per_line;
        result.strips_avg_total_moves_per_line   = strips_res->avg_total_moves_per_line;
        result.strips_avg_hblank_moves_per_line  = strips_res->avg_hblank_moves_per_line;
        result.strips_max_hblank_moves_per_line  = strips_res->max_hblank_moves_per_line;
        result.strips_avg_visible_moves_per_line = strips_res->avg_visible_moves_per_line;
        result.strips_max_visible_moves_per_line = strips_res->max_visible_moves_per_line;
        result.strips_slot_count                 = strips_res->slot_table.slots.size();
        result.finalize_psnr(*image, strips_res->total_error);
        return result;
    }

    // --- Standard bitplane modes ---

    // Force transparent pixels to black before quantization/encoding
    if (has_transparency) {
        for (std::size_t i = 0; i < tmask.size(); ++i)
            if (tmask[i]) image->pixels()[i] = Color3f{0, 0, 0};
    }

    auto max_colors = std::size_t{1} << depth;

    // ----- Plain lores/hires + --best multi-restart sweep ---------------
    // Same shape as the EHB plain-best path above (api.cpp:1922) but
    // for indexed lores/hires without copper or strips. Each trial gets
    // a fresh jittered source → fresh median-cut basin → fresh palette,
    // then refine_with_dither + dither + render preview. Ranked by
    // best_metric (SSIMULACRA2 by default), winner's state populates
    // the PipelineResult and we return early — bypassing the general
    // single-pass encode body below.
    //
    // Gated to clean cases only: no user palette, no locks/reserves/
    // pins, no transparency. These features touch palette generation
    // in ways the trial closure doesn't replicate; the user's setup
    // already constrains those to specific slots so a sweep won't
    // help anyway.
    bool lores_plain_best_eligible =
        options.best &&
        (mode == amiga::Mode::lores ||
         mode == amiga::Mode::lores_interlace ||
         mode == amiga::Mode::hires ||
         mode == amiga::Mode::hires_interlace) &&
        !options.copper && !options.scap && !options.dual_playfield &&
        !has_user_palette(options) && !has_transparency &&
        options.locks.empty() && options.reserves.empty() &&
        options.pins.empty();
    if (lores_plain_best_eligible) {
        struct LoresTrial {
            Palette pal;
            std::vector<std::uint8_t> indices;
            bitplane::BitplaneData planes;
            Image rendered;
            float total_error;
        };
        dither::Settings base_dith;
        base_dith.method = parse_dither(options.dither);
        base_dith.strength = options.dither_strength;
        base_dith.error_clamp = options.error_clamp;
        auto encode_once = [&](const Image& img,
                               const dither::Settings& d,
                               int diversity) -> Result<LoresTrial> {
            auto q = quantize::quantize(img, max_colors,
                                        quantize_algo(chipset, mode),
                                        diversity);
            if (!q) return std::unexpected{q.error()};
            Palette pal_t = std::move(*q);
            snap_to_chipset(pal_t, chipset, mode);
            auto pal_size_t = std::min(pal_t.size(), max_colors);
            // Dither-aware refinement on the trial palette. Same gates
            // as the main path: skip for chunky/EGA/CGA/atari-hi.
            if (options.refine_iterations > 0 &&
                d.method != dither::Method::none &&
                !amiga::is_chunky(mode) && !amiga::is_cga(mode) &&
                !amiga::is_ega(mode) && !amiga::is_atari_hi(mode)) {
                auto refined = quantize::refine_with_dither(
                    img,
                    Palette{"refined",
                            {pal_t.colors.begin(),
                             pal_t.colors.begin() +
                                 static_cast<std::ptrdiff_t>(pal_size_t)}},
                    d, chipset, mode,
                    static_cast<std::size_t>(options.refine_iterations));
                if (refined) {
                    pal_t.colors = std::move(refined->colors);
                    pal_size_t = pal_t.colors.size();
                }
            }
            std::span<const Color3f> pal_span_t{pal_t.colors.data(), pal_size_t};
            auto dr = dither::apply(img, pal_span_t, d);
            auto pl = bitplane::encode(dr.indices, img.width(), img.height(),
                                       depth);
            if (!pl) return std::unexpected{pl.error()};
            auto pv = pipeline::render_preview(*pl, std::vector<Color3f>(
                                                pal_span_t.begin(),
                                                pal_span_t.end()),
                                                /*is_ham=*/false,
                                                options.interlace, chipset);
            if (!pv) return std::unexpected{pv.error()};
            return LoresTrial{
                std::move(pal_t), std::move(dr.indices), *std::move(pl),
                *std::move(pv), dr.total_error,
            };
        };
        auto bm = pipeline::parse_best_metric(options.best_metric);
        auto winner = pipeline::best_sweep<LoresTrial>(
            *image, base_dith, options.palette_diversity,
            /*jitter_count=*/8,
            encode_once,
            [](const LoresTrial& t) -> const Image& { return t.rendered; },
            options.on_progress, /*jitter_amplitude=*/1.0f, bm);
        if (winner) {
            auto pal_size_w = std::min(winner->pal.size(), max_colors);
            std::vector<Color3f> used_pal(
                winner->pal.colors.begin(),
                winner->pal.colors.begin() +
                    static_cast<std::ptrdiff_t>(pal_size_w));

            PipelineResult result;
            result.rendered = std::move(winner->rendered);
            result.planes = std::move(winner->planes);
            result.palette = std::move(used_pal);
            result.indices = std::move(winner->indices);
            result.mode = mode;
            result.hires = compound_hires ||
                           amiga::get_mode_params(mode).is_hires;
            result.interlace = options.interlace;
            result.dpf = false;
            result.aga = is_aga;
            result.has_transparency = false;
            result.finalize_psnr(*image, winner->total_error);
            return result;
        }
        // Fall through to single-pass on sweep failure.
    }

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
    auto lock_zero = !user_pal && options.lock_color0 &&
                        (has_transparency || !is_atari);

    // Validate locks/pins (no-op for HAM/copper paths above which return earlier).
    // Locks override the implicit reserve-zero rule when index 0 is locked.
    if (auto v = palette_locks::validate_locks(options.locks, max_colors); !v)
        return std::unexpected{v.error()};
    if (auto v = palette_locks::validate_pins(options.pins, options.locks,
                                              max_colors,
                                              image->width(), image->height(),
                                              lock_zero); !v)
        return std::unexpected{v.error()};
    auto reserves_in_pal = palette_locks::validate_reserves(
        options.reserves, options.locks, max_colors, lock_zero);
    if (!reserves_in_pal) return std::unexpected{reserves_in_pal.error()};
    std::size_t reserve_count = *reserves_in_pal;
    // Build a reserved_mask the dither path uses to exclude these slots
    // from the candidate set (locks DON'T appear here — they remain
    // valid dither targets per their lock-not-reserve semantics).
    std::vector<bool> reserved_mask(max_colors, false);
    for (auto& r : options.reserves) {
        auto i = static_cast<std::size_t>(r.index);
        if (r.index >= 0 && i < max_colors) reserved_mask[i] = true;
    }

    Palette pal;
    std::vector<bool> locked_mask(max_colors, false);
    // CGA-320 only: set to the 0x3D9 mode-control-2 byte the DJGPP viewer
    // must write to match the auto-picked palette variant (bg=0 since the
    // API layer doesn't expose --cga-bg). 0xFF means "not set".
    std::uint8_t cga_mode_ctrl2 = 0xFF;
    if (amiga::is_atari_hi(mode)) {
        pal.colors = {Color3f{1.0f, 1.0f, 1.0f}, Color3f{0.0f, 0.0f, 0.0f}};
    } else if (mode == amiga::Mode::cga_640) {
        // CGA 2-color monochrome: fixed black + white.
        pal.colors = {Color3f{0.0f, 0.0f, 0.0f}, Color3f{1.0f, 1.0f, 1.0f}};
    } else if (mode == amiga::Mode::cga_320) {
        // CGA 320x200 4-color: auto-select the hardware palette whose 4
        // colors best cover the image. We try all four variants
        // (p0-low green/red/brown, p0-high light-green/red/yellow,
        //  p1-low cyan/magenta/grey, p1-high cyan/magenta/white)
        // with the default black background and pick the one with
        // lowest sum-of-nearest-OKLab-distance² across the image.
        palette::CgaPalette best = palette::CgaPalette::p1_high;
        float best_err = std::numeric_limits<float>::infinity();
        for (auto p : {palette::CgaPalette::p0_low,
                       palette::CgaPalette::p0_high,
                       palette::CgaPalette::p1_low,
                       palette::CgaPalette::p1_high}) {
            auto pal4 = palette::cga_build_palette(p, 0);
            std::array<color_space::OKLab, 4> pal_lab;
            for (std::size_t i = 0; i < 4; ++i)
                pal_lab[i] = color_space::linear_to_oklab(pal4[i]);
            float err = 0.0f;
            for (std::size_t y = 0; y < image->height(); ++y) {
                for (std::size_t x = 0; x < image->width(); ++x) {
                    auto lab = color_space::linear_to_oklab((*image)[x, y]);
                    float d_best = std::numeric_limits<float>::infinity();
                    for (auto& pl : pal_lab) {
                        float dL = lab.L - pl.L;
                        float da = lab.a - pl.a;
                        float db = lab.b - pl.b;
                        float d = dL*dL + da*da + db*db;
                        if (d < d_best) d_best = d;
                    }
                    err += d_best;
                }
            }
            if (err < best_err) { best_err = err; best = p; }
        }
        auto pal4 = palette::cga_build_palette(best, 0);
        pal.colors.assign(pal4.begin(), pal4.end());
        // Encode picked variant into the hardware 0x3D9 byte: bit5 = palette
        // select (p0/p1), bit4 = intensity (low/high), bits 0-3 = bg (= 0).
        cga_mode_ctrl2 = static_cast<std::uint8_t>(static_cast<int>(best) << 4);
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
    } else if (mode == amiga::Mode::ega_320 || mode == amiga::Mode::ega_640) {
        // Same fix as main.cpp: palette order must be kCgaHw exactly so
        // bitplane index i = kCga IRGB index i; assemble_locked_palette
        // with lock_color0 otherwise shifts the palette up by 1.
        pal.colors.reserve(16);
        for (auto hex : palette::kCgaHw)
            pal.colors.push_back(color_space::srgb_hex_to_linear(hex));
    } else {
        auto qcount = palette_locks::quant_count(max_colors, options.locks,
                                                 lock_zero);
        // Reserves take additional slots out of the quantizer's budget.
        // Floor at 1 — validate_reserves already rejected the all-reserved case.
        if (qcount > reserve_count) qcount -= reserve_count;
        else                        qcount = 1;
        Result<Palette> quantized;
        if (amiga::is_ega(mode)) {
            // All EGA modes on a 5154 ECD support 16 of the 64-color
            // IrgbIRGB gamut — the monitor reads all 6 signal pins at
            // both 15.75 kHz (200-line) and 21.85 kHz (350-line) hsync
            // rates. Dedicated histogram quantizer picks K distinct
            // entries; continuous median-cut + post-snap collapses ~30%
            // of slots on typical images. Pre-snap the image to the
            // EGA gamut first so the quantizer is consistent with it.
            Image snapped(image->width(), image->height());
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x)
                    snapped[x, y] = palette::quantize_to_ega((*image)[x, y]);
            quantized = quantize::ega_histogram(snapped, qcount);
        } else {
            quantized = quantize::quantize(*image, qcount,
                                           quantize_algo(chipset, mode),
                                           options.palette_diversity);
        }
        if (!quantized) return std::unexpected{quantized.error()};
        // Snap palette to discrete gamut where applicable (STF/EGA/VGA).
        if (amiga::is_stf(mode) || amiga::is_vga(mode))
            snap_to_chipset(*quantized, chipset, mode);
        auto assembled = palette_locks::assemble_locked_palette(
            *quantized, options.locks, max_colors, lock_zero, chipset, mode);
        pal = std::move(assembled.palette);
        locked_mask = std::move(assembled.locked);
        // Overlay reserved slots: snap each user-supplied colour to the
        // chipset/mode precision and drop it into the slot. Mark in
        // locked_mask so refine treats them as fixed; reserved_mask is
        // separate (refine doesn't see it — refine only operates on
        // unlocked slots and reserved slots are already locked).
        for (auto& r : options.reserves) {
            auto i = static_cast<std::size_t>(r.index);
            if (r.index >= 0 && i < max_colors) {
                pal.colors[i] = palette_locks::to_color(
                    LockSpec{r.index, r.r, r.g, r.b}, chipset, mode);
                locked_mask[i] = true;
            }
        }
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

    // Dither-aware palette refinement (auto-palette only, 4 iterations).
    // Skip for:
    //   - CGA modes: hardware-fixed palettes can't be reprogrammed.
    //   - Chunky VGA (256 colors): median-cut's initial centroids are
    //     already near-optimal against the 18-bit DAC grid, and empirically
    //     refinement HURTS PSNR (~2 dB worse on fantasy.png) while costing
    //     4× dither passes of extra time.
    //   - EGA modes: ega_histogram_quantize already picks near-optimal
    //     16-of-64 slots from the IrgbIRGB gamut with collision avoidance.
    //     Refinement's per-iteration snap-to-gamut collapses centroids onto
    //     the same gamut entry (reduces 16 slots → ~11 effective colors)
    //     and drops PSNR by 3+ dB.
    if (options.refine_iterations > 0 && !has_user_palette(options) &&
        dith.method != dither::Method::none && reserve_count == 0 &&
        !amiga::is_cga(mode) && !amiga::is_chunky(mode) &&
        !amiga::is_ega(mode) && !amiga::is_atari_hi(mode)) {
        auto refined = quantize::refine_with_dither(
            *image,
            Palette{"refined", {pal.colors.begin(),
                                pal.colors.begin() + static_cast<std::ptrdiff_t>(pal_size)}},
            dith, chipset, mode,
            static_cast<std::size_t>(options.refine_iterations), locked_mask);
        if (refined) {
            pal.colors = std::move(refined->colors);
            pal_size = pal.colors.size();
        }
    }

    std::span<const Color3f> pal_span{pal.colors.data(), pal_size};

    dither::DitherResult dither_result;
    // Build the dither candidate set:
    //   - skip reserved slots (hard-reserve: image content may not land there).
    //   - skip slot 0 too when has_transparency (legacy "transparent = 0" rule).
    if (reserve_count > 0 || has_transparency) {
        std::vector<Color3f> cand_pal;
        std::vector<std::uint8_t> cand_to_full;
        cand_pal.reserve(pal_size);
        cand_to_full.reserve(pal_size);
        for (std::size_t i = 0; i < pal_size; ++i) {
            if (has_transparency && i == 0) continue;
            if (reserved_mask[i]) continue;
            cand_pal.push_back(pal.colors[i]);
            cand_to_full.push_back(static_cast<std::uint8_t>(i));
        }
        std::span<const Color3f> dither_span{cand_pal.data(), cand_pal.size()};
        dither_result = dither::apply(*image, dither_span, dith);
        for (auto& idx : dither_result.indices) idx = cand_to_full[idx];
        if (has_transparency) {
            for (std::size_t i = 0;
                 i < tmask.size() && i < dither_result.indices.size(); ++i)
                if (tmask[i]) dither_result.indices[i] = 0;
        }
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
    // See main.cpp for rationale. DOS planar (EGA + VGA 10h/12h) uses
    // Layout::standard so planes can be blitted contiguously via the
    // sequencer map mask.
    bool dos_planar = (amiga::is_ega(mode) || amiga::is_vga(mode))
                      && !amiga::is_chunky(mode);
    auto bp_layout = amiga::is_atari(mode)
        ? bitplane::Layout::word_interleaved
        : dos_planar ? bitplane::Layout::standard
                     : bitplane::Layout::interleaved;
    auto planes = bitplane::encode(dither_result.indices,
                                   image->width(), image->height(),
                                   depth, bp_layout);
    if (!planes) return std::unexpected{planes.error()};

    // Build used palette vector
    std::vector<Color3f> used_palette(pal_span.begin(), pal_span.end());

    // Render preview before any DPF expansion: the encoded N-plane data is
    // what the hardware would read out of PF2, and bitplane::render gives a
    // direct color preview. (After expansion the "combined" indices read
    // from all planes would be non-contiguous, so render the unexpanded
    // image then transform the planes/palette to their final DPF layout.)
    auto preview = pipeline::render_preview(
        *planes, used_palette,
        /*is_ham=*/false, options.interlace, chipset);

    // Dual-playfield expansion: place the encoded N-plane image into the
    // even hardware planes (PF2), leave the odd planes (PF1) zeroed, and
    // shift the palette into the upper color registers so PF2 lookups land
    // there (8-15 OCS / 16-31 AGA). Also offset the per-pixel indices so
    // palettized PNG export keeps referencing the correct slots.
    if (use_dpf) {
        auto expanded = bitplane::expand_to_dpf_pf2(*planes);
        if (!expanded) return std::unexpected{expanded.error()};
        planes = *std::move(expanded);
        auto pf2_base = std::size_t{1} << (planes->depth / 2);
        std::vector<Color3f> shifted(pf2_base, Color3f{0, 0, 0});
        shifted.insert(shifted.end(), used_palette.begin(), used_palette.end());
        used_palette = std::move(shifted);
        for (auto& idx : dither_result.indices)
            idx = static_cast<std::uint8_t>(idx + pf2_base);
    }
    if (!preview) return std::unexpected{preview.error()};

    PipelineResult result;
    result.rendered = *std::move(preview);
    result.planes = *std::move(planes);
    result.palette = std::move(used_palette);
    result.indices = std::move(dither_result.indices);
    result.mode = mode;
    result.hires = compound_hires || amiga::get_mode_params(mode).is_hires;
    result.interlace = options.interlace;
    result.dpf = use_dpf;
    result.aga = is_aga;
    result.has_transparency = has_transparency;
    result.transparency_mask = tmask;
    if (has_transparency) {
        for (std::size_t i = 0; i < tmask.size(); ++i)
            if (tmask[i]) result.rendered.pixels()[i] = Color3f{0, 0, 0};
    }
    result.finalize_psnr(*image, dither_result.total_error);
    result.cga_mode_ctrl2 = cga_mode_ctrl2;
    if (tile_active) {
        auto cropped = tile_crop_result(result, tile_w, tile_h, depth, bp_layout);
        if (!cropped) return std::unexpected{cropped.error()};
    }
    return result;
}

namespace {  // reopen anon for the trailing helpers

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
    int sliced_grid_entries = 0;
    int strips_op_count = 0;
    int max_moves = 0;
    if (p.copper && !p.scanline_changes.empty()) {
        auto h = p.rendered.height();
        auto cpl = p.changes_per_line;
        sliced_grid_entries = static_cast<int>(h * cpl);
        r.changesPerLine = static_cast<int>(cpl);
        max_moves = static_cast<int>(p.max_moves_per_line);
    }
    if (p.scap && !p.strips_line_moves.empty()) {
        std::size_t total_ops = 0;
        std::size_t per_line_max = 0;
        for (auto& moves : p.strips_line_moves) {
            total_ops += moves.size();
            per_line_max = std::max(per_line_max, moves.size());
        }
        strips_op_count = static_cast<int>(total_ops);
        max_moves = std::max(max_moves, static_cast<int>(per_line_max));
    }
    auto sb = compute_size_breakdown(
        r.planeBytes,
        static_cast<int>(p.palette.size()),
        p.aga, sliced_grid_entries, strips_op_count, r.height, max_moves);
    r.copperBytes = sb.copper_bytes;
    r.diskBytes = sb.disk_bytes;
    r.chipBytes = sb.chip_bytes;
    r.maxMovesPerLine = max_moves;
    r.quantError = p.quant_error;
    r.psnr = p.psnr;
    r.s2 = p.ssimulacra2_score;
    r.hasTransparency = p.has_transparency;
    r.genesisUniqueTiles = static_cast<int>(p.genesis_unique_tiles);
    r.tileDataBytes      = static_cast<int>(p.tile_data_bytes);
    r.genesisTotalCells  = static_cast<int>(p.genesis_total_cells);
    // c64 charset diagnostic: copy raw_frame so the web preview can
    // render the generated charset under the image. Cheap (a few KB
    // for typical charsets) and only populated when cols/rows are set.
    if (amiga::is_c64_charset(p.mode) && p.c64_cols > 0 && p.c64_rows > 0) {
        r.c64CharsetData = p.raw_frame;
        r.c64Mc1     = p.c64_mc1;
        r.c64Mc2     = p.c64_mc2;
        r.c64BgColor = p.c64_bg_color;
    }
    // Genesis tile diagnostic: u16 vectors get serialised as little-endian
    // 2-byte sequences so JS can DataView them with .getUint16(off, true).
    if (amiga::is_genesis(p.mode) && !p.genesis_tile_bytes.empty()) {
        r.genesisTileBytes = p.genesis_tile_bytes;
        auto u16_to_bytes = [](const std::vector<std::uint16_t>& src) {
            std::vector<std::uint8_t> out(src.size() * 2);
            for (std::size_t i = 0; i < src.size(); ++i) {
                out[i * 2]     = static_cast<std::uint8_t>(src[i] & 0xFF);
                out[i * 2 + 1] = static_cast<std::uint8_t>((src[i] >> 8) & 0xFF);
            }
            return out;
        };
        r.genesisTilemapBytes  = u16_to_bytes(p.genesis_tilemap_cells);
        r.genesisPaletteBytes  = u16_to_bytes(p.genesis_palette_words);
    }
    // SNES Mode 7 tile diagnostic: split packed_bytes into tilemap (first
    // 16 KB) + tile data (rest). Palette is 256 RGB triples for 256 mode,
    // empty for Direct (BBGGGRRR pixels self-decode).
    if (amiga::is_snes(p.mode) && !p.raw_frame.empty()) {
        constexpr std::size_t kTilemapBytes = 128 * 128;
        if (p.raw_frame.size() >= kTilemapBytes) {
            r.snesTilemapBytes.assign(
                p.raw_frame.begin(),
                p.raw_frame.begin()
                    + static_cast<std::ptrdiff_t>(kTilemapBytes));
            r.snesTileBytes.assign(
                p.raw_frame.begin()
                    + static_cast<std::ptrdiff_t>(kTilemapBytes),
                p.raw_frame.end());
        }
        if (!p.palette.empty()) {
            r.snesPaletteBytes.reserve(p.palette.size() * 3);
            for (auto& c : p.palette) {
                auto srgb = color_space::linear_to_srgb(c);
                auto chan = [](float v) {
                    return static_cast<std::uint8_t>(
                        std::clamp(static_cast<int>(std::lround(v * 255.0f)),
                                    0, 255));
                };
                r.snesPaletteBytes.push_back(chan(srgb.r));
                r.snesPaletteBytes.push_back(chan(srgb.g));
                r.snesPaletteBytes.push_back(chan(srgb.b));
            }
        }
    }
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
    iff_opts.dpf = result->dpf;
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

// Build a "c_array_block" string emitting `name[]` filled with `bytes`
// as 0xNN values, 16 per row. Used by the c64-charset / SNES inline
// header writers.
static std::string emit_byte_array(std::string_view name,
                                   std::span<const std::uint8_t> bytes,
                                   std::string_view type = "unsigned char") {
    std::string out;
    out.reserve(bytes.size() * 6 + 64);
    out += std::format("static const {} {}[] = {{\n   ",
                        type, name);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out += std::format(" 0x{:02X}", bytes[i]);
        if (i + 1 < bytes.size()) out += ',';
        if ((i + 1) % 16 == 0 && i + 1 < bytes.size()) out += "\n   ";
    }
    out += "\n};\n\n";
    return out;
}

// Minimal SNES Mode 7 .h: tiles + tilemap + (256-mode) palette.
// Direct mode skips the palette since pixels self-decode (BBGGGRRR).
static std::string snes_header(const PipelineResult& p,
                                std::string_view sym) {
    std::string out;
    out += std::format(
        "// Generated by png2amiga. Do not edit.\n"
        "//   SNES Mode 7 — {} tile(s), {}×{} tilemap.\n\n"
        "#pragma once\n\n",
        p.genesis_unique_tiles, 128, 128);
    constexpr std::size_t kTilemapBytes = 128 * 128;
    if (p.raw_frame.size() < kTilemapBytes) return out;
    auto tilemap = std::span<const std::uint8_t>(
        p.raw_frame.data(), kTilemapBytes);
    auto tile_bytes = std::span<const std::uint8_t>(
        p.raw_frame.data() + kTilemapBytes,
        p.raw_frame.size() - kTilemapBytes);
    out += std::format("#define {}_TILES   {}\n",
                        sym, p.genesis_unique_tiles);
    out += std::format("#define {}_TILE_BYTES   {}\n\n",
                        sym, tile_bytes.size());
    out += emit_byte_array(std::string(sym) + "_tiles", tile_bytes);
    out += emit_byte_array(std::string(sym) + "_tilemap", tilemap);
    if (!p.palette.empty()) {
        // Emit BGR555 words (LE 2-byte pairs).
        std::vector<std::uint8_t> pal_bytes;
        pal_bytes.reserve(p.palette.size() * 2);
        for (auto& c : p.palette) {
            auto w = console_color::to_bgr555_word(c);
            pal_bytes.push_back(static_cast<std::uint8_t>(w & 0xFF));
            pal_bytes.push_back(static_cast<std::uint8_t>((w >> 8) & 0xFF));
        }
        out += emit_byte_array(std::string(sym) + "_palette",
                                pal_bytes);
    }
    return out;
}

ConvertResult convert_cheader(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);

    auto sym = options.symbol_name.empty()
        ? std::string{"img"} : options.symbol_name;

    // C64 charset: route through the existing c64::charset_header writer.
    // raw_frame holds charset_data + screen + color; cols/rows/glyph-count
    // come back through PipelineResult.
    if (amiga::is_c64_charset(result->mode)) {
        c64::EncodeResult enc;
        enc.cols = result->c64_cols;
        enc.rows = result->c64_rows;
        enc.unique_glyphs = result->c64_unique_glyphs;
        enc.bg_color = result->c64_bg_color;
        enc.mc1 = result->c64_mc1;
        enc.mc2 = result->c64_mc2;
        std::size_t charset_bytes = enc.unique_glyphs * 8;
        std::size_t cells = enc.cols * enc.rows;
        if (result->raw_frame.size() < charset_bytes + 2 * cells)
            return make_error("c64-charset .h: raw_frame too small");
        enc.bitmap.assign(
            result->raw_frame.begin(),
            result->raw_frame.begin()
                + static_cast<std::ptrdiff_t>(charset_bytes));
        enc.screen_ram.assign(
            result->raw_frame.begin()
                + static_cast<std::ptrdiff_t>(charset_bytes),
            result->raw_frame.begin()
                + static_cast<std::ptrdiff_t>(charset_bytes + cells));
        enc.color_ram.assign(
            result->raw_frame.begin()
                + static_cast<std::ptrdiff_t>(charset_bytes + cells),
            result->raw_frame.begin()
                + static_cast<std::ptrdiff_t>(charset_bytes + 2 * cells));
        auto pal_choice = c64::parse_palette(options.c64_palette);
        bool mc = result->mode == amiga::Mode::c64_charset_multicolor;
        auto hdr = c64::charset_header(enc, sym, mc, pal_choice);
        if (!hdr) return make_error(hdr.error().message);
        std::vector<std::uint8_t> bytes(hdr->begin(), hdr->end());
        return make_result(std::move(bytes), *result);
    }

    // Sega Genesis: SGDK-compatible header via cheader_genesis.
    if (amiga::is_genesis(result->mode)
        && !result->genesis_tile_bytes.empty()) {
        cheader_genesis::GenesisHeaderOptions hopts;
        hopts.tile_bytes   = result->genesis_tile_bytes;
        hopts.tilemap      = result->genesis_tilemap_cells;
        hopts.palette      = result->genesis_palette_words;
        hopts.width_pixels  = result->rendered.width();
        hopts.height_pixels = result->rendered.height();
        hopts.symbol = sym;
        auto hdr = cheader_genesis::generate(hopts);
        if (!hdr) return make_error(hdr.error().message);
        return make_result(std::move(*hdr), *result);
    }

    // SNES Mode 7: minimal inline header (tiles + tilemap + palette).
    if (amiga::is_snes(result->mode)) {
        auto txt = snes_header(*result, sym);
        std::vector<std::uint8_t> bytes(txt.begin(), txt.end());
        return make_result(std::move(bytes), *result);
    }

    auto ch_opts = pipeline::make_ch_opts({
        .symbol_override = options.symbol_name,
        .aga = result->aga,
        .dpf = result->dpf,
        .total_unique_colors =
            static_cast<std::size_t>(count_unique_colors(result->rendered)),
    });
    // sliced per-line copper data, when present.
    if (result->copper && !result->scanline_changes.empty()) {
        ch_opts.copper_changes = &result->scanline_changes;
        ch_opts.copper_changes_per_line = result->changes_per_line;
    }
    // strips mid-line MOVE list, when present. Mirrors convert_viewer's
    // wiring so the .h carries the same _strips_copper_list[] UWORD array
    // (data only — no init code; that lives in the .cpp viewer).
    if (result->scap && !result->strips_line_moves.empty()) {
        ch_opts.strips_line_moves = &result->strips_line_moves;
        bool strips_ehb  = result->mode == amiga::Mode::ehb;
        bool strips_ham6 = result->mode == amiga::Mode::ham6;
        auto& table = strips_ehb  ? strips::kStrips6bplEhb
                    : strips_ham6 ? strips::kStrips6bplHam6
                                : strips::kStrips6bplOcs;
        ch_opts.strips_label = strips_ehb  ? "strips_ehb_ocs"
                           : strips_ham6 ? "strips_ham6_ocs"
                                       : "strips_dpf_ocs";
        ch_opts.strips_anchor_hpos = table.line_gate_hpos;
        ch_opts.strips_total_planes = table.total_planes;
    }
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

    // DOS modes: 16-bit C viewer via cheader_dos_c, targeting
    // ia16-elf-gcc (real-mode 8088+; no DPMI, no 32-bit code).
    // Build the mode-specific raw bytes + palette bytes, then call
    // cheader_dos_c::generate which dispatches per mode.
    if (amiga::is_vga(result->mode) || amiga::is_ega(result->mode) ||
        amiga::is_cga(result->mode)) {
        using amiga::Mode;
        cheader_dos_c::Options opts;
        if (!options.symbol_name.empty())
            opts.symbol_name = options.symbol_name;

        auto mode = result->mode;
        std::size_t w = result->rendered.width();
        std::size_t h = result->rendered.height();
        std::vector<std::uint8_t> raw;
        std::vector<std::uint8_t> palette_bytes;

        if (mode == Mode::cga_320 || mode == Mode::cga_640 ||
            mode == Mode::cga_composite) {
            // 16 KB banked CGAPIC frame.
            if (!result->raw_frame.empty())
                raw = result->raw_frame;
            else
                raw = cheader_dos_c::pack_cga_banked(
                    result->indices, w, h, mode);
            if (mode == Mode::cga_320 && result->cga_mode_ctrl2 != 0xFF)
                opts.cga_mode_ctrl2 = result->cga_mode_ctrl2;
        } else if (mode == Mode::cga_text80x100) {
            raw = result->raw_frame;   // char+attr pairs
        } else if (mode == Mode::ega_320 || mode == Mode::ega_640 ||
                   mode == Mode::ega_hi) {
            raw = result->planes.data;
            // EGA palette: CGA-compat IRGB for 200-line (ega_320/640),
            // full IrgbIRGB via linear_to_ega+ega_to_hw for 350-line.
            palette_bytes.reserve(16);
            if (mode == Mode::ega_hi) {
                for (auto& c : result->palette) {
                    auto rrggbb = palette::linear_to_ega(c);
                    palette_bytes.push_back(palette::ega_to_hw(rrggbb));
                }
                while (palette_bytes.size() < 16) palette_bytes.push_back(0);
            } else {
                for (std::size_t i = 0; i < 16; ++i)
                    palette_bytes.push_back(static_cast<std::uint8_t>(
                        (i & 0x07) | ((i & 0x08) << 1)));
            }
        } else if (mode == Mode::vga_13h) {
            // Chunky 256-color: build 768-byte DAC.
            palette_bytes.assign(768, 0);
            std::size_t n = std::min<std::size_t>(result->palette.size(), 256);
            for (std::size_t i = 0; i < n; ++i) {
                auto v = palette::linear_to_vga(result->palette[i]);
                palette_bytes[i * 3 + 0] = (v >> 16) & 0x3F;
                palette_bytes[i * 3 + 1] = (v >>  8) & 0x3F;
                palette_bytes[i * 3 + 2] =  v        & 0x3F;
            }
            raw.assign(result->indices.begin(), result->indices.end());
        } else if (mode == Mode::vga_10h || mode == Mode::vga_12h) {
            raw = result->planes.data;
            palette_bytes.reserve(48);
            std::size_t n = std::min<std::size_t>(result->palette.size(), 16);
            for (std::size_t i = 0; i < n; ++i) {
                auto v = palette::linear_to_vga(result->palette[i]);
                palette_bytes.push_back((v >> 16) & 0x3F);
                palette_bytes.push_back((v >>  8) & 0x3F);
                palette_bytes.push_back( v        & 0x3F);
            }
            while (palette_bytes.size() < 48) palette_bytes.push_back(0);
        } else {
            return make_error("DOS viewer: mode not supported on ia16 path");
        }

        auto viewer = cheader_dos_c::generate(mode, w, h, raw, palette_bytes, opts);
        if (!viewer) return make_error(viewer.error().message);
        std::vector<std::uint8_t> bytes(viewer->begin(), viewer->end());
        return make_result(std::move(bytes), *result);
    }

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

    auto ch_opts = pipeline::make_ch_opts({
        .symbol_override = options.symbol_name,
        .hires = result->hires,
        .interlace = result->interlace,
        .aga = (resolve_chipset(options.chipset, result->mode) == amiga::Chipset::aga),
        .fade_in = true,            // always enable fade-in for web/compile exports
        .dpf = result->dpf,
        .total_unique_colors =
            static_cast<std::size_t>(count_unique_colors(result->rendered)),
    });
    if (result->copper && !result->scanline_changes.empty()) {
        ch_opts.copper_changes = &result->scanline_changes;
        ch_opts.copper_changes_per_line = result->changes_per_line;
    }
    if (result->scap && !result->strips_line_moves.empty()) {
        ch_opts.strips_line_moves = &result->strips_line_moves;
        bool strips_ehb  = result->mode == amiga::Mode::ehb;
        bool strips_ham6 = result->mode == amiga::Mode::ham6;
        auto& table = strips_ehb  ? strips::kStrips6bplEhb
                    : strips_ham6 ? strips::kStrips6bplHam6
                                : strips::kStrips6bplOcs;
        ch_opts.strips_label = strips_ehb  ? "strips_ehb_ocs"
                           : strips_ham6 ? "strips_ham6_ocs"
                                       : "strips_dpf_ocs";
        ch_opts.strips_anchor_hpos = table.line_gate_hpos;
        ch_opts.strips_total_planes = table.total_planes;
        ch_opts.fade_in = false;  // strips carries its own per-line palette
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

    // C64 + Genesis already pack their raw bytes into raw_frame
    // verbatim during run_pipeline (c64: bitmap+screen+color RAM,
    // genesis: tile_bytes + u16-BE tilemap + u16-BE palette). Hand
    // them straight back.
    if (amiga::is_c64(result->mode) || amiga::is_genesis(result->mode)) {
        std::vector<std::uint8_t> raw = std::move(result->raw_frame);
        return make_result(std::move(raw), *result);
    }

    // IBM PC / DOS modes: mode-specific raw layout.
    //   Text modes (CGA 8x8 / EGA 8x14): char+attr pairs (raw_frame). No
    //     palette — attribute byte already encodes fg/bg from the fixed
    //     16-entry CGA master palette.
    //   CGA composite: 16KB banked CGAPIC frame (raw_frame), no palette.
    //   CGA 320/640: 4 bitplanes (or 1) from bitplane encoder — palette is
    //     fixed in hardware so we emit just the planes.
    //   VGA 13h: 8bpp indices (raw_frame) + 256×3-byte DAC palette
    //     (6-bit per channel).
    //   EGA: 4 bitplanes + 16-byte IrgbIRGB palette.
    //   VGA planar (Mode 10h / 12h): 4 planes + 16×3-byte DAC palette.
    if (amiga::is_cga_text(result->mode) ||
        result->mode == amiga::Mode::cga_composite ||
        amiga::is_chunky(result->mode)) {
        std::vector<std::uint8_t> raw = std::move(result->raw_frame);
        if (amiga::is_chunky(result->mode)) {
            for (auto& c : result->palette) {
                auto v = palette::linear_to_vga(c);
                raw.push_back(static_cast<std::uint8_t>((v >> 16) & 0x3F));
                raw.push_back(static_cast<std::uint8_t>((v >> 8)  & 0x3F));
                raw.push_back(static_cast<std::uint8_t>( v        & 0x3F));
            }
        }
        return make_result(std::move(raw), *result);
    }
    if (amiga::is_ega(result->mode) || amiga::is_vga(result->mode) ||
        amiga::is_cga(result->mode)) {
        std::vector<std::uint8_t> raw;
        if (amiga::is_cga(result->mode)) {
            // CGA 320/640: pack indices into the banked 16 KB CGAPIC
            // frame. Palette is hardware-fixed so we emit no palette bytes.
            raw = cheader_dos_c::pack_cga_banked(
                result->indices, result->rendered.width(),
                result->rendered.height(), result->mode);
        } else {
            raw = std::move(result->planes.data);
            if (amiga::is_ega(result->mode)) {
                for (auto& c : result->palette) {
                    auto e = palette::linear_to_ega(c);
                    raw.push_back(palette::ega_to_hw(e));
                }
            } else if (amiga::is_vga(result->mode)) {
                for (auto& c : result->palette) {
                    auto v = palette::linear_to_vga(c);
                    raw.push_back(static_cast<std::uint8_t>((v >> 16) & 0x3F));
                    raw.push_back(static_cast<std::uint8_t>((v >> 8)  & 0x3F));
                    raw.push_back(static_cast<std::uint8_t>( v        & 0x3F));
                }
            }
        }
        return make_result(std::move(raw), *result);
    }

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

// ---------------------------------------------------------------------------
// c64 PRG / Koala / Art-Studio raw exports.
// ---------------------------------------------------------------------------

namespace {

ConvertResult c64_export(const std::uint8_t* input_data, std::size_t input_size,
                         const Options& options,
                         const std::function<Result<c64::prg::PrgData>(
                             amiga::Mode, const c64::EncodeResult&)>& gen,
                         const char* label) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return make_error(result.error().message);
    if (!amiga::is_c64(result->mode))
        return make_error(std::string(label)
                          + " export only supported for c64 modes");
    auto enc = c64::prg::unpack_pipeline_raw(
        result->mode, result->raw_frame, result->c64_bg_color);
    if (!enc) return make_error(enc.error().message);
    auto prg = gen(result->mode, *enc);
    if (!prg) return make_error(prg.error().message);
    return make_result(std::move(prg->bytes), *result);
}

}  // namespace

ConvertResult convert_prg(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    return c64_export(input_data, input_size, options,
                      [](amiga::Mode m, const c64::EncodeResult& r) {
                          return c64::prg::from_mode(m, r);
                      },
                      "PRG");
}

ConvertResult convert_koa(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    return c64_export(input_data, input_size, options,
                      [](amiga::Mode m, const c64::EncodeResult& r) {
                          if (m != amiga::Mode::c64_multicolor)
                              return Result<c64::prg::PrgData>{
                                  std::unexpected{Error{
                                      ErrorCode::unsupported_mode,
                                      ".koa requires c64-multicolor mode"}}};
                          return c64::prg::koala_raw(r);
                      },
                      "Koala raw");
}

ConvertResult convert_hir(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    return c64_export(input_data, input_size, options,
                      [](amiga::Mode m, const c64::EncodeResult& r) {
                          if (m != amiga::Mode::c64_hires)
                              return Result<c64::prg::PrgData>{
                                  std::unexpected{Error{
                                      ErrorCode::unsupported_mode,
                                      ".hir requires c64-hires mode"}}};
                          return c64::prg::hires_raw(r);
                      },
                      "Art Studio raw");
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

// Run the encoder and return its full intermediate state. Pipes through
// the same run_pipeline() the rest of the API uses, then copies relevant
// PipelineResult fields out into the public EncodeState. Used by the
// batch CLI handler so it can slice the atlas's bitplane data per-frame
// after a single encode pass.
EncodeStateOrError encode_state(const std::uint8_t* input_data,
                                std::size_t input_size,
                                const Options& options) {
    EncodeStateOrError out;
    auto r = run_pipeline(input_data, input_size, options);
    if (!r) {
        out.error_msg = r.error().message;
        return out;
    }
    auto& p = *r;
    auto& s = out.state;
    s.rendered = std::move(p.rendered);
    s.planes = std::move(p.planes);
    s.palette = std::move(p.palette);
    s.indices = std::move(p.indices);
    s.mode = p.mode;
    s.aga = p.aga;
    s.hires = p.hires;
    s.interlace = p.interlace;
    s.dpf = p.dpf;
    s.copper = p.copper;
    s.scap = p.scap;
    s.has_transparency = p.has_transparency;
    s.transparency_mask = std::move(p.transparency_mask);
    s.scanline_palettes = std::move(p.scanline_palettes);
    s.scanline_changes = std::move(p.scanline_changes);
    s.strips_line_moves = std::move(p.strips_line_moves);
    s.copper_num_colors = p.copper_num_colors;
    s.changes_per_line = p.changes_per_line;
    s.max_moves_per_line = p.max_moves_per_line;
    s.copper_changes = p.copper_changes;
    s.strips_avg_total_moves_per_line   = p.strips_avg_total_moves_per_line;
    s.strips_avg_hblank_moves_per_line  = p.strips_avg_hblank_moves_per_line;
    s.strips_max_hblank_moves_per_line  = p.strips_max_hblank_moves_per_line;
    s.strips_avg_visible_moves_per_line = p.strips_avg_visible_moves_per_line;
    s.strips_max_visible_moves_per_line = p.strips_max_visible_moves_per_line;
    s.strips_slot_count                 = p.strips_slot_count;
    s.quant_error = p.quant_error;
    s.psnr = p.psnr;
    s.ssimulacra2_score = p.ssimulacra2_score;
    s.raw_frame = std::move(p.raw_frame);
    s.c64_bg_color = p.c64_bg_color;
    s.c64_cols = p.c64_cols;
    s.c64_rows = p.c64_rows;
    s.c64_unique_glyphs = p.c64_unique_glyphs;
    s.c64_mc1 = p.c64_mc1;
    s.c64_mc2 = p.c64_mc2;
    s.genesis_unique_tiles = p.genesis_unique_tiles;
    s.tile_data_bytes      = p.tile_data_bytes;
    s.genesis_total_cells = p.genesis_total_cells;
    s.genesis_tile_bytes = std::move(p.genesis_tile_bytes);
    s.genesis_tilemap_cells = std::move(p.genesis_tilemap_cells);
    s.genesis_palette_words = std::move(p.genesis_palette_words);
    return out;
}

// Per-mode dither tuning lookup. Builds a dither_tuning::Context from
// the API options — same fields the encoder uses internally — and
// returns the strength/error_clamp the encoder would default to. The
// web UI calls this on mode change to refresh its sliders.
DitherDefaults dither_defaults_for(const Options& options) {
    auto opts = decompose_mode_options(options);
    auto mode = parse_mode(opts.mode);
    auto chipset = opts.chipset == "aga"
        ? amiga::Chipset::aga
        : amiga::Chipset::ocs;
    auto depth = std::clamp(opts.depth, 1, 8);
    auto tune = dither_tuning::defaults_for(dither_tuning::Context{
        .mode    = mode,
        .depth   = depth,
        .dpf     = opts.dual_playfield,
        .scap    = opts.scap,
        .copper  = opts.copper,
        .chipset = chipset,
        .method  = parse_dither(opts.dither),
    });
    return DitherDefaults{tune.strength, tune.error_clamp};
}

} // namespace png2amiga::api
