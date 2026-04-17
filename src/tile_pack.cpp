#include "tile_pack.hpp"

#include "bitplane.hpp"

#include "color_space.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <vector>

namespace png2amiga::tile_pack {

namespace {

// Flatten a cell's sRGB-equivalent per-pixel bytes into a lookup key.
// Using packed sRGB bytes (not raw Color3f floats) avoids floating-point
// equality fragility — two pixels that round to the same sRGB8 triple are
// treated as equal regardless of tiny linear-RGB rounding differences.
struct CellKey {
    std::vector<std::uint8_t> bytes;

    bool operator==(const CellKey& o) const noexcept {
        return bytes == o.bytes;
    }
};

struct CellKeyHash {
    std::size_t operator()(const CellKey& k) const noexcept {
        // FNV-1a 64-bit
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (auto b : k.bytes) {
            h ^= b;
            h *= 0x100000001b3ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

std::uint8_t linear_to_srgb_byte(float v) noexcept {
    float c = color_space::linear_to_srgb(v);
    int i = static_cast<int>(c * 255.0f + 0.5f);
    return static_cast<std::uint8_t>(std::clamp(i, 0, 255));
}

CellKey extract_cell_key(const Image& image, std::size_t x0, std::size_t y0,
                         std::size_t w, std::size_t h) {
    CellKey k;
    k.bytes.reserve(w * h * 4);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto& p = image[x0 + x, y0 + y];
            k.bytes.push_back(linear_to_srgb_byte(p.r));
            k.bytes.push_back(linear_to_srgb_byte(p.g));
            k.bytes.push_back(linear_to_srgb_byte(p.b));
            k.bytes.push_back(static_cast<std::uint8_t>(
                std::clamp(image.alpha_at(x0 + x, y0 + y) * 255.0f, 0.0f, 255.0f)));
        }
    }
    return k;
}

// H-flip a 4-byte-per-pixel CellKey in place (bytes laid out row-major RGBA).
CellKey hflip_cell(const CellKey& src, std::size_t w, std::size_t h) {
    CellKey out;
    out.bytes.resize(src.bytes.size());
    for (std::size_t y = 0; y < h; ++y) {
        auto* sr = src.bytes.data() + y * w * 4;
        auto* dr = out.bytes.data() + y * w * 4;
        for (std::size_t x = 0; x < w; ++x) {
            auto dx = (w - 1 - x) * 4;
            auto sx = x * 4;
            dr[dx + 0] = sr[sx + 0];
            dr[dx + 1] = sr[sx + 1];
            dr[dx + 2] = sr[sx + 2];
            dr[dx + 3] = sr[sx + 3];
        }
    }
    return out;
}

CellKey vflip_cell(const CellKey& src, std::size_t w, std::size_t h) {
    CellKey out;
    out.bytes.resize(src.bytes.size());
    for (std::size_t y = 0; y < h; ++y) {
        auto* sr = src.bytes.data() + y * w * 4;
        auto* dr = out.bytes.data() + (h - 1 - y) * w * 4;
        std::copy_n(sr, w * 4, dr);
    }
    return out;
}

// Collect unique sRGB-byte colors present in `image` (alpha ignored for now).
// Returns palette in first-seen order.
std::vector<Color3f> extract_palette(const Image& image) {
    std::unordered_map<std::uint32_t, std::size_t> seen;
    std::vector<Color3f> pal;
    auto w = image.width();
    auto h = image.height();
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto& p = image[x, y];
            auto r = linear_to_srgb_byte(p.r);
            auto g = linear_to_srgb_byte(p.g);
            auto b = linear_to_srgb_byte(p.b);
            std::uint32_t key = (static_cast<std::uint32_t>(r) << 16) |
                                (static_cast<std::uint32_t>(g) << 8) |
                                static_cast<std::uint32_t>(b);
            if (seen.insert({key, pal.size()}).second) {
                pal.push_back(p);
            }
        }
    }
    return pal;
}

// Given a palette, return a lookup function from sRGB8 packed key → index.
std::unordered_map<std::uint32_t, std::uint8_t>
build_palette_lookup(std::span<const Color3f> pal) {
    std::unordered_map<std::uint32_t, std::uint8_t> lut;
    for (std::size_t i = 0; i < pal.size(); ++i) {
        auto& p = pal[i];
        std::uint32_t key = (static_cast<std::uint32_t>(linear_to_srgb_byte(p.r)) << 16) |
                            (static_cast<std::uint32_t>(linear_to_srgb_byte(p.g)) << 8) |
                            static_cast<std::uint32_t>(linear_to_srgb_byte(p.b));
        lut[key] = static_cast<std::uint8_t>(i);
    }
    return lut;
}

// Canonicalize a cell key: return the lex-min of its four flip orientations
// plus the flip flags required to map canonical→original. Single source of
// truth shared by build_tile_pool, slice_tilemap, and pack_tiles.
struct CanonResult {
    CellKey key;
    bool h_flip;
    bool v_flip;
};

CanonResult canonicalize(CellKey raw, std::size_t tw, std::size_t th,
                         DedupMode dedup) {
    if (dedup == DedupMode::exact) {
        return {std::move(raw), false, false};
    }
    auto kh  = hflip_cell(raw, tw, th);
    auto kv  = vflip_cell(raw, tw, th);
    auto khv = hflip_cell(kv, tw, th);
    struct Cand { CellKey k; bool hf; bool vf; };
    Cand cands[4] = {
        {std::move(raw), false, false},
        {std::move(kh),  true,  false},
        {std::move(kv),  false, true},
        {std::move(khv), true,  true},
    };
    int best = 0;
    for (int i = 1; i < 4; ++i) {
        if (cands[i].k.bytes < cands[best].k.bytes) best = i;
    }
    return {std::move(cands[best].k), cands[best].hf, cands[best].vf};
}

// Encode the canonical-bytes pool into a bitplane buffer. Shared between
// pack_tiles (which also emits a tilemap) and build_tile_pool (pool-only).
Result<bitplane::BitplaneData>
encode_pool_bitplanes(std::span<const CellKey> canon,
                     std::span<const Color3f> palette,
                     std::size_t tile_w, std::size_t tile_h,
                     std::size_t depth,
                     bitplane::Layout layout) {
    auto n = canon.size();
    std::vector<std::uint8_t> indices(n * tile_w * tile_h);
    auto lut = build_palette_lookup(palette);
    for (std::size_t t = 0; t < n; ++t) {
        auto& bytes = canon[t].bytes;
        for (std::size_t y = 0; y < tile_h; ++y) {
            for (std::size_t x = 0; x < tile_w; ++x) {
                auto off = (y * tile_w + x) * 4;
                std::uint32_t k =
                    (static_cast<std::uint32_t>(bytes[off + 0]) << 16) |
                    (static_cast<std::uint32_t>(bytes[off + 1]) << 8) |
                    static_cast<std::uint32_t>(bytes[off + 2]);
                auto it = lut.find(k);
                indices[(t * tile_h + y) * tile_w + x] =
                    (it != lut.end()) ? it->second : std::uint8_t{0};
            }
        }
    }
    return bitplane::encode(indices, tile_w, n * tile_h, depth, layout);
}

} // namespace

