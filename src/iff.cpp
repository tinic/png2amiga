#include "iff.hpp"
#include "color_space.hpp"
#include "palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace png2amiga::iff {

namespace {

// ---------------------------------------------------------------------------
// IFF chunk helpers
// ---------------------------------------------------------------------------

// Write a 4-byte big-endian chunk ID
void write_id(std::vector<std::uint8_t>& out, const char* id) {
    out.push_back(static_cast<std::uint8_t>(id[0]));
    out.push_back(static_cast<std::uint8_t>(id[1]));
    out.push_back(static_cast<std::uint8_t>(id[2]));
    out.push_back(static_cast<std::uint8_t>(id[3]));
}

// Write a 32-bit big-endian value
void write_u32(std::vector<std::uint8_t>& out, std::uint32_t val) {
    out.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(val & 0xFF));
}

// Write a 16-bit big-endian value
void write_u16(std::vector<std::uint8_t>& out, std::uint16_t val) {
    out.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(val & 0xFF));
}

// Write a single byte
void write_u8(std::vector<std::uint8_t>& out, std::uint8_t val) {
    out.push_back(val);
}

// Patch a 32-bit value at a specific offset (for updating chunk sizes)
void patch_u32(std::vector<std::uint8_t>& out, std::size_t offset,
               std::uint32_t val) {
    out[offset + 0] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
    out[offset + 2] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
    out[offset + 3] = static_cast<std::uint8_t>(val & 0xFF);
}

