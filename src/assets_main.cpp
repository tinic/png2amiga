// png2amiga-assets — PNG → Amiga tile/sprite/atlas asset converter.
//
// First-slice scope: `tiles` verb only. Slices a tilesheet PNG into WxH cells,
// exact-dedupes them, bitplane-encodes the unique tile set, and emits a
// C++26 header + source for direct inclusion in a native Amiga engine.

#include "atlas_pack.hpp"
#include "bitplane.hpp"
#include "cpp_writer.hpp"
#include "png_io.hpp"
#include "strip_pack.hpp"
#include "tile_pack.hpp"
#include "tileset_manifest.hpp"
#include "types.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace png2amiga;

namespace {

struct TilesConfig {
    std::string input;
    std::string output_base;
    std::size_t tile_w = 16;
    std::size_t tile_h = 16;
    std::size_t depth = 4;
    std::string symbol;
    std::string namespace_name;
    cpp_writer::Chipset chipset = cpp_writer::Chipset::ocs;
    std::string chip_attr = "PA_ASSET_CHIP";  // default for Amiga chip RAM
    std::string preview;        // if non-empty: write a PNG reconstructed from the packed data
    tile_pack::DedupMode dedup = tile_pack::DedupMode::flip;
};

void print_usage() {
    std::println(
        "png2amiga-assets — Amiga tile/sprite/atlas packer\n"
        "\n"
        "Usage:\n"
        "  png2amiga-assets tiles   [options] INPUT.png OUTPUT_BASE\n"
        "  png2amiga-assets strip   [options] INPUT.png OUTPUT_BASE\n"
        "  png2amiga-assets atlas   [options] INPUT1.png .. OUTPUT_BASE\n"
        "  png2amiga-assets tileset [options] INPUT.png OUTPUT_BASE\n"
        "     (pool-only: writes .hpp/.cpp + companion .pamt manifest)\n"
        "  png2amiga-assets map     [options] --manifest OUT.pamt\n"
        "                           --pool-header HDR.hpp INPUT.png OUTPUT_BASE\n"
        "     (level-map against a pre-built tile pool)\n"
        "\n"
        "  All verbs write OUTPUT_BASE.hpp and OUTPUT_BASE.cpp.\n"
        "\n"
        "Shared options:\n"
        "  --depth N              Bitplane depth (1-8, default: 4)\n"
        "  --chipset ocs|aga      Palette format (default: ocs)\n"
        "  --symbol NAME          Override namespace leaf (default: from filename)\n"
        "  --namespace NS         Full namespace (default: assets::<symbol>)\n"
        "  --chip-attr ATTR       Attribute for chip-RAM placement\n"
        "                         (default: PA_ASSET_CHIP — a macro defined by\n"
        "                         pa/chip_attr.hpp; expands to gnu::section on m68k,\n"
        "                         empty on host)\n"
        "  --no-chip-attr         Disable the chip-RAM attribute\n"
        "  --preview OUT.png      Write a round-trip PNG for validation\n"
        "\n"
        "tiles options:\n"
        "  --tile-size WxH        Tile size in pixels (default: 16x16)\n"
        "  --dedup exact|flip     Dedup mode (default: flip — H/V/180° collapse)\n"
        "\n"
        "strip options:\n"
        "  --frame WxH            Frame size in pixels (REQUIRED)\n"
        "  --mask                 Emit a 1-bit mask plane from alpha\n"
        "  --alpha-threshold F    Alpha ≥ F counts as opaque (default 0.5)\n"
        "  --no-reserve-color0    Don't reserve palette[0] for transparency\n"
        "  --preshift N           Pre-shifted copies per frame in {{1,2,4,8,16}} (default: 1)\n"
        "\n"
        "atlas options:\n"
        "  --page WxH             Chip-RAM-sized bitmap page (default: 320x256)\n"
        "  --mask                 Emit 1-bit mask planes from alpha\n"
        "  --alpha-threshold F    Alpha ≥ F counts as opaque (default 0.5)\n"
        "  --no-reserve-color0    Don't reserve palette[0] for transparency\n"
        "  (positional args = many INPUT.png ..., last arg = OUTPUT_BASE)\n"
        "\n"
        "  -h, --help             Show this message\n");
}

// Sanitize a filename into a valid C++ identifier.
std::string sanitize_symbol(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
        else out.push_back('_');
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), '_');
    }
    if (out.empty()) out = "asset";
    return out;
}

