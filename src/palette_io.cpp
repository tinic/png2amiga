#include "palette_io.hpp"
#include "color_space.hpp"
#include "palette.hpp"

// nlohmann/json's exceptions hit strict warnings as errors in some
// of our config combos (assignment-suppress-on-throw) — silence the
// header's own warnings without affecting the rest of the TU.
#define JSON_USE_IMPLICIT_CONVERSIONS 0
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace png2amiga::palette_io {

namespace {

// ---------------------------------------------------------------------------
// Format detection
// ---------------------------------------------------------------------------

enum class Format : unsigned char {
    gimp_gpl,
    iff_cmap,
    ocs_binary,
    text_hex,
    json,
    unknown,
};

Format detect_format(std::span<const std::uint8_t> data) {
    if (data.size() < 4)
        return Format::unknown;

    // GIMP Palette: starts with "GIMP Palette"
    constexpr std::string_view gimp_magic = "GIMP Palette";
    if (data.size() >= gimp_magic.size()) {
        auto header = std::string_view(
            reinterpret_cast<const char*>(data.data()), gimp_magic.size());
        if (header == gimp_magic)
            return Format::gimp_gpl;
    }

    // JSON: first non-whitespace character is `{` or `[`. Matches the
    // single-line dump produced by --print-palette-json.
    for (std::size_t i = 0; i < data.size(); ++i) {
        char c = static_cast<char>(data[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '{' || c == '[') return Format::json;
        break;
    }

    // IFF: starts with "FORM"
    if (data[0] == 'F' && data[1] == 'O' && data[2] == 'R' && data[3] == 'M')
        return Format::iff_cmap;

    // OCS binary .pal: even size, all words have top nibble 0 (0x0RGB),
    // and contains non-printable bytes (distinguishes from hex text).
    if (data.size() >= 2 && data.size() % 2 == 0 && data.size() <= 512) {
        bool looks_binary = false;
        bool valid_ocs = true;
        for (std::size_t i = 0; i < data.size(); i += 2) {
            if (data[i] & 0xF0) valid_ocs = false;  // top nibble must be 0
            if (data[i] < 0x20 || data[i] > 0x7E) looks_binary = true;
        }
        if (valid_ocs && looks_binary)
            return Format::ocs_binary;
    }

    // Fallback: try text hex (lines of hex colors)
    return Format::text_hex;
}

// ---------------------------------------------------------------------------
// Read a 32-bit big-endian value from raw bytes
// ---------------------------------------------------------------------------

std::uint32_t read_u32_be(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

// ---------------------------------------------------------------------------
// GIMP .gpl parser
//
// Format:
//   GIMP Palette
//   Name: <name>           (optional)
//   Columns: <n>           (optional)
//   #<comment>             (optional)
//   R G B\t<name>          (one per color, R/G/B are 0-255 decimal)
// ---------------------------------------------------------------------------

Result<Palette> parse_gimp_gpl(std::span<const std::uint8_t> data) {
    auto text = std::string_view(
        reinterpret_cast<const char*>(data.data()), data.size());

    Palette pal;
    pal.name = "gimp";

    std::size_t pos = 0;
    auto next_line = [&]() -> std::string_view {
        if (pos >= text.size()) return {};
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        auto line = text.substr(pos, eol - pos);
        pos = eol + 1;
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        return line;
    };

    // First line must be "GIMP Palette"
    auto first = next_line();
    if (first != "GIMP Palette") {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "Not a GIMP Palette file (missing header)",
        }};
    }

    // Parse remaining lines
    while (pos < text.size()) {
        auto line = next_line();
        if (line.empty()) continue;
        if (line.starts_with('#')) continue;       // comment
        if (line.starts_with("Name:")) {
            pal.name = std::string(line.substr(5));
            // Trim leading whitespace
            while (!pal.name.empty() && pal.name.front() == ' ')
                pal.name.erase(pal.name.begin());
            continue;
        }
        if (line.starts_with("Columns:")) continue;

        // Parse "R G B" (possibly tab-separated with a name after)
        // Skip leading whitespace
        auto p = line.data();
        auto end = p + line.size();

        auto skip_ws = [&]() {
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
        };

        skip_ws();
        int r = 0, g = 0, b = 0;

        auto parse_int = [&](int& val) -> bool {
            auto [ptr, ec] = std::from_chars(p, end, val);
            if (ec != std::errc{}) return false;
            p = ptr;
            skip_ws();
            return true;
        };

        if (!parse_int(r) || !parse_int(g) || !parse_int(b))
            continue;  // skip malformed lines

        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);

        pal.colors.push_back(color_space::srgb_u8_to_linear(
            static_cast<std::uint8_t>(r),
            static_cast<std::uint8_t>(g),
            static_cast<std::uint8_t>(b)));
    }

    if (pal.colors.empty()) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "GIMP Palette file contains no colors",
        }};
    }

    return pal;
}