// ---------------------------------------------------------------------------
// ByteRun1 compression (PackBits variant)
//
// For each row of each bitplane:
//   n >= 0:  copy n+1 literal bytes
//   n < 0:   repeat next byte (-n+1) times
//   n = -128: no-op
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> byterun1_compress(
    const std::uint8_t* data, std::size_t length) {
    std::vector<std::uint8_t> out;
    out.reserve(length + length / 128 + 1);

    std::size_t pos = 0;
    while (pos < length) {
        // Look for a run of identical bytes
        std::size_t run_start = pos;
        while (pos + 1 < length && data[pos] == data[pos + 1] && pos - run_start < 127) {
            ++pos;
        }

        auto run_len = pos - run_start + 1;

        if (run_len >= 3) {
            // Emit run: -(run_len - 1), byte
            out.push_back(static_cast<std::uint8_t>(
                static_cast<std::int8_t>(-(static_cast<int>(run_len) - 1))));
            out.push_back(data[run_start]);
            ++pos;
        } else {
            // Emit literal sequence
            pos = run_start;
            auto lit_start = pos;
            while (pos < length && pos - lit_start < 128) {
                // Check if a worthwhile run starts here
                if (pos + 2 < length && data[pos] == data[pos + 1] &&
                    data[pos] == data[pos + 2]) {
                    break;
                }
                ++pos;
            }

            auto lit_len = pos - lit_start;
            if (lit_len > 0) {
                out.push_back(static_cast<std::uint8_t>(lit_len - 1));
                for (std::size_t i = 0; i < lit_len; ++i) {
                    out.push_back(data[lit_start + i]);
                }
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// CAMG viewport mode word
// ---------------------------------------------------------------------------

std::uint32_t make_camg(amiga::Mode mode, bool hires, bool interlace) {
    std::uint32_t camg = 0;
    auto params = amiga::get_mode_params(mode);
    if (hires)       camg |= 0x8000;  // HIRES
    if (params.is_ham) camg |= 0x0800;  // HAM
    if (params.is_ehb) camg |= 0x0080;  // EXTRA_HALFBRITE
    if (interlace)   camg |= 0x0004;  // LACE
    return camg;
}

} // namespace

// ---------------------------------------------------------------------------
// Write IFF ILBM
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> write_ilbm(
    const bitplane::BitplaneData& planes,
    std::span<const Color3f> palette,
    amiga::Mode mode,
    const IffOptions& options) {
    std::vector<std::uint8_t> out;
    out.reserve(planes.total_bytes() + 256);  // rough estimate

    // FORM header (size patched later)
    write_id(out, "FORM");
    auto form_size_offset = out.size();
    write_u32(out, 0);  // placeholder
    write_id(out, "ILBM");

    // --- BMHD chunk ---
    write_id(out, "BMHD");
    write_u32(out, 20);  // BMHD is always 20 bytes

    write_u16(out, static_cast<std::uint16_t>(planes.width));
    write_u16(out, static_cast<std::uint16_t>(planes.height));
    write_u16(out, 0);  // x origin
    write_u16(out, 0);  // y origin
    write_u8(out, static_cast<std::uint8_t>(planes.depth));  // numPlanes
    write_u8(out, static_cast<std::uint8_t>(
        options.has_transparency ? 2 : 0));   // masking: 0=none, 2=transparent color
    write_u8(out, static_cast<std::uint8_t>(options.compression));  // compression
    write_u8(out, 0);   // pad
    write_u16(out, 0);  // transparent color (always index 0)
    write_u8(out, 10);  // x aspect (square pixels)
    write_u8(out, 10);  // y aspect (square pixels)
    write_u16(out, static_cast<std::uint16_t>(planes.width));   // page width
    write_u16(out, static_cast<std::uint16_t>(planes.height));  // page height

    // --- CMAP chunk ---
    // For EHB mode, only write the 32 base colors — the Amiga hardware
    // automatically generates the other 32 at half brightness.
    auto cmap_colors = palette;
    if (mode == amiga::Mode::ehb && palette.size() > 32) {
        cmap_colors = palette.first(32);
    }

    auto cmap_size = cmap_colors.size() * 3;
    write_id(out, "CMAP");
    write_u32(out, static_cast<std::uint32_t>(cmap_size));

    for (auto& color : cmap_colors) {
        auto srgb = color_space::linear_to_srgb(color).clamped();
        write_u8(out, static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f)));
        write_u8(out, static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f)));
        write_u8(out, static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f)));
    }
    // Pad CMAP to even length
    if (cmap_size % 2 != 0) {
        write_u8(out, 0);
    }

    // --- CAMG chunk ---
    auto camg = make_camg(mode, options.hires, options.interlace);
    write_id(out, "CAMG");
    write_u32(out, 4);
    write_u32(out, camg);

    // --- PCHG chunk (per-line palette changes) ---
    // Standard Amiga "Palette CHanGes" IFF extension (Sebastiano Vigna,
    // 1991). Readable by PCHG-aware viewers (Recoil, IrfanView IFF
    // plugin, ViewTek, etc.) and by hand-rolled Copper loaders.
    //
    // We emit the 12-bit variant (flag PCHGB_12BIT). Each scanline's
    // palette is diffed against the previous scanline — the first
    // scanline is diffed against the CMAP (which stores line 0's
    // palette). Only changed registers are written, keeping the chunk
    // compact even with per-line full-palette swaps.
    if (options.scanline_palettes &&
        !options.scanline_palettes->empty()) {
        auto& scan_pals = *options.scanline_palettes;
        auto height = planes.height;
        auto colors_per_line = scan_pals[0].size();

        // Build per-line change lists (register, 12-bit RGB).
        struct Change { std::uint8_t reg; std::uint16_t rgb12; };
        std::vector<std::vector<Change>> line_changes(height);
        std::vector<std::uint16_t> prev(colors_per_line);
        // Initial state = line 0's palette (matches CMAP). This means
        // line 0 emits no changes (CMAP already provides it). We start
        // diffing from line 1 against line 0.
        if (!scan_pals.empty()) {
            auto& pal0 = scan_pals[0];
            for (std::size_t i = 0; i < colors_per_line; ++i) {
                auto c = (i < pal0.size()) ? pal0[i] : Color3f{};
                prev[i] = palette::linear_to_ocs(c);
            }
        }
        std::size_t changed_lines = 0;
        std::uint16_t min_reg = 0xFFFF, max_reg = 0;
        std::uint16_t max_per_line = 0;
        std::uint32_t total = 0;
        for (std::size_t y = 0; y < height; ++y) {
            auto& pal = (y < scan_pals.size()) ? scan_pals[y] : scan_pals[0];
            auto& chgs = line_changes[y];
            for (std::size_t i = 0; i < colors_per_line; ++i) {
                auto c = (i < pal.size()) ? pal[i] : Color3f{};
                auto rgb = palette::linear_to_ocs(c);
                if (rgb != prev[i]) {
                    chgs.push_back({static_cast<std::uint8_t>(i), rgb});
                    prev[i] = rgb;
                }
            }
            if (!chgs.empty()) {
                ++changed_lines;
                if (chgs.front().reg < min_reg) min_reg = chgs.front().reg;
                if (chgs.back().reg > max_reg)  max_reg = chgs.back().reg;
                auto sz = static_cast<std::uint16_t>(chgs.size());
                if (sz > max_per_line) max_per_line = sz;
                total += static_cast<std::uint32_t>(chgs.size());
            }
        }
        if (min_reg == 0xFFFF) min_reg = 0;

        write_id(out, "PCHG");
        auto pchg_size_off = out.size();
        write_u32(out, 0);  // placeholder
        auto pchg_start = out.size();

        // PCHGHeader (20 bytes, all big-endian)
        write_u16(out, 0);              // Compression: 0 = none
        write_u16(out, 1);              // Flags: bit 0 = PCHGB_12BIT
        write_u16(out, 0);              // StartLine = 0
        write_u16(out, static_cast<std::uint16_t>(height));  // LineCount
        write_u16(out, static_cast<std::uint16_t>(changed_lines));
        write_u16(out, min_reg);
        write_u16(out, max_reg);
        write_u16(out, max_per_line);
        write_u32(out, total);

        // LineMask: ceil(LineCount/32) ULONGs, bit N set if line N has
        // any changes. Bit order: bit 31 of word 0 = line 0.
        auto mask_words = (height + 31) / 32;
        std::vector<std::uint32_t> mask(mask_words, 0);
        for (std::size_t y = 0; y < height; ++y) {
            if (!line_changes[y].empty()) {
                mask[y / 32] |= (1u << (31 - (y % 32)));
            }
        }
        for (auto w : mask) write_u32(out, w);

        // Per-changed-line data: SmallLineChanges, grouped by register
        // range 0..15 (count16) and 16..31 (count32), each followed by
        // SmallPaletteChange entries.
        //   SmallPaletteChange: high nibble = register within group,
        //                       low 12 bits = RGB444 (RRRRGGGGBBBB).
        // Registers 32+ are not expressible in the 12-bit PCHG variant;
        // we clip silently (rare for our HAM6/EHB/lores cases).
        for (std::size_t y = 0; y < height; ++y) {
            auto& chgs = line_changes[y];
            if (chgs.empty()) continue;
            std::uint8_t count16 = 0, count32 = 0;
            for (auto& c : chgs) {
                if (c.reg < 16) ++count16;
                else if (c.reg < 32) ++count32;
            }
            write_u8(out, count16);
            write_u8(out, count32);
            // Group 0..15 first, then 16..31
            for (auto& c : chgs) {
                if (c.reg >= 16) continue;
                std::uint16_t entry = static_cast<std::uint16_t>(
                    (c.reg << 12) | (c.rgb12 & 0x0FFF));
                write_u16(out, entry);
            }
            for (auto& c : chgs) {
                if (c.reg < 16 || c.reg >= 32) continue;
                std::uint16_t entry = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(c.reg - 16) << 12) |
                    (c.rgb12 & 0x0FFF));
                write_u16(out, entry);
            }
        }

        auto pchg_size = static_cast<std::uint32_t>(out.size() - pchg_start);
        patch_u32(out, pchg_size_off, pchg_size);
        if (pchg_size % 2 != 0) write_u8(out, 0);  // even-pad
    }

    // --- BODY chunk ---
    write_id(out, "BODY");
    auto body_size_offset = out.size();
    write_u32(out, 0);  // placeholder
    auto body_start = out.size();

    if (options.compression == Compression::none) {
        // Uncompressed: write interleaved bitplane data row by row
        for (std::size_t y = 0; y < planes.height; ++y) {
            for (std::size_t p = 0; p < planes.depth; ++p) {
                auto offset = planes.plane_row_offset(p, y);
                out.insert(out.end(),
                           planes.data.begin() + static_cast<std::ptrdiff_t>(offset),
                           planes.data.begin() + static_cast<std::ptrdiff_t>(offset + planes.bytes_per_row));
            }
        }
    } else {
        // ByteRun1 compressed: compress each plane row independently
        for (std::size_t y = 0; y < planes.height; ++y) {
            for (std::size_t p = 0; p < planes.depth; ++p) {
                auto offset = planes.plane_row_offset(p, y);
                auto compressed = byterun1_compress(
                    planes.data.data() + offset, planes.bytes_per_row);
                out.insert(out.end(), compressed.begin(), compressed.end());
            }
        }
    }

    auto body_size = out.size() - body_start;
    patch_u32(out, body_size_offset, static_cast<std::uint32_t>(body_size));
    // Pad BODY to even length
    if (body_size % 2 != 0) {
        write_u8(out, 0);
    }

    // Patch FORM size (everything after "FORM" + size field)
    auto form_size = out.size() - form_size_offset - 4;
    patch_u32(out, form_size_offset, static_cast<std::uint32_t>(form_size));

    return out;
}

// ---------------------------------------------------------------------------
// Save IFF ILBM to file
// ---------------------------------------------------------------------------

Result<void> save_ilbm(std::string_view path,
                       const bitplane::BitplaneData& planes,
                       std::span<const Color3f> palette,
                       amiga::Mode mode,
                       const IffOptions& options) {
    auto data = write_ilbm(planes, palette, mode, options);
    if (!data) return std::unexpected{data.error()};

    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to open file for writing: " + path_str,
        }};
    }

    file.write(reinterpret_cast<const char*>(data->data()),
               static_cast<std::streamsize>(data->size()));
    if (!file) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to write IFF data to: " + path_str,
        }};
    }

    return {};
}

} // namespace png2amiga::iff