// ---------------------------------------------------------------------------
// Shared-tile-pool builder: produces tile data + palette, no tilemap.
// ---------------------------------------------------------------------------

Result<TilePool> build_tile_pool(const Image& image, const Options& opts) {
    auto w = image.width();
    auto h = image.height();
    if (opts.tile_w == 0 || opts.tile_h == 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "tile size must be > 0"}};
    }
    if (w % opts.tile_w != 0 || h % opts.tile_h != 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("image {}x{} not a multiple of tile size {}x{}",
                        w, h, opts.tile_w, opts.tile_h)}};
    }
    if (opts.tile_w % 16 != 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("tile width {} must be a multiple of 16 (blitter word)",
                        opts.tile_w)}};
    }
    if (opts.depth < 1 || opts.depth > 8) {
        return std::unexpected{Error{ErrorCode::invalid_depth,
            std::format("depth must be 1..8, got {}", opts.depth)}};
    }

    auto map_w = w / opts.tile_w;
    auto map_h = h / opts.tile_h;
    std::unordered_map<CellKey, std::uint16_t, CellKeyHash> by_canon;
    std::vector<CellKey> canon;
    by_canon.reserve(map_w * map_h);
    canon.reserve(map_w * map_h);

    for (std::size_t cy = 0; cy < map_h; ++cy) {
        for (std::size_t cx = 0; cx < map_w; ++cx) {
            auto key = extract_cell_key(image,
                                        cx * opts.tile_w, cy * opts.tile_h,
                                        opts.tile_w, opts.tile_h);
            auto cr = canonicalize(std::move(key), opts.tile_w, opts.tile_h,
                                   opts.dedup);
            auto [it, inserted] = by_canon.try_emplace(
                cr.key, static_cast<std::uint16_t>(canon.size()));
            if (inserted) canon.push_back(std::move(cr.key));
        }
    }

    auto pal = extract_palette(image);
    auto max_colors = std::size_t{1} << opts.depth;
    if (pal.size() > max_colors) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            std::format("image has {} unique colors, depth {} allows only {}",
                        pal.size(), opts.depth, max_colors)}};
    }

    auto planes = encode_pool_bitplanes(canon, pal, opts.tile_w, opts.tile_h,
                                        opts.depth, opts.layout);
    if (!planes) return std::unexpected{planes.error()};

    TilePool out;
    out.planes = std::move(*planes);
    out.palette = std::move(pal);
    // Save canonical key bytes separately so slice_tilemap can re-hash.
    out.canonical_keys.reserve(canon.size());
    for (auto& c : canon) out.canonical_keys.push_back(std::move(c.bytes));
    out.tile_w = opts.tile_w;
    out.tile_h = opts.tile_h;
    out.depth = opts.depth;
    out.bytes_per_row = out.planes.bytes_per_row;
    out.tile_bytes = opts.tile_h * opts.depth * out.bytes_per_row;
    out.dedup = opts.dedup;
    return out;
}