// ---------------------------------------------------------------------------
// IFF CMAP parser
//
// Scans an IFF file for a CMAP chunk. The CMAP chunk contains 3 bytes per
// color (R, G, B in sRGB 0-255 range). Other chunks are skipped.
// ---------------------------------------------------------------------------

Result<Palette> parse_iff_cmap(std::span<const std::uint8_t> data) {
    if (data.size() < 12) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "IFF file too small",
        }};
    }

    // Verify FORM header
    if (std::memcmp(data.data(), "FORM", 4) != 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "Not an IFF file (missing FORM header)",
        }};
    }

    // Skip FORM header + size + type ID (e.g., "ILBM")
    std::size_t pos = 12;

    Palette pal;
    pal.name = "iff";

    // Walk chunks looking for CMAP
    while (pos + 8 <= data.size()) {
        char chunk_id[5]{};
        std::memcpy(chunk_id, data.data() + pos, 4);
        auto chunk_size = read_u32_be(data.data() + pos + 4);
        pos += 8;

        // Bound the untrusted chunk_size against the remaining file so
        // neither (pos += chunk_size) nor the CMAP reader walks past the
        // buffer. A hostile IFF could claim chunk_size = 0xFFFFFFFF to
        // wrap pos (on 32-bit / WASM) or allocate a huge colors vector.
        if (chunk_size > data.size() - pos) {
            return std::unexpected{Error{
                ErrorCode::invalid_png,
                "IFF chunk size exceeds file",
            }};
        }

        if (std::strcmp(chunk_id, "CMAP") == 0) {
            // Read RGB triplets. Cap at 256 entries (max palette size)
            // so a malicious CMAP can't allocate a huge vector.
            auto num_colors = std::min(chunk_size / 3, std::uint32_t{256});
            for (std::uint32_t i = 0; i < num_colors && pos + 2 < data.size(); ++i) {
                auto r = data[pos++];
                auto g = data[pos++];
                auto b = data[pos++];
                pal.colors.push_back(color_space::srgb_u8_to_linear(r, g, b));
            }
            break;  // Found CMAP, done
        }

        // Skip to next chunk (chunks are padded to even size). chunk_size
        // is already bounded against remaining file above, so addition
        // is safe.
        pos += chunk_size;
        if (chunk_size % 2 != 0) ++pos;
    }

    if (pal.colors.empty()) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "IFF file contains no CMAP chunk",
        }};
    }

    return pal;
}

// ---------------------------------------------------------------------------
// Text hex parser
//
// One RRGGBB hex color per line. Lines starting with '#' or ';' are comments.
// Empty lines are skipped. Leading/trailing whitespace is trimmed.
// ---------------------------------------------------------------------------

