// Standalone tool: invoke api::run_pipeline directly and write the rendered
// preview as PNG. Used to A/B compare against the main.cpp inline pipelines
// during the main.cpp/api.cpp merge effort — if the two paths produce
// byte-identical PNGs for the same inputs, migrating main.cpp's inline
// encoders to call api::encode_state is provably safe.
//
// Minimal flag set — only what the merge audit needs:
//   --mode <name>          mode string (e.g. "lores", "ehb")
//   --depth <N>            bitplane depth
//   --sliced               enable sliced palette
//   --strips               enable strips palette
//   --best                 multi-restart parallel sweep
//   --reserve-range <r> <c>  e.g. "16-31 ff0000"
//   --dither <name>        e.g. "floyd-steinberg"
//   --dither-strength <f>
//   --error-clamp <f>
//   --lock-color0 / --no-lock-color0
//   --interlace
//   --copper-changes <K>
//   --raw-out <path>       also write EncodeState::raw_frame
//   --pal-out <path>       also write the CPC companion .pal bytes
//   <input.png> <output.png>
//
// Anything not in this list is left at api::Options defaults — that's the
// point: this is the bare api::run_pipeline path, no CLI tuning.
#include "api.hpp"
#include "check_ordered_refine.hpp"
#include "ham.hpp"
#include "png_io.hpp"
#include <nlohmann/json.hpp>
#include <cmath>

#include <array>

#include <cstring>
#include <fstream>
#include <iostream>
#include <print>
#include <span>
#include <string_view>
#include <vector>

using namespace png2amiga;

namespace {

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    return bytes;
}

bool parse_hex(std::string_view s, api::ReserveSpec& out) {
    auto h = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.size() != 6) return false;
    int r = h(s[0]) * 16 + h(s[1]);
    int g = h(s[2]) * 16 + h(s[3]);
    int b = h(s[4]) * 16 + h(s[5]);
    if (r < 0 || g < 0 || b < 0) return false;
    out.r = static_cast<std::uint8_t>(r);
    out.g = static_cast<std::uint8_t>(g);
    out.b = static_cast<std::uint8_t>(b);
    return true;
}

