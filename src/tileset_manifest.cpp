#include "tileset_manifest.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <string>

namespace png2amiga::tileset_manifest {

namespace {

constexpr std::uint16_t kVersion = 1;
constexpr char kMagic[4] = {'P', 'A', 'T', 'M'};

void put_u16(std::ostream& os, std::uint16_t v) {
    std::uint8_t buf[2] = {
        static_cast<std::uint8_t>(v >> 8),
        static_cast<std::uint8_t>(v & 0xFF),
    };
    os.write(reinterpret_cast<char*>(buf), 2);
}
void put_u32(std::ostream& os, std::uint32_t v) {
    std::uint8_t buf[4] = {
        static_cast<std::uint8_t>((v >> 24) & 0xFF),
        static_cast<std::uint8_t>((v >> 16) & 0xFF),
        static_cast<std::uint8_t>((v >>  8) & 0xFF),
        static_cast<std::uint8_t>( v        & 0xFF),
    };
    os.write(reinterpret_cast<char*>(buf), 4);
}

bool get_bytes(std::istream& is, void* dst, std::size_t n) {
    return static_cast<bool>(is.read(static_cast<char*>(dst),
                                     static_cast<std::streamsize>(n)));
}
bool get_u16(std::istream& is, std::uint16_t& out) {
    std::uint8_t b[2];
    if (!get_bytes(is, b, 2)) return false;
    out = static_cast<std::uint16_t>((b[0] << 8) | b[1]);
    return true;
}
bool get_u32(std::istream& is, std::uint32_t& out) {
    std::uint8_t b[4];
    if (!get_bytes(is, b, 4)) return false;
    out = (static_cast<std::uint32_t>(b[0]) << 24) |
          (static_cast<std::uint32_t>(b[1]) << 16) |
          (static_cast<std::uint32_t>(b[2]) <<  8) |
           static_cast<std::uint32_t>(b[3]);
    return true;
}

// Palette colors are serialized as 3 × u16 (0..65535 quantum of each linear
// channel). This is lossless vs a Color3f round-trip — the float payload is
// clamped to [0,1] and scaled into a 16-bit integer the reader re-floats.
void put_color(std::ostream& os, const Color3f& c) {
    auto enc = [](float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return static_cast<std::uint16_t>(v * 65535.0f + 0.5f);
    };
    put_u16(os, enc(c.r));
    put_u16(os, enc(c.g));
    put_u16(os, enc(c.b));
}
bool get_color(std::istream& is, Color3f& out) {
    std::uint16_t r, g, b;
    if (!get_u16(is, r) || !get_u16(is, g) || !get_u16(is, b)) return false;
    out = Color3f{r / 65535.0f, g / 65535.0f, b / 65535.0f};
    return true;
}

} // namespace

Result<void> write(std::string_view path,
                   const tile_pack::TilePool& pool,
                   std::string_view source_namespace) {
    std::ofstream os{std::string{path}, std::ios::binary | std::ios::trunc};
    if (!os) {
        return std::unexpected{Error{ErrorCode::write_failed,
            std::format("cannot open {}", path)}};
    }
    os.write(kMagic, 4);
    put_u16(os, kVersion);
    put_u16(os, static_cast<std::uint16_t>(pool.tile_w));
    put_u16(os, static_cast<std::uint16_t>(pool.tile_h));
    os.put(static_cast<char>(pool.depth));
    os.put(pool.dedup == tile_pack::DedupMode::flip ? 1 : 0);
    put_u32(os, static_cast<std::uint32_t>(pool.canonical_keys.size()));
    auto key_bytes = static_cast<std::uint32_t>(pool.tile_w * pool.tile_h * 4);
    put_u32(os, key_bytes);
    put_u32(os, static_cast<std::uint32_t>(pool.palette.size()));
    put_u16(os, static_cast<std::uint16_t>(source_namespace.size()));
    os.write(source_namespace.data(),
             static_cast<std::streamsize>(source_namespace.size()));

    for (auto& k : pool.canonical_keys) {
        if (k.size() != key_bytes) {
            return std::unexpected{Error{ErrorCode::write_failed,
                "canonical key size mismatch — pool corruption"}};
        }
        os.write(reinterpret_cast<const char*>(k.data()),
                 static_cast<std::streamsize>(k.size()));
    }
    for (auto& c : pool.palette) put_color(os, c);
    if (!os) {
        return std::unexpected{Error{ErrorCode::write_failed,
            std::format("write {} failed", path)}};
    }
    return {};
}

Result<Manifest> read(std::string_view path) {
    std::ifstream is{std::string{path}, std::ios::binary};
    if (!is) {
        return std::unexpected{Error{ErrorCode::file_not_found,
            std::format("cannot open manifest {}", path)}};
    }
    char magic[4];
    if (!get_bytes(is, magic, 4) || std::memcmp(magic, kMagic, 4) != 0) {
        return std::unexpected{Error{ErrorCode::invalid_png,  // reused code
            "manifest magic mismatch (not a PATM file)"}};
    }
    std::uint16_t version;
    if (!get_u16(is, version) || version != kVersion) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            std::format("manifest version {} (expected {})", version, kVersion)}};
    }
    std::uint16_t tw, th;
    std::uint32_t num_tiles, key_bytes, palette_n;
    std::uint16_t ns_len;
    std::uint8_t depth, dedup;
    if (!get_u16(is, tw) || !get_u16(is, th)) goto truncated;
    if (!get_bytes(is, &depth, 1) || !get_bytes(is, &dedup, 1)) goto truncated;
    if (!get_u32(is, num_tiles) || !get_u32(is, key_bytes) ||
        !get_u32(is, palette_n)) goto truncated;
    if (!get_u16(is, ns_len)) goto truncated;
    {
        Manifest m;
        m.source_namespace.resize(ns_len);
        if (ns_len && !get_bytes(is, m.source_namespace.data(), ns_len))
            goto truncated;

        if (key_bytes != static_cast<std::uint32_t>(tw) * th * 4) {
            return std::unexpected{Error{ErrorCode::unsupported_mode,
                "manifest key_bytes doesn't match tile_w*tile_h*4"}};
        }

        m.pool.tile_w = tw;
        m.pool.tile_h = th;
        m.pool.depth = depth;
        m.pool.dedup = dedup ? tile_pack::DedupMode::flip
                             : tile_pack::DedupMode::exact;
        m.pool.canonical_keys.reserve(num_tiles);
        for (std::uint32_t i = 0; i < num_tiles; ++i) {
            std::vector<std::uint8_t> buf(key_bytes);
            if (!get_bytes(is, buf.data(), key_bytes)) goto truncated;
            m.pool.canonical_keys.push_back(std::move(buf));
        }
        m.pool.palette.reserve(palette_n);
        for (std::uint32_t i = 0; i < palette_n; ++i) {
            Color3f c;
            if (!get_color(is, c)) goto truncated;
            m.pool.palette.push_back(c);
        }
        return m;
    }
truncated:
    return std::unexpected{Error{ErrorCode::unsupported_mode,
        std::format("manifest {} truncated", path)}};
}

} // namespace png2amiga::tileset_manifest
