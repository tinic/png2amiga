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
#include "palette.hpp"
#include "palette_io.hpp"
#include "palette_locks.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"
#include "version.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <unordered_set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace png2amiga;

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
    std::size_t ham_beam = 16;

    // HAM triple-pixel refinement post-pass. 0 = off, 16 default
    // (sweet spot — plateau past that).
    std::size_t ham_triple = 16;

    // HAM greedy encoder (skip DP beam search, ~20× faster, ~1 dB worse).
    bool ham_fast = false;

    // Palette diversity (ham_convert-style). 0 = off, 1-5 = progressively
    // aggressive removal of near-duplicate palette entries, re-seeded from
    // poorly-served image regions.
    int palette_diversity = 0;

    // Quantizer selection (empty = auto: OCS brute-force for OCS, median-cut
    // for AGA). "pnn" uses Pairwise Nearest Neighbor (experimental).
    std::string quantizer;

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

    // Palette refinement
    int refine_iterations = 4;         // dither-aware palette refinement iterations (0=off)

    // Copper palette
    bool copper = false;               // per-scanline palette changes
    int copper_changes = 0;            // 0 = auto (based on chipset/depth)
    bool fade_in = false;              // 16-step fade-in from black
    bool reserve_color0 = true;        // reserve index 0 for black (border)

    // Mask export
    std::string mask_path;             // output path for transparency mask
    bool mask_invert = false;          // invert mask polarity

    // Cropping
    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;                    // 0 = no crop
    int crop_h = 0;
    bool crop_auto = false;            // auto-crop to mode aspect ratio

    // Output
    bool preview = false;              // show terminal image preview (iTerm2)

    // Palette index manipulation
    std::vector<api::LockSpec> locks;  // --lock-index <id> <rgbhex>
    std::vector<api::PinSpec>  pins;   // --pin-index-at <id> <x> <y>
};

void print_version() {
    std::println("png2amiga {}", png2amiga::version);
}

