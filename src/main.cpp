#include "amiga.hpp"
#include "api.hpp"
#include "bitplane.hpp"
#include "cheader.hpp"
#include "degas.hpp"
#include "color_space.hpp"
#include "copper.hpp"
#include "dither.hpp"
#include "ham.hpp"
#include "iff.hpp"
#include "log.hpp"
#include "palette.hpp"
#include "palette_io.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace png2amiga;

// ---------------------------------------------------------------------------
// Pad bitplane data to mode display width (for viewer .cpp export only)
// ---------------------------------------------------------------------------

void pad_planes_to_mode(bitplane::BitplaneData& planes, amiga::Mode mode,
                        bool hires = false) {
    auto display_w = (hires || amiga::get_mode_params(mode).is_hires)
        ? std::size_t{640} : amiga::default_width(mode);
    if (planes.width == display_w) return;
    auto old_bpr = planes.bytes_per_row;
    auto new_bpr = ((display_w + 15) / 16) * 2;
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

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Config {
    std::string input_path;
    std::string output_path;           // .png for preview, .iff for ILBM, .h for C header
    amiga::Mode mode = amiga::Mode::lores;
    bool hires = false;                // compound mode hires override
    bool interlace = false;            // LACE bit in CAMG
    std::size_t depth = 5;
    preprocess::Settings preprocess{};
    std::optional<std::size_t> width;
    std::optional<std::size_t> height;
    bool match_range = false;

    // HAM encoding
    ham::Quality ham_quality = ham::Quality::optimal;
    std::size_t ham_beam = 16;

    // Dithering
    dither::Method dither_method = dither::Method::floyd_steinberg;
    bool dither_explicit = false;       // true if user passed --dither
    float dither_strength = 1.0f;
    float error_clamp = 0.12f;

    // Chipset
    std::optional<amiga::Chipset> chipset;  // empty = auto-detect from mode

    // Palette
    std::string palette_file;            // load palette from file (empty = auto)

    // C header
    std::string symbol_name;           // base name for C symbols (default: derived from output)

    // Transparency
    float alpha_threshold = 0.0f;      // offset from 0.5 midpoint (-0.5..0.5)
    dither::Method alpha_dither = dither::Method::none;  // none = use threshold
    float alpha_dither_strength = 1.0f; // strength for alpha dither (independent)

    // Copper palette
    bool copper = false;               // per-scanline palette changes
    int copper_changes = 0;            // 0 = auto (based on chipset/depth)

    // Cropping
    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;                    // 0 = no crop
    int crop_h = 0;
    bool crop_auto = false;            // auto-crop to mode aspect ratio
};

void print_usage() {
    std::println(stderr,
        "Usage: png2amiga [options] input.[png|jpg] [-o output.png|output.iff|output.h]\n"
        "\n"
        "Modes:\n"
        "  --mode <mode>                   Graphics mode (default: lores)\n"
        "         lores, lores-lace, hires, hires-lace,\n"
        "         ham6, ham6-lace, ham6-hires, ham6-hires-lace,\n"
        "         ham8, ham8-lace, ham8-hires, ham8-hires-lace,\n"
        "         ehb, ehb-lace,\n"
        "         stf-low, stf-med, stf-hi, ste-low, ste-med, ste-hi\n"
        "  --depth <1-8>                   Bitplane depth (default: 5)\n"
        "  --chipset ocs|aga               OCS 12-bit / AGA 24-bit (default: auto)\n"
        "  --copper                        Per-scanline copper palettes\n"
        "\n"
        "HAM encoding:\n"
        "  --ham-quality fast|optimal      HAM encoding quality (default: optimal)\n"
        "  --ham-beam <1-256>              Beam width for DP search (default: 48)\n"
        "\n"
        "Dithering:\n"
        "  --dither <method>               none|bayer2x2|bayer4x4|bayer8x8|\n"
        "                                  checker|h2x4|clustered-dot|\n"
        "                                  line2|line-checker|line4|line8|\n"
        "                                  floyd-steinberg|atkinson|sierra-lite|\n"
        "                                  stucki|jarvis (default: floyd-steinberg)\n"
        "  --dither-strength <float>       Dither amount 0.0-1.0 (default: 1.0)\n"
        "  --error-clamp <float>           Max error per channel (default: 0.12)\n"
        "\n"
        "Palette:\n"
        "  --palette <file>                Load palette from file (GIMP .gpl, IFF, hex text)\n"
        "\n"
        "Image processing:\n"
        "  --brightness <float>            Brightness -1.0 to 1.0 (default: 0.0)\n"
        "  --contrast <float>              Contrast 0.0-3.0 (default: 1.0)\n"
        "  --saturation <float>            Saturation 0.0-3.0 (default: 1.0)\n"
        "  --gamma <float>                 Gamma 0.1-8.0 (default: 1.0)\n"
        "  --hue-shift <float>             Hue rotation -180 to 180 (default: 0)\n"
        "  --sharpen <float>               Sharpen/blur -1.0 to 2.0 (default: 0.0)\n"
        "  --black-point <float>           Black point 0.0-0.5 (default: 0.0)\n"
        "  --white-point <float>           White point 0.0-0.5 (default: 0.0)\n"
        "  --match-range                   Match image range to palette\n"
        "  --width <int>                   Override output width\n"
        "  --height <int>                  Override output height\n"
        "\n"
        "Transparency (color 0 = transparent when input has alpha):\n"
        "  --alpha-threshold <-0.5..0.5>   Offset from 0.5 midpoint (default: 0)\n"
        "  --alpha-dither <method>         Dither alpha (e.g. checker, bayer4x4)\n"
        "  --alpha-dither-strength <float> Alpha dither strength (default: 1.0)\n"
        "\n"
        "Cropping:\n"
        "  --crop <x,y,w,h>               Manual crop region (pixels)\n"
        "  --crop-auto                     Auto-crop to mode aspect ratio (center)\n"
        "\n"
        "C header output:\n"
        "  --symbol <name>                 Base symbol name (default: from filename)\n"
        "\n"
        "Output:\n"
        "  .png extension -> preview PNG image\n"
        "  .iff extension -> IFF ILBM Amiga image file\n"
        "  .h extension   -> C header with UWORD bitplane arrays\n"
        "  .raw extension -> raw interleaved bitplane data (no header)\n"
        "  .pal extension -> OCS 12-bit palette (2 bytes/color, big-endian 0x0RGB)");
}

Result<dither::Method> parse_dither_method(std::string_view s) {
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
    return std::unexpected{Error{ErrorCode::unsupported_mode,
        "Unknown dither method: " + std::string(s)}};
}

Result<Config> parse_args(int argc, char* argv[]) {
    Config config;
    int positional = 0;

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view(argv[i]);

        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        }

        if (arg == "--match-range") {
            config.match_range = true;
            continue;
        }


        if (arg == "--crop-auto") {
            config.crop_auto = true;
            continue;
        }

        if (arg == "--copper") {
            config.copper = true;
            continue;
        }

        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            config.output_path = std::string(argv[++i]);
            continue;
        }

        // (--alpha-dither moved to value-args below)

        if (arg.starts_with("--") && i + 1 < argc) {
            auto val = std::string_view(argv[++i]);

            if (arg == "--mode") {
                auto v = std::string(val);
                // Decompose compound modes: extract base mode + hires/lace flags
                bool mode_hires = v.find("hires") != std::string::npos;
                bool mode_lace = v.size() > 4 && v.find("-lace") != std::string::npos;
                if (v == "lores") config.mode = amiga::Mode::lores;
                else if (v == "lores-lace") config.mode = amiga::Mode::lores_interlace;
                else if (v == "hires") config.mode = amiga::Mode::hires;
                else if (v == "hires-lace") config.mode = amiga::Mode::hires_interlace;
                else if (v.starts_with("ham6")) config.mode = amiga::Mode::ham6;
                else if (v.starts_with("ham8")) config.mode = amiga::Mode::ham8;
                else if (v.starts_with("ehb")) config.mode = amiga::Mode::ehb;
                else if (v == "stf-low") config.mode = amiga::Mode::stf_low;
                else if (v == "stf-med") config.mode = amiga::Mode::stf_med;
                else if (v == "ste-low") config.mode = amiga::Mode::ste_low;
                else if (v == "ste-med") config.mode = amiga::Mode::ste_med;
                else if (v == "stf-hi") config.mode = amiga::Mode::stf_hi;
                else if (v == "ste-hi") config.mode = amiga::Mode::ste_hi;
                else return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown mode: " + v}};
                // Apply compound mode overrides + set flags from built-in modes
                if (mode_hires) { config.hires = true; if (!config.width) config.width = 640; }
                auto mp = amiga::get_mode_params(config.mode);
                config.interlace = mode_lace || mp.is_interlaced;
                config.hires = config.hires || mp.is_hires;
            }
            else if (arg == "--depth") {
                config.depth = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
            }
            else if (arg == "--chipset") {
                if (val == "ocs") config.chipset = amiga::Chipset::ocs;
                else if (val == "aga") config.chipset = amiga::Chipset::aga;
                else return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown chipset: " + std::string(val) + " (use ocs or aga)"}};
            }
            else if (arg == "--ham-quality") {
                if (val == "fast") config.ham_quality = ham::Quality::fast;
                else if (val == "optimal") config.ham_quality = ham::Quality::optimal;
                else return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown HAM quality: " + std::string(val) + " (use fast or optimal)"}};
            }
            else if (arg == "--ham-beam") {
                config.ham_beam = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
                if (config.ham_beam < 1) config.ham_beam = 1;
                if (config.ham_beam > 256) config.ham_beam = 256;
            }
            else if (arg == "--copper-changes") {
                config.copper_changes = std::atoi(std::string(val).c_str());
            }
            else if (arg == "--weight-l") {
                color_space::WEIGHT_L = std::stof(std::string(val));
            }
            else if (arg == "--weight-a") {
                color_space::WEIGHT_A = std::stof(std::string(val));
            }
            else if (arg == "--weight-b") {
                color_space::WEIGHT_B = std::stof(std::string(val));
            }
            else if (arg == "--dither") {
                auto m = parse_dither_method(val);
                if (!m) return std::unexpected{m.error()};
                config.dither_method = *m;
                config.dither_explicit = true;
            }
            else if (arg == "--dither-strength") {
                config.dither_strength = std::stof(std::string(val));
            }
            else if (arg == "--error-clamp") {
                config.error_clamp = std::stof(std::string(val));
            }
            else if (arg == "--brightness") {
                config.preprocess.brightness = std::stof(std::string(val));
            }
            else if (arg == "--contrast") {
                config.preprocess.contrast = std::stof(std::string(val));
            }
            else if (arg == "--saturation") {
                config.preprocess.saturation = std::stof(std::string(val));
            }
            else if (arg == "--gamma") {
                config.preprocess.gamma = std::stof(std::string(val));
            }
            else if (arg == "--hue-shift") {
                config.preprocess.hue_shift = std::stof(std::string(val));
            }
            else if (arg == "--sharpen") {
                config.preprocess.sharpen = std::stof(std::string(val));
            }
            else if (arg == "--black-point") {
                config.preprocess.black_point = std::stof(std::string(val));
            }
            else if (arg == "--white-point") {
                config.preprocess.white_point = std::stof(std::string(val));
            }
            else if (arg == "--width") {
                config.width = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
            }
            else if (arg == "--height") {
                config.height = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
            }
            else if (arg == "--symbol") {
                config.symbol_name = std::string(val);
            }
            else if (arg == "--alpha-threshold") {
                config.alpha_threshold = std::stof(std::string(val));
            }
            else if (arg == "--alpha-dither") {
                auto m = parse_dither_method(val);
                if (!m) return std::unexpected{m.error()};
                config.alpha_dither = *m;
            }
            else if (arg == "--alpha-dither-strength") {
                config.alpha_dither_strength = std::stof(std::string(val));
            }
            else if (arg == "--palette") {
                config.palette_file = std::string(val);
            }
            else if (arg == "--crop") {
                // Parse "x,y,w,h" format
                auto s = std::string(val);
                int parsed[4]{};
                int idx = 0;
                std::size_t pos = 0;
                while (pos < s.size() && idx < 4) {
                    auto comma = s.find(',', pos);
                    auto token = s.substr(pos, comma - pos);
                    parsed[idx++] = std::atoi(token.c_str());
                    pos = (comma == std::string::npos) ? s.size() : comma + 1;
                }
                if (idx == 4) {
                    config.crop_x = parsed[0];
                    config.crop_y = parsed[1];
                    config.crop_w = parsed[2];
                    config.crop_h = parsed[3];
                } else {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Crop format must be x,y,w,h (e.g. 10,20,300,200)"}};
                }
            }
            else {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown option: " + std::string(arg)}};
            }
        }
        else if (arg.starts_with("--")) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "Unknown option: " + std::string(arg)}};
        }
        else {
            if (positional == 0) config.input_path = std::string(arg);
            else if (positional == 1) config.output_path = std::string(arg);
            ++positional;
        }
    }

    if (config.input_path.empty()) {
        print_usage();
        std::exit(1);
    }

    return config;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    return s.substr(s.size() - suffix.size()) == suffix;
}