// ---------------------------------------------------------------------------
// Level-PNG → tilemap-against-pool.
// ---------------------------------------------------------------------------

Result<MapResult> slice_tilemap(const Image& level, const TilePool& pool) {
    auto w = level.width();
    auto h = level.height();
    if (pool.tile_w == 0 || pool.tile_h == 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "tile pool has zero-size tiles"}};
    }
    if (w % pool.tile_w != 0 || h % pool.tile_h != 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("level {}x{} not a multiple of tile size {}x{}",
                        w, h, pool.tile_w, pool.tile_h)}};
    }

    // Build lookup: canonical bytes → pool tile index.
    std::unordered_map<CellKey, std::uint16_t, CellKeyHash> by_canon;
    by_canon.reserve(pool.canonical_keys.size());
    for (std::size_t i = 0; i < pool.canonical_keys.size(); ++i) {
        CellKey k;
        k.bytes = pool.canonical_keys[i];
        by_canon.emplace(std::move(k), static_cast<std::uint16_t>(i));
    }

    auto map_w = w / pool.tile_w;
    auto map_h = h / pool.tile_h;
    MapResult r;
    r.tilemap.resize(map_w * map_h);
    r.map_w = map_w;
    r.map_h = map_h;
    r.num_cells = map_w * map_h;

    for (std::size_t cy = 0; cy < map_h; ++cy) {
        for (std::size_t cx = 0; cx < map_w; ++cx) {
            auto raw = extract_cell_key(level,
                                        cx * pool.tile_w, cy * pool.tile_h,
                                        pool.tile_w, pool.tile_h);
            auto cr = canonicalize(std::move(raw), pool.tile_w, pool.tile_h,
                                   pool.dedup);
            auto it = by_canon.find(cr.key);
            if (it == by_canon.end()) {
                ++r.unknown_cells;
                // Emit a tombstone index (0 with flip bits unset). Caller can
                // detect this via unknown_cells > 0 and either reject or warn.
                r.tilemap[cy * map_w + cx] = MapCell::make(0, false, false);
                continue;
            }
            r.tilemap[cy * map_w + cx] =
                MapCell::make(it->second, cr.h_flip, cr.v_flip);
        }
    }
    return r;
}