bool parse_size(std::string_view s, std::size_t& w, std::size_t& h) {
    auto x = s.find('x');
    if (x == std::string_view::npos) return false;
    try {
        w = std::stoul(std::string{s.substr(0, x)});
        h = std::stoul(std::string{s.substr(x + 1)});
    } catch (...) { return false; }
    return w > 0 && h > 0;
}

int run_tiles(std::span<const std::string_view> args) {
    TilesConfig cfg;

    std::vector<std::string_view> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto a = args[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                std::println(stderr, "error: {} requires an argument", a);
                std::exit(1);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--tile-size") {
            if (!parse_size(next(), cfg.tile_w, cfg.tile_h)) {
                std::println(stderr, "error: bad --tile-size"); return 1;
            }
        }
        else if (a == "--depth") {
            cfg.depth = std::stoul(std::string{next()});
        }
        else if (a == "--chipset") {
            auto v = next();
            if (v == "ocs") cfg.chipset = cpp_writer::Chipset::ocs;
            else if (v == "aga") cfg.chipset = cpp_writer::Chipset::aga;
            else { std::println(stderr, "error: bad --chipset {}", v); return 1; }
        }
        else if (a == "--symbol")     cfg.symbol = next();
        else if (a == "--namespace")  cfg.namespace_name = next();
        else if (a == "--chip-attr")  cfg.chip_attr = next();
        else if (a == "--preview")    cfg.preview = next();
        else if (a == "--dedup") {
            auto v = next();
            if (v == "exact")      cfg.dedup = tile_pack::DedupMode::exact;
            else if (v == "flip")  cfg.dedup = tile_pack::DedupMode::flip;
            else { std::println(stderr, "error: bad --dedup {}", v); return 1; }
        }
        else if (a == "--no-chip-attr") cfg.chip_attr.clear();
        else if (a.starts_with("--")) {
            std::println(stderr, "error: unknown flag {}", a); return 1;
        }
        else positional.push_back(a);
    }

    if (positional.size() != 2) {
        std::println(stderr, "error: tiles needs INPUT.png OUTPUT_BASE (got {} positional)",
                     positional.size());
        print_usage();
        return 1;
    }
    cfg.input  = positional[0];
    cfg.output_base = positional[1];

    if (cfg.symbol.empty()) {
        cfg.symbol = sanitize_symbol(
            std::filesystem::path{cfg.input}.stem().string());
    }
    if (cfg.namespace_name.empty()) {
        cfg.namespace_name = "assets::" + cfg.symbol;
    }

    auto img = png_io::load(cfg.input);
    if (!img) {
        std::println(stderr, "error: load {}: {}", cfg.input, img.error().message);
        return 1;
    }

    tile_pack::Options pack_opts;
    pack_opts.tile_w = cfg.tile_w;
    pack_opts.tile_h = cfg.tile_h;
    pack_opts.depth = cfg.depth;
    pack_opts.dedup = cfg.dedup;

    auto pack = tile_pack::pack_tiles(*img, pack_opts);
    if (!pack) {
        std::println(stderr, "error: pack_tiles: {}", pack.error().message);
        return 1;
    }

    std::println("{}: {}x{} cells -> {} unique tiles ({}:1 dedup) "
                 "{} colors, {} bytes/tile, {} bytes data",
                 cfg.input, pack->map_w, pack->map_h, pack->num_unique_tiles,
                 pack->num_cells > 0
                    ? static_cast<double>(pack->num_cells) / static_cast<double>(pack->num_unique_tiles)
                    : 0.0,
                 pack->palette.size(),
                 pack->tile_bytes,
                 pack->planes.total_bytes());

    cpp_writer::Options wopts;
    wopts.namespace_name = cfg.namespace_name;
    wopts.chipset = cfg.chipset;
    wopts.chip_attr = cfg.chip_attr;

    auto w = cpp_writer::write_tile_pack(cfg.output_base, *pack, wopts);
    if (!w) {
        std::println(stderr, "error: write {}: {}", cfg.output_base, w.error().message);
        return 1;
    }
    std::println("wrote {}.hpp, {}.cpp", cfg.output_base, cfg.output_base);

    // Round-trip preview: decode bitplanes → indices, map back through the
    // palette, compose the full tilemap from cells, and save a PNG. Compare
    // byte-for-byte (via pixel diff) to the source to validate encoding.
    if (!cfg.preview.empty()) {
        auto decoded = bitplane::decode(pack->planes);
        if (!decoded) {
            std::println(stderr, "error: decode: {}", decoded.error().message);
            return 1;
        }
        // Lay out the full tilesheet (map_w*tile_w) × (map_h*tile_h).
        std::size_t W = pack->map_w * pack->planes.width;
        std::size_t H = pack->map_h * (pack->planes.height / pack->num_unique_tiles);
        std::size_t TW = pack->planes.width;
        std::size_t TH = pack->planes.height / pack->num_unique_tiles;
        std::vector<Color3f> out(W * H);
        for (std::size_t cy = 0; cy < pack->map_h; ++cy) {
            for (std::size_t cx = 0; cx < pack->map_w; ++cx) {
                auto& cell = pack->tilemap[cy * pack->map_w + cx];
                auto tile_idx = cell.index();
                bool hf = cell.h_flip();
                bool vf = cell.v_flip();
                for (std::size_t y = 0; y < TH; ++y) {
                    auto sy = vf ? (TH - 1 - y) : y;
                    for (std::size_t x = 0; x < TW; ++x) {
                        auto sx = hf ? (TW - 1 - x) : x;
                        auto src_idx = (*decoded)[(tile_idx * TH + sy) * TW + sx];
                        out[(cy * TH + y) * W + cx * TW + x] =
                            pack->palette[src_idx];
                    }
                }
            }
        }
        Image preview{W, H, std::move(out)};
        auto sw = png_io::save(cfg.preview, preview);
        if (!sw) {
            std::println(stderr, "error: preview {}: {}", cfg.preview, sw.error().message);
            return 1;
        }
        std::println("wrote preview {}", cfg.preview);
    }
    return 0;
}