Result<Palette> parse_text_hex(std::span<const std::uint8_t> data) {
    auto text = std::string_view(
        reinterpret_cast<const char*>(data.data()), data.size());

    Palette pal;
    pal.name = "hex";

    std::size_t pos = 0;
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        auto line = text.substr(pos, eol - pos);
        pos = eol + 1;

        // Strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        // Trim leading/trailing whitespace
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);

        if (line.empty()) continue;
        if (line.front() == '#' || line.front() == ';') continue;

        // Parse hex: accept "RRGGBB" or "0xRRGGBB" or "$RRGGBB"
        auto hex_str = line;
        if (hex_str.starts_with("0x") || hex_str.starts_with("0X"))
            hex_str.remove_prefix(2);
        else if (hex_str.starts_with("$"))
            hex_str.remove_prefix(1);

        if (hex_str.size() < 6) continue;

        // Only parse first 6 hex chars
        hex_str = hex_str.substr(0, 6);

        // Validate all hex digits
        bool valid = true;
        for (auto c : hex_str) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;

        std::uint32_t rgb = 0;
        auto [ptr, ec] = std::from_chars(hex_str.data(),
                                          hex_str.data() + hex_str.size(),
                                          rgb, 16);
        if (ec != std::errc{}) continue;

        auto r = static_cast<std::uint8_t>((rgb >> 16) & 0xFF);
        auto g = static_cast<std::uint8_t>((rgb >> 8) & 0xFF);
        auto b = static_cast<std::uint8_t>(rgb & 0xFF);

        pal.colors.push_back(color_space::srgb_u8_to_linear(r, g, b));
    }

    if (pal.colors.empty()) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "Text palette file contains no valid hex colors",
        }};
    }

    return pal;
}

// ---------------------------------------------------------------------------
// Read entire file into memory
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> read_file(std::string_view path) {
    auto path_str = std::string(path);
    std::ifstream file(path_str, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::file_not_found,
            "Failed to open palette file: " + path_str,
        }};
    }

    auto size = file.tellg();
    if (size <= 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "Palette file is empty: " + path_str,
        }};
    }
    // Cap at 16 MB. Real palette files are at most a few KB (OCS .pal is
    // 64 bytes, .gpl is < 10 KB, IFF with copper palettes maybe 64 KB).
    // A 16 MB cap leaves plenty of room while bounding DoS from a hostile
    // file pointing the tool at a huge blob.
    constexpr std::streamoff kMaxPaletteSize = 16 * 1024 * 1024;
    if (size > kMaxPaletteSize) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "Palette file too large: " + path_str,
        }};
    }

    file.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()),
              static_cast<std::streamsize>(size));

    return data;
}

// ---------------------------------------------------------------------------
// OCS binary .pal: 2 bytes per color, big-endian 0x0RGB
// ---------------------------------------------------------------------------

Result<Palette> parse_ocs_binary(std::span<const std::uint8_t> data) {
    Palette pal;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        auto w = static_cast<std::uint16_t>((data[i] << 8) | data[i + 1]);
        pal.colors.push_back(palette::ocs_to_linear(w));
    }
    if (pal.colors.empty()) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "Binary palette file is empty",
        }};
    }
    return pal;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// JSON palette parser — closes the round-trip with --print-palette-json.