void print_usage() {
    std::println(stderr,
        "png2amiga {}\n"
        "\n"
        "Usage: png2amiga [options] input.[png|jpg|webp] [-o output.png|output.iff|output.h]\n"
        "\n",
        png2amiga::version);
    std::println(stderr,
        "Modes:\n"
        "  --mode <mode>                   Graphics mode (default: lores)\n"
        "         lores, lores-lace, hires, hires-lace,\n"
        "         ham6, ham6-lace, ham6-hires, ham6-hires-lace,\n"
        "         ham8, ham8-lace, ham8-hires, ham8-hires-lace,\n"
        "         ehb, ehb-lace,\n"
        "         stf-low, stf-med, stf-hi, ste-low, ste-med, ste-hi\n"
        "  --depth <1-8>                   Bitplane depth (default: 5)\n"
        "  --chipset ocs|aga               OCS 12-bit / AGA 24-bit (default: auto)\n"
        "  --copper                        Copper-Augmented Palette (CAP):\n"
        "                                  per-scanline palette swaps via the\n"
        "                                  copper, picked greedily by OKLab\n"
        "                                  error reduction\n"
        "  --copper-changes <0-16>         CAP swaps per line (0 = auto, picks the\n"
        "                                  worst-case K that fits the 14-MOVE\n"
        "                                  budget; auto mode also tries K+1..K+3)\n"
        "\n"
        "HAM encoding:\n"
        "  --ham-beam <1-256>              Beam width for DP search (default: 48)\n"
        "  --palette-diversity <0-9>       Remove near-duplicate palette entries (experimental)\n"
        "\n"
        "Dithering:\n"
        "  --dither <method>               none|bayer2x2|bayer4x4|bayer8x8|\n"
        "                                  checker|clustered-dot|\n"
        "                                  h2x4|v4x2|bayer4x2|bayer2x4|\n"
        "                                  line2|line-checker|line4|line8|\n"
        "                                  vline2|vline-checker|vline4|vline8|\n"
        "                                  halftone8x8|diagonal8x8|spiral5x5|\n"
        "                                  hex8x8|hex5x5|blue-noise|\n"
        "                                  ign|white-noise|r2|crosshatch|\n"
        "                                  radial|value-noise|\n"
        "                                  floyd-steinberg|atkinson|sierra-lite|\n"
        "                                  stucki|jarvis|ostromoukhov\n"
        "                                  (default: floyd-steinberg)\n"
        "  --dither-strength <float>       Dither amount 0.0-1.0 (default: 1.0)\n"
        "  --error-clamp <float>           Max error per channel (default: 0.12)\n"
        "  --refine <0-32>                Dither-aware palette refinement iterations\n"
        "                                  (default: 4, 0 = off)\n"
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
        "  --mask <file>                   Export transparency mask (.png/.iff/.raw)\n"
        "  --mask-invert                   Invert mask (1=transparent, 0=opaque)\n"
        "\n"
        "Cropping:\n"
        "  --crop <x,y,w,h>               Manual crop region (pixels)\n"
        "  --crop-auto                     Auto-crop to mode aspect ratio (center)\n"
        "\n"
        "Palette index manipulation (lores/hires/EHB/Atari only):\n"
        "  --lock-index <id> <rgbhex>      Lock palette slot to a specific color\n"
        "                                    e.g. --lock-index 0 000000 (force black at 0)\n"
        "                                    repeatable; locks override implicit color 0 = black\n"
        "  --pin-index-at <id> <x> <y>     After dither, swap whatever index pixel (x,y)\n"
        "                                    landed at with slot <id>. Repeatable.\n"
        "                                    Pin targets must not be locked.\n"
        "\n"
        "C header output:\n"
        "  --symbol <name>                 Base symbol name (default: from filename)\n"
        "\n"
        "Output:\n"
        "  --preview                       Show iTerm2 inline image preview\n"
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
    if (s == "vline2") return dither::Method::vline2;
    if (s == "vline-checker") return dither::Method::vline_checker;
    if (s == "vline4") return dither::Method::vline4;
    if (s == "vline8") return dither::Method::vline8;
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
    if (s == "ostromoukhov") return dither::Method::ostromoukhov;
    if (s == "gilbert") return dither::Method::gilbert;
    if (s == "ign") return dither::Method::ign;
    if (s == "white-noise") return dither::Method::white_noise;
    if (s == "r2") return dither::Method::r2_sequence;
    if (s == "crosshatch") return dither::Method::crosshatch;
    if (s == "radial") return dither::Method::radial;
    if (s == "value-noise") return dither::Method::value_noise;
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

        if (arg == "--version" || arg == "-V") {
            print_version();
            std::exit(0);
        }

        if (arg == "--match-range") {
            config.match_range = true;
            continue;
        }

        if (arg == "--preview") {
            config.preview = true;
            continue;
        }


        if (arg == "--no-reserve-color0") {
            config.reserve_color0 = false;
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

        if (arg == "--ham-fast") {
            config.ham_fast = true;
            continue;
        }

        if (arg == "--fade-in") {
            config.fade_in = true;
            continue;
        }

        if (arg == "--mask-invert") {
            config.mask_invert = true;
            continue;
        }

        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            config.output_path = std::string(argv[++i]);
            continue;
        }

        // --lock-index <id> <rgbhex>: lock palette slot to a specific color.
        if (arg == "--lock-index" && i + 2 < argc) {
            int idx = std::atoi(argv[++i]);
            if (idx < 0 || idx > 255) {
                return std::unexpected{Error{ErrorCode::invalid_depth,
                    "--lock-index id must be 0..255"}};
            }
            auto hex = std::string(argv[++i]);
            // Strip leading '#' or '0x'/'0X'
            if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
            if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
                hex.erase(0, 2);
            // Expand 3-digit hex to 6 (e.g. "f0c" -> "ff00cc")
            if (hex.size() == 3) {
                std::string expanded;
                for (char c : hex) { expanded.push_back(c); expanded.push_back(c); }
                hex = expanded;
            }
            if (hex.size() != 6) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--lock-index color must be a 3- or 6-digit hex value (e.g. f0c or ff00cc)"}};
            }
            auto parse_byte = [](std::string_view s) -> int {
                int v = 0;
                for (char c : s) {
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= (c - '0');
                    else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
                    else return -1;
                }
                return v;
            };
            int r = parse_byte(std::string_view(hex).substr(0, 2));
            int g = parse_byte(std::string_view(hex).substr(2, 2));
            int b = parse_byte(std::string_view(hex).substr(4, 2));
            if (r < 0 || g < 0 || b < 0) {
                return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "--lock-index color must be a hex value (e.g. ff00cc)"}};
            }
            config.locks.push_back({idx, r, g, b});
            continue;
        }

        // --pin-index-at <id> <x> <y>: pin palette slot to source pixel.
        if (arg == "--pin-index-at" && i + 3 < argc) {
            int idx = std::atoi(argv[++i]);
            int x = std::atoi(argv[++i]);
            int y = std::atoi(argv[++i]);
            if (idx < 0 || idx > 255 || x < 0 || y < 0) {
                return std::unexpected{Error{ErrorCode::invalid_depth,
                    "--pin-index-at id must be 0..255 and x/y >= 0"}};
            }
            config.pins.push_back({idx, x, y});
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
                int d = std::atoi(std::string(val).c_str());
                if (d < 1 || d > 8) {
                    return std::unexpected{Error{ErrorCode::invalid_depth,
                        "--depth must be 1..8"}};
                }
                config.depth = static_cast<std::size_t>(d);
            }
            else if (arg == "--chipset") {
                if (val == "ocs") config.chipset = amiga::Chipset::ocs;
                else if (val == "aga") config.chipset = amiga::Chipset::aga;
                else return std::unexpected{Error{ErrorCode::unsupported_mode,
                    "Unknown chipset: " + std::string(val) + " (use ocs or aga)"}};
            }
            else if (arg == "--ham-beam") {
                config.ham_beam = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
                if (config.ham_beam < 1) config.ham_beam = 1;
                if (config.ham_beam > 256) config.ham_beam = 256;
            }
            else if (arg == "--ham-triple") {
                config.ham_triple = static_cast<std::size_t>(std::atoi(std::string(val).c_str()));
                if (config.ham_triple > 256) config.ham_triple = 256;
            }
            else if (arg == "--palette-diversity") {
                config.palette_diversity = std::atoi(std::string(val).c_str());
                if (config.palette_diversity < 0) config.palette_diversity = 0;
                if (config.palette_diversity > 9) config.palette_diversity = 9;
            }
            else if (arg == "--quantizer") {
                config.quantizer = std::string(val);
                if (config.quantizer != "" && config.quantizer != "auto" &&
                    config.quantizer != "median-cut" &&
                    config.quantizer != "ocs-bruteforce" &&
                    config.quantizer != "pnn") {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Unknown quantizer: " + config.quantizer +
                        " (use auto, median-cut, ocs-bruteforce, pnn)"}};
                }
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
            else if (arg == "--refine") {
                config.refine_iterations = std::stoi(std::string(val));
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
                int v = std::atoi(std::string(val).c_str());
                if (v <= 0 || v > 32768) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "--width must be 1..32768"}};
                }
                config.width = static_cast<std::size_t>(v);
            }
            else if (arg == "--height") {
                int v = std::atoi(std::string(val).c_str());
                if (v <= 0 || v > 32768) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "--height must be 1..32768"}};
                }
                config.height = static_cast<std::size_t>(v);
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
            else if (arg == "--mask") {
                config.mask_path = std::string(val);
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
                if (idx != 4) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Crop format must be x,y,w,h (e.g. 10,20,300,200)"}};
                }
                if (parsed[0] < 0 || parsed[1] < 0 ||
                    parsed[2] <= 0 || parsed[3] <= 0) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Crop x/y must be >= 0 and w/h must be > 0"}};
                }
                config.crop_x = parsed[0];
                config.crop_y = parsed[1];
                config.crop_w = parsed[2];
                config.crop_h = parsed[3];
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