struct StripConfig {
    std::string input;
    std::string output_base;
    std::size_t frame_w = 0;
    std::size_t frame_h = 0;
    std::size_t depth = 4;
    std::string symbol;
    std::string namespace_name;
    cpp_writer::Chipset chipset = cpp_writer::Chipset::ocs;
    std::string chip_attr = "PA_ASSET_CHIP";
    std::string preview;
    bool emit_mask = false;
    float alpha_threshold = 0.5f;
    bool reserve_color0 = true;
    std::size_t preshift = 1;
};

int run_strip(std::span<const std::string_view> args) {
    StripConfig cfg;
    std::vector<std::string_view> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto a = args[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                std::println(stderr, "error: {} requires an argument", a);
                std::exit(1);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--frame") {
            if (!parse_size(next(), cfg.frame_w, cfg.frame_h)) {
                std::println(stderr, "error: bad --frame"); return 1;
            }
        }
        else if (a == "--depth")      cfg.depth = std::stoul(std::string{next()});
        else if (a == "--chipset") {
            auto v = next();
            if (v == "ocs") cfg.chipset = cpp_writer::Chipset::ocs;
            else if (v == "aga") cfg.chipset = cpp_writer::Chipset::aga;
            else { std::println(stderr, "error: bad --chipset {}", v); return 1; }
        }
        else if (a == "--symbol")           cfg.symbol = next();
        else if (a == "--namespace")        cfg.namespace_name = next();
        else if (a == "--chip-attr")        cfg.chip_attr = next();
        else if (a == "--no-chip-attr")     cfg.chip_attr.clear();
        else if (a == "--preview")          cfg.preview = next();
        else if (a == "--mask")             cfg.emit_mask = true;
        else if (a == "--alpha-threshold")  cfg.alpha_threshold = std::stof(std::string{next()});
        else if (a == "--no-reserve-color0") cfg.reserve_color0 = false;
        else if (a == "--preshift")         cfg.preshift = std::stoul(std::string{next()});
        else if (a.starts_with("--")) {
            std::println(stderr, "error: unknown flag {}", a); return 1;
        }
        else positional.push_back(a);
    }

    if (positional.size() != 2) {
        std::println(stderr, "error: strip needs INPUT.png OUTPUT_BASE");
        print_usage();
        return 1;
    }
    cfg.input = positional[0];
    cfg.output_base = positional[1];

    if (cfg.frame_w == 0 || cfg.frame_h == 0) {
        std::println(stderr, "error: --frame WxH is required for strip");
        return 1;
    }
    if (cfg.symbol.empty()) {
        cfg.symbol = sanitize_symbol(
            std::filesystem::path{cfg.input}.stem().string());
    }
    if (cfg.namespace_name.empty()) {
        cfg.namespace_name = "assets::" + cfg.symbol;
    }

    auto img = png_io::load(cfg.input);
    if (!img) {
        std::println(stderr, "error: load {}: {}", cfg.input, img.error().message);
        return 1;
    }

    strip_pack::Options opts;
    opts.frame_w = cfg.frame_w;
    opts.frame_h = cfg.frame_h;
    opts.depth = cfg.depth;
    opts.emit_mask = cfg.emit_mask;
    opts.alpha_threshold = cfg.alpha_threshold;
    opts.reserve_color0 = cfg.reserve_color0;
    opts.preshift = cfg.preshift;

    auto pack = strip_pack::pack_strip(*img, opts);
    if (!pack) {
        std::println(stderr, "error: pack_strip: {}", pack.error().message);
        return 1;
    }

    std::println("{}: {} frames of {}x{}, {} colors{}, {} B/frame, {} B total{}",
                 cfg.input, pack->num_frames, pack->frame_w, pack->frame_h,
                 pack->palette.size(),
                 pack->has_mask ? " + mask" : "",
                 pack->frame_bytes, pack->planes.total_bytes(),
                 pack->has_mask
                    ? std::format(" + {} B mask", pack->mask_data.size())
                    : std::string{});

    cpp_writer::Options wopts;
    wopts.namespace_name = cfg.namespace_name;
    wopts.chipset = cfg.chipset;
    wopts.chip_attr = cfg.chip_attr;
    auto w = cpp_writer::write_strip_pack(cfg.output_base, *pack, wopts);
    if (!w) {
        std::println(stderr, "error: write {}: {}", cfg.output_base, w.error().message);
        return 1;
    }
    std::println("wrote {}.hpp, {}.cpp", cfg.output_base, cfg.output_base);

    // Round-trip preview: decode bitplanes for shift=0 copy only, strip the
    // storage padding, lay out frames as a horizontal strip matching the source.
    if (!cfg.preview.empty()) {
        auto decoded = bitplane::decode(pack->planes);
        if (!decoded) {
            std::println(stderr, "error: decode: {}", decoded.error().message);
            return 1;
        }
        std::vector<std::uint8_t> mask_indices;
        if (pack->has_mask) {
            bitplane::BitplaneData mbp = pack->planes;
            mbp.data = pack->mask_data;
            mbp.depth = 1;
            auto dm = bitplane::decode(mbp);
            if (!dm) {
                std::println(stderr, "error: mask decode: {}", dm.error().message);
                return 1;
            }
            mask_indices = std::move(*dm);
        }

        std::size_t W = pack->num_frames * pack->frame_w;
        std::size_t H = pack->frame_h;
        auto SW = pack->storage_w;
        std::vector<Color3f> out_px(W * H);
        std::vector<float> out_alpha;
        if (pack->has_mask) out_alpha.assign(W * H, 1.0f);

        for (std::size_t f = 0; f < pack->num_frames; ++f) {
            // Only the shift=0 copy is visited: same pixels at x_off=0.
            auto copy_base = (f * pack->preshift + 0) * H;
            for (std::size_t y = 0; y < H; ++y) {
                for (std::size_t x = 0; x < pack->frame_w; ++x) {
                    auto src = (copy_base + y) * SW + x;
                    auto src_idx = (*decoded)[src];
                    auto dst = y * W + f * pack->frame_w + x;
                    out_px[dst] = pack->palette[src_idx];
                    if (pack->has_mask) {
                        out_alpha[dst] = mask_indices[src] ? 1.0f : 0.0f;
                    }
                }
            }
        }
        Image preview{W, H, std::move(out_px)};
        if (pack->has_mask) preview.set_alpha(std::move(out_alpha));
        auto sw = png_io::save(cfg.preview, preview);
        if (!sw) {
            std::println(stderr, "error: preview {}: {}", cfg.preview, sw.error().message);
            return 1;
        }
        std::println("wrote preview {}", cfg.preview);
    }
    return 0;
}