// Derive a C symbol name from a filename path
std::string derive_symbol_name(std::string_view path) {
    // Extract filename without directory
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos)
        path = path.substr(slash + 1);
    auto backslash = path.rfind('\\');
    if (backslash != std::string_view::npos)
        path = path.substr(backslash + 1);

    // Remove extension
    auto dot = path.rfind('.');
    if (dot != std::string_view::npos)
        path = path.substr(0, dot);

    // Sanitize: replace non-alphanumeric with underscore
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

    // Compute the largest rectangle of the target aspect ratio that fits
    auto target_ratio = static_cast<double>(target_w) / static_cast<double>(target_h);
    auto src_ratio = static_cast<double>(src_w) / static_cast<double>(src_h);

    std::size_t crop_w, crop_h;
    if (src_ratio > target_ratio) {
        // Source is wider than target ratio — crop width
        crop_h = src_h;
        crop_w = static_cast<std::size_t>(
            static_cast<double>(src_h) * target_ratio + 0.5);
    } else {
        // Source is taller than target ratio — crop height
        crop_w = src_w;
        crop_h = static_cast<std::size_t>(
            static_cast<double>(src_w) / target_ratio + 0.5);
    }

    // Center the crop
    auto cx = (src_w - crop_w) / 2;
    auto cy = (src_h - crop_h) / 2;

    return crop_image(src, cx, cy, crop_w, crop_h);
}

// ---------------------------------------------------------------------------
// iTerm2 inline image display
// ---------------------------------------------------------------------------

constexpr auto base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        auto a = data[i], b = data[i + 1], c = data[i + 2];
        out += base64_chars[(a >> 2) & 0x3F];
        out += base64_chars[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
        out += base64_chars[((b & 0x0F) << 2) | ((c >> 6) & 0x03)];
        out += base64_chars[c & 0x3F];
        i += 3;
    }
    if (i < data.size()) {
        auto a = data[i];
        out += base64_chars[(a >> 2) & 0x3F];
        if (i + 1 < data.size()) {
            auto b = data[i + 1];
            out += base64_chars[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
            out += base64_chars[((b & 0x0F) << 2)];
        } else {
            out += base64_chars[((a & 0x03) << 4)];
            out += '=';
        }
        out += '=';
    }
    return out;
}

