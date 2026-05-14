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
#include "palette_search.hpp"
#include "ssimulacra2.hpp"
#include "pipeline.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "quantize_metal.hpp"
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
    if (s == "cga-text80x50")   return amiga::Mode::cga_text80x50;
    if (s == "cga-text80x25")   return amiga::Mode::cga_text80x25;
    if (s == "cga-text80x200")  return amiga::Mode::cga_text80x200;
    if (s == "cga-text40x200")  return amiga::Mode::cga_text40x200;
    if (s == "cga-text40x100")  return amiga::Mode::cga_text40x100;
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
    // Single source of truth — the (mode, chipset, "auto") form
    // gives the chipset-aware default; see quantize::resolve_algorithm
    // body for the per-mode rationale.
    return quantize::resolve_algorithm(mode, chipset, "");
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
    // Single source of truth in dither.cpp::kMethodNames. Unknown
    // strings fall back to FS to match historical behaviour (the
    // pre-table api.cpp parser silently fell through to FS).
    return dither::parse_method_or_null(s).value_or(
        dither::Method::floyd_steinberg);
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
                                   std::vector<bool>* out_tmask = nullptr,
                                   // When non-null, skip decode + crop + scale +
                                   // preprocess. Caller has already produced an
                                   // image at target size with all preprocessing
                                   // applied. tmask, if needed, must be supplied
                                   // separately by the caller (out_tmask is left
                                   // untouched). Used by run_pipeline_image() to
                                   // bypass the 8-bit PNG round-trip CLI sites
                                   // had to do to feed pre-decoded float pixels
                                   // through the bytes-only entry point.
                                   const Image* prepared_image = nullptr) {
    if (prepared_image) {
        // Build tmask from the prepared image's alpha channel (if
        // any) so encode_state_image callers — chiefly main.cpp's
        // joint-mode --ji per-input render path — get transparency
        // honored. Without this, the dither candidate set fails
        // to exclude slot 0 and the post-dither force-to-0 step
        // never fires, off-by-one shifting every opaque pixel's
        // routing. Threshold matches the rest of the pipeline
        // (alpha < 0.5 ⇒ transparent).
        if (out_tmask) {
            out_tmask->clear();
            if (prepared_image->has_alpha()) {
                auto a = prepared_image->alpha();
                out_tmask->resize(a.size(), false);
                bool any = false;
                for (std::size_t i = 0; i < a.size(); ++i) {
                    if (a[i] < 0.5f) {
                        (*out_tmask)[i] = true;
                        any = true;
                    }
                }
                if (!any) out_tmask->clear();  // all opaque
            }
        }
        return *prepared_image;
    }
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

    // Check for transparency. Either alpha < 255, OR pixel sRGB matches
    // one of options.transparent_colors (sentinel-magenta atlases etc —
    // see main.cpp for the CLI parallel; both paths are needed because
    // the CLI passes a pre-loaded Image while the WASM/programmatic
    // entry runs from raw bytes).
    auto matches_sentinel = [&](std::uint8_t r, std::uint8_t g,
                                 std::uint8_t b) {
        for (auto& tc : options.transparent_colors)
            if (tc[0] == r && tc[1] == g && tc[2] == b) return true;
        return false;
    };
    bool any_transparent = false;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        if (raw[i * 4 + 3] < 255 ||
            (!options.transparent_colors.empty() &&
             matches_sentinel(raw[i*4], raw[i*4+1], raw[i*4+2]))) {
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
        if (any_transparent) {
            if (matches_sentinel(raw[base], raw[base+1], raw[base+2])) {
                src_alpha[i] = 0.0f;
            } else {
                src_alpha[i] = static_cast<float>(raw[base + 3]) / 255.0f;
            }
        }
    }
    free_raw();

    // Source orientation: kingcon-compatible flip + rotate, applied to
    // the loaded pixel + alpha buffers before any crop/scale/encode work.
    // Order matches kingcon: flip-x, then flip-y, then rotate clockwise.
    auto rotate_quarters = ((options.rotate_quarters % 4) + 4) % 4;
    bool any_orient = options.flip_x || options.flip_y || rotate_quarters != 0;
    if (any_orient) {
        auto remap = [&](auto& buf) {
            std::vector<typename std::remove_reference_t<decltype(buf)>::value_type>
                tmp(buf.size());
            std::size_t src_w = width, src_h = height;
            for (std::size_t y = 0; y < src_h; ++y) {
                for (std::size_t x = 0; x < src_w; ++x) {
                    std::size_t sx = options.flip_x ? (src_w - 1 - x) : x;
                    std::size_t sy = options.flip_y ? (src_h - 1 - y) : y;
                    std::size_t dx = x, dy = y;
                    std::size_t dw = src_w;
                    switch (rotate_quarters) {
                        case 1:   // 90° CW:  (x,y) → (h-1-y, x), new w=h, new h=w
                            dx = src_h - 1 - y; dy = x;
                            dw = src_h;
                            break;
                        case 2:   // 180°
                            dx = src_w - 1 - x; dy = src_h - 1 - y;
                            break;
                        case 3:   // 270° CW
                            dx = y; dy = src_w - 1 - x;
                            dw = src_h;
                            break;
                        default: break;
                    }
                    tmp[dy * dw + dx] = buf[sy * src_w + sx];
                }
            }
            buf = std::move(tmp);
        };
        remap(pixels);
        if (any_transparent) remap(src_alpha);
        if (rotate_quarters == 1 || rotate_quarters == 3) {
            std::swap(width, height);
            std::swap(w, h);
        }
        pixel_count = width * height;
    }

    // --trim is implemented at the CLI level as sugar that sets
    // crop_x/y/w/h to the non-transparent bbox before run_pipeline is
    // called, so target_w / target_h end up derived from the trimmed
    // dimensions. The Options::trim flag is carried for symmetry but
    // not acted on here.

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
// big buffer, then crop the center W x H back out for export. Running
// error diffusion across the source-period-3W width lets the dither
// converge to a W-periodic pattern; the center tile inherits that
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

// Apply the center crop to a PipelineResult that was encoded from a
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

// ---------------------------------------------------------------------------
// PlainAutoTrial — shared output of `encode_plain_auto`. Both run_pipeline's
// non-best plain auto branch (the one that built palette + dither + encode +
// render inline) AND the lores/hires --best multi-restart sweep call this
// function with the same args. Per-trial differences (palette_diversity,
// jittered image, dither_strength) flow as parameters.
//
// The architectural rationale: previous --best path inlined a near-copy of
// the non-best pipeline. Each time the non-best path got a feature added
// (assemble_with_reserves, lock_color0 prepend, refine_with_dither's
// locked_mask, etc.), the --best lambda silently fell behind. Most recently
// the --best path was missing the `lock_color0` prepend, producing palettes
// where slot 0 was a near-black quantizer centroid (#110000 / #111111)
// rather than pure #000000. Running both paths through one function makes
// future drift impossible.
//
// Scope: handles the auto-quantize palette branch only. Special-palette
// branches (CGA / EGA-200 fixed kCgaHw / Atari hi mono / user --palette)
// build `pal` themselves and bypass this helper — the --best path is gated
// against those features anyway. DPF expansion / final preview tweaks /
// transparency-mask blackout happen post-helper at the call site.
// ---------------------------------------------------------------------------
struct PlainAutoTrial {
    Palette                       pal;          // assembled, snapped, refined
    std::vector<bool>             locked_mask;  // pal.colors.size() entries
    std::size_t                   pal_size;     // first N are the live palette
    std::vector<std::uint8_t>     indices;      // dither_result.indices
    bitplane::BitplaneData        planes;
    Image                         rendered;
    float                         total_error;
};

Result<PlainAutoTrial> encode_plain_auto(
    const Image& img,
    std::size_t depth,
    std::size_t max_colors,
    amiga::Mode mode,
    amiga::Chipset chipset,
    const dither::Settings& dith,
    int palette_diversity,
    int refine_iterations,
    bool lock_color0,
    bool has_transparency,
    bool use_dpf,
    bool match_range,
    const std::vector<LockSpec>&    locks,
    const std::vector<ReserveSpec>& reserves,
    const std::vector<PinSpec>&     pins,
    const std::vector<bool>&        tmask) {
    // Atari uses the full palette (no border slot tied to index 0).
    bool is_atari_local = amiga::is_atari(mode);
    bool lock_zero = lock_color0 && (has_transparency || !is_atari_local);

    // 1bpp short-circuit: a 2-centroid quantizer on natural images
    // picks two near-midtones, which crushes contrast and yields the
    // S2≈-65 disaster everyone sees at --depth 1. Force {black, white}
    // and let the ditherer span the full luminance range. Skipped when
    // the user explicitly placed locks or reserves — they take control
    // of the slot contents and the regular path honours that.
    if (max_colors == 2 && locks.empty() && reserves.empty()) {
        PlainAutoTrial out;
        out.pal.name   = "bw";
        out.pal.colors = {Color3f{0.0f, 0.0f, 0.0f}, Color3f{1.0f, 1.0f, 1.0f}};
        out.pal_size   = 2;
        out.locked_mask = {true, true};
        auto dr = dither::apply(img,
            std::span<const Color3f>(out.pal.colors), dith);
        if (has_transparency) {
            for (std::size_t i = 0;
                 i < tmask.size() && i < dr.indices.size(); ++i)
                if (tmask[i]) dr.indices[i] = 0;
        }
        out.indices = std::move(dr.indices);
        out.total_error = dr.total_error;
        if (!pins.empty()) {
            auto pin_result = palette_locks::apply_pins(
                out.pal, out.indices, out.locked_mask, pins,
                img.width(), img.height());
            if (!pin_result) return std::unexpected{pin_result.error()};
        }
        bool dos_planar_bw = (amiga::is_ega(mode) || amiga::is_vga(mode))
                             && !amiga::is_chunky(mode);
        auto bp_layout_bw = is_atari_local
            ? bitplane::Layout::word_interleaved
            : dos_planar_bw ? bitplane::Layout::standard
                            : bitplane::Layout::interleaved;
        auto bp_res = bitplane::encode(out.indices,
            img.width(), img.height(), depth, bp_layout_bw);
        if (!bp_res) return std::unexpected{bp_res.error()};
        out.planes = *std::move(bp_res);
        std::vector<Color3f> pal_view(out.pal.colors.begin(),
                                      out.pal.colors.end());
        auto pv = pipeline::render_preview(out.planes, pal_view,
            /*is_ham=*/false, /*is_lace=*/false, chipset);
        if (!pv) return std::unexpected{pv.error()};
        out.rendered = *std::move(pv);
        return out;
    }

    // Reserve-count + reserved_mask (caller may pass zero reserves).
    auto reserves_in_pal = palette_locks::validate_reserves(
        reserves, locks, max_colors, lock_zero);
    if (!reserves_in_pal) return std::unexpected{reserves_in_pal.error()};
    std::size_t reserve_count = *reserves_in_pal;
    std::vector<bool> reserved_mask(max_colors, false);
    for (auto& r : reserves) {
        auto i = static_cast<std::size_t>(r.index);
        if (r.index >= 0 && i < max_colors) reserved_mask[i] = true;
    }

    // Quantize + assemble. EGA modes (ega-hi specifically — the 200-line
    // ega_320/640 use kCgaHw and take a separate non-auto path at the
    // call site) go through ega_histogram on a pre-snapped image.
    auto qc = palette_locks::quant_counts_for_assemble(
        max_colors, locks, reserve_count, lock_zero);
    Image ega_snapped;
    const bool is_ega_mode = amiga::is_ega(mode);
    if (is_ega_mode) {
        ega_snapped = Image(img.width(), img.height());
        for (std::size_t y = 0; y < img.height(); ++y)
            for (std::size_t x = 0; x < img.width(); ++x)
                ega_snapped[x, y] = palette::quantize_to_ega(img[x, y]);
    }
    auto qfn = [&](std::size_t k) -> Result<Palette> {
        if (is_ega_mode) return quantize::ega_histogram(ega_snapped, k);
        return quantize::quantize(img, k,
                                  quantize_algo(chipset, mode),
                                  palette_diversity);
    };
    auto quantized = palette_locks::two_pass_quantize(
        qfn, qc.qcount, qc.kfallback, lock_zero);
    if (!quantized) return std::unexpected{quantized.error()};
    if (amiga::is_stf(mode) || amiga::is_vga(mode))
        snap_to_chipset(*quantized, chipset, mode);
    auto assembled = palette_locks::assemble_with_reserves(
        *quantized, locks, reserves, max_colors, lock_zero, chipset, mode);

    PlainAutoTrial out;
    out.pal = std::move(assembled.palette);
    out.locked_mask = std::move(assembled.locked);
    out.pal_size = std::min(out.pal.size(), max_colors);

    if (match_range)
        preprocess::match_palette_range(const_cast<Image&>(img), out.pal);

    // Dither-aware refinement (auto-palette only). Same gates as the
    // pre-extraction non-best path. The locked_mask flow keeps slot 0
    // (when lock_color0 is true) at pure black across iterations.
    if (refine_iterations > 0 &&
        dith.method != dither::Method::none && reserve_count == 0 &&
        !amiga::is_cga(mode) && !amiga::is_chunky(mode) &&
        !amiga::is_ega(mode) && !amiga::is_atari_hi(mode)) {
        auto refined = quantize::refine_with_dither(
            img,
            Palette{"refined", {out.pal.colors.begin(),
                                out.pal.colors.begin() +
                                    static_cast<std::ptrdiff_t>(out.pal_size)}},
            dith, chipset, mode,
            static_cast<std::size_t>(refine_iterations), out.locked_mask);
        if (refined) {
            out.pal.colors = std::move(refined->colors);
            out.pal_size = out.pal.colors.size();
        }
    }

    std::span<const Color3f> pal_span{out.pal.colors.data(), out.pal_size};

    // Dither candidate set excludes reserved slots and (when transparent)
    // index 0. For best-path callers that pass empty reserves and no
    // transparency, the simple branch fires.
    dither::DitherResult dr;
    if (reserve_count > 0 || has_transparency) {
        std::vector<Color3f> cand_pal;
        std::vector<std::uint8_t> cand_to_full;
        cand_pal.reserve(out.pal_size);
        cand_to_full.reserve(out.pal_size);
        for (std::size_t i = 0; i < out.pal_size; ++i) {
            if (has_transparency && i == 0) continue;
            if (reserved_mask[i]) continue;
            cand_pal.push_back(out.pal.colors[i]);
            cand_to_full.push_back(static_cast<std::uint8_t>(i));
        }
        std::span<const Color3f> dither_span{cand_pal.data(), cand_pal.size()};
        dr = dither::apply(img, dither_span, dith);
        for (auto& idx : dr.indices) idx = cand_to_full[idx];
        if (has_transparency) {
            for (std::size_t i = 0;
                 i < tmask.size() && i < dr.indices.size(); ++i)
                if (tmask[i]) dr.indices[i] = 0;
        }
    } else {
        dr = dither::apply(img, pal_span, dith);
    }
    out.indices = std::move(dr.indices);
    out.total_error = dr.total_error;

    // Pin-index swaps post-dither. Each --best trial picks its own
    // palette + indices; the pin swap is a visual no-op (it swaps both
    // palette entries and pixel indices), so per-trial application
    // doesn't affect ranking — the rendered image is unchanged.
    if (!pins.empty()) {
        auto pin_result = palette_locks::apply_pins(
            out.pal, out.indices, out.locked_mask, pins,
            img.width(), img.height());
        if (!pin_result) return std::unexpected{pin_result.error()};
    }

    // Sort by perceptual brightness, except for HAM/DPF where palette
    // layout is constrained. Best-path eligibility excludes DPF.
    if (!amiga::is_ham(mode) && !use_dpf) {
        palette_locks::sort_by_brightness(out.pal.colors, out.locked_mask,
                                          out.indices,
                                          out.pal.colors.size());
    }

    bool dos_planar = (amiga::is_ega(mode) || amiga::is_vga(mode))
                      && !amiga::is_chunky(mode);
    auto bp_layout = is_atari_local
        ? bitplane::Layout::word_interleaved
        : dos_planar ? bitplane::Layout::standard
                     : bitplane::Layout::interleaved;
    auto bp_res = bitplane::encode(out.indices, img.width(), img.height(),
                                   depth, bp_layout);
    if (!bp_res) return std::unexpected{bp_res.error()};
    out.planes = *std::move(bp_res);

    auto pal_view = std::vector<Color3f>(
        out.pal.colors.begin(),
        out.pal.colors.begin() +
            static_cast<std::ptrdiff_t>(out.pal_size));
    auto pv = pipeline::render_preview(out.planes, pal_view,
                                        /*is_ham=*/false,
                                        /*is_lace=*/false, chipset);
    if (!pv) return std::unexpected{pv.error()};
    out.rendered = *std::move(pv);
    return out;
}

}  // close anon namespace so run_pipeline gets external linkage and can
   // be reached from pipeline.cpp via the api:: forwarder.