//
// Schema produced by --print-palette-json (single-line by default; any
// pretty-printer with whitespace, key reordering, or extra fields
// round-trips since we use a real JSON parser):
//
//   {
//     "palette": [
//       {"idx": N, "rgb": "RRGGBB", "locked": B, "reserved": B,
//        "pixels": N},
//       ...
//     ],
//     "depth": N, "chipset": "ocs|aga", "mode": "lores|..."
//   }
//
// We consume `palette[].rgb` (required) and `palette[].locked` /
// `palette[].reserved` (optional). Slot index = array order; the
// `idx` field is informational. Other top-level fields are passed
// through but not enforced — depth/chipset/mode the caller's CLI
// flags supersede.
// ---------------------------------------------------------------------------
Result<Palette> parse_json_palette(std::span<const std::uint8_t> data) {
    Palette pal;
    pal.name = "json";
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(data.begin(), data.end(),
                                     /*cb=*/nullptr,
                                     /*allow_exceptions=*/true,
                                     /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            std::string{"JSON palette: parse error — "} + e.what(),
        }};
    }
    if (!doc.is_object() || !doc.contains("palette") ||
        !doc["palette"].is_array()) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "JSON palette: top-level object must have an array "
            "field \"palette\"",
        }};
    }
    const auto& arr = doc["palette"];
    int slot = 0;
    for (const auto& entry : arr) {
        if (!entry.is_object() || !entry.contains("rgb") ||
            !entry["rgb"].is_string()) {
            return std::unexpected{Error{
                ErrorCode::invalid_png,
                std::string{"JSON palette: slot "} +
                    std::to_string(slot) +
                    " missing string \"rgb\"",
            }};
        }
        std::string hex = entry["rgb"].get<std::string>();
        if (hex.size() != 6 ||
            !std::all_of(hex.begin(), hex.end(), [](char c) {
                return std::isxdigit(static_cast<unsigned char>(c));
            })) {
            return std::unexpected{Error{
                ErrorCode::invalid_png,
                std::string{"JSON palette: slot "} +
                    std::to_string(slot) +
                    " malformed rgb \"" + hex + "\"",
            }};
        }
        std::uint32_t rgb = 0;
        std::from_chars(hex.data(), hex.data() + 6, rgb, 16);
        pal.colors.push_back(color_space::srgb_u8_to_linear(
            static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
            static_cast<std::uint8_t>((rgb >>  8) & 0xFF),
            static_cast<std::uint8_t>(rgb & 0xFF)));
        if (entry.contains("locked") && entry["locked"].is_boolean() &&
            entry["locked"].get<bool>())
            pal.locked_indices.push_back(slot);
        if (entry.contains("reserved") &&
            entry["reserved"].is_boolean() &&
            entry["reserved"].get<bool>())
            pal.reserved_indices.push_back(slot);
        ++slot;
    }
    if (pal.colors.empty()) {
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "JSON palette: \"palette\" array is empty",
        }};
    }
    return pal;
}

Result<Palette> load_palette(std::string_view path) {
    auto data = read_file(path);
    if (!data) return std::unexpected{data.error()};
    return load_palette_from_memory(*data);
}

Result<Palette> load_palette_from_memory(std::span<const std::uint8_t> data) {
    auto format = detect_format(data);

    switch (format) {
    case Format::gimp_gpl:
        return parse_gimp_gpl(data);
    case Format::iff_cmap:
        return parse_iff_cmap(data);
    case Format::ocs_binary:
        return parse_ocs_binary(data);
    case Format::text_hex:
        return parse_text_hex(data);
    case Format::json:
        return parse_json_palette(data);
    case Format::unknown:
        return std::unexpected{Error{
            ErrorCode::invalid_png,
            "Unrecognized palette file format",
        }};
    }

    return std::unexpected{Error{
        ErrorCode::invalid_png,
        "Unrecognized palette file format",
    }};
}

// ---------------------------------------------------------------------------
// Write OCS 12-bit palette
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> encode_ocs_palette(
    std::span<const Color3f> colors) {
    std::vector<std::uint8_t> data;
    data.reserve(colors.size() * 2);

    for (auto& color : colors) {
        auto ocs = palette::linear_to_ocs(color);
        // Big-endian 16-bit
        data.push_back(static_cast<std::uint8_t>((ocs >> 8) & 0xFF));
        data.push_back(static_cast<std::uint8_t>(ocs & 0xFF));
    }

    return data;
}

Result<void> save_ocs_palette(std::string_view path,
                              std::span<const Color3f> colors) {
    auto data = encode_ocs_palette(colors);
    if (!data) return std::unexpected{data.error()};

    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to open palette file for writing: " + path_str,
        }};
    }

    file.write(reinterpret_cast<const char*>(data->data()),
               static_cast<std::streamsize>(data->size()));
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to write palette data to: " + path_str,
        }};
    }

    return {};
}

} // namespace png2amiga::palette_io