struct AtlasConfig {
    std::vector<std::string> inputs;
    std::string output_base;
    std::size_t depth = 5;
    std::size_t page_w = 320;
    std::size_t page_h = 256;
    std::string symbol;
    std::string namespace_name;
    cpp_writer::Chipset chipset = cpp_writer::Chipset::ocs;
    std::string chip_attr = "PA_ASSET_CHIP";
    bool emit_mask = false;
    float alpha_threshold = 0.5f;
    bool reserve_color0 = true;
};

int run_atlas(std::span<const std::string_view> args) {
    AtlasConfig cfg;
    std::vector<std::string_view> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto a = args[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                std::println(stderr, "error: {} requires an argument", a);
                std::exit(1);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--page") {
            if (!parse_size(next(), cfg.page_w, cfg.page_h)) {
                std::println(stderr, "error: bad --page"); return 1;
            }
        }
        else if (a == "--depth") cfg.depth = std::stoul(std::string{next()});
        else if (a == "--chipset") {
            auto v = next();
            if (v == "ocs") cfg.chipset = cpp_writer::Chipset::ocs;
            else if (v == "aga") cfg.chipset = cpp_writer::Chipset::aga;
            else { std::println(stderr, "error: bad --chipset {}", v); return 1; }
        }
        else if (a == "--symbol")           cfg.symbol = next();
        else if (a == "--namespace")        cfg.namespace_name = next();
        else if (a == "--chip-attr")        cfg.chip_attr = next();
        else if (a == "--no-chip-attr")     cfg.chip_attr.clear();
        else if (a == "--mask")             cfg.emit_mask = true;
        else if (a == "--alpha-threshold")  cfg.alpha_threshold = std::stof(std::string{next()});
        else if (a == "--no-reserve-color0") cfg.reserve_color0 = false;
        else if (a.starts_with("--")) {
            std::println(stderr, "error: unknown flag {}", a); return 1;
        }
        else positional.push_back(a);
    }

    if (positional.size() < 2) {
        std::println(stderr, "error: atlas needs INPUT.png [INPUT2.png ...] OUTPUT_BASE");
        print_usage();
        return 1;
    }
    // Last positional is the output base; all others are inputs.
    cfg.output_base = positional.back();
    for (std::size_t i = 0; i + 1 < positional.size(); ++i) {
        cfg.inputs.emplace_back(positional[i]);
    }

    if (cfg.symbol.empty()) {
        cfg.symbol = sanitize_symbol(
            std::filesystem::path{cfg.output_base}.stem().string());
    }
    if (cfg.namespace_name.empty()) {
        cfg.namespace_name = "assets::" + cfg.symbol;
    }

    std::vector<atlas_pack::Input> pack_inputs;
    pack_inputs.reserve(cfg.inputs.size());
    for (auto& p : cfg.inputs) {
        auto stem = std::filesystem::path{p}.stem().string();
        pack_inputs.push_back({stem, p});
    }

    atlas_pack::Options opts;
    opts.depth = cfg.depth;
    opts.page_w = cfg.page_w;
    opts.page_h = cfg.page_h;
    opts.emit_mask = cfg.emit_mask;
    opts.alpha_threshold = cfg.alpha_threshold;
    opts.reserve_color0 = cfg.reserve_color0;

    auto pack = atlas_pack::pack_atlas(pack_inputs, opts);
    if (!pack) {
        std::println(stderr, "error: pack_atlas: {}", pack.error().message);
        return 1;
    }

    std::println("{} inputs → {} page(s) of {}x{}, depth {}, {} colors{}, "
                 "utilization {:.1f}%",
                 pack_inputs.size(), pack->pages.size(),
                 pack->page_w, pack->page_h, pack->depth,
                 pack->palette.size(), pack->has_mask ? " + mask" : "",
                 pack->utilization * 100.0f);

    cpp_writer::Options wopts;
    wopts.namespace_name = cfg.namespace_name;
    wopts.chipset = cfg.chipset;
    wopts.chip_attr = cfg.chip_attr;
    auto w = cpp_writer::write_atlas_pack(cfg.output_base, *pack, wopts);
    if (!w) {
        std::println(stderr, "error: write {}: {}", cfg.output_base, w.error().message);
        return 1;
    }
    std::println("wrote {}.hpp, {}.cpp", cfg.output_base, cfg.output_base);
    return 0;
}