// Auto-crop region selection is inlined into the pipeline so the
// transparency mask can be sampled from the same region.

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
    case dither::Method::vline2: return "vline2";
    case dither::Method::vline_checker: return "vline-checker";
    case dither::Method::vline4: return "vline4";
    case dither::Method::vline8: return "vline8";
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
    case dither::Method::ostromoukhov: return "ostromoukhov";
    case dither::Method::gilbert: return "gilbert";
    case dither::Method::ign: return "ign";
    case dither::Method::white_noise: return "white-noise";
    case dither::Method::r2_sequence: return "r2";
    case dither::Method::crosshatch: return "crosshatch";
    case dither::Method::radial: return "radial";
    case dither::Method::value_noise: return "value-noise";
    }
    return "unknown";
}

// Determine effective chipset: auto-detect from mode if not specified.
// Modes requiring >6 bitplanes (HAM8) force AGA regardless.
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
                              amiga::Chipset chipset,
                              int palette_diversity = 0,
                              std::string_view quantizer = {}) {
    // PNN with chipset-aware snap: OCS → OCS-snapped PNN, AGA → continuous.
    if (quantizer == "pnn") {
        auto pal = quantize::pnn_quantize(image.pixels(), max_colors,
                                          palette_diversity,
                                          /*snap_to_ocs=*/chipset != amiga::Chipset::aga);
        return pal;
    }
    quantize::Algorithm algo;
    if (quantizer == "median-cut") {
        algo = quantize::Algorithm::median_cut;
    } else if (quantizer == "ocs-bruteforce") {
        algo = quantize::Algorithm::ocs_bruteforce;
    } else {
        // auto
        algo = (chipset == amiga::Chipset::aga)
            ? quantize::Algorithm::median_cut
            : quantize::Algorithm::ocs_bruteforce;
    }
    return quantize::quantize(image, max_colors, algo, palette_diversity);
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

// ---------------------------------------------------------------------------
// Save transparency mask to file (PNG, IFF, or raw 1-bitplane)
// ---------------------------------------------------------------------------

void save_mask(std::string_view path, const std::vector<bool>& tmask,
               std::size_t w, std::size_t h, bool invert, bool interlace) {
    if (tmask.empty()) {
        std::println(stderr, "Mask: no transparency in source image");
        return;
    }

    // Build B/W image from mask
    // Default: white (1) = opaque, black (0) = transparent
    // Inverted: white (1) = transparent, black (0) = opaque
    auto build_image = [&]() {
        Image img(w, h);
        for (std::size_t i = 0; i < w * h; ++i) {
            bool transparent = (i < tmask.size()) && tmask[i];
            bool white = invert ? transparent : !transparent;
            float v = white ? 1.0f : 0.0f;
            img.pixels()[i] = Color3f{v, v, v};
        }
        return img;
    };

    auto build_indices = [&]() {
        std::vector<std::uint8_t> indices(w * h, 0);
        for (std::size_t i = 0; i < w * h; ++i) {
            bool transparent = (i < tmask.size()) && tmask[i];
            bool set = invert ? transparent : !transparent;
            indices[i] = set ? 1 : 0;
        }
        return indices;
    };

    auto path_str = std::string(path);

    if (ends_with(path, ".png")) {
        auto img = build_image();
        auto result = png_io::save(path, img);
        if (!result) {
            std::println(stderr, "Mask PNG write error: {}", result.error().message);
            return;
        }
        std::println("Mask:   {} ({}x{} PNG)", path, w, h);
    } else if (ends_with(path, ".iff") || ends_with(path, ".ilbm")) {
        auto indices = build_indices();
        auto planes = bitplane::encode(indices, w, h, 1);
        if (!planes) {
            std::println(stderr, "Mask encode error: {}", planes.error().message);
            return;
        }
        std::vector<Color3f> mask_palette = {
            Color3f{0.0f, 0.0f, 0.0f},
            Color3f{1.0f, 1.0f, 1.0f},
        };
        iff::IffOptions iff_opts;
        iff_opts.interlace = interlace;
        auto result = iff::save_ilbm(path, *planes, mask_palette,
                                     amiga::Mode::lores, iff_opts);
        if (!result) {
            std::println(stderr, "Mask IFF write error: {}", result.error().message);
            return;
        }
        std::println("Mask:   {} ({}x{} IFF, 1 bitplane)", path, w, h);
    } else if (ends_with(path, ".raw")) {
        auto indices = build_indices();
        auto planes = bitplane::encode(indices, w, h, 1);
        if (!planes) {
            std::println(stderr, "Mask encode error: {}", planes.error().message);
            return;
        }
        std::ofstream file(path_str, std::ios::binary);
        if (!file) {
            std::println(stderr, "Mask: failed to open {}", path);
            return;
        }
        file.write(reinterpret_cast<const char*>(planes->data.data()),
                   static_cast<std::streamsize>(planes->data.size()));
        std::println("Mask:   {} ({}x{} raw, {} bytes)", path, w, h,
                     planes->data.size());
    } else {
        std::println(stderr, "Mask: unsupported extension for '{}' (use .png, .iff, or .raw)", path);
    }
}

// Save preview PNG with pixel aspect scaling and optional transparency
// Display preview in terminal and optionally save to file
// Save raw bitplane data + palette + copper changes to a binary file.
void save_raw(std::string_view path,
              const bitplane::BitplaneData& planes,
              std::span<const Color3f> palette,
              amiga::Chipset chipset,
              const std::vector<std::vector<copper::CopperChange>>* copper = nullptr,
              std::size_t cpl = 0) {
    bool aga = (chipset == amiga::Chipset::aga);
    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) { std::println(stderr, "Failed to open: {}", path_str); return; }

    // Bitplanes
    file.write(reinterpret_cast<const char*>(planes.data.data()),
               static_cast<std::streamsize>(planes.data.size()));

    // Palette (big-endian 0x0RGB)
    for (auto& c : palette) {
        auto hi = aga ? palette::linear_to_aga_hilo(c).hi : palette::linear_to_ocs(c);
        auto buf = std::array<std::uint8_t, 2>{
            static_cast<std::uint8_t>(hi >> 8), static_cast<std::uint8_t>(hi & 0xFF)};
        file.write(reinterpret_cast<const char*>(buf.data()), 2);
    }
    if (aga) {
        for (auto& c : palette) {
            auto lo = palette::linear_to_aga_hilo(c).lo;
            auto buf = std::array<std::uint8_t, 2>{
                static_cast<std::uint8_t>(lo >> 8), static_cast<std::uint8_t>(lo & 0xFF)};
            file.write(reinterpret_cast<const char*>(buf.data()), 2);
        }
    }

    // Copper changes (UWORD reg + UWORD color per entry, cpl entries per line)
    if (copper && !copper->empty()) {
        for (auto& line : *copper) {
            for (std::size_t s = 0; s < cpl; ++s) {
                std::array<std::uint8_t, 4> buf;
                if (s < line.size()) {
                    auto hi = aga ? palette::linear_to_aga_hilo(line[s].color).hi
                                  : palette::linear_to_ocs(line[s].color);
                    buf = {0, line[s].reg,
                           static_cast<std::uint8_t>(hi >> 8),
                           static_cast<std::uint8_t>(hi & 0xFF)};
                } else {
                    buf = {0xFF, 0xFF, 0x00, 0x00};
                }
                file.write(reinterpret_cast<const char*>(buf.data()), 4);
            }
        }
        if (aga) {
            for (auto& line : *copper) {
                for (std::size_t s = 0; s < cpl; ++s) {
                    std::array<std::uint8_t, 4> buf;
                    if (s < line.size()) {
                        auto lo = palette::linear_to_aga_hilo(line[s].color).lo;
                        buf = {0, line[s].reg,
                               static_cast<std::uint8_t>(lo >> 8),
                               static_cast<std::uint8_t>(lo & 0xFF)};
                    } else {
                        buf = {0xFF, 0xFF, 0x00, 0x00};
                    }
                    file.write(reinterpret_cast<const char*>(buf.data()), 4);
                }
            }
        }
    }

    auto total = static_cast<std::size_t>(file.tellp());
    std::println("Raw:    {} ({} bytes)", path, total);
}

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

    bool interlace = config->interlace || params.is_interlaced;
    // Only force even height for interlace (fields must be equal).
    auto round_h = [interlace](double v) -> std::size_t {
        auto r = static_cast<std::size_t>(std::lround(v));
        if (interlace) return (r + 1) & ~std::size_t{1};
        return r;
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
        target_h = round_h(static_cast<double>(target_w) * w_par / src_aspect);
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
        target_h = round_h(static_cast<double>(target_w) * par / src_aspect);
        // Atari: clamp to fixed height, pad/crop handled after scaling
        if (params.screen_height > 0 && target_h > params.screen_height)
            target_h = params.screen_height;
    }

    std::println("Input:  {}x{}", image->width(), image->height());

    // Determine the effective source crop region BEFORE building the mask,
    // so we can sample alpha from the cropped region rather than the full
    // source. Otherwise the mask references pixels the image will no
    // longer contain after crop.
    std::size_t crop_x = 0, crop_y = 0;
    std::size_t crop_w = image->width(), crop_h = image->height();
    bool will_crop = false;
    if (config->crop_w > 0 && config->crop_h > 0) {
        auto cx = static_cast<std::size_t>(config->crop_x);
        auto cy = static_cast<std::size_t>(config->crop_y);
        auto cw = static_cast<std::size_t>(config->crop_w);
        auto ch = static_cast<std::size_t>(config->crop_h);
        if (cx + cw > image->width() || cy + ch > image->height() || cw == 0 || ch == 0) {
            std::println(stderr, "Crop region {}x{}+{}+{} exceeds image {}x{}",
                         cw, ch, cx, cy, image->width(), image->height());
            return 1;
        }
        crop_x = cx; crop_y = cy; crop_w = cw; crop_h = ch;
        will_crop = true;
    } else if (config->crop_auto) {
        auto target_ratio = static_cast<double>(target_w) / static_cast<double>(target_h);
        auto src_ratio = static_cast<double>(image->width()) /
                         static_cast<double>(image->height());
        if (src_ratio > target_ratio) {
            crop_h = image->height();
            crop_w = static_cast<std::size_t>(
                static_cast<double>(image->height()) * target_ratio + 0.5);
        } else {
            crop_w = image->width();
            crop_h = static_cast<std::size_t>(
                static_cast<double>(image->width()) / target_ratio + 0.5);
        }
        crop_x = (image->width() - crop_w) / 2;
        crop_y = (image->height() - crop_h) / 2;
        will_crop = (crop_w != image->width() || crop_h != image->height());
    }

    // Compute transparency mask from source alpha BEFORE crop/scale
    // (crop and scale lose the alpha channel). Bilinearly sample the
    // (possibly cropped) source region into target resolution, then
    // dither/threshold at target so the dither pattern matches output pixels.
    std::vector<bool> transparency_mask;
    bool has_transparency = false;

    if (image->has_alpha()) {
        // Scale the cropped source region to target resolution (bilinear).
        std::vector<float> scaled_alpha(target_w * target_h);
        auto last_x = crop_x + crop_w - 1;
        auto last_y = crop_y + crop_h - 1;
        for (std::size_t y = 0; y < target_h; ++y) {
            auto fy = static_cast<double>(crop_y) +
                      (static_cast<double>(y) + 0.5) *
                          static_cast<double>(crop_h) /
                          static_cast<double>(target_h) - 0.5;
            if (fy < static_cast<double>(crop_y)) fy = static_cast<double>(crop_y);
            auto sy = std::min(static_cast<std::size_t>(fy), last_y);
            auto sy1 = std::min(sy + 1, last_y);
            auto ty = static_cast<float>(fy - static_cast<double>(sy));

            for (std::size_t x = 0; x < target_w; ++x) {
                auto fx = static_cast<double>(crop_x) +
                          (static_cast<double>(x) + 0.5) *
                              static_cast<double>(crop_w) /
                              static_cast<double>(target_w) - 0.5;
                if (fx < static_cast<double>(crop_x)) fx = static_cast<double>(crop_x);
                auto sx = std::min(static_cast<std::size_t>(fx), last_x);
                auto sx1 = std::min(sx + 1, last_x);
                auto tx = static_cast<float>(fx - static_cast<double>(sx));

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

    // Apply the crop region we already validated above.
    if (will_crop) {
        auto cropped = crop_image(*image, crop_x, crop_y, crop_w, crop_h);
        if (!cropped) {
            std::println(stderr, "Crop error: {}", cropped.error().message);
            return 1;
        }
        if (config->crop_w > 0 && config->crop_h > 0) {
            std::println("Crop:   {}x{}+{}+{} -> {}x{}",
                         crop_w, crop_h, crop_x, crop_y,
                         cropped->width(), cropped->height());
        } else {
            std::println("Crop:   auto {}x{} -> {}x{} (center, {:g}:{:g} aspect)",
                         image->width(), image->height(),
                         cropped->width(), cropped->height(),
                         static_cast<double>(target_w),
                         static_cast<double>(target_h));
        }
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
        // Interlace needs even height. If only 1 row short, pad instead of
        // resampling the entire image (avoids blur from bicubic).
        if (image->width() == target_w && target_h == image->height() + 1) {
            Image padded(target_w, target_h);
            for (std::size_t y = 0; y < image->height(); ++y)
                for (std::size_t x = 0; x < target_w; ++x)
                    padded[x, y] = (*image)[x, y];
            for (std::size_t x = 0; x < target_w; ++x)
                padded[x, target_h - 1] = (*image)[x, image->height() - 1];
            image = std::move(padded);
        } else {
            auto scaled = scale::bicubic(*image, target_w, target_h);
            if (!scaled) {
                std::println(stderr, "Scale error: {}", scaled.error().message);
                return 1;
            }
            image = *std::move(scaled);
        }
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
        if (!config->locks.empty() || !config->pins.empty()) {
            std::println(stderr, "Error: --lock-index / --pin-index-at "
                                 "are not supported in HAM modes "
                                 "(palette is dynamic per pixel)");
            return 1;
        }
        if (config->fade_in) {
            std::println(stderr, "Error: --fade-in is not supported in HAM modes "
                                 "(HAM modify bits carry absolute R/G/B values, "
                                 "so fading the base palette would corrupt the image)");
            return 1;
        }
        auto ham_params = amiga::get_mode_params(config->mode);
        // Both HAM6 and HAM8 default to Floyd-Steinberg. HAM8's 6-bit
        // MODIFY still introduces visible banding on smooth gradients
        // (~4 sRGB-value-per-step), and FS improves blurred PSNR by
        // +0.1 to +0.7 dB on all test images. User can disable with
        // `--dither none`.
        auto ham_default_dither = dither::Method::floyd_steinberg;
        auto ham_dither = config->dither_explicit
            ? config->dither_method : ham_default_dither;
        std::println("Mode:   HAM{} (beam: {}, dither: {})",
                     ham_params.bitplane_depth,
                     config->ham_beam,
                     dither_name(ham_dither));

        ham::HamOptions ham_opts;
        ham_opts.beam_width = config->ham_beam;
        ham_opts.dither_method = ham_dither;
        ham_opts.dither_strength = config->dither_strength;
        ham_opts.error_clamp = config->error_clamp;
        ham_opts.palette_diversity = config->palette_diversity;
        ham_opts.quantizer = config->quantizer;
        ham_opts.triple_beam = config->ham_triple;
        ham_opts.greedy = config->ham_fast;

        // Force transparent pixels to black before HAM encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        Result<ham::HamResult> ham_result = config->copper
            ? ham::encode_ham_copper(*image, config->mode, chipset, ham_opts,
                                   config->hires, static_cast<std::size_t>(config->copper_changes))
            : ham::encode_ham(*image, config->mode, chipset, ham_opts);

        if (!ham_result) {
            std::println(stderr, "HAM encode error: {}", ham_result.error().message);
            return 1;
        }

        // Render preview first so we can count unique colors for the stats line.
        auto data_bits = ham_result->planes.depth - 2;
        auto ham_preview = !ham_result->scanline_palettes.empty()
            ? ham::render_ham_copper(ham_result->planes,
                                    ham_result->scanline_palettes, data_bits)
            : ham::render_ham(ham_result->planes,
                             ham_result->base_palette, data_bits);
        int ham_unique = ham_preview ? count_unique_colors(*ham_preview) : 0;

        float ham_psnr = ham_preview
            ? color_space::compute_psnr_blurred(image->pixels(),
                                                ham_preview->pixels(),
                                                image->width(), image->height())
            : 0.0f;
        if (config->copper && !ham_result->copper_changes.empty()) {
            std::size_t total_ch = 0;
            for (auto& ch : ham_result->copper_changes) total_ch += ch.size();
            float avg_ch = ham_result->copper_changes.size() > 0
                ? static_cast<float>(total_ch) / static_cast<float>(ham_result->copper_changes.size())
                : 0.0f;
            std::println("Encoded: {} bitplanes, {} bytes, {} colors, {:.1f} avg CAP/line, error: {:.4f}, PSNR: {:.2f} dB",
                         ham_result->planes.depth, ham_result->planes.total_bytes(),
                         ham_unique, avg_ch, ham_result->total_error, ham_psnr);
        } else {
            std::println("Encoded: {} bitplanes, {} bytes, {} colors, error: {:.4f}, PSNR: {:.2f} dB",
                         ham_result->planes.depth, ham_result->planes.total_bytes(),
                         ham_unique, ham_result->total_error, ham_psnr);
        }

        if (ham_preview)
            if (config->preview) show_terminal_preview(*ham_preview, config->mode, config->hires, config->interlace);

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
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
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
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
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
                save_raw(config->output_path, ham_result->planes,
                         ham_result->base_palette, chipset,
                         config->copper ? &ham_result->copper_changes : nullptr,
                         ham_result->changes_per_line);
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
                // Use the preview rendered earlier for the stats line.
                if (!ham_preview) {
                    std::println(stderr, "Render error: {}", ham_preview.error().message);
                    return 1;
                }
                auto result = save_preview(config->output_path, *ham_preview,
                                           has_transparency, transparency_mask,
                                           config->mode, config->hires, config->interlace);
                if (!result) {
                    std::println(stderr, "PNG write error: {}", result.error().message);
                    return 1;
                }
                std::println("PNG:    {}", config->output_path);
            }
        }

        // Mask export (HAM mode)
        if (!config->mask_path.empty())
            save_mask(config->mask_path, transparency_mask,
                      target_w, target_h, config->mask_invert, config->interlace);

        return 0;
    }

    // --- EHB mode ---
    if (config->mode == amiga::Mode::ehb) {
        // EHB is always 6 bitplanes, 32 base + 32 half-brightness
        auto ehb_depth = std::size_t{6};

        // Force transparent pixels to black before encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        // --- EHB with copper ---
        if (config->copper) {
            if (!config->locks.empty() || !config->pins.empty()) {
                std::println(stderr, "Error: --lock-index / --pin-index-at "
                                     "are not supported with EHB + --copper");
                return 1;
            }

            dither::Settings dith;
            dith.method = config->dither_method;
            dith.strength = config->dither_strength;
            dith.error_clamp = config->error_clamp;

            std::println("Dither: {} (strength: {:.2f})",
                         dither_name(dith.method), dith.strength);

            // Copper encoder optimizes 32 base colors per scanline (depth=5)
            auto copper_result = copper::encode_copper(*image, 5, dith, chipset,
                static_cast<std::size_t>(config->copper_changes),
                nullptr, true, {}, config->palette_diversity);
            if (!copper_result) {
                std::println(stderr, "Copper encode error: {}",
                             copper_result.error().message);
                return 1;
            }
            std::println("Mode:   EHB + CAP ({} changes/line, max {} MOVEs/line)",
                         copper_result->changes_per_line,
                         copper_result->max_moves_per_line);

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
                        auto kernel = dither::error_diffusion_kernel(dith.method);
                        for (auto& [kdx, kdy, kw] : kernel) {
                            auto nx = static_cast<std::ptrdiff_t>(x) + kdx;
                            auto ny = static_cast<std::ptrdiff_t>(y) + kdy;
                            if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                                ny >= 0 && static_cast<std::size_t>(ny) < h) {
                                auto& e = err_buf[static_cast<std::size_t>(ny) * w +
                                                  static_cast<std::size_t>(nx)];
                                e.L += qe.L * kw;
                                e.a += qe.a * kw;
                                e.b += qe.b * kw;
                            }
                        }
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

            float ehb_psnr = color_space::compute_psnr_blurred(
                image->pixels(), rendered.pixels(), w, h);
            std::println("Encoded: {} bitplanes, {} bytes, {} colors, {:.1f} avg CAP/line, error: {:.4f}, PSNR: {:.2f} dB",
                         planes->depth, planes->total_bytes(),
                         count_unique_colors(rendered),
                         copper_result->avg_changes_per_line,
                         total_error, ehb_psnr);

            if (config->preview) show_terminal_preview(rendered, config->mode, config->hires, config->interlace);

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
                    ch_opts.fade_in = config->fade_in;
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

            // Mask export (EHB copper mode)
            if (!config->mask_path.empty())
                save_mask(config->mask_path, transparency_mask,
                          target_w, target_h, config->mask_invert, config->interlace);

            return 0;
        }

        // --- EHB without copper ---
        std::println("Mode:   EHB (Extra Half-Brite)");

        // Validate locks/pins for EHB (target must be 0-31)
        if (auto v = palette_locks::validate_locks(config->locks, 32); !v) {
            std::println(stderr, "{}", v.error().message);
            return 1;
        }
        bool reserve_zero_ehb = !config->palette_file.empty() ? false : config->reserve_color0;
        if (auto v = palette_locks::validate_pins(config->pins, config->locks, 32,
                                                  image->width(), image->height(),
                                                  reserve_zero_ehb); !v) {
            std::println(stderr, "{}", v.error().message);
            return 1;
        }

        Palette base_pal;
        std::vector<bool> base_locked(32, false);
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
            // Apply locks on top of user palette
            for (auto& lock : config->locks) {
                auto idx = static_cast<std::size_t>(lock.index);
                if (idx < base_pal.colors.size()) {
                    base_pal.colors[idx] = palette_locks::to_color(lock, chipset, config->mode);
                    base_locked[idx] = true;
                }
            }
            std::println("Palette: {} colors loaded from {}",
                         base_pal.size(), config->palette_file);
        } else {
            // 32 base colors total: locks + reserved black at 0 + quantized fill
            auto qcount = palette_locks::quant_count(32, config->locks, true);
            auto quantized = auto_quantize(*image, qcount, chipset, config->palette_diversity, config->quantizer);
            if (!quantized) {
                std::println(stderr, "Quantize error: {}",
                             quantized.error().message);
                return 1;
            }
            auto assembled = palette_locks::assemble_locked_palette(
                *quantized, config->locks, 32, true, chipset, config->mode);
            base_pal = std::move(assembled.palette);
            base_locked = std::move(assembled.locked);
        }

        // Build full 64-color EHB palette
        auto ehb_pal = palette::make_ehb_palette(base_pal.colors);

        if (config->match_range) {
            preprocess::match_palette_range(*image, ehb_pal);
        }

        // Dither against all 64 colors
        dither::Settings dith;
        dith.method = config->dither_method;
        dith.strength = config->dither_strength;
        dith.error_clamp = config->error_clamp;

        // Note: dither-aware refinement is skipped for EHB because the
        // hardware-derived half-brite colors (sRGB DAC halving) create a
        // non-linear constraint that the linear centroid approach can't
        // capture correctly.

        std::println("Palette: {} base + {} half-brite = {} colors",
                     base_pal.size(), base_pal.size(), ehb_pal.size());
        std::println("Dither: {} (strength: {:.2f})",
                     dither_name(dith.method), dith.strength);

        auto dither_result = dither::apply(*image, ehb_pal.colors, dith);

        // Apply EHB pin-index swaps. Pins act on the BASE 32 only;
        // half-brite copies (32-63) auto-track via re-derivation.
        for (auto& pin : config->pins) {
            auto target = static_cast<std::size_t>(pin.index);
            auto pixel_offset = static_cast<std::size_t>(pin.y) * image->width() +
                                static_cast<std::size_t>(pin.x);
            if (pixel_offset >= dither_result.indices.size()) {
                std::println(stderr, "--pin-index-at {}: pixel out of bounds",
                             pin.index);
                return 1;
            }
            auto src = static_cast<std::size_t>(dither_result.indices[pixel_offset]);
            if (src >= 32) {
                std::println(stderr,
                    "--pin-index-at {}: source pixel ({},{}) dithered to "
                    "half-brite slot {} (EHB pins must land on a base color, slots 0-31)",
                    pin.index, pin.x, pin.y, src);
                return 1;
            }
            if (src == target) { base_locked[target] = true; continue; }
            if (base_locked[target]) {
                std::println(stderr, "--pin-index-at {}: target is locked", pin.index);
                return 1;
            }
            std::swap(base_pal.colors[src], base_pal.colors[target]);
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
            ehb_pal = palette::make_ehb_palette(base_pal.colors);
        }

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

        // Render preview
        auto preview = bitplane::render(*planes, full_palette);
        if (!preview) {
            std::println(stderr, "Render error: {}", preview.error().message);
            return 1;
        }

        float ehb_psnr = color_space::compute_psnr_blurred(
            image->pixels(), preview->pixels(),
            image->width(), image->height());
        std::println("Encoded: {} bitplanes, {} bytes, {} colors, error: {:.4f}, PSNR: {:.2f} dB",
                     planes->depth, planes->total_bytes(),
                     count_unique_colors(*preview), dither_result.total_error, ehb_psnr);

        if (config->preview) show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

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
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;

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
                ch_opts2.aga = (chipset == amiga::Chipset::aga);
                ch_opts2.fade_in = config->fade_in;

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
                save_raw(config->output_path, planes.value(),
                         full_palette, chipset);
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

        // Mask export (EHB mode)
        if (!config->mask_path.empty())
            save_mask(config->mask_path, transparency_mask,
                      target_w, target_h, config->mask_invert, config->interlace);

        return 0;
    }

    // --- Copper palette mode ---
    if (config->copper) {
        if (!config->pins.empty()) {
            std::println(stderr, "Error: --pin-index-at "
                                 "is not supported with --copper");
            return 1;
        }
        // IFF output with copper is now supported via the PCHG chunk.

        // Force transparent pixels to black before encoding
        if (has_transparency) {
            for (std::size_t i = 0; i < transparency_mask.size(); ++i)
                if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
        }

        dither::Settings dith;
        dith.method = config->dither_method;
        dith.strength = config->dither_strength;
        dith.error_clamp = config->error_clamp;

        std::println("Dither: {} (strength: {:.2f})",
                     dither_name(dith.method), dith.strength);

        // Build locked slot list from --lock-index specs
        std::vector<std::pair<std::size_t, Color3f>> copper_locks;
        for (auto& lock : config->locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            copper_locks.emplace_back(idx,
                palette_locks::to_color(lock, chipset, config->mode));
        }

        auto copper_result = copper::encode_copper(*image, config->depth, dith, chipset,
            static_cast<std::size_t>(config->copper_changes), nullptr,
            config->reserve_color0, copper_locks, config->palette_diversity);
        if (!copper_result) {
            std::println(stderr, "Copper encode error: {}",
                         copper_result.error().message);
            return 1;
        }
        // Print actual cpl after orchestration (auto mode may have stretched
        // or fallen back).
        std::println("Mode:   CAP ({} changes/line, max {} MOVEs/line)",
                     copper_result->changes_per_line,
                     copper_result->max_moves_per_line);

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

        // Use base palette for IFF CMAP
        std::vector<Color3f> cmap_palette = copper_result->base_palette;

        // Render preview using per-scanline palettes
        auto preview = copper::render_copper(copper_result->planes,
                                             copper_result->scanline_palettes);
        if (!preview) {
            std::println(stderr, "Render error: {}", preview.error().message);
            return 1;
        }

        float cop_psnr = color_space::compute_psnr_blurred(
            image->pixels(), preview->pixels(),
            image->width(), image->height());
        std::println("Encoded: {} bitplanes, {} bytes, {} colors, {:.1f} avg CAP/line, error: {:.4f}, PSNR: {:.2f} dB",
                     copper_result->planes.depth, copper_result->planes.total_bytes(),
                     count_unique_colors(*preview),
                     copper_result->avg_changes_per_line,
                     copper_result->total_error, cop_psnr);

        if (config->preview) show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

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
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;
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
                ch_opts2.aga = (chipset == amiga::Chipset::aga);
                ch_opts2.fade_in = config->fade_in;
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
                save_raw(config->output_path, copper_result->planes,
                         cmap_palette, chipset,
                         &copper_result->scanline_changes,
                         copper_result->changes_per_line);
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

        // Mask export (copper mode)
        if (!config->mask_path.empty())
            save_mask(config->mask_path, transparency_mask,
                      target_w, target_h, config->mask_invert, config->interlace);

        return 0;
    }

    // --- Standard bitplane modes ---

    // Force transparent pixels to black before quantization/encoding
    if (has_transparency) {
        for (std::size_t i = 0; i < transparency_mask.size(); ++i)
            if (transparency_mask[i]) image->pixels()[i] = Color3f{0, 0, 0};
    }

    // Build palette
    auto max_colors = std::size_t{1} << config->depth;
    auto is_atari_std = amiga::is_atari(config->mode);
    bool user_pal_std = !config->palette_file.empty();
    auto reserve_zero_std = !user_pal_std && config->reserve_color0 &&
                             (has_transparency || !is_atari_std);

    // Validate locks/pins
    if (auto v = palette_locks::validate_locks(config->locks, max_colors); !v) {
        std::println(stderr, "{}", v.error().message);
        return 1;
    }
    if (auto v = palette_locks::validate_pins(config->pins, config->locks, max_colors,
                                              image->width(), image->height(),
                                              reserve_zero_std); !v) {
        std::println(stderr, "{}", v.error().message);
        return 1;
    }

    Palette pal;
    std::vector<bool> std_locked(max_colors, false);
    if (user_pal_std) {
        auto loaded = palette_io::load_palette(config->palette_file);
        if (!loaded) {
            std::println(stderr, "Palette load error: {}", loaded.error().message);
            return 1;
        }
        pal = *std::move(loaded);
        if (pal.colors.size() > max_colors)
            pal.colors.resize(max_colors);
        snap_palette(pal, chipset, config->mode);
        // Apply locks on top of user palette
        for (auto& lock : config->locks) {
            auto idx = static_cast<std::size_t>(lock.index);
            if (idx < pal.colors.size()) {
                pal.colors[idx] = palette_locks::to_color(lock, chipset, config->mode);
                std_locked[idx] = true;
            }
        }
        std::println("Palette: {} colors (loaded from {})",
                     pal.size(), config->palette_file);
    } else if (amiga::is_atari_hi(config->mode)) {
        // Monochrome: fixed white + black palette
        pal.colors = {Color3f{1.0f, 1.0f, 1.0f}, Color3f{0.0f, 0.0f, 0.0f}};
        std::println("Palette: 2 colors (monochrome)");
    } else {
        auto qcount = palette_locks::quant_count(max_colors, config->locks, reserve_zero_std);
        auto quantized = auto_quantize(*image, qcount, chipset, config->palette_diversity, config->quantizer);
        if (!quantized) {
            std::println(stderr, "Quantize error: {}", quantized.error().message);
            return 1;
        }
        // STF: snap OCS brute-force result to 9-bit precision
        if (amiga::is_stf(config->mode))
            snap_palette(*quantized, chipset, config->mode);
        auto assembled = palette_locks::assemble_locked_palette(
            *quantized, config->locks, max_colors, reserve_zero_std,
            chipset, config->mode);
        pal = std::move(assembled.palette);
        std_locked = std::move(assembled.locked);
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

    dither::Settings dith;
    dith.method = config->dither_method;
    dith.strength = config->dither_strength;
    dith.error_clamp = config->error_clamp;

    // Dither-aware palette refinement: iteratively run the ditherer,
    // recompute each slot's centroid from the pixels actually assigned
    // to it, and update the palette. Converges to a palette that's
    // optimal for the dithered output, not just nearest-color assignment.
    if (config->refine_iterations > 0 && config->palette_file.empty()) {
        auto refined = quantize::refine_with_dither(
            *image,
            Palette{"refined", {pal.colors.begin(),
                                pal.colors.begin() + static_cast<std::ptrdiff_t>(pal_size)}},
            dith, chipset, config->mode,
            static_cast<std::size_t>(config->refine_iterations),
            std_locked);
        if (refined) {
            pal.colors = std::move(refined->colors);
            pal_size = pal.colors.size();
        }
    }

    std::span<const Color3f> pal_span{pal.colors.data(), pal_size};

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

    // Apply pin-index swaps (post-quantization, post-dither).
    if (!config->pins.empty()) {
        auto pin_result = palette_locks::apply_pins(
            pal, dither_result.indices, std_locked, config->pins,
            image->width(), image->height());
        if (!pin_result) {
            std::println(stderr, "{}", pin_result.error().message);
            return 1;
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

    // Render preview
    auto preview = bitplane::render(*planes, used_palette);
    if (!preview) {
        std::println(stderr, "Render error: {}", preview.error().message);
        return 1;
    }

    float std_psnr = color_space::compute_psnr_blurred(
        image->pixels(), preview->pixels(),
        image->width(), image->height());
    std::println("Encoded: {} bitplanes, {} bytes, {} colors, error: {:.4f}, PSNR: {:.2f} dB",
                 planes->depth, planes->total_bytes(),
                 count_unique_colors(*preview), dither_result.total_error, std_psnr);

    // Terminal preview
    if (config->preview) show_terminal_preview(*preview, config->mode, config->hires, config->interlace);

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
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;

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
                ch_opts.aga = (chipset == amiga::Chipset::aga);
                ch_opts.fade_in = config->fade_in;

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
            save_raw(config->output_path, planes.value(),
                     used_palette, chipset);
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

    // Mask export (standard bitplane mode)
    if (!config->mask_path.empty())
        save_mask(config->mask_path, transparency_mask,
                  target_w, target_h, config->mask_invert, config->interlace);

    return 0;
}