void iterm2_display(const Image& image, unsigned scale = 2) {
    auto png = png_io::encode(image);
    if (!png) return;
    auto encoded = base64_encode(*png);
    std::println("\033]1337;File=inline=1;size={};width={}px;height={}px:{}\a",
                 png->size(), image.width() * scale, image.height() * scale,
                 encoded);
}

[[maybe_unused]]
void iterm2_display_with_mask(const Image& image, const std::vector<bool>& mask,
                              unsigned scale = 2) {
    auto png = png_io::encode(image, mask);
    if (!png) return;
    auto encoded = base64_encode(*png);
    std::println("\033]1337;File=inline=1;size={};width={}px;height={}px:{}\a",
                 png->size(), image.width() * scale, image.height() * scale,
                 encoded);
}

const char* dither_name(dither::Method m) {
    switch (m) {
    case dither::Method::none: return "none";
    case dither::Method::bayer2x2: return "bayer2x2";
    case dither::Method::bayer4x4: return "bayer4x4";
    case dither::Method::bayer8x8: return "bayer8x8";
    case dither::Method::checker: return "checker";
    case dither::Method::h2x4: return "h2x4";
    case dither::Method::clustered_dot: return "clustered-dot";
    case dither::Method::line2: return "line2";
    case dither::Method::line_checker: return "line-checker";
    case dither::Method::line4: return "line4";
    case dither::Method::v4x2: return "v4x2";
    case dither::Method::bayer4x2: return "bayer4x2";
    case dither::Method::bayer2x4: return "bayer2x4";
    case dither::Method::line8: return "line8";
    case dither::Method::halftone8x8: return "halftone8x8";
    case dither::Method::diagonal8x8: return "diagonal8x8";
    case dither::Method::spiral5x5: return "spiral5x5";
    case dither::Method::hex8x8: return "hex8x8";
    case dither::Method::hex5x5: return "hex5x5";
    case dither::Method::blue_noise: return "blue-noise";
    case dither::Method::floyd_steinberg: return "floyd-steinberg";
    case dither::Method::atkinson: return "atkinson";
    case dither::Method::sierra_lite: return "sierra-lite";
    case dither::Method::stucki: return "stucki";
    case dither::Method::jarvis: return "jarvis";
    }
    return "unknown";
}

// Determine effective chipset: auto-detect from mode if not specified.
// Modes requiring >6 bitplanes (HAM7, HAM8) force AGA regardless.
// All other modes default to OCS but can be overridden with --chipset aga
// (e.g., HAM6 on AGA gives 24-bit base palette precision).
amiga::Chipset effective_chipset(const Config& cfg) {
    auto params = amiga::get_mode_params(cfg.mode);
    bool requires_aga = params.bitplane_depth > 6;
    if (requires_aga) return amiga::Chipset::aga;
    if (cfg.chipset.has_value()) return *cfg.chipset;
    return amiga::Chipset::ocs;
}

// Quantize palette: OCS uses brute-force, AGA uses median-cut
Result<Palette> auto_quantize(const Image& image, std::size_t max_colors,
                              amiga::Chipset chipset) {
    if (chipset == amiga::Chipset::aga) {
        return quantize::quantize(image, max_colors,
                                  quantize::Algorithm::median_cut);
    }
    return quantize::quantize(image, max_colors,
                              quantize::Algorithm::ocs_bruteforce);
}

// Snap palette to chipset/mode precision
void snap_palette(Palette& pal, amiga::Chipset chipset, amiga::Mode mode) {
    if (amiga::is_stf(mode)) {
        for (auto& c : pal.colors)
            c = palette::quantize_to_stf(c);
    } else if (chipset != amiga::Chipset::aga) {
        for (auto& c : pal.colors)
            c = palette::quantize_to_ocs(c);
    }
}

// Scale preview image for correct pixel aspect ratio.
// hires: double vertically (tall pixels). lores_interlace: double horizontally.
Image scale_preview(const Image& src, std::size_t sx, std::size_t sy) {
    if (sx == 1 && sy == 1) return src;
    auto dw = src.width() * sx;
    auto dh = src.height() * sy;
    Image dst(dw, dh);
    for (std::size_t y = 0; y < dh; ++y) {
        for (std::size_t x = 0; x < dw; ++x) {
            dst[x, y] = src[x / sx, y / sy];
        }
    }
    return dst;
}

// Scale transparency mask for preview
std::vector<bool> scale_mask(const std::vector<bool>& mask,
                             std::size_t w, std::size_t h,
                             std::size_t sx, std::size_t sy) {
    if (sx == 1 && sy == 1) return mask;
    auto dw = w * sx;
    auto dh = h * sy;
    std::vector<bool> dst(dw * dh);
    for (std::size_t y = 0; y < dh; ++y) {
        for (std::size_t x = 0; x < dw; ++x) {
            dst[y * dw + x] = mask[(y / sy) * w + (x / sx)];
        }
    }
    return dst;
}

// Save preview PNG with pixel aspect scaling and optional transparency
// Display preview in terminal and optionally save to file
Result<void> save_preview(std::string_view path, const Image& preview,
                          bool has_trans, const std::vector<bool>& mask,
                          amiga::Mode mode, bool hires = false,
                          bool interlace = false) {
    (void)mode;
    bool is_hires = hires;
    bool is_lace = interlace;
    std::size_t sx = is_hires ? 1 : 2;
    std::size_t sy = is_lace ? 1 : 2;
    if (is_hires && is_lace) { sx = 1; sy = 1; }
    auto scaled = scale_preview(preview, sx, sy);
    if (has_trans) {
        auto scaled_mask = scale_mask(mask, preview.width(), preview.height(),
                                      sx, sy);
        return png_io::save(path, scaled, scaled_mask);
    }
    return png_io::save(path, scaled);
}

// Show preview in terminal (iTerm2 inline image protocol)
void show_terminal_preview(const Image& preview, amiga::Mode /*mode*/,
                           bool hires = false, bool interlace = false) {
    bool is_hires = hires;
    bool is_lace = interlace;
    std::size_t sx = is_hires ? 1 : 2;
    std::size_t sy = is_lace ? 1 : 2;
    if (is_hires && is_lace) { sx = 1; sy = 1; }
    auto scaled = scale_preview(preview, sx, sy);
    iterm2_display(scaled);
}

} // namespace