// ---------------------------------------------------------------------------
// `tileset` verb — builds a shared tile pool + writes .hpp/.cpp + manifest.
// ---------------------------------------------------------------------------

struct TilesetConfig {
    std::string input;
    std::string output_base;
    std::size_t tile_w = 16;
    std::size_t tile_h = 16;
    std::size_t depth = 4;
    std::string symbol;
    std::string namespace_name;
    cpp_writer::Chipset chipset = cpp_writer::Chipset::ocs;
    std::string chip_attr = "PA_ASSET_CHIP";
    tile_pack::DedupMode dedup = tile_pack::DedupMode::flip;
    std::string manifest_path;     // if empty: derived from output_base
};

int run_tileset(std::span<const std::string_view> args) {
    TilesetConfig cfg;
    std::vector<std::string_view> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto a = args[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                std::println(stderr, "error: {} requires an argument", a);
                std::exit(1);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--tile-size") {
            if (!parse_size(next(), cfg.tile_w, cfg.tile_h)) {
                std::println(stderr, "error: bad --tile-size"); return 1;
            }
        }
        else if (a == "--depth")     cfg.depth = std::stoul(std::string{next()});
        else if (a == "--chipset") {
            auto v = next();
            if (v == "ocs") cfg.chipset = cpp_writer::Chipset::ocs;
            else if (v == "aga") cfg.chipset = cpp_writer::Chipset::aga;
            else { std::println(stderr, "error: bad --chipset"); return 1; }
        }
        else if (a == "--symbol")        cfg.symbol = next();
        else if (a == "--namespace")     cfg.namespace_name = next();
        else if (a == "--chip-attr")     cfg.chip_attr = next();
        else if (a == "--no-chip-attr")  cfg.chip_attr.clear();
        else if (a == "--manifest")      cfg.manifest_path = next();
        else if (a == "--dedup") {
            auto v = next();
            if (v == "exact")      cfg.dedup = tile_pack::DedupMode::exact;
            else if (v == "flip")  cfg.dedup = tile_pack::DedupMode::flip;
            else { std::println(stderr, "error: bad --dedup"); return 1; }
        }
        else if (a.starts_with("--")) {
            std::println(stderr, "error: unknown flag {}", a); return 1;
        }
        else positional.push_back(a);
    }
    if (positional.size() != 2) {
        std::println(stderr, "error: tileset needs INPUT.png OUTPUT_BASE");
        print_usage();
        return 1;
    }
    cfg.input = positional[0];
    cfg.output_base = positional[1];
    if (cfg.symbol.empty()) {
        cfg.symbol = sanitize_symbol(
            std::filesystem::path{cfg.input}.stem().string());
    }
    if (cfg.namespace_name.empty()) cfg.namespace_name = "assets::" + cfg.symbol;
    if (cfg.manifest_path.empty())  cfg.manifest_path = cfg.output_base + ".pamt";

    auto img = png_io::load(cfg.input);
    if (!img) {
        std::println(stderr, "error: load {}: {}", cfg.input, img.error().message);
        return 1;
    }
    tile_pack::Options po;
    po.tile_w = cfg.tile_w; po.tile_h = cfg.tile_h; po.depth = cfg.depth;
    po.dedup = cfg.dedup;
    auto pool = tile_pack::build_tile_pool(*img, po);
    if (!pool) {
        std::println(stderr, "error: build_tile_pool: {}", pool.error().message);
        return 1;
    }
    std::println("{}: {}x{} sheet → {} unique tiles, {} colors, {} B pool",
                 cfg.input,
                 img->width() / cfg.tile_w, img->height() / cfg.tile_h,
                 pool->canonical_keys.size(),
                 pool->palette.size(),
                 pool->planes.total_bytes());

    cpp_writer::Options wopts;
    wopts.namespace_name = cfg.namespace_name;
    wopts.chipset = cfg.chipset;
    wopts.chip_attr = cfg.chip_attr;
    auto w = cpp_writer::write_tile_pool(cfg.output_base, *pool, wopts);
    if (!w) {
        std::println(stderr, "error: write {}: {}", cfg.output_base, w.error().message);
        return 1;
    }
    auto mw = tileset_manifest::write(cfg.manifest_path, *pool, cfg.namespace_name);
    if (!mw) {
        std::println(stderr, "error: manifest {}: {}", cfg.manifest_path, mw.error().message);
        return 1;
    }
    std::println("wrote {}.hpp, {}.cpp, {}",
                 cfg.output_base, cfg.output_base, cfg.manifest_path);
    return 0;
}