// Two adversarial rows: duplicate SET paths must not crowd out a useful
// held color, and distinct AGA low nibbles must never be merged.
int check_ham_beam() {
    auto linear = [](ham::SRGBColor c) {
        return color_space::srgb_u8_to_linear(c.r, c.g, c.b);
    };
    for (bool low_nibbles : {false, true}) {
        std::vector<ham::SRGBColor> colors(16, {0, 0, 0});
        if (low_nibbles) {
            for (auto& c : colors) c.g = 1;
            colors[1].g = 2;
        }
        std::vector<Color3f> palette;
        for (auto c : colors) palette.push_back(linear(c));
        std::vector<Color3f> row;
        if (low_nibbles) {
            row = {linear({0, 1, 0}),
                   linear({0, 2, 255}),
                   linear({0, 2, 255})};
        } else {
            row = {linear({0, 8, 0}),
                   linear({0, 17, 255})};
        }
        std::array<ham::HamPrecomp, 1> pre{ham::HamPrecomp(palette, 4)};
        std::array<std::span<const ham::SRGBColor>, 1> palettes{colors};
        std::vector<std::uint16_t> strips(row.size(), 0);
        auto result = ham::encode_scanline_dp_per_strip(
            row, colors[0], pre, palettes, strips, 16, ham::HamMetric::srgb_mse);
        // Preparing G=17 at pixel zero costs 9^2, then B=255 is exact.
        // With 24-bit anchors, preparing G=2 costs 1^2, then stays exact.
        const float upper_bound = (low_nibbles ? 1.0f : 81.0f) / 65536.0f;
        if (result.values.size() != row.size() || result.error > upper_bound + 1e-7f) {
            std::println(stderr, "HAM beam lost a useful held color (low_nibbles={}, error={})",
                         low_nibbles, result.error);
            return 1;
        }
    }
    std::println("PASS: HAM6 beam preserves distinct held colors, including AGA low nibbles");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--check-ham-beam") == 0)
        return check_ham_beam();

    if (argc == 2 && std::strcmp(argv[1], "--check-ordered-refine") == 0)
        return check_ordered_refine();

    api::Options opts;
    opts.mode = "lores";
    std::string in_path, out_path, raw_out, pal_out, meta_out;
    bool apply_tuning = false;
    bool dither_strength_set = false;
    bool error_clamp_set = false;
    bool refine_set = false;
    bool depth_set = false;
    bool diversity_set = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                std::println(stderr, "{}: missing argument", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--mode") opts.mode = std::string(next(a));
        else if (a == "--raw-out") raw_out = std::string(next(a));
        else if (a == "--meta-out")
            meta_out = std::string(next(a));
        else if (a == "--pal-out") pal_out = std::string(next(a));
        else if (a == "--depth") {
            opts.depth = std::atoi(std::string(next(a)).c_str());
            depth_set = true;
        }
        else if (a == "--sliced" || a == "--copper") opts.copper = true;
        else if (a == "--strips") opts.scap = true;
        else if (a == "--dpf" || a == "--dual-playfield") opts.dual_playfield = true;
        else if (a == "--best") opts.best = true;
        else if (a == "--cell-refine")
            opts.cell_refine = true;
        else if (a == "--tile-budget")
            opts.tile_budget = std::stoul(std::string(next(a)));
        else if (a == "--tile-reserve")
            opts.tile_reserve = std::stoul(std::string(next(a)));
        else if (a == "--c64-petscii-graphics")
            opts.c64_petscii_graphics_only = true;
        else if (a == "--cga-text-metric")
            opts.cga_text_metric = std::string(next(a));
        else if (a == "--interlace") opts.interlace = true;
        else if (a == "--chipset") opts.chipset = std::string(next(a));
        else if (a == "--lock-color0") opts.lock_color0 = true;
        else if (a == "--no-lock-color0") opts.lock_color0 = false;
        else if (a == "--copper-changes") opts.copper_changes = std::atoi(std::string(next(a)).c_str());
        else if (a == "--dither") opts.dither = std::string(next(a));
        else if (a == "--dither-strength") {
            opts.dither_strength = std::stof(std::string(next(a)));
            dither_strength_set = true;
        }
        else if (a == "--error-clamp") {
            opts.error_clamp = std::stof(std::string(next(a)));
            error_clamp_set = true;
        }
        else if (a == "--apply-tuning") apply_tuning = true;
        else if (a == "--gamma") opts.gamma = std::stof(std::string(next(a)));
        else if (a == "--brightness") opts.brightness = std::stof(std::string(next(a)));
        else if (a == "--contrast") opts.contrast = std::stof(std::string(next(a)));
        else if (a == "--saturation") opts.saturation = std::stof(std::string(next(a)));
        else if (a == "--hue-shift") opts.hue_shift = std::stof(std::string(next(a)));
        else if (a == "--sharpen") opts.sharpen = std::stof(std::string(next(a)));
        else if (a == "--black-point") opts.black_point = std::stof(std::string(next(a)));
        else if (a == "--white-point") opts.white_point = std::stof(std::string(next(a)));
        else if (a == "--match-range") opts.match_range = true;
        else if (a == "--ham-metric") opts.ham_metric = std::string(next(a));
        else if (a == "--ham-beam") opts.ham_beam = std::atoi(std::string(next(a)).c_str());
        else if (a == "--ham-triple") opts.ham_triple = std::atoi(std::string(next(a)).c_str());
        else if (a == "--ham-fast") opts.ham_fast = true;
        else if (a == "--refine-iterations") {
            opts.refine_iterations = std::atoi(std::string(next(a)).c_str());
            refine_set = true;
        }
        else if (a == "--palette-diversity") {
            opts.palette_diversity = std::atoi(std::string(next(a)).c_str());
            diversity_set = true;
        }
        else if (a == "--quantizer") opts.quantizer = std::string(next(a));
        else if (a == "--reserve-range") {
            auto range = next(a);
            auto color = next(a);
            api::ReserveSpec base;
            if (!parse_hex(color, base)) {
                std::println(stderr, "bad hex: {}", color);
                return 2;
            }
            // Parse "N" or "N-M" range.
            auto dash = range.find('-');
            int lo = std::atoi(std::string(range.substr(0, dash)).c_str());
            int hi = (dash == std::string_view::npos)
                ? lo
                : std::atoi(std::string(range.substr(dash + 1)).c_str());
            for (int j = lo; j <= hi; ++j) {
                api::ReserveSpec r = base;
                r.index = j;
                opts.reserves.push_back(r);
            }
        }
        else if (a == "--lock-index") {
            auto idx_s = next(a);
            auto color = next(a);
            int idx = std::atoi(std::string(idx_s).c_str());
            api::ReserveSpec tmp;
            if (!parse_hex(color, tmp)) {
                std::println(stderr, "bad hex: {}", color);
                return 2;
            }
            opts.locks.push_back({idx, tmp.r, tmp.g, tmp.b});
        }
        else if (a == "--pin-index-at") {
            auto idx_s = next(a);
            auto x_s   = next(a);
            auto y_s   = next(a);
            opts.pins.push_back({
                std::atoi(std::string(idx_s).c_str()),
                std::atoi(std::string(x_s).c_str()),
                std::atoi(std::string(y_s).c_str())});
        } else if (in_path.empty()) {
            in_path = std::string(a);
        } else if (out_path.empty()) {
            out_path = std::string(a);
        } else {
            std::println(stderr, "unexpected arg: {}", a);
            return 2;
        }
    }
    if (in_path.empty() || out_path.empty()) {
        std::println(stderr,
            "usage: api_pipeline_smoke [opts...] <in.png> <out.png>");
        return 2;
    }

    if (apply_tuning) {
        // Mode-aware depth default first — dither_tuning's lookup is keyed
        // on depth, and CLI's main.cpp resolves the per-mode default
        // (hires non-AGA → 4, lores → 5) BEFORE calling
        // dither_tuning::defaults_for. Doing the same here so the tuning
        // bucket matches what CLI sees.
        if (!depth_set) {
            bool aga = (opts.chipset == "aga");
            if (opts.mode == "hires" || opts.mode == "hires-lace") {
                opts.depth = aga ? 8 : 4;
            } else if (opts.mode == "lores" || opts.mode == "lores-lace") {
                opts.depth = aga ? 8 : 5;
            }
            // DPF override: --dpf forces depth=3 (OCS) / 4 (AGA) — main.cpp
            // L5655 does this BEFORE its dither_tuning lookup, so the tune
            // bucket sees the post-DPF depth. Mirror it here to keep the
            // dither_strength lookup matched.
            if (opts.dual_playfield &&
                (opts.mode == "lores" || opts.mode == "lores-lace" ||
                 opts.mode == "hires" || opts.mode == "hires-lace")) {
                opts.depth = aga ? 4 : 3;
            }
            // Fixed-buffer modes have hardware-defined depths. main.cpp
            // overrides Config::depth for these inside its dispatch
            // (api::run_pipeline does the same internally), but the
            // dither_tuning lookup needs to see the right depth too.
            else if (opts.mode == "vga-13h" || opts.mode == "snes-mode7-direct"
                  || opts.mode == "snes-mode7-256")
                opts.depth = 8;
            else if (opts.mode == "vga-10h" || opts.mode == "vga-12h"
                  || opts.mode == "ega-320" || opts.mode == "ega-640"
                  || opts.mode == "ega-hi"
                  || opts.mode == "stf-low" || opts.mode == "ste-low")
                opts.depth = 4;
            else if (opts.mode == "stf-med" || opts.mode == "ste-med"
                  || opts.mode == "cga-320")
                opts.depth = 2;
            else if (opts.mode == "stf-hi" || opts.mode == "ste-hi"
                  || opts.mode == "cga-640")
                opts.depth = 1;
            else if (opts.mode.starts_with("genesis"))
                opts.depth = 4;
        }
        // dither_strength / error_clamp auto-tuning lives in the
        // encoder (api::run_pipeline resolves the -1.0f sentinel). Smoke
        // leaves the fields at sentinel here when the user didn't pass
        // them explicitly, mirroring make_api_options' new behavior.
        if (!dither_strength_set) opts.dither_strength = -1.0f;
        if (!error_clamp_set)     opts.error_clamp     = -1.0f;
        // Per-mode dither auto-override: main.cpp picks opt-checker for
        // Genesis (tile-aligned 4×4 ordered dither — preserves tile
        // dedup across cells, ~40 % VRAM savings on photos vs FS which
        // serpentines across tile boundaries). Web frontend has the
        // same override. Smoke must mirror it for byte-equality.
        bool dither_explicit = (opts.dither != "" && opts.dither != "floyd-steinberg");
        if (!dither_explicit && opts.mode.starts_with("genesis")) {
            opts.dither = "opt-checker";
        }
        // CLI's Config::refine_iterations default is 8 (set in main.cpp).
        // api::Options::refine_iterations default is 4.
        if (!refine_set) opts.refine_iterations = 8;
        // Hires plain modes: CLI bumps palette_diversity 4 → 5 when the
        // user didn't pass --palette-diversity (main.cpp L5166-5170).
        // Mirror it here so the api-equiv tests stay byte-identical.
        if (!diversity_set &&
            (opts.mode == "hires" || opts.mode == "hires-lace")) {
            opts.palette_diversity = 5;
        }
    }

    auto bytes = read_file(in_path);
    auto r = api::encode_state(bytes.data(), bytes.size(), opts);
    if (!r.ok()) {
        std::println(stderr, "api::encode_state error: {}", r.error_msg);
        return 1;
    }
    auto& state = r.state;
    auto png = png_io::encode(state.rendered);
    if (!png) {
        std::println(stderr, "png encode error: {}", png.error().message);
        return 1;
    }
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png->data()),
              static_cast<std::streamsize>(png->size()));
    if (!raw_out.empty()) {
        std::ofstream rf(raw_out, std::ios::binary);
        rf.write(reinterpret_cast<const char*>(state.raw_frame.data()),
                 static_cast<std::streamsize>(state.raw_frame.size()));
    }
    if (!pal_out.empty() && amiga::is_cpc(state.mode)) {
        auto pal = api::cpc_pal_bytes(state.mode, state.palette);
        std::ofstream pf(pal_out, std::ios::binary);
        pf.write(reinterpret_cast<const char*>(pal.data()),
                 static_cast<std::streamsize>(pal.size()));
    }
    if (!meta_out.empty()) {
        std::vector<std::array<int, 3>> colors;
        for (auto c : state.palette) {
            auto rgb = color_space::linear_to_srgb(c).clamped();
            colors.push_back({static_cast<int>(std::lround(rgb.r * 255)),
                              static_cast<int>(std::lround(rgb.g * 255)),
                              static_cast<int>(std::lround(rgb.b * 255))});
        }
        nlohmann::json meta = {{"palette", colors},
                               {"bg", state.c64_bg_color},
                               {"mc1", state.c64_mc1},
                               {"mc2", state.c64_mc2},
                               {"cols", state.c64_cols},
                               {"rows", state.c64_rows},
                               {"glyphs", state.c64_unique_glyphs}};
        std::ofstream mf(meta_out);
        mf << meta.dump();
    }
    // Debug aid: dump raw indices alongside the PNG so callers can A/B
    // against the CLI's --output-indexed output without re-running.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // MSVC deprecation warning for getenv
#endif
    if (const char* env = std::getenv("API_DUMP_INDICES")) {
        std::ofstream f(env, std::ios::binary);
        f.write(reinterpret_cast<const char*>(state.indices.data()),
                static_cast<std::streamsize>(state.indices.size()));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return 0;
}