int main(int argc, char* argv[]) {
    auto config = parse_args(argc, argv);
    if (!config) {
        std::println(stderr, "Error: {}", config.error().message);
        return 1;
    }

    // --- Validate mode combinations ---
    if (config->copper && config->interlace) {
        std::println(stderr,
            "Error: --copper is not compatible with interlace modes "
            "(copper WAITs conflict with field switching)");
        return 1;
    }

    // --- Validate mode + depth against chipset ---
    {
        auto cs = effective_chipset(*config);
        auto cs_name = (cs == amiga::Chipset::aga) ? "AGA" : "OCS";

        if (config->depth < 1 || config->depth > 8) {
            std::println(stderr, "Error: depth must be 1-8, got {}",
                         config->depth);
            return 1;
        }

        // HAM modes with explicit --chipset ocs that need AGA
        if (amiga::is_ham(config->mode) && config->chipset.has_value() &&
            *config->chipset == amiga::Chipset::ocs) {
            auto ham_bp = amiga::get_mode_params(config->mode).bitplane_depth;
            if (ham_bp > amiga::max_depth(amiga::Chipset::ocs)) {
                std::println(stderr,
                    "Error: HAM{} requires AGA (>6 bitplanes), cannot use OCS",
                    ham_bp);
                return 1;
            }
        }

        // Check depth against mode+chipset limits
        // (HAM/EHB have fixed depths so --depth is ignored for them,
        //  but for standard modes the user's depth must be valid)
        if (!amiga::is_ham(config->mode) && config->mode != amiga::Mode::ehb &&
            !amiga::is_atari(config->mode)) {
            auto max_d = amiga::max_user_depth(config->mode, cs);
            if (config->depth > max_d) {
                auto mp = amiga::get_mode_params(config->mode);
                const char* mode_name = mp.is_hires ? "hires" : "lores";
                std::println(stderr,
                    "Error: {} {} supports max {} bitplanes ({} colors), got {}",
                    cs_name, mode_name, max_d, std::size_t{1} << max_d,
                    config->depth);
                return 1;
            }
        }
    }

    // Load input image
    auto image = png_io::load(config->input_path);
    if (!image) {
        std::println(stderr, "Error loading {}: {}", config->input_path,
                     image.error().message);
        return 1;
    }

    // Compute target dimensions from source aspect ratio
    auto params = amiga::get_mode_params(config->mode);
    auto src_w = image->width();
    auto src_h = image->height();
    auto src_aspect = static_cast<double>(src_w) / static_cast<double>(src_h);

    // Round to nearest even number (Amiga prefers even heights)
    auto round_even = [](double v) -> std::size_t {
        auto r = static_cast<std::size_t>(std::lround(v));
        return (r + 1) & ~std::size_t{1};
    };

    // Pixel aspect ratio: PAR = pixel_width / pixel_height
    // hires: 0.5 (tall pixels), lores_interlace: 2.0 (wide pixels)
    auto par = static_cast<double>(params.preview_scale_x)
             / static_cast<double>(params.preview_scale_y);
    // For compound lace modes (e.g. ham6-lace), base mode isn't interlaced
    // but config->interlace is true — adjust PAR for double vertical resolution
    if (config->interlace && !params.is_interlaced) par *= 2.0;

    std::size_t target_w, target_h;
    if (config->width && config->height) {
        // Both specified: use as-is (user explicitly wants this aspect)
        target_w = *config->width;
        target_h = *config->height;
    } else if (config->width) {
        target_w = *config->width;
        // Adjust PAR when width differs from mode default (e.g. ham6 at 640px = hires)
        auto w_par = (target_w != params.screen_width && params.screen_width > 0)
            ? par * static_cast<double>(params.screen_width) / static_cast<double>(target_w)
            : par;
        target_h = round_even(static_cast<double>(target_w) * w_par / src_aspect);
    } else if (config->height) {
        target_h = *config->height;
        target_w = static_cast<std::size_t>(
            std::lround(static_cast<double>(target_h) * src_aspect / par));
    } else {
        // Use mode default width. For lores modes, don't upscale if source
        // is smaller. For hires, always use 640 (that's the point of hires).
        target_w = params.is_hires
            ? params.screen_width
            : std::min(params.screen_width, src_w);
        target_h = round_even(static_cast<double>(target_w) * par / src_aspect);
        // Atari: clamp to fixed height, pad/crop handled after scaling
        if (params.screen_height > 0 && target_h > params.screen_height)
            target_h = params.screen_height;
    }

    std::println("Input:  {}x{}", image->width(), image->height());

    // Compute transparency mask from source alpha BEFORE crop/scale
    // (crop and scale lose the alpha channel).
    // Scale alpha to target resolution first, then dither/threshold at
    // target resolution so the dither pattern matches output pixels.
    std::vector<bool> transparency_mask;
    bool has_transparency = false;

    if (image->has_alpha()) {
        auto alpha_w = image->width();
        auto alpha_h = image->height();

        // Scale alpha to target resolution (bilinear)
        std::vector<float> scaled_alpha(target_w * target_h);
        for (std::size_t y = 0; y < target_h; ++y) {
            auto fy = static_cast<double>(y) * static_cast<double>(alpha_h)
                      / static_cast<double>(target_h);
            auto sy = static_cast<std::size_t>(fy);
            if (sy >= alpha_h - 1) sy = alpha_h - 1;
            auto ty = static_cast<float>(fy - static_cast<double>(sy));
            auto sy1 = std::min(sy + 1, alpha_h - 1);

            for (std::size_t x = 0; x < target_w; ++x) {
                auto fx = static_cast<double>(x) * static_cast<double>(alpha_w)
                          / static_cast<double>(target_w);
                auto sx = static_cast<std::size_t>(fx);
                if (sx >= alpha_w - 1) sx = alpha_w - 1;
                auto tx = static_cast<float>(fx - static_cast<double>(sx));
                auto sx1 = std::min(sx + 1, alpha_w - 1);

                // Bilinear interpolation
                float a00 = image->alpha_at(sx,  sy);
                float a10 = image->alpha_at(sx1, sy);
                float a01 = image->alpha_at(sx,  sy1);
                float a11 = image->alpha_at(sx1, sy1);
                float a = a00 * (1-tx) * (1-ty) + a10 * tx * (1-ty)
                        + a01 * (1-tx) * ty     + a11 * tx * ty;
                scaled_alpha[y * target_w + x] = a;
            }
        }

        // Apply threshold or dither at target resolution
        transparency_mask.resize(target_w * target_h);

        if (config->alpha_dither != dither::Method::none) {
            auto adm = config->alpha_dither;
            for (std::size_t y = 0; y < target_h; ++y) {
                for (std::size_t x = 0; x < target_w; ++x) {
                    float a = scaled_alpha[y * target_w + x];
                    float threshold = dither::ordered_threshold(adm, x, y);
                    transparency_mask[y * target_w + x] =
                        (a + threshold * config->alpha_dither_strength) < (0.5f + config->alpha_threshold);
                }
            }
        } else {
            for (std::size_t y = 0; y < target_h; ++y) {
                for (std::size_t x = 0; x < target_w; ++x) {
                    transparency_mask[y * target_w + x] =
                        scaled_alpha[y * target_w + x] < (0.5f + config->alpha_threshold);
                }
            }
        }

        for (auto v : transparency_mask) {
            if (v) { has_transparency = true; break; }
        }

        if (has_transparency) {
            std::println("Alpha:  {} transparent pixels ({})",
                std::count(transparency_mask.begin(),
                           transparency_mask.end(), true),
                config->alpha_dither != dither::Method::none
                    ? dither_name(config->alpha_dither) : "threshold");
        }
    }

    // Crop (before scaling)
    if (config->crop_w > 0 && config->crop_h > 0) {
        auto cropped = crop_image(*image,
            static_cast<std::size_t>(config->crop_x),
            static_cast<std::size_t>(config->crop_y),
            static_cast<std::size_t>(config->crop_w),
            static_cast<std::size_t>(config->crop_h));
        if (!cropped) {
            std::println(stderr, "Crop error: {}", cropped.error().message);
            return 1;
        }
        image = *std::move(cropped);
        std::println("Crop:   {}x{}+{}+{} -> {}x{}",
                     config->crop_w, config->crop_h,
                     config->crop_x, config->crop_y,
                     image->width(), image->height());
    } else if (config->crop_auto) {
        auto cropped = auto_crop_to_aspect(*image, target_w, target_h);
        if (!cropped) {
            std::println(stderr, "Auto-crop error: {}", cropped.error().message);
            return 1;
        }
        std::println("Crop:   auto {}x{} -> {}x{} (center, {:g}:{:g} aspect)",
                     image->width(), image->height(),
                     cropped->width(), cropped->height(),
                     static_cast<double>(target_w), static_cast<double>(target_h));
        image = *std::move(cropped);
    }

    // Display actual bitplane depth (HAM/EHB override user's --depth)
    auto actual_depth = config->depth;
    if (amiga::is_ham(config->mode))
        actual_depth = amiga::get_mode_params(config->mode).bitplane_depth;
    else if (config->mode == amiga::Mode::ehb)
        actual_depth = 6;
    else if (amiga::is_atari(config->mode)) {
        actual_depth = amiga::get_mode_params(config->mode).bitplane_depth;
        config->depth = actual_depth;
    }
    std::println("Target: {}x{} @ {} bitplanes", target_w, target_h, actual_depth);

    // Scale
    if (image->width() != target_w || image->height() != target_h) {
        auto scaled = scale::bicubic(*image, target_w, target_h);
        if (!scaled) {
            std::println(stderr, "Scale error: {}", scaled.error().message);
            return 1;
        }
        image = *std::move(scaled);
    }

    // Atari: center vertically in fixed-height frame if image is shorter
    if (params.screen_height > 0 && image->height() < params.screen_height) {
        auto w = image->width();
        auto h = image->height();
        auto fh = params.screen_height;
        Image padded(w, fh);
        auto y_off = (fh - h) / 2;
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x)
                padded[x, y + y_off] = (*image)[x, y];
        if (has_transparency) {
            std::vector<bool> new_mask(w * fh, true);
            for (std::size_t y = 0; y < h; ++y)
                for (std::size_t x = 0; x < w; ++x)
                    new_mask[(y + y_off) * w + x] = transparency_mask[y * w + x];
            transparency_mask = std::move(new_mask);
        }
        std::println("Center: {}x{} -> {}x{} (vertical pad)", w, h, w, fh);
        image = std::move(padded);
    }

    // Preprocess
    preprocess::apply(*image, config->preprocess);

    auto chipset = effective_chipset(*config);
    std::println("Chipset: {}", chipset == amiga::Chipset::aga ? "AGA (24-bit)" : "OCS (12-bit)");

    // AGA depth 6 in standard mode: set KILLEHB to prevent hardware
    // from triggering EHB. This allows a true 64-color indexed palette.

    // --- HAM modes ---
    if (amiga::is_ham(config->mode)) {
        auto ham_params = amiga::get_mode_params(config->mode);
        auto quality_str = config->ham_quality == ham::Quality::fast ? "fast" : "optimal";
        auto ham_dither = config->dither_explicit
            ? config->dither_method : dither::Method::none;
        std::println("Mode:   HAM{} (quality: {}, beam: {}, dither: {})",
                     ham_params.bitplane_depth,
                     quality_str, config->ham_beam,
                     dither_name(ham_dither));

        ham::HamOptions ham_opts;
        ham_opts.quality = config->ham_quality;
        ham_opts.beam_width = config->ham_beam;
        // HAM defaults to no dither (error diffusion hurts HAM quality).
        // User can still override with --dither.
        ham_opts.dither_method = config->dither_explicit
            ? config->dither_method : dither::Method::none;
        ham_opts.dither_strength = config->dither_strength;
        ham_opts.error_clamp = config->error_clamp;

        Result<ham::HamResult> ham_result = config->copper
            ? ham::encode_ham_copper(*image, config->mode, chipset, ham_opts,
                                   config->hires, static_cast<std::size_t>(config->copper_changes))
            : ham::encode_ham(*image, config->mode, chipset, ham_opts);

        if (!ham_result) {
            std::println(stderr, "HAM encode error: {}", ham_result.error().message);
            return 1;
        }

        if (config->copper) {
            auto cpl = (chipset == amiga::Chipset::aga)
                ? std::size_t{8} : std::size_t{4};
            std::println("Copper: {} base colors, {} changes/line",
                         ham_result->base_palette.size(), cpl);
        }

        std::println("Encoded: {} bitplanes, {} bytes, {} base palette colors, error: {:.4f}",
                     ham_result->planes.depth, ham_result->planes.total_bytes(),
                     ham_result->base_palette.size(), ham_result->total_error);

        // Terminal preview (HAM decode)
        {
            auto data_bits = ham_result->planes.depth - 2;
            auto ham_preview = !ham_result->scanline_palettes.empty()
                ? ham::render_ham_copper(ham_result->planes,
                                        ham_result->scanline_palettes, data_bits)
                : ham::render_ham(ham_result->planes,
                                 ham_result->base_palette, data_bits);
            if (ham_preview)
                show_terminal_preview(*ham_preview, config->mode, config->hires, config->interlace);
        }

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;
                iff_opts.has_transparency = has_transparency;
                if (!ham_result->scanline_palettes.empty()) {
                    iff_opts.scanline_palettes = &ham_result->scanline_palettes;
                }

                auto result = iff::save_ilbm(
                    config->output_path, ham_result->planes,
                    ham_result->base_palette, config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}", result.error().message);
                    return 1;
                }
                std::println("IFF:    {}", config->output_path);
            } else if (ends_with(config->output_path, ".h")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                if (!ham_result->copper_changes.empty()) {
                    ch_opts.copper_changes = &ham_result->copper_changes;
                    ch_opts.copper_changes_per_line = ham_result->changes_per_line;
                }

                auto result = cheader::save(
                    config->output_path, ham_result->planes,
                    ham_result->base_palette, config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "C header write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("Header: {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                if (!ham_result->copper_changes.empty()) {
                    ch_opts.copper_changes = &ham_result->copper_changes;
                    ch_opts.copper_changes_per_line = ham_result->changes_per_line;
                }

                pad_planes_to_mode(ham_result->planes, config->mode, config->hires);
                auto result = cheader::save_viewer(
                    config->output_path, ham_result->planes,
                    ham_result->base_palette, config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "Viewer write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("Viewer: {}", config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                auto path_str = std::string(config->output_path);
                std::ofstream file(path_str, std::ios::binary);
                if (!file) {
                    std::println(stderr, "Failed to open: {}", path_str);
                    return 1;
                }
                file.write(
                    reinterpret_cast<const char*>(ham_result->planes.data.data()),
                    static_cast<std::streamsize>(ham_result->planes.data.size()));
                std::println("Raw:    {} ({} bytes)",
                             config->output_path,
                             ham_result->planes.total_bytes());
            } else if (ends_with(config->output_path, ".pal")) {
                auto result = palette_io::save_ocs_palette(
                    config->output_path, ham_result->base_palette);
                if (!result) {
                    std::println(stderr, "Palette write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("Pal:    {} ({} colors, {} bytes)",
                             config->output_path,
                             ham_result->base_palette.size(),
                             ham_result->base_palette.size() * 2);
            } else {
                // Render preview using HAM decoder
                auto data_bits = ham_result->planes.depth - 2;
                auto preview = !ham_result->scanline_palettes.empty()
                    ? ham::render_ham_copper(ham_result->planes,
                                            ham_result->scanline_palettes,
                                            data_bits)
                    : ham::render_ham(ham_result->planes,
                                     ham_result->base_palette,
                                     data_bits);
                if (!preview) {
                    std::println(stderr, "Render error: {}", preview.error().message);
                    return 1;
                }
                auto result = save_preview(config->output_path, *preview,
                                           has_transparency, transparency_mask,
                                           config->mode, config->hires, config->interlace);
                if (!result) {
                    std::println(stderr, "PNG write error: {}", result.error().message);
                    return 1;
                }
                std::println("PNG:    {}", config->output_path);
            }
        }

        return 0;
    }

    // --- EHB mode ---
    if (config->mode == amiga::Mode::ehb) {
        // EHB is always 6 bitplanes, 32 base + 32 half-brightness
        auto ehb_depth = std::size_t{6};

        // --- EHB with copper ---
        if (config->copper) {
            auto cpl = copper::max_changes_per_line(5, false, config->hires, chipset);
            std::println("Mode:   EHB + Copper ({} changes/line)", cpl);

            dither::Settings dith;
            dith.method = config->dither_method;
            dith.strength = config->dither_strength;
            dith.error_clamp = config->error_clamp;

            std::println("Dither: {} (strength: {:.2f})",
                         dither_name(dith.method), dith.strength);

            // Copper encoder optimizes 32 base colors per scanline (depth=5)
            auto copper_result = copper::encode_copper(*image, 5, dith, chipset,
                false, config->hires, static_cast<std::size_t>(config->copper_changes));
            if (!copper_result) {
                std::println(stderr, "Copper encode error: {}",
                             copper_result.error().message);
                return 1;
            }

            // Re-dither each scanline against its 64-color EHB palette
            auto w = image->width();
            auto h = image->height();
            std::vector<std::uint8_t> all_indices(w * h);
            float total_error = 0.0f;

            bool use_ordered = dither::is_ordered(dith.method) &&
                               dith.method != dither::Method::none;
            bool use_diffusion = !use_ordered &&
                                 dith.method != dither::Method::none;
            std::vector<color_space::OKLab> err_buf;
            if (use_diffusion) err_buf.resize(w * h);

            for (std::size_t y = 0; y < h; ++y) {
                auto& base32 = copper_result->scanline_palettes[y];
                Palette bp;
                bp.colors.assign(base32.begin(), base32.end());
                auto ehb64 = palette::make_ehb_palette(bp.colors);

                auto row = image->row(y);
                std::vector<color_space::OKLab> pal_lab(ehb64.colors.size());
                for (std::size_t i = 0; i < ehb64.colors.size(); ++i)
                    pal_lab[i] = color_space::linear_to_oklab(ehb64.colors[i]);

                for (std::size_t x = 0; x < w; ++x) {
                    auto pixel_lab = color_space::linear_to_oklab(row[x]);
                    if (!err_buf.empty()) {
                        auto& e = err_buf[y * w + x];
                        auto ec = dith.error_clamp;
                        pixel_lab.L += std::clamp(e.L, -ec, ec);
                        pixel_lab.a += std::clamp(e.a, -ec, ec);
                        pixel_lab.b += std::clamp(e.b, -ec, ec);
                    }
                    if (use_ordered) {
                        float thr = dither::ordered_threshold(dith.method, x, y);
                        pixel_lab.L += thr * dith.strength * 0.15f;
                        pixel_lab.a += thr * dith.strength * 0.03f;
                        pixel_lab.b += thr * dith.strength * 0.03f;
                    }
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
                    if (use_diffusion) {
                        auto& cl = pal_lab[best_k];
                        color_space::OKLab qe = {
                            (pixel_lab.L - cl.L) * dith.strength,
                            (pixel_lab.a - cl.a) * dith.strength,
                            (pixel_lab.b - cl.b) * dith.strength};
                        auto sp = [&](std::size_t nx, std::size_t ny, float wt) {
                            if (nx < w && ny < h) {
                                auto& e = err_buf[ny * w + nx];
                                e.L += qe.L * wt; e.a += qe.a * wt; e.b += qe.b * wt;
                            }
                        };
                        sp(x+1, y, 7.f/16); if (x>0) sp(x-1, y+1, 3.f/16);
                        sp(x, y+1, 5.f/16); sp(x+1, y+1, 1.f/16);
                    }
                }
            }

            if (has_transparency) {
                for (std::size_t i = 0; i < transparency_mask.size() && i < all_indices.size(); ++i)
                    if (transparency_mask[i]) all_indices[i] = 0;
            }

            auto planes = bitplane::encode(all_indices, w, h, ehb_depth);
            if (!planes) {
                std::println(stderr, "Encode error: {}", planes.error().message);
                return 1;
            }

            std::println("Encoded: {} bitplanes, {} bytes, {} colors, {} changes/line, error: {:.4f}",
                         planes->depth, planes->total_bytes(),
                         copper_result->num_colors, copper_result->changes_per_line,
                         total_error);

            // Per-scanline preview
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

            show_terminal_preview(rendered, config->mode, config->hires, config->interlace);

            // Use base palette for CMAP
            std::vector<Color3f> cmap_palette = copper_result->base_palette;

            if (!config->output_path.empty()) {
                if (ends_with(config->output_path, ".png")) {
                    auto result = save_preview(config->output_path, rendered,
                                               has_transparency, transparency_mask,
                                               config->mode, config->hires, config->interlace);
                    if (!result) {
                        std::println(stderr, "PNG write error: {}", result.error().message);
                        return 1;
                    }
                    std::println("PNG:    {}", config->output_path);
                } else if (ends_with(config->output_path, ".cpp") ||
                           ends_with(config->output_path, ".c")) {
                    cheader::CHeaderOptions ch_opts;
                    ch_opts.symbol_name = config->symbol_name.empty()
                        ? derive_symbol_name(config->output_path)
                        : config->symbol_name;
                    ch_opts.hires = config->hires;
                    ch_opts.interlace = config->interlace;
                    ch_opts.copper_changes = &copper_result->scanline_changes;
                    ch_opts.copper_changes_per_line = copper_result->changes_per_line;

                    pad_planes_to_mode(planes.value(), config->mode, config->hires);
                    auto result = cheader::save_viewer(
                        config->output_path, planes.value(),
                        cmap_palette, config->mode, ch_opts);
                    if (!result) {
                        std::println(stderr, "Viewer write error: {}", result.error().message);
                        return 1;
                    }
                    std::println("Viewer: {}", config->output_path);
                } else {
                    std::println(stderr, "EHB copper: only .png and .cpp output supported");
                    return 1;
                }
            }
            return 0;
        }

        // --- EHB without copper ---
        std::println("Mode:   EHB (Extra Half-Brite)");

        Palette base_pal;
        if (!config->palette_file.empty()) {
            auto loaded = palette_io::load_palette(config->palette_file);
            if (!loaded) {
                std::println(stderr, "Palette load error: {}",
                             loaded.error().message);
                return 1;
            }
            base_pal = *std::move(loaded);
            if (base_pal.colors.size() > 32)
                base_pal.colors.resize(32);
            snap_palette(base_pal, chipset, config->mode);
            std::println("Palette: {} colors loaded from {}",
                         base_pal.size(), config->palette_file);
        } else {
            // 31 colors + black at index 0 = 32 base colors
            auto quantized = auto_quantize(*image, 31, chipset);
            if (!quantized) {
                std::println(stderr, "Quantize error: {}",
                             quantized.error().message);
                return 1;
            }
            base_pal = *std::move(quantized);
            base_pal.colors.insert(base_pal.colors.begin(),
                                   Color3f{0.0f, 0.0f, 0.0f});
        }

        // Build full 64-color EHB palette
        auto ehb_pal = palette::make_ehb_palette(base_pal.colors);
        std::println("Palette: {} base + {} half-brite = {} colors",
                     base_pal.size(), base_pal.size(), ehb_pal.size());

        if (config->match_range) {
            preprocess::match_palette_range(*image, ehb_pal);
        }

        // Dither against all 64 colors
        dither::Settings dith;
        dith.method = config->dither_method;
        dith.strength = config->dither_strength;
        dith.error_clamp = config->error_clamp;

        std::println("Dither: {} (strength: {:.2f})",
                     dither_name(dith.method), dith.strength);

        auto dither_result = dither::apply(*image, ehb_pal.colors, dith);

        // Encode to 6 bitplanes
        auto planes = bitplane::encode(dither_result.indices,
                                       image->width(), image->height(),
                                       ehb_depth);
        if (!planes) {
            std::println(stderr, "Encode error: {}", planes.error().message);
            return 1;
        }

        std::vector<Color3f> full_palette(ehb_pal.colors.begin(),
                                          ehb_pal.colors.end());

        std::println("Encoded: {} bitplanes, {} bytes, error: {:.4f}",
                     planes->depth, planes->total_bytes(),
                     dither_result.total_error);

        // Render preview
        auto preview = bitplane::render(*planes, full_palette);
        if (!preview) {
            std::println(stderr, "Render error: {}", preview.error().message);
            return 1;
        }

        show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;

                // IFF writer will trim palette to 32 base colors for EHB
                auto result = iff::save_ilbm(
                    config->output_path, planes.value(), full_palette,
                    config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("IFF:    {} ({} bytes)",
                             config->output_path, planes->total_bytes());
            } else if (ends_with(config->output_path, ".h")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;

                auto result = cheader::save(
                    config->output_path, planes.value(), full_palette,
                    config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "C header write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("Header: {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                cheader::CHeaderOptions ch_opts2;
                ch_opts2.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts2.hires = config->hires;
                ch_opts2.interlace = config->interlace;

                pad_planes_to_mode(planes.value(), config->mode, config->hires);
                auto result2 = cheader::save_viewer(
                    config->output_path, planes.value(), full_palette,
                    config->mode, ch_opts2);
                if (!result2) {
                    std::println(stderr, "Viewer write error: {}",
                                 result2.error().message);
                    return 1;
                }
                std::println("Viewer: {}", config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                auto path_str = std::string(config->output_path);
                std::ofstream file(path_str, std::ios::binary);
                if (!file) {
                    std::println(stderr, "Failed to open: {}", path_str);
                    return 1;
                }
                file.write(reinterpret_cast<const char*>(planes->data.data()),
                           static_cast<std::streamsize>(planes->data.size()));
                std::println("Raw:    {} ({} bytes)",
                             config->output_path, planes->total_bytes());
            } else if (ends_with(config->output_path, ".pal")) {
                auto result = palette_io::save_ocs_palette(
                    config->output_path, full_palette);
                if (!result) {
                    std::println(stderr, "Palette write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("Pal:    {} ({} colors, {} bytes)",
                             config->output_path, full_palette.size(),
                             full_palette.size() * 2);
            } else {
                auto result = save_preview(config->output_path, *preview,
                                           has_transparency, transparency_mask,
                                           config->mode, config->hires, config->interlace);
                if (!result) {
                    std::println(stderr, "PNG write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("PNG:    {}", config->output_path);
            }
        }

        return 0;
    }

    // --- Copper palette mode ---
    if (config->copper) {
        if (!config->output_path.empty() &&
            (ends_with(config->output_path, ".iff") ||
             ends_with(config->output_path, ".ilbm"))) {
            std::println(stderr, "Error: IFF output is not supported with copper mode");
            return 1;
        }

        auto cpl = copper::max_changes_per_line(config->depth, false, config->hires, chipset);
        std::println("Mode:   Copper ({} changes/line)", cpl);

        dither::Settings dith;
        dith.method = config->dither_method;
        dith.strength = config->dither_strength;
        dith.error_clamp = config->error_clamp;

        std::println("Dither: {} (strength: {:.2f})",
                     dither_name(dith.method), dith.strength);

        auto copper_result = copper::encode_copper(*image, config->depth, dith, chipset,
            false, config->hires, static_cast<std::size_t>(config->copper_changes));
        if (!copper_result) {
            std::println(stderr, "Copper encode error: {}",
                         copper_result.error().message);
            return 1;
        }

        // Apply transparency mask: transparent pixels → index 0
        if (has_transparency) {
            auto decoded = bitplane::decode(copper_result->planes);
            if (decoded) {
                for (std::size_t i = 0; i < decoded->size() && i < transparency_mask.size(); ++i) {
                    if (transparency_mask[i]) (*decoded)[i] = 0;
                }
                auto re_encoded = bitplane::encode(*decoded,
                    copper_result->planes.width, copper_result->planes.height,
                    copper_result->planes.depth);
                if (re_encoded) copper_result->planes = *std::move(re_encoded);
            }
        }

        std::println("Encoded: {} bitplanes, {} bytes, {} colors, {} changes/line, error: {:.4f}",
                     copper_result->planes.depth, copper_result->planes.total_bytes(),
                     copper_result->num_colors, copper_result->changes_per_line,
                     copper_result->total_error);

        // Use base palette for IFF CMAP
        std::vector<Color3f> cmap_palette = copper_result->base_palette;

        // Render preview using per-scanline palettes
        auto preview = copper::render_copper(copper_result->planes,
                                             copper_result->scanline_palettes);
        if (!preview) {
            std::println(stderr, "Render error: {}", preview.error().message);
            return 1;
        }

        show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

        // Output
        if (!config->output_path.empty()) {
            if (ends_with(config->output_path, ".iff") ||
                ends_with(config->output_path, ".ilbm")) {
                iff::IffOptions iff_opts;
                iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;
                iff_opts.has_transparency = has_transparency;
                iff_opts.scanline_palettes = &copper_result->scanline_palettes;

                auto result = iff::save_ilbm(
                    config->output_path, copper_result->planes,
                    cmap_palette, config->mode, iff_opts);
                if (!result) {
                    std::println(stderr, "IFF write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("IFF:    {} ({} bytes)",
                             config->output_path,
                             copper_result->planes.total_bytes());
            } else if (ends_with(config->output_path, ".h")) {
                cheader::CHeaderOptions ch_opts;
                ch_opts.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;
                ch_opts.copper_changes = &copper_result->scanline_changes;
                ch_opts.copper_changes_per_line = copper_result->changes_per_line;

                auto result = cheader::save(
                    config->output_path, copper_result->planes,
                    cmap_palette, config->mode, ch_opts);
                if (!result) {
                    std::println(stderr, "C header write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("Header: {}", config->output_path);
            } else if (ends_with(config->output_path, ".cpp") ||
                       ends_with(config->output_path, ".c")) {
                cheader::CHeaderOptions ch_opts2;
                ch_opts2.symbol_name = config->symbol_name.empty()
                    ? derive_symbol_name(config->output_path)
                    : config->symbol_name;
                ch_opts2.hires = config->hires;
                ch_opts2.interlace = config->interlace;
                ch_opts2.copper_changes = &copper_result->scanline_changes;
                ch_opts2.copper_changes_per_line = copper_result->changes_per_line;

                pad_planes_to_mode(copper_result->planes, config->mode, config->hires);
                auto result2 = cheader::save_viewer(
                    config->output_path, copper_result->planes,
                    cmap_palette, config->mode, ch_opts2);
                if (!result2) {
                    std::println(stderr, "Viewer write error: {}",
                                 result2.error().message);
                    return 1;
                }
                std::println("Viewer: {}", config->output_path);
            } else if (ends_with(config->output_path, ".raw")) {
                auto path_str = std::string(config->output_path);
                std::ofstream file(path_str, std::ios::binary);
                if (!file) {
                    std::println(stderr, "Failed to open: {}", path_str);
                    return 1;
                }
                file.write(
                    reinterpret_cast<const char*>(
                        copper_result->planes.data.data()),
                    static_cast<std::streamsize>(
                        copper_result->planes.data.size()));
                std::println("Raw:    {} ({} bytes)",
                             config->output_path,
                             copper_result->planes.total_bytes());
            } else {
                auto result = save_preview(config->output_path, *preview,
                                           has_transparency, transparency_mask,
                                           config->mode, config->hires, config->interlace);
                if (!result) {
                    std::println(stderr, "PNG write error: {}",
                                 result.error().message);
                    return 1;
                }
                std::println("PNG:    {}", config->output_path);
            }
        }

        return 0;
    }

    // --- Standard bitplane modes ---

    // Build palette
    auto max_colors = std::size_t{1} << config->depth;
    Palette pal;
    if (!config->palette_file.empty()) {
        auto loaded = palette_io::load_palette(config->palette_file);
        if (!loaded) {
            std::println(stderr, "Palette load error: {}", loaded.error().message);
            return 1;
        }
        pal = *std::move(loaded);
        if (pal.colors.size() > max_colors)
            pal.colors.resize(max_colors);
        snap_palette(pal, chipset, config->mode);
        std::println("Palette: {} colors (loaded from {})",
                     pal.size(), config->palette_file);
    } else if (amiga::is_atari_hi(config->mode)) {
        // Monochrome: fixed white + black palette
        pal.colors = {Color3f{1.0f, 1.0f, 1.0f}, Color3f{0.0f, 0.0f, 0.0f}};
        std::println("Palette: 2 colors (monochrome)");
    } else {
        // Amiga: always reserve index 0 for black (border/background color).
        // Atari: use full palette (no border register tied to index 0).
        // Transparency: also reserves index 0 for transparent (black).
        auto is_atari = amiga::is_atari(config->mode);
        auto reserve_zero = has_transparency || !is_atari;
        auto quant_n = reserve_zero ? (max_colors > 1 ? max_colors - 1 : 1) : max_colors;
        auto quantized = auto_quantize(*image, quant_n, chipset);
        if (!quantized) {
            std::println(stderr, "Quantize error: {}", quantized.error().message);
            return 1;
        }
        pal = *std::move(quantized);
        if (reserve_zero)
            pal.colors.insert(pal.colors.begin(), Color3f{0.0f, 0.0f, 0.0f});
        // STF: snap OCS brute-force result to 9-bit precision
        if (amiga::is_stf(config->mode)) snap_palette(pal, chipset, config->mode);
        std::println("Palette: {} colors (auto, {})",
                     pal.size(),
                     amiga::is_stf(config->mode) ? "STF 9-bit" :
                     chipset == amiga::Chipset::aga ? "median-cut" : "OCS brute-force");
    }

    if (config->match_range) {
        preprocess::match_palette_range(*image, pal);
    }

    // Apply dithering
    auto pal_size = std::min(pal.size(), max_colors);
    std::span<const Color3f> pal_span{pal.colors.data(), pal_size};

    dither::Settings dith;
    dith.method = config->dither_method;
    dith.strength = config->dither_strength;
    dith.error_clamp = config->error_clamp;

    std::println("Dither: {} (strength: {:.2f})",
                 dither_name(dith.method), dith.strength);

    auto dither_result = dither::apply(*image, pal_span, dith);

    // Apply transparency mask: transparent pixels → index 0
    if (has_transparency) {
        for (std::size_t i = 0; i < dither_result.indices.size(); ++i) {
            if (transparency_mask[i]) {
                dither_result.indices[i] = 0;
            }
        }
    }

    // Encode to bitplanes (Atari uses word-interleaved layout)
    auto bp_layout = amiga::is_atari(config->mode)
        ? bitplane::Layout::word_interleaved
        : bitplane::Layout::interleaved;
    auto planes = bitplane::encode(dither_result.indices,
                                   image->width(), image->height(),
                                   config->depth, bp_layout);
    if (!planes) {
        std::println(stderr, "Encode error: {}", planes.error().message);
        return 1;
    }

    std::vector<Color3f> used_palette(pal_span.begin(), pal_span.end());

    std::println("Encoded: {} bitplanes, {} bytes, error: {:.4f}",
                 planes->depth, planes->total_bytes(), dither_result.total_error);

    // Render preview
    auto preview = bitplane::render(*planes, used_palette);
    if (!preview) {
        std::println(stderr, "Render error: {}", preview.error().message);
        return 1;
    }

    // Terminal preview
    show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

    // Output
    if (!config->output_path.empty()) {
        if (ends_with(config->output_path, ".pi1") ||
            ends_with(config->output_path, ".pi2") ||
            ends_with(config->output_path, ".pi3")) {
            if (!amiga::is_atari(config->mode)) {
                std::println(stderr, "Error: Degas output requires an Atari ST/STE mode");
                return 1;
            }
            auto result = degas::save(config->output_path, planes.value(),
                                      used_palette, config->mode);
            if (!result) {
                std::println(stderr, "Degas write error: {}", result.error().message);
                return 1;
            }
            std::println("Degas:  {} (32034 bytes)", config->output_path);
        } else if (ends_with(config->output_path, ".iff") ||
            ends_with(config->output_path, ".ilbm")) {
            iff::IffOptions iff_opts;
            iff_opts.hires = config->hires;
                iff_opts.interlace = config->interlace;
            iff_opts.has_transparency = has_transparency;

            auto result = iff::save_ilbm(
                config->output_path, planes.value(), used_palette,
                config->mode, iff_opts);
            if (!result) {
                std::println(stderr, "IFF write error: {}", result.error().message);
                return 1;
            }
            std::println("IFF:    {} ({} bytes)",
                         config->output_path, planes->total_bytes());
        } else if (ends_with(config->output_path, ".h")) {
            cheader::CHeaderOptions ch_opts;
            ch_opts.symbol_name = config->symbol_name.empty()
                ? derive_symbol_name(config->output_path)
                : config->symbol_name;
            ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;

            auto result = cheader::save(
                config->output_path, planes.value(), used_palette,
                config->mode, ch_opts);
            if (!result) {
                std::println(stderr, "C header write error: {}",
                             result.error().message);
                return 1;
            }
            std::println("Header: {}", config->output_path);
        } else if (ends_with(config->output_path, ".cpp") ||
                   ends_with(config->output_path, ".c")) {
            cheader::CHeaderOptions ch_opts;
            ch_opts.symbol_name = config->symbol_name.empty()
                ? derive_symbol_name(config->output_path)
                : config->symbol_name;
            ch_opts.hires = config->hires;
                ch_opts.interlace = config->interlace;

            pad_planes_to_mode(planes.value(), config->mode, config->hires);
            auto result = cheader::save_viewer(
                config->output_path, planes.value(), used_palette,
                config->mode, ch_opts);
            if (!result) {
                std::println(stderr, "Viewer write error: {}",
                             result.error().message);
                return 1;
            }
            std::println("Viewer: {}", config->output_path);
        } else if (ends_with(config->output_path, ".raw")) {
            auto path_str = std::string(config->output_path);
            std::ofstream file(path_str, std::ios::binary);
            if (!file) {
                std::println(stderr, "Failed to open: {}", path_str);
                return 1;
            }
            file.write(reinterpret_cast<const char*>(planes->data.data()),
                       static_cast<std::streamsize>(planes->data.size()));
            std::println("Raw:    {} ({} bytes)",
                         config->output_path, planes->total_bytes());
        } else if (ends_with(config->output_path, ".pal")) {
            auto result = palette_io::save_ocs_palette(
                config->output_path, used_palette);
            if (!result) {
                std::println(stderr, "Palette write error: {}",
                             result.error().message);
                return 1;
            }
            std::println("Pal:    {} ({} colors, {} bytes)",
                         config->output_path, used_palette.size(),
                         used_palette.size() * 2);
        } else {
            auto result = save_preview(config->output_path, *preview,
                                       has_transparency, transparency_mask,
                                       config->mode, config->hires, config->interlace);
            if (!result) {
                std::println(stderr, "PNG write error: {}", result.error().message);
                return 1;
            }
            std::println("PNG:    {}", config->output_path);
        }
    }

    return 0;
}