// ---------------------------------------------------------------------------
// `map` verb — slices a level PNG against a prebuilt tile pool (manifest).
// ---------------------------------------------------------------------------

struct MapConfig {
    std::string input;
    std::string output_base;
    std::string manifest;
    std::string pool_header;
    std::string pool_namespace;
    std::string symbol;
    std::string namespace_name;
    cpp_writer::Chipset chipset = cpp_writer::Chipset::ocs;
    std::string chip_attr = "PA_ASSET_CHIP";
};

int run_map(std::span<const std::string_view> args) {
    MapConfig cfg;
    std::vector<std::string_view> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto a = args[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                std::println(stderr, "error: {} requires an argument", a);
                std::exit(1);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--manifest")        cfg.manifest = next();
        else if (a == "--pool-header")     cfg.pool_header = next();
        else if (a == "--pool-namespace")  cfg.pool_namespace = next();
        else if (a == "--symbol")          cfg.symbol = next();
        else if (a == "--namespace")       cfg.namespace_name = next();
        else if (a == "--chipset") {
            auto v = next();
            if (v == "ocs") cfg.chipset = cpp_writer::Chipset::ocs;
            else if (v == "aga") cfg.chipset = cpp_writer::Chipset::aga;
            else { std::println(stderr, "error: bad --chipset"); return 1; }
        }
        else if (a == "--chip-attr")      cfg.chip_attr = next();
        else if (a == "--no-chip-attr")   cfg.chip_attr.clear();
        else if (a.starts_with("--")) {
            std::println(stderr, "error: unknown flag {}", a); return 1;
        }
        else positional.push_back(a);
    }
    if (positional.size() != 2) {
        std::println(stderr, "error: map needs INPUT.png OUTPUT_BASE");
        print_usage();
        return 1;
    }
    cfg.input = positional[0];
    cfg.output_base = positional[1];
    if (cfg.manifest.empty()) {
        std::println(stderr, "error: map requires --manifest <tileset.pamt>");
        return 1;
    }

    auto manifest = tileset_manifest::read(cfg.manifest);
    if (!manifest) {
        std::println(stderr, "error: read manifest {}: {}",
                     cfg.manifest, manifest.error().message);
        return 1;
    }
    if (cfg.pool_namespace.empty()) cfg.pool_namespace = manifest->source_namespace;
    if (cfg.pool_header.empty()) {
        cfg.pool_header = std::filesystem::path{cfg.pool_namespace}.filename().string() + ".hpp";
    }
    if (cfg.symbol.empty()) {
        cfg.symbol = sanitize_symbol(
            std::filesystem::path{cfg.input}.stem().string());
    }
    if (cfg.namespace_name.empty()) cfg.namespace_name = "assets::" + cfg.symbol;

    auto img = png_io::load(cfg.input);
    if (!img) {
        std::println(stderr, "error: load {}: {}", cfg.input, img.error().message);
        return 1;
    }
    auto mr = tile_pack::slice_tilemap(*img, manifest->pool);
    if (!mr) {
        std::println(stderr, "error: slice_tilemap: {}", mr.error().message);
        return 1;
    }
    std::println("{}: {}x{} cells mapped against {} ({} tiles), {} unknown",
                 cfg.input, mr->map_w, mr->map_h,
                 cfg.pool_namespace,
                 manifest->pool.canonical_keys.size(),
                 mr->unknown_cells);
    if (mr->unknown_cells > 0) {
        std::println(stderr,
            "warning: {} cells did not match the pool — level may not share a "
            "tileset with {}", mr->unknown_cells, cfg.manifest);
    }

    cpp_writer::Options wopts;
    wopts.namespace_name = cfg.namespace_name;
    wopts.chipset = cfg.chipset;
    wopts.chip_attr = cfg.chip_attr;
    cpp_writer::MapWriteOptions mopts;
    mopts.pool_namespace = cfg.pool_namespace;
    mopts.pool_header = cfg.pool_header;
    auto w = cpp_writer::write_tilemap(cfg.output_base, *mr, wopts, mopts);
    if (!w) {
        std::println(stderr, "error: write {}: {}", cfg.output_base, w.error().message);
        return 1;
    }
    std::println("wrote {}.hpp, {}.cpp", cfg.output_base, cfg.output_base);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        print_usage();
        return args.empty() ? 1 : 0;
    }

    auto verb = args[0];
    args.erase(args.begin());

    if (verb == "tiles")     return run_tiles(args);
    if (verb == "strip")     return run_strip(args);
    if (verb == "atlas")     return run_atlas(args);
    if (verb == "tileset")   return run_tileset(args);
    if (verb == "map")       return run_map(args);

    std::println(stderr, "error: unknown verb '{}'", verb);
    print_usage();
    return 1;
}