Result<PackResult> pack_tiles(const Image& image, const Options& opts) {
    auto w = image.width();
    auto h = image.height();

    if (opts.tile_w == 0 || opts.tile_h == 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "tile size must be > 0"}};
    }
    if (w % opts.tile_w != 0 || h % opts.tile_h != 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("image {}x{} not a multiple of tile size {}x{}",
                        w, h, opts.tile_w, opts.tile_h)}};
    }
    if (opts.tile_w % 16 != 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            std::format("tile width {} must be a multiple of 16 (blitter word)",
                        opts.tile_w)}};
    }
    if (opts.depth < 1 || opts.depth > 8) {
        return std::unexpected{Error{ErrorCode::invalid_depth,
            std::format("depth must be 1..8, got {}", opts.depth)}};
    }

    auto map_w = w / opts.tile_w;
    auto map_h = h / opts.tile_h;
    auto num_cells = map_w * map_h;

    // Step 1: dedupe cells. In `flip` mode the four orientations
    // {identity, hflip, vflip, hvflip} collapse to one canonical tile; the
    // blitter's descending-mode blit synthesizes the flips at render time.
    // Canonical form = lexicographically smallest of the four byte sequences.
    std::unordered_map<CellKey, std::uint16_t, CellKeyHash> unique_by_canon;
    std::vector<CellKey> canon_of_tile;   // tile_idx → canonical pixels (what's stored)
    std::vector<MapCell> tilemap(num_cells);

    unique_by_canon.reserve(num_cells);
    canon_of_tile.reserve(num_cells);

    for (std::size_t cy = 0; cy < map_h; ++cy) {
        for (std::size_t cx = 0; cx < map_w; ++cx) {
            auto key = extract_cell_key(image,
                                        cx * opts.tile_w, cy * opts.tile_h,
                                        opts.tile_w, opts.tile_h);

            CellKey canon = key;
            bool cell_hf = false, cell_vf = false;

            if (opts.dedup == DedupMode::flip) {
                auto kh  = hflip_cell(key, opts.tile_w, opts.tile_h);
                auto kv  = vflip_cell(key, opts.tile_w, opts.tile_h);
                auto khv = hflip_cell(kv, opts.tile_w, opts.tile_h);
                // Pick canonical = lex-min of the four, track the flip that
                // transforms canonical → original (what the decoder applies).
                struct Cand { CellKey k; bool hf; bool vf; };
                Cand cands[4] = {
                    {std::move(key), false, false},
                    {std::move(kh),  true,  false},
                    {std::move(kv),  false, true},
                    {std::move(khv), true,  true},
                };
                int best = 0;
                for (int i = 1; i < 4; ++i) {
                    if (cands[i].k.bytes < cands[best].k.bytes) best = i;
                }
                canon = std::move(cands[best].k);
                // cands[best] is the canonical form. To go from canonical
                // back to original we apply the SAME flip that produced the
                // candidate — h/v flips are their own inverses.
                cell_hf = cands[best].hf;
                cell_vf = cands[best].vf;
            }

            auto [it, inserted] = unique_by_canon.try_emplace(
                canon, static_cast<std::uint16_t>(canon_of_tile.size()));
            if (inserted) {
                canon_of_tile.push_back(std::move(canon));
            }
            tilemap[cy * map_w + cx] = MapCell::make(it->second, cell_hf, cell_vf);
        }
    }

    auto num_unique = canon_of_tile.size();
    if (num_unique > 0x3FFF) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            std::format("too many unique tiles: {} (max 16383)", num_unique)}};
    }

    // Step 2: extract palette from the whole image and verify it fits.
    auto pal = extract_palette(image);
    auto max_colors = std::size_t{1} << opts.depth;
    if (pal.size() > max_colors) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            std::format("image has {} unique colors, depth {} allows only {}",
                        pal.size(), opts.depth, max_colors)}};
    }

    auto pal_lookup = build_palette_lookup(pal);

    // Step 3: build indexed image by walking canonical tile bytes. Stack tiles
    // vertically into a single (tile_w) × (num_unique * tile_h) indexed image,
    // then bitplane-encode it once.
    std::size_t tile_w = opts.tile_w;
    std::size_t tile_h = opts.tile_h;
    std::vector<std::uint8_t> indices(num_unique * tile_w * tile_h);

    for (std::size_t t = 0; t < num_unique; ++t) {
        auto& bytes = canon_of_tile[t].bytes;     // RGBA bytes, row-major
        for (std::size_t y = 0; y < tile_h; ++y) {
            for (std::size_t x = 0; x < tile_w; ++x) {
                auto off = (y * tile_w + x) * 4;
                std::uint32_t key =
                    (static_cast<std::uint32_t>(bytes[off + 0]) << 16) |
                    (static_cast<std::uint32_t>(bytes[off + 1]) << 8) |
                    static_cast<std::uint32_t>(bytes[off + 2]);
                auto it = pal_lookup.find(key);
                indices[(t * tile_h + y) * tile_w + x] =
                    (it != pal_lookup.end()) ? it->second : std::uint8_t{0};
            }
        }
    }

    auto planes = bitplane::encode(indices, tile_w, num_unique * tile_h,
                                   opts.depth, opts.layout);
    if (!planes) return std::unexpected{planes.error()};

    PackResult r;
    r.planes = std::move(*planes);
    r.palette = std::move(pal);
    r.tilemap = std::move(tilemap);
    r.map_w = map_w;
    r.map_h = map_h;
    r.num_cells = num_cells;
    r.num_unique_tiles = num_unique;
    r.bytes_per_row = r.planes.bytes_per_row;
    r.tile_bytes = tile_h * opts.depth * r.bytes_per_row;
    return r;
}

} // namespace png2amiga::tile_pack
