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
//   <input.png> <output.png>
//
// Anything not in this list is left at api::Options defaults — that's the
// point: this is the bare api::run_pipeline path, no CLI tuning.
#include "api.hpp"
#include "png_io.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    api::Options opts;
    opts.mode = "lores";
    std::string in_path, out_path;
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
        else if (a == "--depth") {
            opts.depth = std::atoi(std::string(next(a)).c_str());
            depth_set = true;
        }
        else if (a == "--sliced" || a == "--copper") opts.copper = true;
        else if (a == "--strips") opts.scap = true;
        else if (a == "--dpf" || a == "--dual-playfield") opts.dual_playfield = true;
        else if (a == "--best") opts.best = true;
        else if (a == "--interlace") opts.interlace = true;
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
        // Mirror main.cpp's make_api_options: when the user didn't pass
        // --dither-strength / --error-clamp explicitly, plug in the
        // dither_tuning::defaults_for value for this (mode, depth, copper,
        // chipset, method) bucket. CLI inline pipelines do this; api::
        // run_pipeline expects pre-tuned options, so any caller bypassing
        // make_api_options has to apply tuning manually.
        //
        // IMPORTANT: tune BEFORE the per-mode dither override below. CLI
        // does the same — `make_api_options` runs before
        // genesis/cga/cga-text auto-fallback flips opts.dither. Tuning
        // with the user-default FS context (since opts.dither is still
        // "floyd-steinberg" here) picks the FS-tuned strength which is
        // what the per-mode override then encodes with.
        auto tune = api::dither_defaults_for(opts);
        if (!dither_strength_set) opts.dither_strength = tune.strength;
        if (!error_clamp_set)     opts.error_clamp     = tune.error_clamp;
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
    return 0;
}