// --best eligibility — defined here so run_pipeline (programmatic API)
// and main.cpp (CLI dispatch) share a single source of truth. main.cpp
// dispatches most modes through encode_state_image rather than
// run_pipeline, so the gate has to be reachable from both layers.
Result<void> check_best_supported(const Options& options,
                                  amiga::Mode mode,
                                  bool has_transparency) {
    if (!options.best) return {};
    // has_transparency used to gate EHB --best off (legacy: pop_search
    // for EHB wasn't transparency-aware). Both that and the lores/
    // hires gates have since been lifted; keep the parameter for ABI
    // stability + the gate's pre-existing call sites.
    (void)has_transparency;
    auto mode_params = amiga::get_mode_params(mode);
    bool ham_best   = amiga::is_ham(mode) &&
                      (mode_params.bitplane_depth == 6 ||
                       mode_params.bitplane_depth == 8);
    bool ehb_best   = (mode == amiga::Mode::ehb) &&
                      !has_user_palette(options);
    bool plain_best = (mode == amiga::Mode::lores ||
                       mode == amiga::Mode::lores_interlace ||
                       mode == amiga::Mode::hires ||
                       mode == amiga::Mode::hires_interlace) &&
                      !options.copper && !options.scap &&
                      !options.dual_playfield && !options.tile &&
                      !has_user_palette(options);
    bool copper_best = options.copper;
    bool scap_best   = options.scap;
    bool snes_best   = (mode == amiga::Mode::snes_mode7_256);
    if (ham_best || ehb_best || plain_best || copper_best ||
        scap_best || snes_best) return {};
    return std::unexpected{Error{
        ErrorCode::unsupported_mode,
        "--best is not supported in this configuration. "
        "Supported: HAM6/HAM8, plain EHB (no user palette), "
        "plain lores/hires (no --copper, no --scap, no --dpf, "
        "no --tile, no --palette), sliced (--copper), strips "
        "(--scap), and snes-mode7-256. Drop --best or change "
        "one of those "
        "options."}};
}

Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const Options& orig_options,
                                    // When set, bypass decode + scale +
                                    // preprocess. The image must already be at
                                    // the target dimensions for the mode (i.e.
                                    // whatever compute_target_dims would
                                    // produce); the rest of the pipeline runs
                                    // exactly as on the bytes-input path. Lets
                                    // CLI sites that already have a float
                                    // Image hand it in directly without the
                                    // 8-bit PNG round-trip png_io::encode
                                    // would impose.
                                    const Image* prepared_image = nullptr) {
    auto options = decompose_mode_options(orig_options);
    auto mode = parse_mode(options.mode);
    bool compound_hires =
        (orig_options.mode.starts_with("ham") ||
         orig_options.mode.starts_with("lores") ||
         orig_options.mode.starts_with("hires") ||
         orig_options.mode.starts_with("ehb")) &&
        orig_options.mode.find("hires") != std::string::npos;

    // Hires plain modes prefer one extra reseed pass (palette_diversity=5)
    // over the API default of 4. Mean S2 sweep over the photo example set:
    // d=2 +0.99, d=4 +0.62, d=3 flat. Lores buckets all prefer ≤ 4, so the
    // bump is hires-only. Triggered when the caller left the API default 4
    // (the CLI sets diversity explicitly when --palette-diversity is given;
    // tools/api_pipeline_smoke applies the same mirror in --apply-tuning so
    // api-equiv-* tests stay byte-identical). The web frontend benefits
    // here too — Vue defaults Options.palette_diversity to 4 unconditionally.
    // Auto-tune dither strength + error_clamp when the caller left them
    // at the -1.0f sentinel (api::Options defaults). This is the SINGLE
    // location where the dither_tuning lookup feeds the encoder — web /
    // CLI / library callers all converge here, so adding a new tuning
    // bucket or changing the per-mode constants lands once and reaches
    // every caller automatically. Previously the web maintained its own
    // refreshDitherDefaults watcher that mirrored this lookup; now it
    // can leave the fields at sentinel and the encoder handles it.
    if (options.dither_strength < 0.0f || options.error_clamp < 0.0f) {
        auto chipset_for_tune = (options.chipset == "aga")
            ? amiga::Chipset::aga : amiga::Chipset::ocs;
        auto tune = dither_tuning::defaults_for(dither_tuning::Context{
            .mode    = mode,
            .depth   = std::clamp(options.depth, 1, 8),
            .dpf     = options.dual_playfield,
            .scap    = options.scap,
            .copper  = options.copper,
            .chipset = chipset_for_tune,
            .method  = parse_dither(options.dither),
        });
        if (options.dither_strength < 0.0f) options.dither_strength = tune.strength;
        if (options.error_clamp     < 0.0f) options.error_clamp     = tune.error_clamp;
    }

    if (options.palette_diversity == 4 &&
        (mode == amiga::Mode::hires || mode == amiga::Mode::hires_interlace)) {
        options.palette_diversity = 5;
    }

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

    // --reserve-range gating. Modes where reserves can't yet be honored
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
                          "the modify ops produce arbitrary colors, no "
                          "fixed slot to reserve)");
        // DPF: PF1 is zeroed in the current implementation
        // (`expand_to_dpf_pf2` puts the encoded depth-3/4 image into PF2
        // and leaves the upper-bank planes all-zero). So a reserve on
        // the depth-3/4 base palette IS effectively a PF2 reserve, no
        // ambiguity. Validate against the PF2 max colors (1<<depth)
        // rather than the full 6/8-plane width.
        // (PF1 reserves are out of scope; PF1 is all-zero today.)
        if (amiga::is_genesis(mode))
            return reject("not supported in Genesis modes (4 separate "
                          "16-color palette lines; reserve target ambiguous)");
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
    if (prepared_image) {
        peek_w = static_cast<int>(prepared_image->width());
        peek_h = static_cast<int>(prepared_image->height());
    } else {
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
    }

    // When an explicit --crop / --trim region is active, derive target
    // dims from the crop (post-orientation) source rather than the raw
    // file dims, so --no-scale + --trim produces a tight bbox-sized
    // output instead of re-stretching to the un-cropped source size.
    auto src_w_eff = static_cast<std::size_t>(peek_w);
    auto src_h_eff = static_cast<std::size_t>(peek_h);
    if (options.crop_w > 0 && options.crop_h > 0) {
        src_w_eff = static_cast<std::size_t>(options.crop_w);
        src_h_eff = static_cast<std::size_t>(options.crop_h);
    }
    auto [target_w, target_h] = compute_target_dims(
        src_w_eff, src_h_eff, options, mode);

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
                                      target_w, target_h, &tmask,
                                      prepared_image);
    if (!image) return std::unexpected{image.error()};
    bool has_transparency = !tmask.empty();

    if (auto bcheck = check_best_supported(options, mode, has_transparency);
        !bcheck) {
        return std::unexpected{bcheck.error()};
    }

    // Tile pre-processing: replicate the loaded image (and the alpha
    // mask, if any) into a 3x3 grid. The rest of the pipeline runs on
    // the 3W x 3H buffer; the center crop happens at the per-branch
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
        auto cga_kernel = cga_text::parse_kernel(options.cga_text_kernel);
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
                                    cga_metric, cga_kernel,
                                    options.on_progress);
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
        // Reserve / lock plumbing — mirror the std-lores pattern
        // (quant_counts_for_assemble + two_pass_quantize +
        // assemble_with_reserves) so reserved DAC entries are pinned at
        // user values, the quantizer fills only unreserved slots, and the
        // dither candidate set excludes reserves. Without this the chunky
        // VGA path silently ignored reserves and the rendered output
        // depended on the reserve color.
        auto reserves_in_pal_vga = palette_locks::validate_reserves(
            options.reserves, options.locks, max_colors,
            options.lock_color0);
        if (!reserves_in_pal_vga)
            return std::unexpected{reserves_in_pal_vga.error()};
        auto qc_vga = palette_locks::quant_counts_for_assemble(
            max_colors, options.locks, *reserves_in_pal_vga,
            options.lock_color0);
        // Match CLI's std-lores resolver: gpu-restart on Apple Metal,
        // median-cut otherwise. The Metal kernel's atomic-float
        // accumulation was the historical non-determinism source —
        // since v1.66 the gpu_restart pipeline has been swapped to
        // GPU-argmin + CPU-sum so it's reproducible and matches the
        // CLI bit-for-bit.
        auto qfn = [&](std::size_t k) -> Result<Palette> {
            auto algo = quantize::resolve_algorithm(mode, chipset,
                                                     options.quantizer);
            return quantize::quantize(*image, k, algo,
                                       options.palette_diversity);
        };
        auto qr = palette_locks::two_pass_quantize(
            qfn, qc_vga.qcount, qc_vga.kfallback, options.lock_color0);
        if (!qr) return std::unexpected{qr.error()};
        auto assembled = palette_locks::assemble_with_reserves(
            *qr, options.locks, options.reserves,
            max_colors, options.lock_color0, chipset, mode);
        Palette pal = std::move(assembled.palette);
        for (auto& c : pal.colors) c = palette::quantize_to_vga(c);
        dither::Settings dith;
        dith.method = parse_dither(options.dither);
        dith.strength = options.dither_strength;
        dith.error_clamp = options.error_clamp;
        // Build dither candidate set excluding reserved slots; the
        // map back to full-slot indices keeps the rendered preview /
        // raw output consistent with the user-supplied palette.
        std::vector<bool> reserved_mask_vga(max_colors, false);
        for (auto& r : options.reserves) {
            auto i = static_cast<std::size_t>(r.index);
            if (r.index >= 0 && i < max_colors) reserved_mask_vga[i] = true;
        }
        bool any_excluded_vga = std::any_of(reserved_mask_vga.begin(),
                                            reserved_mask_vga.end(),
                                            [](bool b) { return b; });
        dither::DitherResult dith_result;
        if (any_excluded_vga) {
            std::vector<Color3f> cand;
            std::vector<std::uint8_t> cand_to_full;
            cand.reserve(max_colors);
            cand_to_full.reserve(max_colors);
            for (std::size_t i = 0; i < pal.colors.size(); ++i) {
                if (reserved_mask_vga[i]) continue;
                cand.push_back(pal.colors[i]);
                cand_to_full.push_back(static_cast<std::uint8_t>(i));
            }
            dith_result = dither::apply(*image, cand, dith);
            for (auto& idx : dith_result.indices) idx = cand_to_full[idx];
        } else {
            dith_result = dither::apply(*image, pal.colors, dith);
        }
        // Reseat `quantized` so the rest of the block can keep using
        // its name without further surgery.
        auto quantized = std::make_optional(std::move(pal));

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
    // Brute-force per-cell quantization (4 colors per 4×8 cell,
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
        //    color set.
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
        // Build a 3W×3H buffer per 8×8 tile where the center block is
        // the source tile and the 8 surrounding blocks are mirror
        // reflections (h-flip on left/right, v-flip on top/bottom,
        // both on corners). Run ED on the full 24×24, take the center
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
                auto& pal_lab_ref = shadowed ? shadow_lab[pal]
                                              : palette_lab[pal];
                std::span<const color_space::OKLab> pl_span(pal_lab_ref.data(),
                                                             pal_lab_ref.size());
                std::fill(block_idx.begin(), block_idx.end(), std::uint8_t{0});
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
                // Copy center 8×8 back to the global buffers.
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
            // plain sliced's setting; HAM8 uses amplitude 0.4 (its 64-color
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
            float amp = (ham_params.bitplane_depth == 8) ? 0.4f : 1.0f;
            auto winner = pipeline::best_sweep<HamTrial>(
                *image, dith, options.palette_diversity,
                /*jitter_count=*/8,
                encode_once,
                [](const HamTrial& t) -> const Image& { return t.rendered; },
                options.on_progress, amp);
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
                std::vector<std::uint8_t> indices;
                Image rendered;
                float total_error;
            };
            auto encode_once = [&](const Image& img,
                                   const dither::Settings& d,
                                   int diversity,
                                   bool beam_on,
                                   std::function<void(float, std::string_view)>
                                       inner_progress = {}) -> Result<EhbSlicedTrial> {
                // Pre-build the global base palette ourselves with
                // PNN + pair-aware refinement (and 1-opt on --best),
                // then hand it to encode_copper as the line-0 seed.
                // sliced still evolves from there per-line via its own
                // copper-budget-aware planner — but the seed is the
                // EHB-aware best palette we can build offline.
                Palette seed_pal;
                {
                    auto qfn = [&](std::size_t k) -> Result<Palette> {
                        auto q = quantize::quantize(img, k,
                                                    quantize::Algorithm::pnn,
                                                    diversity);
                        if (!q) return std::unexpected{q.error()};
                        Palette p = std::move(*q);
                        snap_to_chipset(p, chipset, mode);
                        return p;
                    };
                    // Subtract locks + reserves: same fix as plain EHB
                    // and copper sliced. Without this, the quantizer
                    // outputs more colors than the unlocked-and-
                    // unreserved tail can hold; the locked overlay
                    // overwrites darks/mids and the dither's candidate
                    // set ends up bright-only.
                    auto qc = palette_locks::quant_counts_for_assemble(
                        32, options.locks, options.reserves.size(),
                        options.lock_color0);
                    auto sr = palette_locks::two_pass_quantize(
                        qfn, qc.qcount, qc.kfallback, options.lock_color0);
                    if (!sr) return std::unexpected{sr.error()};
                    auto assembled = palette_locks::assemble_with_reserves(
                        *sr, options.locks, options.reserves,
                        32, options.lock_color0, chipset, mode);
                    seed_pal.colors = std::move(assembled.palette.colors);
                    seed_pal.name = sr->name;
                    // Lock mask: lock_zero + reserves are held fixed during
                    // refine so its iterative convergence isn't biased by
                    // the reserve color (without it the unlocked slots'
                    // values depend on which reserve color the user picked).
                    std::array<bool, 32> ehb_seed_locked{};
                    if (options.lock_color0) ehb_seed_locked[0] = true;
                    for (auto& r : options.reserves) {
                        auto i = static_cast<std::size_t>(r.index);
                        if (r.index >= 0 && i < ehb_seed_locked.size())
                            ehb_seed_locked[i] = true;
                    }
                    palette::refine_ehb_base_palette(
                        std::span<Color3f>(seed_pal.colors.data(), 32),
                        img.pixels(),
                        /*snap_to_ocs=*/chipset != amiga::Chipset::aga,
                        /*max_iters=*/8,
                        std::span<const bool>(ehb_seed_locked));
                    if (options.lock_color0)
                        seed_pal.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
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
                        if (options.lock_color0)
                            seed_pal.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
                    }
                }
                // Forward locks + reserves so encode_copper enforces
                // them across every per-line palette evolution. Without
                // this the seed_pal pins the colors at line 0 only;
                // copper's MOVE planner is free to swap them out on
                // subsequent lines.
                std::vector<std::pair<std::size_t, Color3f>>
                    ehb_sliced_locked;
                std::vector<std::size_t> ehb_cap_excluded;
                for (auto& l : options.locks) {
                    ehb_sliced_locked.emplace_back(l.index,
                        palette_locks::to_color(l, chipset, mode));
                }
                for (auto& r : options.reserves) {
                    auto i = static_cast<std::size_t>(r.index);
                    if (r.index >= 0 && i < 32) {
                        ehb_sliced_locked.emplace_back(i,
                            palette_locks::to_color(
                                LockSpec{r.index, r.r, r.g, r.b},
                                chipset, mode));
                        ehb_cap_excluded.push_back(i);
                    }
                }
                auto cr = copper::encode_copper(
                    img, 5, d, chipset,
                    static_cast<std::size_t>(options.copper_changes),
                    &seed_pal.colors, options.lock_color0,
                    ehb_sliced_locked, diversity,
                    skip_initial, options.interlace,
                    /*is_ehb=*/true,
                    std::move(inner_progress),
                    // Forward the sentinel when the CLI flag wasn't set
                    // so encode_copper picks its depth/is_ehb-aware default.
                    options.sliced_spread_radius >= 0
                        ? static_cast<std::size_t>(options.sliced_spread_radius)
                        : std::numeric_limits<std::size_t>::max(),
                    options.sliced_spread_decay >= 0.0f
                        ? options.sliced_spread_decay : -1.0f,
                    options.sliced_vertical_dither,
                    ehb_cap_excluded,
                    /*quantizer_override=*/std::nullopt,
                    beam_on);
                if (!cr) return std::unexpected{cr.error()};

                // For each reserved base slot, also exclude its half-brite
                // sibling (idx + 32) from the per-row 64-color dither
                // candidate set — otherwise the dither could route image
                // pixels through the half-brite of a reserved base, which
                // would render as half(reserve_color) and visibly leak.
                // cand_to_full[y] maps the filtered candidate index back
                // to the actual 0..63 EHB slot for indexing into ehb64.
                std::vector<bool> ehb_blocked(64, false);
                for (auto i : ehb_cap_excluded) {
                    if (i < 32) {
                        ehb_blocked[i] = true;
                        ehb_blocked[i + 32] = true;
                    }
                }

                auto w = img.width();
                auto h = img.height();
                std::vector<std::uint8_t> all_indices(w * h);

                std::vector<std::vector<color_space::OKLab>> pal_lab_per_row(h);
                std::vector<std::vector<std::uint8_t>> cand_to_full_per_row;
                if (!ehb_cap_excluded.empty()) cand_to_full_per_row.resize(h);
                // Lock mask for the per-row refine: lock_zero + reserves
                // are held fixed so refine's iterative convergence stays
                // color-independent (without it, the refine path differs
                // per reserve color and unlocked slots drift accordingly).
                std::array<bool, 32> ehb_per_row_locked{};
                if (options.lock_color0) ehb_per_row_locked[0] = true;
                for (auto i : ehb_cap_excluded) {
                    if (i < ehb_per_row_locked.size())
                        ehb_per_row_locked[i] = true;
                }
                for (std::size_t y = 0; y < h; ++y) {
                    auto& base32 = cr->scanline_palettes[y];
                    while (base32.size() < 32) base32.emplace_back(0.0f, 0.0f, 0.0f);
                    // Pair-aware refinement on the row's base palette
                    // using only that row's pixels — same idea as plain
                    // EHB but per-line, so each scanline's 32 base
                    // colors are jointly optimal under the half-brite
                    // pairing for the colors actually appearing on
                    // that row.
                    palette::refine_ehb_base_palette(
                        std::span<Color3f>(base32.data(), 32),
                        std::span<const Color3f>(
                            img.pixels().data() + y * w, w),
                        /*snap_to_ocs=*/chipset != amiga::Chipset::aga,
                        /*max_iters=*/4,
                        std::span<const bool>(ehb_per_row_locked));
                    if (options.lock_color0)
                        base32[0] = Color3f{0.0f, 0.0f, 0.0f};
                    Palette bp;
                    bp.colors.assign(base32.begin(), base32.end());
                    auto ehb64 = palette::make_ehb_palette(bp.colors);
                    // Update copper-result palette so downstream
                    // consumers (preview render, IFF CMAP) see the
                    // refined values rather than the pre-refinement
                    // copy.
                    base32.assign(bp.colors.begin(), bp.colors.end());
                    if (ehb_cap_excluded.empty()) {
                        pal_lab_per_row[y].resize(ehb64.colors.size());
                        for (std::size_t i = 0; i < ehb64.colors.size(); ++i)
                            pal_lab_per_row[y][i] =
                                color_space::linear_to_oklab(ehb64.colors[i]);
                    } else {
                        pal_lab_per_row[y].reserve(64);
                        cand_to_full_per_row[y].reserve(64);
                        for (std::size_t i = 0; i < ehb64.colors.size(); ++i) {
                            if (ehb_blocked[i]) continue;
                            pal_lab_per_row[y].push_back(
                                color_space::linear_to_oklab(ehb64.colors[i]));
                            cand_to_full_per_row[y].push_back(
                                static_cast<std::uint8_t>(i));
                        }
                    }
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
                        all_indices[y * w + x] = ehb_cap_excluded.empty()
                            ? static_cast<std::uint8_t>(k)
                            : cand_to_full_per_row[y][k];
                        return {chosen, thr};
                    });

                if (d.method == dither::Method::dbs) {
                    if (ehb_cap_excluded.empty()) {
                        dither::apply_dbs_post_pass(
                            img, all_indices,
                            [&](std::size_t /*x*/, std::size_t y)
                                -> std::span<const color_space::OKLab> {
                                return pal_lab_per_row[y];
                            });
                    } else {
                        std::vector<std::vector<std::uint8_t>>
                            full_to_cand_per_row(h);
                        for (std::size_t y = 0; y < h; ++y) {
                            full_to_cand_per_row[y].assign(64, 255);
                            auto& cand = cand_to_full_per_row[y];
                            for (std::size_t k = 0; k < cand.size(); ++k)
                                full_to_cand_per_row[y][cand[k]] =
                                    static_cast<std::uint8_t>(k);
                        }
                        std::vector<std::uint8_t> cand_indices(all_indices.size());
                        for (std::size_t i = 0; i < all_indices.size(); ++i)
                            cand_indices[i] =
                                full_to_cand_per_row[i / w][all_indices[i]];
                        dither::apply_dbs_post_pass(
                            img, cand_indices,
                            [&](std::size_t /*x*/, std::size_t y)
                                -> std::span<const color_space::OKLab> {
                                return pal_lab_per_row[y];
                            });
                        for (std::size_t i = 0; i < cand_indices.size(); ++i)
                            all_indices[i] =
                                cand_to_full_per_row[i / w][cand_indices[i]];
                    }
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
                    std::move(all_indices),
                    std::move(rendered),
                    total_err,
                };
            };

            std::optional<EhbSlicedTrial> winner;
            if (options.best) {
                // Same sweep shape as plain sliced and strips EHB: 8 jitter
                // seeds × 5 strengths × 4 diversities + 1 baseline ×
                // 2 beam states = ~322 trials. 32-color base palette
                // → shallower median-cut basins, amplitude 1.0
                // (AGA-only weakening doesn't apply here since EHB is
                // OCS-bound).
                float best_score = -std::numeric_limits<float>::infinity();
                int sweep_idx = 0;
                for (bool beam_on : {false, true}) {
                    auto sweep_winner = pipeline::best_sweep<EhbSlicedTrial>(
                        *image, dith, options.palette_diversity,
                        /*jitter_count=*/8,
                        [&](const Image& jittered_in,
                            const dither::Settings& d, int div)
                                -> Result<EhbSlicedTrial> {
                            return encode_once(jittered_in, d, div, beam_on);
                        },
                        [](const EhbSlicedTrial& t) -> const Image& {
                            return t.rendered;
                        },
                        [&](float p, std::string_view s) {
                            if (s == "done" && sweep_idx == 0) return;
                            if (options.on_progress) options.on_progress(
                                (static_cast<float>(sweep_idx) + p) * 0.5f, s);
                        },
                        /*jitter_amplitude=*/1.0f);
                    if (!sweep_winner.has_value()) continue;
                    float s2 = ssimulacra2::compute(
                        image->pixels(), sweep_winner->rendered.pixels(),
                        image->width(), image->height());
                    if (s2 > best_score) {
                        best_score = s2;
                        winner = std::move(sweep_winner);
                    }
                    ++sweep_idx;
                }
            }
            if (!winner.has_value()) {
                auto r = encode_once(*image, dith,
                                     options.palette_diversity,
                                     options.sliced_beam,
                                     options.on_progress);
                if (!r) return std::unexpected{r.error()};
                winner = std::move(*r);
                if (options.on_progress) options.on_progress(1.0f, "done");
            }

            auto& first_pal = winner->copper_result.scanline_palettes[0];
            PipelineResult result;
            result.rendered = std::move(winner->rendered);
            result.planes = std::move(winner->planes);
            result.indices = std::move(winner->indices);
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
        if (auto v = palette_locks::validate_pins(options.pins, options.locks,
                                                  options.reserves, 32,
                                                  image->width(), image->height(),
                                                  has_transparency); !v)
            return std::unexpected{v.error()};

        // Plain EHB --best: multi-restart sweep over (dither_strength ×
        // diversity × pre-image jitter), ranked by best_metric.
        //
        // Locks and pins now both flow through. Locks: pop_search
        // honors the locked_mask coming from the assembled palette so
        // the per-slot pin-down works during mutation/crossover; only
        // free slots vary across restarts. Pins: encode_plain_auto
        // applies the pin swap per trial — visual no-op so ranking
        // isn't affected. User palettes still exclude the sweep.
        bool ehb_can_sweep = options.best
                          && !has_user_palette(options);

        // EHB pop-search path. Same eligibility as the legacy sweep; we
        // gate on chipset==OCS because pop_search.cpp's snap-OCS step
        // is OCS-only. Builds 32 base colors via the enriched-source
        // PNN seed (matches the legacy sweep's seeding) → uses that as
        // the seed_palettes for the evolutionary search → ehb_expand
        // makes each candidate score under its 64-entry expansion.
        bool ehb_pop_eligible = ehb_can_sweep &&
                                 chipset == amiga::Chipset::ocs &&
                                 !options.copper && !options.scap &&
                                 !options.dual_playfield;
        if (ehb_pop_eligible) {
            // Build enriched-image PNN seed (same recipe as the legacy
            // sweep's encode_once) — gives pop search a high-quality
            // EHB-aware seed instead of plain median-cut.
            Image enriched(image->width(), image->height() * 2);
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x)
                    enriched[x, y] = (*image)[x, y];
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < image->width(); ++x) {
                    auto p = (*image)[x, y];
                    auto s = color_space::linear_to_srgb(p).clamped();
                    Color3f doubled{
                        std::clamp(s.r * 2.0f, 0.0f, 1.0f),
                        std::clamp(s.g * 2.0f, 0.0f, 1.0f),
                        std::clamp(s.b * 2.0f, 0.0f, 1.0f),
                    };
                    enriched[x, image->height() + y] =
                        color_space::srgb_to_linear(doubled);
                }
            std::vector<Palette> ehb_seeds;
            if (auto q = quantize::quantize(enriched, 32,
                    quantize::Algorithm::pnn,
                    options.palette_diversity); q) {
                snap_to_chipset(*q, chipset, mode);
                palette::refine_ehb_base_palette(
                    std::span<Color3f>(q->colors.data(), 32),
                    image->pixels(),
                    /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
                if (options.lock_color0)
                    q->colors[0] = Color3f{0.0f, 0.0f, 0.0f};
                ehb_seeds.push_back(std::move(*q));
            }

            dither::Settings base_dith;
            base_dith.method = parse_dither(options.dither);
            base_dith.strength = options.dither_strength;
            base_dith.error_clamp = options.error_clamp;
            palette_search::PopSearchOptions pso;
            pso.pop_size      = 128;
            pso.generations   = 64;
            pso.stale_limit   = 32;
            pso.ehb_expand    = true;
            pso.seed_palettes = std::move(ehb_seeds);
            pso.on_progress   = options.on_progress;
            // Transparency support: pop_search forces tmask pixels
            // to slot 0 post-dither and excludes slot 0 from the
            // dither candidate set / mutate gate. Identical wiring
            // as the plain lores/hires path; mark slot 0 in both
            // masks so cpu_fitness keeps it black and the dither
            // can't route opaque pixels there. The half-brite half
            // of the expanded EHB palette (slots 32-63) is left
            // open — slot 32 is half-brite-of-black = black anyway,
            // so opaque-black routing there is visually correct.
            if (has_transparency) {
                pso.tmask = tmask;
                std::vector<bool> mask0(32, false);
                mask0[0] = true;
                pso.locked_mask         = mask0;
                pso.dither_exclude_mask = mask0;
            }
            // Search runs at depth=5 (pop_search internal gate is
            // depth ∈ [1,5]); EHB hardware uses 6 bitplanes but the
            // *base* palette has only 32 entries.
            auto pop = palette_search::run_population_search(
                *image, /*depth=*/5, /*max_colors=*/32,
                mode, chipset, base_dith, options.lock_color0, pso);
            if (pop) {
                // Re-expand to the full 64-entry EHB palette so the
                // encoder + IFF CMAP write the right thing.
                auto ehbp = palette::make_ehb_palette(pop->palette.colors);
                auto enc = bitplane::encode(pop->indices,
                    image->width(), image->height(), 6);
                if (enc) {
                    if (options.on_progress)
                        options.on_progress(1.0f, "done");
                    PipelineResult result;
                    result.rendered  = std::move(pop->rendered);
                    result.planes    = std::move(*enc);
                    result.palette   = std::move(ehbp.colors);
                    result.indices   = std::move(pop->indices);
                    result.mode      = mode;
                    result.hires     = false;
                    result.interlace = options.interlace;
                    result.dpf       = false;
                    result.aga       = is_aga;
                    result.has_transparency = has_transparency;
                    if (has_transparency) result.transparency_mask = tmask;
                    result.finalize_psnr(*image, pop->total_error);
                    return result;
                }
            }
            // Fall through to legacy sweep on pop failure.
        }
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
                // color either matches a bright pixel directly OR is
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
                auto qfn = [&](std::size_t k) -> Result<Palette> {
                    auto q = quantize::quantize(
                        enriched, k, quantize::Algorithm::pnn, diversity);
                    if (!q) return std::unexpected{q.error()};
                    Palette p = std::move(*q);
                    snap_to_chipset(p, chipset, mode);
                    return p;
                };
                std::size_t k1 = options.lock_color0 ? 31 : 32;
                auto qr = palette_locks::two_pass_quantize(
                    qfn, k1, 32, options.lock_color0);
                if (!qr) return std::unexpected{qr.error()};
                Palette bp = std::move(*qr);
                palette_locks::finalize_palette(bp.colors, 32,
                                                 options.lock_color0);
                // Pair-aware refinement: jointly optimize the 32 base
                // colors under the hardware-tied half-brite pairing.
                palette::refine_ehb_base_palette(
                    std::span<Color3f>(bp.colors.data(), 32),
                    img.pixels(),
                    /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
                if (options.lock_color0)
                    bp.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
                auto ehbp = palette::make_ehb_palette(bp.colors);
                auto dr = dither::apply(img, ehbp.colors, d);
                // Force transparent pixels (per the outer tmask) to
                // slot 0 post-dither. The EHB legacy sweep doesn't
                // run pop_search, so this is its own opportunity to
                // honor transparency.
                if (has_transparency) {
                    for (std::size_t i = 0;
                         i < tmask.size() && i < dr.indices.size(); ++i)
                        if (tmask[i]) dr.indices[i] = 0;
                }
                auto bp_res = bitplane::encode(dr.indices,
                                                img.width(), img.height(), 6);
                if (!bp_res) return std::unexpected{bp_res.error()};
                std::vector<Color3f> full_pal(ehbp.colors.begin(),
                                              ehbp.colors.end());
                auto pv = pipeline::render_preview(
                    *bp_res, full_pal, /*is_ham=*/false,
                    options.interlace, chipset);
                if (!pv) return std::unexpected{pv.error()};
                return EhbPlainTrial{
                    std::move(bp), std::move(ehbp), *std::move(bp_res),
                    std::move(dr.indices), *std::move(pv), dr.total_error,
                };
            };
            auto winner = pipeline::best_sweep<EhbPlainTrial>(
                *image, base_dith, options.palette_diversity,
                /*jitter_count=*/8,
                encode_once,
                [](const EhbPlainTrial& t) -> const Image& { return t.rendered; },
                options.on_progress, /*jitter_amplitude=*/1.0f);
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
                if (options.lock_color0)
                    base32[0] = Color3f{0.0f, 0.0f, 0.0f};
                winner->base_pal.colors = std::move(base32);
                winner->ehb_pal = palette::make_ehb_palette(
                    winner->base_pal.colors);
                // Re-dither + re-bitplane + re-render with the refined
                // palette so the result indices/preview reflect it.
                dither::Settings post_dith = base_dith;
                auto dr = dither::apply(*image, winner->ehb_pal.colors,
                                         post_dith);
                auto bp_res = bitplane::encode(
                    dr.indices, image->width(), image->height(), 6);
                if (bp_res) {
                    auto pv = pipeline::render_preview(
                        *bp_res, std::vector<Color3f>(
                            winner->ehb_pal.colors.begin(),
                            winner->ehb_pal.colors.end()),
                        /*is_ham=*/false, options.interlace, chipset);
                    if (pv) {
                        winner->planes = *std::move(bp_res);
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
        bool lock_zero_ehb = !user_pal_ehb
            && (has_transparency || options.lock_color0);
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
            auto qc = palette_locks::quant_counts_for_assemble(
                32, options.locks, reserves_ehb, lock_zero_ehb);
            auto quantized = palette_locks::two_pass_quantize(
                [&](std::size_t k) -> Result<Palette> {
                    return quantize::quantize(*image, k,
                                              quantize::Algorithm::pnn,
                                              options.palette_diversity);
                },
                qc.qcount, qc.kfallback, lock_zero_ehb);
            if (!quantized) return std::unexpected{quantized.error()};
            auto assembled = palette_locks::assemble_with_reserves(
                *quantized, options.locks, options.reserves,
                32, lock_zero_ehb, chipset, mode);
            base_pal = std::move(assembled.palette);
            base_locked = std::move(assembled.locked);
        }

        // Pair-aware refinement: jointly optimize the 32 base colors
        // under the hardware-tied half-brite pairing. Skipped when the
        // user supplied an external palette or has user-defined locks
        // beyond slot-0 — the refinement isn't lock-aware yet. The
        // common slot-0=black lock from --lock-color0 is allowed: we
        // run refine over all 32 entries and re-zero slot 0 after, so
        // the constraint holds.
        bool any_user_lock = false;
        for (std::size_t i = 1; i < base_locked.size(); ++i)
            any_user_lock = any_user_lock || base_locked[i];
        if (!user_pal_ehb && !any_user_lock
                && base_pal.colors.size() >= 32) {
            palette::refine_ehb_base_palette(
                std::span<Color3f>(base_pal.colors.data(), 32),
                image->pixels(),
                /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
            if (lock_zero_ehb)
                base_pal.colors[0] = Color3f{0.0f, 0.0f, 0.0f};
            // Reclaim duplicate half-brite slots by doubling their
            // base color where possible. Doubles the effective
            // color count when the source has dark clusters that
            // collapsed onto the same hb code.
            palette::dedupe_ehb_halfbrite(
                std::span<Color3f>(base_pal.colors.data(), 32),
                /*preserve_slot0=*/lock_zero_ehb);
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
            // dither::apply on a fixed 64-color EHB palette showed it
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
                // Lock + pin compose: rewrite the pinned pixel's index
                // without swapping palette entries (a swap would clobber
                // the lock). See palette_locks::apply_pins for the
                // rationale.
                dither_result.indices[pixel_offset] =
                    static_cast<std::uint8_t>(target);
                continue;
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

        // Sort the BASE 32 by perceptual brightness, keeping locked
        // slots fixed and remapping dither indices (both base 0..31
        // and half-brite 32..63 columns) to match. The half-brite
        // mirror is rebuilt inside sort_by_brightness from the
        // reordered base.
        {
            std::vector<Color3f> ehb64 = ehb_pal.colors;
            palette_locks::sort_by_brightness(
                ehb64, base_locked, dither_result.indices,
                /*sort_n=*/32, /*hb_mirror=*/true);
            ehb_pal.colors = std::move(ehb64);
            // Sync base_pal too so any downstream consumers see the
            // same ordering.
            base_pal.colors.assign(ehb_pal.colors.begin(),
                                    ehb_pal.colors.begin() + 32);
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
                               const dither::Settings& d, int diversity,
                               bool beam_on,
                               std::function<void(float, std::string_view)>
                                   inner_progress = {}) {
            return copper::encode_copper(
                img, depth, d, chipset,
                static_cast<std::size_t>(options.copper_changes),
                copper_user_pal.empty() ? nullptr : &copper_user_pal,
                options.lock_color0, copper_locks,
                diversity,
                skip_initial_lace, options.interlace,
                /*is_ehb=*/false,
                std::move(inner_progress),
                spread_r, spread_d,
                options.sliced_vertical_dither,
                sliced_excluded,
                /*quantizer_override=*/std::nullopt,
                beam_on);
        };

        Result<copper::CopperResult> copper_result;
        if (options.best) {
            // Plain sliced: 8 jitter seeds. 16-color (or wider) palette
            // so the median-cut basin is less acute than DPF's 8-color
            // PF2; 8 seeds × 5×4 = 161 trials is the sweet spot.
            //
            // --best ALSO sweeps the beam axis (phase B forward-look
            // scavenge in encode_copper): two best_sweeps total, one
            // with beam off, one with beam on; the SSIMULACRA2-ranked
            // sweep returns the winner. On images where beam wins
            // (ocs_4096 type — high per-row diversity, low greedy
            // budget use), --best now picks +5..+11 S2 of additional
            // gain over the same flag without beam. On images where
            // beam loses (shooter, fromthe — smooth content where the
            // beam disturbs cross-row palette continuity), the
            // beam-off sweep wins on score and beam isn't selected.
            // Cost is 2× the previous --best wall time for sliced.
            struct CapTrial {
                copper::CopperResult result;
                Image rendered;
            };
            float jitter_amp = (chipset == amiga::Chipset::aga)
                ? 0.4f : 1.0f;
            std::optional<CapTrial> best_overall;
            float best_overall_score = -std::numeric_limits<float>::infinity();
            int sweep_idx = 0;
            for (bool beam_on : {false, true}) {
                auto sweep_winner = pipeline::best_sweep<CapTrial>(
                    *image, dith, options.palette_diversity,
                    /*jitter_count=*/8,
                    [&](const Image& jittered_in,
                        const dither::Settings& d, int div) -> Result<CapTrial> {
                        auto enc = encode_once(jittered_in, d, div, beam_on);
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
                    [&](float p, std::string_view s) {
                        if (s == "done" && sweep_idx == 0) return;
                        if (options.on_progress) options.on_progress(
                            (static_cast<float>(sweep_idx) + p) * 0.5f, s);
                    },
                    jitter_amp);
                ++sweep_idx;
                if (!sweep_winner.has_value()) continue;
                float score = ssimulacra2::compute(
                    image->pixels(), sweep_winner->rendered.pixels(),
                    image->width(), image->height());
                if (score > best_overall_score) {
                    best_overall_score = score;
                    best_overall = std::move(sweep_winner);
                }
            }
            if (best_overall.has_value()) {
                copper_result = std::move(best_overall->result);
            } else {
                copper_result = encode_once(*image, dith,
                                            options.palette_diversity,
                                            /*beam_on=*/false);
            }
        } else {
            copper_result = encode_once(*image, dith,
                                        options.palette_diversity,
                                        options.sliced_beam,
                                        options.on_progress);
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
    //   * DPF: OCS lores DPF (depth=3), 8 PF2 colors. cpp export ready.
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
                options.sliced_vertical_dither,
                options.best,
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
                    options.sliced_spread_radius,
                    options.sliced_spread_decay,
                    options.sliced_vertical_dither,
                    strips_user_pal_span,
                    strips_ehb_reserves,
                    options.sliced_beam);
            }()
            : [&] {
                // strips-DPF reserves/locks: indices reference the OCS
                // DPF 16-entry CLUT layout (CLUT[0..7] = PF1, CLUT[8..15]
                // = PF2). PF1 is zeroed in our encoder, so any CLUT-1..7
                // entry is a silent no-op. PF2 base indices 0..7 map to
                // CLUT registers via the OCS DPF combiner:
                //   PF2 idx 0 → COLOR00 (dual-written to COLOR08 for
                //               AGA-DPF source compatibility)
                //   PF2 idx 1..7 → COLOR09..15
                // Translate CLUT slot → PF2 base index for the planner.
                auto translate_to_pf2 = [&](int user_idx) -> std::optional<int> {
                    if (user_idx < 0) return std::nullopt;
                    auto i = static_cast<std::size_t>(user_idx);
                    if (i == 0 || i == 8) return 0;
                    if (i >= 9 && i <= 15) return static_cast<int>(i - 8);
                    return std::nullopt;
                };
                std::vector<std::pair<std::size_t, Color3f>> strips_dpf_reserves;
                strips_dpf_reserves.reserve(options.reserves.size());
                for (auto& r : options.reserves) {
                    auto pf2 = translate_to_pf2(r.index);
                    if (!pf2) continue;
                    bool dup = std::any_of(strips_dpf_reserves.begin(),
                                           strips_dpf_reserves.end(),
                                           [&](const auto& p) {
                                               return static_cast<int>(p.first) == *pf2;
                                           });
                    if (dup) continue;
                    strips_dpf_reserves.emplace_back(
                        static_cast<std::size_t>(*pf2),
                        palette_locks::to_color(
                            LockSpec{r.index, r.r, r.g, r.b}, chipset, mode));
                }
                std::vector<std::pair<std::size_t, Color3f>> strips_dpf_locks;
                strips_dpf_locks.reserve(options.locks.size());
                for (auto& l : options.locks) {
                    auto pf2 = translate_to_pf2(l.index);
                    if (!pf2) continue;
                    bool dup = std::any_of(strips_dpf_locks.begin(),
                                           strips_dpf_locks.end(),
                                           [&](const auto& p) {
                                               return static_cast<int>(p.first) == *pf2;
                                           });
                    if (dup) continue;
                    strips_dpf_locks.emplace_back(
                        static_cast<std::size_t>(*pf2),
                        palette_locks::to_color(l, chipset, mode));
                }
                return strips::encode_strips_dpf_ocs(
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
                    options.sliced_spread_radius,
                    options.sliced_spread_decay,
                    options.sliced_vertical_dither,
                    strips_user_pal_span,
                    strips_dpf_reserves,
                    strips_dpf_locks,
                    options.sliced_beam);
            }();
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

        // Synthesize per-line palette snapshots for the web tool's
        // per-scanline strip view. Strips planner only retains the
        // line-MOVE deltas, so we replay them against the frame-start
        // palette: for each row, hblank MOVEs (those before the first
        // WAIT in line_moves[y]) update the running state; at the line
        // gate we snapshot — that snapshot is the palette under which
        // the visible part of the scanline begins painting. Mid-line
        // MOVEs after the line gate continue to evolve the running
        // state (they apply at strip boundaries within the row) so
        // subsequent rows pick up where the last visible MOVE left off.
        {
            const std::size_t H = result.rendered.height();
            result.scanline_palettes.resize(H);
            auto running = result.palette;
            for (std::size_t y = 0; y < H && y < result.strips_line_moves.size(); ++y) {
                bool snapped = false;
                for (auto& mv : result.strips_line_moves[y]) {
                    if (mv.kind == strips::ScapOpKind::kWait) {
                        if (!snapped) {
                            result.scanline_palettes[y] = running;
                            snapped = true;
                        }
                    } else if (mv.reg < running.size()) {
                        running[mv.reg] = palette::ocs_to_linear(mv.rgb_ocs);
                    }
                }
                if (!snapped) result.scanline_palettes[y] = running;
            }
        }

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
    // Gated to plain-mode cases. Reserves, transparency, locks
    // (--lock-index), and pins (--pin-index-at) all flow through.
    // Locks: pop_search holds locked positions during mutation /
    // crossover; only free slots vary. Pins: encode_plain_auto applies
    // the pin swap per trial — visual no-op so trial ranking is
    // unaffected. Full user palettes are the only excluded case (every
    // slot is already user-fixed).
    bool lores_plain_best_eligible =
        options.best &&
        (mode == amiga::Mode::lores ||
         mode == amiga::Mode::lores_interlace ||
         mode == amiga::Mode::hires ||
         mode == amiga::Mode::hires_interlace) &&
        !options.copper && !options.scap && !options.dual_playfield &&
        !has_user_palette(options);
    if (lores_plain_best_eligible) {
        // Both --best and the non-best plain auto branch route through
        // encode_plain_auto (anon ns, src/api.cpp). Per-trial knobs flow as
        // function arguments; eligibility above pins locks/reserves/pins/
        // transparency/DPF empty so this matches the non-best behavior
        // bit-for-bit (verified by api-equiv-* ctests). Future feature
        // additions land in encode_plain_auto and propagate to both paths
        // automatically — the previous lock_color0 / refine-locked-mask
        // skew is no longer possible.
        dither::Settings base_dith;
        base_dith.method = parse_dither(options.dither);
        base_dith.strength = options.dither_strength;
        base_dith.error_clamp = options.error_clamp;

        // 1bpp short-circuit: skip pop_search and best_sweep entirely.
        // The palette is fixed to {black, white} (no basin to optimise),
        // so every trial would render identically. encode_plain_auto's
        // own bypass produces the b/w trial in one shot.
        if (max_colors == 2 && options.locks.empty() &&
            options.reserves.empty()) {
            auto trial = encode_plain_auto(
                *image, depth, max_colors, mode, chipset,
                base_dith, options.palette_diversity,
                options.refine_iterations,
                options.lock_color0, has_transparency,
                /*use_dpf=*/false,
                /*match_range=*/options.match_range,
                options.locks, options.reserves, options.pins, tmask);
            if (!trial) return std::unexpected{trial.error()};
            if (options.on_progress) options.on_progress(1.0f, "done");
            std::vector<Color3f> used_pal(
                trial->pal.colors.begin(),
                trial->pal.colors.begin() +
                    static_cast<std::ptrdiff_t>(trial->pal_size));
            PipelineResult result;
            result.rendered = std::move(trial->rendered);
            result.planes   = std::move(trial->planes);
            result.palette  = std::move(used_pal);
            result.indices  = std::move(trial->indices);
            result.mode     = mode;
            result.hires    = compound_hires ||
                              amiga::get_mode_params(mode).is_hires;
            result.interlace = options.interlace;
            result.dpf      = false;
            result.aga      = is_aga;
            result.has_transparency = has_transparency;
            if (has_transparency) result.transparency_mask = tmask;
            result.finalize_psnr(*image, trial->total_error);
            return result;
        }

        // Pop-search-only path: at lores OCS d ∈ [1,5], best_sweep
        // is dominated by pop search every test we've run — and the
        // sweep adds 7-10 s of wall on top of pop. Skip the sweep
        // entirely; pop search self-seeds via internal k-means
        // diversity restarts.
        const auto& mp_curr = amiga::get_mode_params(mode);
        bool pop_only =
            chipset == amiga::Chipset::ocs &&
            !mp_curr.is_hires &&
            depth >= 1 && depth <= 5;
        if (pop_only) {
            // Plain lores OCS --best runs pop search and best_sweep
            // sequentially. Split the user-visible bar in half so the
            // user sees one 0→100% progress, not two reset-to-zero
            // sweeps. pop_phase reports to 0–50%, sweep_phase to
            // 50–100%; pop's intermediate "done" (if any) is swallowed
            // so only sweep's "done" terminates the line.
            auto pop_phase_progress = options.on_progress
                ? std::function<void(float, std::string_view)>(
                    [op = options.on_progress](float p, std::string_view s) {
                        if (s == "done") return;
                        op(0.5f * std::clamp(p, 0.0f, 1.0f), s);
                    })
                : std::function<void(float, std::string_view)>{};
            auto sweep_phase_progress = options.on_progress
                ? std::function<void(float, std::string_view)>(
                    [op = options.on_progress](float p, std::string_view s) {
                        op(0.5f + 0.5f * std::clamp(p, 0.0f, 1.0f), s);
                    })
                : std::function<void(float, std::string_view)>{};
            palette_search::PopSearchOptions pso;
            pso.pop_size      = 128;
            pso.generations   = 64;
            pso.stale_limit   = 32;
            pso.on_progress   = pop_phase_progress;

            // When reserves or transparency are present, run
            // encode_plain_auto once at diversity=0 to produce a
            // properly-assembled seed palette (reserves slotted in
            // at their fixed positions, slot 0 black if transparent).
            // Pop search then refines around that — locked_mask
            // freezes the reserved slots, tmask masks transparent
            // pixels from dither + score.
            // Out-of-range --reserve-range entries (e.g. range 5-7 at
            // d=2 where max_colors=4) are silently dropped by
            // validate_reserves, so a non-empty options.reserves
            // doesn't always mean a reserve will land in the palette.
            // Only build the encode_plain_auto seed when at least
            // one reserve will actually take effect — otherwise the
            // standard internal k-means seeding gives a tighter
            // starting palette (matters at d=2 where the search
            // space is tiny).
            std::size_t reserves_in_pal = 0;
            for (auto& r : options.reserves) {
                if (r.index >= 0 &&
                    static_cast<std::size_t>(r.index) < max_colors)
                    ++reserves_in_pal;
            }
            const bool has_reserves_in_call = reserves_in_pal > 0;
            const bool has_locks_in_call    = !options.locks.empty();
            std::vector<bool> seed_locked_mask;
            // Hold seed_trial across the pop_search call so we can use
            // its rendered output as a fallback when pop_search regresses
            // against the seed (heavy locks compress the search space
            // and cpu_fitness's dither doesn't always match
            // encode_plain_auto's). Compared via SSIMULACRA2 below.
            std::optional<PlainAutoTrial> seed_keep;
            if (has_reserves_in_call || has_transparency ||
                has_locks_in_call) {
                // Use options.palette_diversity for the seed — this
                // matches what the non-best path would produce, so the
                // S2 comparison below is between (pop_search winner)
                // vs (the normal-pass output the user would get
                // without --best), which is what the user asked for.
                auto seed_trial = encode_plain_auto(
                    *image, depth, max_colors, mode, chipset,
                    base_dith, options.palette_diversity,
                    options.refine_iterations,
                    options.lock_color0, has_transparency,
                    /*use_dpf=*/false,
                    /*match_range=*/options.match_range,
                    options.locks, options.reserves, options.pins, tmask);
                if (seed_trial) {
                    Palette seed_pal;
                    seed_pal.name   = "pop-seed";
                    seed_pal.colors = std::vector<Color3f>(
                        seed_trial->pal.colors.begin(),
                        seed_trial->pal.colors.begin() +
                            static_cast<std::ptrdiff_t>(seed_trial->pal_size));
                    pso.seed_palettes.push_back(std::move(seed_pal));
                    // locked_mask = locks ∪ reserves ∪ {slot 0 if
                    // transparent} — used only for mutate/crossover
                    // gating. seed_trial->locked_mask is the assembled
                    // mask containing all three.
                    seed_locked_mask = seed_trial->locked_mask;
                    if (seed_locked_mask.size() < max_colors)
                        seed_locked_mask.resize(max_colors, false);
                    if (has_transparency && !seed_locked_mask.empty())
                        seed_locked_mask[0] = true;
                    pso.locked_mask = seed_locked_mask;
                    // dither_exclude_mask = reserves ∪ {slot 0 if
                    // transparent}. Locks are NOT included — the
                    // dither IS allowed to route image pixels to a
                    // locked color (mi2-redux locks 18/32 slots
                    // expecting them to be reachable).
                    std::vector<bool> excl(max_colors, false);
                    for (auto& r : options.reserves) {
                        if (r.index >= 0 &&
                            static_cast<std::size_t>(r.index) < max_colors)
                            excl[static_cast<std::size_t>(r.index)] = true;
                    }
                    if (has_transparency) excl[0] = true;
                    pso.dither_exclude_mask = std::move(excl);
                    seed_keep = std::move(*seed_trial);
                }
                if (has_transparency) pso.tmask = tmask;
            }

            auto pop = palette_search::run_population_search(
                *image, static_cast<int>(depth), max_colors,
                mode, chipset, base_dith, options.lock_color0, pso);

            // Also run the legacy multi-basin sweep (5 strengths × 2
            // diversities × 8 jitter seeds = 80 trials of
            // encode_plain_auto). Pop search is a single-seed GA — it
            // converges fast on most images but gets stuck in a local
            // optimum on heavy-locks scenes that other quantizer
            // basins would beat. Running both and picking the higher
            // S2 covers both regimes. Wall cost ~doubles, which the
            // user explicitly OK'd for --best.
            auto sweep_encode_once =
                [&](const Image& img, const dither::Settings& d,
                    int diversity) -> Result<PlainAutoTrial> {
                return encode_plain_auto(
                    img, depth, max_colors, mode, chipset,
                    d, diversity, options.refine_iterations,
                    options.lock_color0, has_transparency,
                    /*use_dpf=*/false,
                    /*match_range=*/options.match_range,
                    options.locks, options.reserves, options.pins,
                    tmask);
            };
            auto sweep = pipeline::best_sweep<PlainAutoTrial>(
                *image, base_dith, options.palette_diversity,
                /*jitter_count=*/8,
                sweep_encode_once,
                [](const PlainAutoTrial& t) -> const Image& {
                    return t.rendered;
                },
                sweep_phase_progress, /*jitter_amplitude=*/1.0f);

            // Score helper — masks transparent pixels to black on
            // both sides so SSIMULACRA2 doesn't penalise the ignored
            // regions (matches cpu_fitness's masking).
            auto score_rendered =
                [&](const Image& rnd) -> float {
                if (!has_transparency) {
                    return ssimulacra2::compute(image->pixels(),
                        rnd.pixels(),
                        image->width(), image->height());
                }
                Image src_m(image->width(), image->height());
                Image rnd_m(image->width(), image->height());
                auto sp = src_m.pixels();
                auto rp = rnd_m.pixels();
                auto op = image->pixels();
                auto orp = rnd.pixels();
                for (std::size_t i = 0; i < sp.size(); ++i) {
                    if (i < tmask.size() && tmask[i]) {
                        sp[i] = Color3f{0, 0, 0};
                        rp[i] = Color3f{0, 0, 0};
                    } else {
                        sp[i] = op[i];
                        rp[i] = orp[i];
                    }
                }
                return ssimulacra2::compute(src_m.pixels(),
                    rnd_m.pixels(),
                    image->width(), image->height());
            };

            // 3-way candidate ranking: pop / seed / sweep. seed_keep is
            // only present when locks/reserves/transparency are in
            // play (otherwise pop_search's internal k-means seeding
            // suffices). Pick the highest-scoring candidate.
            enum Kind { K_POP, K_SEED, K_SWEEP };
            struct Cand { float s2; Kind kind; };
            std::vector<Cand> cands;
            if (pop)
                cands.push_back({score_rendered(pop->rendered), K_POP});
            if (seed_keep)
                cands.push_back({score_rendered(seed_keep->rendered),
                                  K_SEED});
            if (sweep)
                cands.push_back({score_rendered(sweep->rendered),
                                  K_SWEEP});

            if (!cands.empty()) {
                auto win = std::max_element(cands.begin(), cands.end(),
                    [](const Cand& a, const Cand& b) {
                        return a.s2 < b.s2;
                    });
                PipelineResult result;
                if (win->kind == K_POP) {
                    auto enc = bitplane::encode(pop->indices,
                        image->width(), image->height(), depth);
                    if (!enc) return std::unexpected{enc.error()};
                    result.rendered = std::move(pop->rendered);
                    result.planes   = std::move(*enc);
                    result.palette  = std::move(pop->palette.colors);
                    result.indices  = std::move(pop->indices);
                    result.finalize_psnr(*image, pop->total_error);
                } else if (win->kind == K_SEED) {
                    std::vector<Color3f> pal_keep(
                        seed_keep->pal.colors.begin(),
                        seed_keep->pal.colors.begin() +
                            static_cast<std::ptrdiff_t>(
                                seed_keep->pal_size));
                    result.rendered = std::move(seed_keep->rendered);
                    result.planes   = std::move(seed_keep->planes);
                    result.palette  = std::move(pal_keep);
                    result.indices  = std::move(seed_keep->indices);
                    result.finalize_psnr(*image,
                        seed_keep->total_error);
                } else {  // K_SWEEP
                    std::vector<Color3f> pal_keep(
                        sweep->pal.colors.begin(),
                        sweep->pal.colors.begin() +
                            static_cast<std::ptrdiff_t>(
                                sweep->pal_size));
                    result.rendered = std::move(sweep->rendered);
                    result.planes   = std::move(sweep->planes);
                    result.palette  = std::move(pal_keep);
                    result.indices  = std::move(sweep->indices);
                    result.finalize_psnr(*image, sweep->total_error);
                }
                result.mode      = mode;
                result.hires     = compound_hires || mp_curr.is_hires;
                result.interlace = options.interlace;
                result.dpf       = false;
                result.aga       = is_aga;
                result.has_transparency = has_transparency;
                result.transparency_mask = tmask;
                return result;
            }
            // All three failed — fall through to single-pass.
        }

        auto encode_once = [&](const Image& img,
                               const dither::Settings& d,
                               int diversity) -> Result<PlainAutoTrial> {
            return encode_plain_auto(
                img, depth, max_colors, mode, chipset,
                d, diversity, options.refine_iterations,
                options.lock_color0, has_transparency,
                /*use_dpf=*/false, /*match_range=*/options.match_range,
                options.locks, options.reserves, options.pins,
                tmask);
        };
        auto winner = pipeline::best_sweep<PlainAutoTrial>(
            *image, base_dith, options.palette_diversity,
            /*jitter_count=*/8,
            encode_once,
            [](const PlainAutoTrial& t) -> const Image& { return t.rendered; },
            options.on_progress, /*jitter_amplitude=*/1.0f);
        if (winner) {
            std::vector<Color3f> used_pal(
                winner->pal.colors.begin(),
                winner->pal.colors.begin() +
                    static_cast<std::ptrdiff_t>(winner->pal_size));

            // d≤5 case is already handled by the pop_only branch above.
            // This block runs for d≥6 (or pop_only fallback) where
            // the sweep winner stands.
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
            result.has_transparency = has_transparency;
            result.transparency_mask = tmask;
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

    // OCS DPF exposes the user-facing 16-entry CLUT view for
    // --reserve-range / --lock-index / --pin-index-at: CLUT 0/8 → PF2
    // idx 0, CLUT 9..15 → PF2 idx 1..7, CLUT 1..7 = PF1 territory
    // (silently dropped). Translate before feeding validate / quantise
    // / assemble / apply_pins so the encoder sees only PF2-base
    // indices. Stays in lockstep with main.cpp's CLI path (the
    // api-equiv-dpf-* ctests pin both ends).
    bool dpf_clut_indexing = use_dpf && chipset != amiga::Chipset::aga;
    auto dpf_clut_to_pf2 = [&](int user_idx) -> std::optional<int> {
        if (user_idx < 0) return std::nullopt;
        auto i = static_cast<std::size_t>(user_idx);
        if (i == 0 || i == 8) return 0;
        if (i >= 9 && i <= 15) return static_cast<int>(i - 8);
        return std::nullopt;
    };
    auto translate_dpf_reserves =
        [&](std::span<const ReserveSpec> raw) {
            std::vector<ReserveSpec> out;
            out.reserve(raw.size());
            if (!dpf_clut_indexing) {
                out.assign(raw.begin(), raw.end());
                return out;
            }
            bool slot0_emitted = false;
            for (auto& r : raw) {
                auto pf2_idx = dpf_clut_to_pf2(r.index);
                if (!pf2_idx) continue;
                if (*pf2_idx == 0) {
                    if (lock_zero) continue;
                    if (slot0_emitted) continue;
                    slot0_emitted = true;
                }
                out.push_back({*pf2_idx, r.r, r.g, r.b});
            }
            return out;
        };
    auto translate_dpf_locks =
        [&](std::span<const LockSpec> raw) {
            std::vector<LockSpec> out;
            out.reserve(raw.size());
            if (!dpf_clut_indexing) {
                out.assign(raw.begin(), raw.end());
                return out;
            }
            bool slot0_emitted = false;
            for (auto& l : raw) {
                auto pf2_idx = dpf_clut_to_pf2(l.index);
                if (!pf2_idx) continue;
                if (*pf2_idx == 0) {
                    if (slot0_emitted) continue;
                    slot0_emitted = true;
                }
                out.push_back({*pf2_idx, l.r, l.g, l.b});
            }
            return out;
        };
    auto translate_dpf_pins =
        [&](std::span<const PinSpec> raw) {
            std::vector<PinSpec> out;
            out.reserve(raw.size());
            if (!dpf_clut_indexing) {
                out.assign(raw.begin(), raw.end());
                return out;
            }
            for (auto& p : raw) {
                auto pf2_idx = dpf_clut_to_pf2(p.index);
                if (!pf2_idx) continue;
                out.push_back({*pf2_idx, p.x, p.y});
            }
            return out;
        };
    std::vector<ReserveSpec> effective_reserves =
        translate_dpf_reserves(options.reserves);
    std::vector<LockSpec> effective_locks =
        translate_dpf_locks(options.locks);
    std::vector<PinSpec> effective_pins =
        translate_dpf_pins(options.pins);

    // Validate locks/pins (no-op for HAM/copper paths above which return earlier).
    // Locks override the implicit reserve-zero rule when index 0 is locked.
    if (auto v = palette_locks::validate_locks(effective_locks, max_colors); !v)
        return std::unexpected{v.error()};
    if (auto v = palette_locks::validate_pins(effective_pins, effective_locks,
                                              effective_reserves, max_colors,
                                              image->width(), image->height(),
                                              lock_zero); !v)
        return std::unexpected{v.error()};
    auto reserves_in_pal = palette_locks::validate_reserves(
        effective_reserves, effective_locks, max_colors, lock_zero);
    if (!reserves_in_pal) return std::unexpected{reserves_in_pal.error()};
    std::size_t reserve_count = *reserves_in_pal;
    // Build a reserved_mask the dither path uses to exclude these slots
    // from the candidate set (locks DON'T appear here — they remain
    // valid dither targets per their lock-not-reserve semantics).
    std::vector<bool> reserved_mask(max_colors, false);
    for (auto& r : effective_reserves) {
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
        //  p1-low cyan/magenta/gray, p1-high cyan/magenta/white)
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
                    for (auto& entry : pal_lab) {
                        float dL = lab.L - entry.L;
                        float da = lab.a - entry.a;
                        float db = lab.b - entry.b;
                        float d = color_space::fma_dist_sq(dL, da, db);
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
        // AUTO palette: route through encode_plain_auto so non-best and
        // --best plain paths share the SAME quantize → assemble → refine →
        // dither → encode → render pipeline. Eliminates the architectural
        // divergence that produced the lock_color0 bug (slot 0 != #000000
        // under --best because the inline best lambda skipped
        // assemble_with_reserves' lock_color0 prepend).
        //
        // The trial result carries everything the non-best path needs:
        // post-assembly palette, locked_mask, dither indices+total_error,
        // encoded planes, rendered preview. Skip the shared post-palette
        // tail (refine/dither/pins/sort/encode/render at L~3486-3599) by
        // early-returning from this branch with the constructed
        // PipelineResult.
        dither::Settings auto_dith;
        auto_dith.method = parse_dither(options.dither);
        auto_dith.strength = options.dither_strength;
        auto_dith.error_clamp = options.error_clamp;
        auto trial = encode_plain_auto(
            *image, depth, max_colors, mode, chipset,
            auto_dith, options.palette_diversity, options.refine_iterations,
            options.lock_color0, has_transparency, use_dpf,
            options.match_range,
            effective_locks, effective_reserves, effective_pins, tmask);
        if (!trial) return std::unexpected{trial.error()};
        std::vector<Color3f> used_palette(
            trial->pal.colors.begin(),
            trial->pal.colors.begin() +
                static_cast<std::ptrdiff_t>(trial->pal_size));
        auto trial_planes = std::move(trial->planes);
        auto trial_indices = std::move(trial->indices);
        // DPF expansion lives in the call-site (post-trial) since the
        // helper returns the un-expanded planes for sliced/copper/strips
        // re-use too (they all need the unexpanded base preview).
        if (use_dpf) {
            auto expanded = bitplane::expand_to_dpf_pf2(trial_planes);
            if (!expanded) return std::unexpected{expanded.error()};
            trial_planes = *std::move(expanded);
            auto pf2_base = std::size_t{1} <<
                            (trial_planes.depth / 2);
            std::vector<Color3f> shifted(pf2_base, Color3f{0, 0, 0});
            shifted.insert(shifted.end(),
                           used_palette.begin(), used_palette.end());
            used_palette = std::move(shifted);
            for (auto& idx : trial_indices)
                idx = static_cast<std::uint8_t>(idx + pf2_base);
        }
        PipelineResult result;
        result.rendered = std::move(trial->rendered);
        result.planes = std::move(trial_planes);
        result.palette = std::move(used_palette);
        result.indices = std::move(trial_indices);
        result.mode = mode;
        result.hires = compound_hires ||
                       amiga::get_mode_params(mode).is_hires;
        result.interlace = options.interlace;
        result.dpf = use_dpf;
        result.aga = is_aga;
        result.has_transparency = has_transparency;
        result.transparency_mask = tmask;
        if (has_transparency) {
            for (std::size_t i = 0; i < tmask.size(); ++i)
                if (tmask[i])
                    result.rendered.pixels()[i] = Color3f{0, 0, 0};
        }
        result.finalize_psnr(*image, trial->total_error);
        return result;
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

    // Sort the visible palette by perceptual brightness (OKLab L), keeping
    // locked slots in place and remapping the dither indices to match.
    // HAM is excluded — its index encodes hardware operations, not slots.
    // DPF is also skipped here because the expansion below shifts the
    // palette into the upper registers; sorting first then expanding
    // would mis-align lock indices vs the shifted palette layout.
    if (!amiga::is_ham(mode) && !use_dpf) {
        palette_locks::sort_by_brightness(pal.colors, locked_mask,
                                          dither_result.indices,
                                          pal.colors.size());
        pal_span = std::span<const Color3f>(pal.colors);
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
    // EHB sliced/strips: report 64 (matching plain EHB) since the
    // hardware always doubles to 64 effective colors per line.
    if (p.mode == amiga::Mode::ehb && r.colors == 32) r.colors = 64;
    // Pack the final palette as sRGB bytes (3 per entry) for the web
    // tool's palette swatch view. Empty when palette is empty.
    //
    // EHB sliced/strips store only the 32 base colors per scanline
    // (the line-0 base is what gets exposed via p.palette); plain EHB
    // already stores 64 (32 base + 32 hardware half-brite). Extend the
    // sliced/strips case to 64 so the swatch view is consistent across
    // all EHB variants.
    std::vector<Color3f> swatch_pal = p.palette;
    if (p.mode == amiga::Mode::ehb && swatch_pal.size() == 32) {
        auto ehb = palette::make_ehb_palette(swatch_pal);
        swatch_pal = std::move(ehb.colors);
    }
    r.paletteBytes.reserve(swatch_pal.size() * 3);
    for (auto& c : swatch_pal) {
        auto s = color_space::linear_to_srgb(c).clamped();
        r.paletteBytes.push_back(static_cast<std::uint8_t>(
            std::round(s.r * 255.0f)));
        r.paletteBytes.push_back(static_cast<std::uint8_t>(
            std::round(s.g * 255.0f)));
        r.paletteBytes.push_back(static_cast<std::uint8_t>(
            std::round(s.b * 255.0f)));
    }
    // Per-pixel palette indices for the web swatch hover-isolate
    // feature. Only forwarded when the mode emits a true 1:1 grid
    // (i.e. p.indices.size() == width × height) — sliced / strips
    // / HAM / tile modes leave it empty.
    auto expected_n = p.rendered.width() * p.rendered.height();
    if (p.indices.size() == expected_n) {
        r.indices = p.indices;
    }

    // Per-scanline palette swatch strip for sliced / strips / copper-HAM
    // modes. Each row's full base palette gets packed as sRGB RGB triples;
    // the web tool renders this as a vertical strip beside the preview.
    if (!p.scanline_palettes.empty()) {
        std::size_t colors_per_row = p.scanline_palettes[0].size();
        if (colors_per_row > 0) {
            r.scanlinePaletteSize = static_cast<int>(colors_per_row);
            r.scanlinePaletteBytes.reserve(
                p.scanline_palettes.size() * colors_per_row * 3);
            for (auto& row_pal : p.scanline_palettes) {
                // Per-row palettes can theoretically differ in length
                // across modes; clamp to the row-0 size so the JS-side
                // grid is rectangular. Pad short rows with black.
                std::size_t n = std::min(row_pal.size(), colors_per_row);
                for (std::size_t i = 0; i < n; ++i) {
                    auto s = color_space::linear_to_srgb(row_pal[i]).clamped();
                    r.scanlinePaletteBytes.push_back(static_cast<std::uint8_t>(
                        std::round(s.r * 255.0f)));
                    r.scanlinePaletteBytes.push_back(static_cast<std::uint8_t>(
                        std::round(s.g * 255.0f)));
                    r.scanlinePaletteBytes.push_back(static_cast<std::uint8_t>(
                        std::round(s.b * 255.0f)));
                }
                for (std::size_t i = n; i < colors_per_row; ++i) {
                    r.scanlinePaletteBytes.push_back(0);
                    r.scanlinePaletteBytes.push_back(0);
                    r.scanlinePaletteBytes.push_back(0);
                }
            }
        }
    }
    r.copperChanges = p.copper_changes;
    r.avgPaletteUsedPerLine =
        pipeline::compute_avg_palette_used_per_line(p.rendered, p.scanline_palettes);
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
        } else if (amiga::is_cga_text(mode)) {
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
// Internal: copy a PipelineResult's fields into an EncodeState. Shared
// between encode_state (bytes input) and encode_state_image (Image input)
// so the public State stays in sync without duplicate field lists.
namespace {
void state_from_pipeline_result(EncodeStateOrError& out,
                                pipeline::PipelineResult& p) {
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
}
}  // anon namespace

EncodeStateOrError encode_state(const std::uint8_t* input_data,
                                std::size_t input_size,
                                const Options& options) {
    EncodeStateOrError out;
    auto r = run_pipeline(input_data, input_size, options);
    if (!r) {
        out.error_msg = r.error().message;
        return out;
    }
    state_from_pipeline_result(out, *r);
    return out;
}

EncodeStateOrError encode_state_image(const Image& image,
                                       const Options& options) {
    EncodeStateOrError out;
    // Bytes input is unused on this path; pass empty + the prepared
    // image. run_pipeline detects the prepared_image and skips
    // load_and_preprocess (no decode, no scale, no preprocess) so the
    // float pixels reach the encoder without an 8-bit PNG round-trip.
    auto r = run_pipeline(nullptr, 0, options, &image);
    if (!r) {
        out.error_msg = r.error().message;
        return out;
    }
    state_from_pipeline_result(out, *r);
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
