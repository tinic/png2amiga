#include "c64_prg.hpp"
#include "c64_displayers.hpp"

#include <cstring>
#include <fstream>
#include <string>

namespace png2amiga::c64::prg {

namespace {

constexpr std::uint16_t kDisplayerLoadAddr = 0x0801;
constexpr std::size_t kCells = 40 * 25;

void write_le16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

// Stitch the displayer (skipping its 2-byte load address) and the image
// data into one PRG, zero-filling the gap from displayer-end to
// image_addr. Mirrors png2c64's build_display_prg verbatim.
PrgData build_display_prg(const std::uint8_t* displayer,
                          std::size_t displayer_size,
                          const std::vector<std::uint8_t>& image_data,
                          std::uint16_t image_addr) {
    auto disp_data = displayer + 2;
    auto disp_data_size = displayer_size - 2;
    auto disp_end = static_cast<std::uint16_t>(
        kDisplayerLoadAddr + disp_data_size);

    std::size_t gap = (image_addr > disp_end)
        ? static_cast<std::size_t>(image_addr - disp_end) : 0;

    PrgData prg;
    write_le16(prg.bytes, kDisplayerLoadAddr);
    prg.bytes.insert(prg.bytes.end(), disp_data, disp_data + disp_data_size);
    prg.bytes.resize(prg.bytes.size() + gap, 0);
    prg.bytes.insert(prg.bytes.end(),
                     image_data.begin(), image_data.end());
    return prg;
}

Error wrong_size_error(std::string_view name) {
    return Error{
        ErrorCode::invalid_dimensions,
        std::string(name) + " requires 40x25 = 1000 cells",
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// Multicolor → Koala
// ---------------------------------------------------------------------------

Result<PrgData> koala(const EncodeResult& r) {
    if (r.screen_ram.size() != kCells || r.color_ram.size() != kCells
        || r.bitmap.size() != kCells * 8)
        return std::unexpected{wrong_size_error("Koala")};

    // EncodeResult layout for c64-multicolor:
    //   bitmap     8000 bytes (40 × 25 × 8)
    //   screen_ram 1000 bytes — upper nibble = c1, lower = c2
    //   color_ram  1000 bytes — c3 in low nibble
    //   bg_color   shared background
    std::vector<std::uint8_t> image_data;
    image_data.reserve(8000 + 1000 + 1000 + 1);
    image_data.insert(image_data.end(), r.bitmap.begin(), r.bitmap.end());
    image_data.insert(image_data.end(), r.screen_ram.begin(),
                      r.screen_ram.end());
    image_data.insert(image_data.end(), r.color_ram.begin(),
                      r.color_ram.end());
    image_data.push_back(r.bg_color & 0x0F);

    return build_display_prg(prg_detail::koala_displayer,
                             sizeof(prg_detail::koala_displayer),
                             image_data, 0x2000);
}

Result<PrgData> hires_bitmap(const EncodeResult& r) {
    if (r.screen_ram.size() != kCells
        || r.bitmap.size() != kCells * 8)
        return std::unexpected{wrong_size_error("Hires")};

    // Hires EncodeResult: screen_ram = upper c1 / lower c0, color_ram unused.
    std::vector<std::uint8_t> image_data;
    image_data.reserve(8000 + 1000);
    image_data.insert(image_data.end(), r.bitmap.begin(), r.bitmap.end());
    image_data.insert(image_data.end(), r.screen_ram.begin(),
                      r.screen_ram.end());

    return build_display_prg(prg_detail::hires_displayer,
                             sizeof(prg_detail::hires_displayer),
                             image_data, 0x2000);
}

// ---------------------------------------------------------------------------
// Raw image-only formats (.koa / .hir) — no displayer prepended.
// ---------------------------------------------------------------------------

Result<PrgData> koala_raw(const EncodeResult& r) {
    if (r.screen_ram.size() != kCells || r.color_ram.size() != kCells
        || r.bitmap.size() != kCells * 8)
        return std::unexpected{wrong_size_error("Koala")};

    PrgData prg;
    prg.bytes.reserve(2 + 8000 + 1000 + 1000 + 1);
    write_le16(prg.bytes, 0x6000);  // Koala Paint load address
    prg.bytes.insert(prg.bytes.end(), r.bitmap.begin(), r.bitmap.end());
    prg.bytes.insert(prg.bytes.end(), r.screen_ram.begin(),
                     r.screen_ram.end());
    prg.bytes.insert(prg.bytes.end(), r.color_ram.begin(),
                     r.color_ram.end());
    prg.bytes.push_back(r.bg_color & 0x0F);
    return prg;
}

Result<PrgData> hires_raw(const EncodeResult& r) {
    if (r.screen_ram.size() != kCells
        || r.bitmap.size() != kCells * 8)
        return std::unexpected{wrong_size_error("Hires")};

    PrgData prg;
    prg.bytes.reserve(2 + 8000 + 1000);
    write_le16(prg.bytes, 0x2000);  // Art Studio load address
    prg.bytes.insert(prg.bytes.end(), r.bitmap.begin(), r.bitmap.end());
    prg.bytes.insert(prg.bytes.end(), r.screen_ram.begin(),
                     r.screen_ram.end());
    return prg;
}

// ---------------------------------------------------------------------------
// FLI / AFLI — 8 screen RAMs at $4000-$5FFF, bitmap at $6000.
// ---------------------------------------------------------------------------

namespace {

// Pad a 1000-byte block to 1024 with zeros.
void append_padded(std::vector<std::uint8_t>& dst,
                   const std::uint8_t* src, std::size_t bytes,
                   std::size_t padded = 1024) {
    dst.insert(dst.end(), src, src + bytes);
    if (bytes < padded)
        dst.resize(dst.size() + (padded - bytes), 0);
}

}  // namespace

Result<PrgData> fli(const EncodeResult& r) {
    if (r.color_ram.size() != kCells || r.bitmap.size() != kCells * 8
        || r.screen_ram.size() != 8 * kCells)
        return std::unexpected{wrong_size_error("FLI")};

    // FLI memory layout from $3C00:
    //   $3C00 colorram (1000) padded to $4000
    //   $4000-$5FFF — 8 screen RAMs (1000 ea, padded to 1024)
    //   $6000 bitmap (8000)
    //   $7F40 bg_color
    //   pad to $8000
    std::vector<std::uint8_t> image_data;
    image_data.reserve(0x8000 - 0x3C00);
    append_padded(image_data, r.color_ram.data(), kCells);
    for (std::size_t i = 0; i < 8; ++i)
        append_padded(image_data, r.screen_ram.data() + i * kCells, kCells);
    image_data.insert(image_data.end(), r.bitmap.begin(), r.bitmap.end());
    image_data.resize(0x7F40 - 0x3C00, 0);
    image_data.push_back(r.bg_color & 0x0F);
    image_data.resize(0x8000 - 0x3C00, 0);

    return build_display_prg(prg_detail::fli_displayer,
                             sizeof(prg_detail::fli_displayer),
                             image_data, 0x3C00);
}

Result<PrgData> afli(const EncodeResult& r) {
    if (r.bitmap.size() != kCells * 8
        || r.screen_ram.size() != 8 * kCells)
        return std::unexpected{wrong_size_error("AFLI")};

    // AFLI memory layout from $4000:
    //   $4000-$5FFF — 8 screen RAMs (1000 ea, padded to 1024)
    //   $6000 bitmap
    //   $7F40 bg
    std::vector<std::uint8_t> image_data;
    image_data.reserve(0x8000 - 0x4000);
    for (std::size_t i = 0; i < 8; ++i)
        append_padded(image_data, r.screen_ram.data() + i * kCells, kCells);
    image_data.insert(image_data.end(), r.bitmap.begin(), r.bitmap.end());
    image_data.resize(0x7F40 - 0x4000, 0);
    image_data.push_back(r.bg_color & 0x0F);
    image_data.resize(0x8000 - 0x4000, 0);

    return build_display_prg(prg_detail::afli_displayer,
                             sizeof(prg_detail::afli_displayer),
                             image_data, 0x4000);
}

// ---------------------------------------------------------------------------
// PETSCII — text mode at $2000.
// ---------------------------------------------------------------------------

Result<PrgData> petscii_text(const EncodeResult& r) {
    if (r.screen_ram.size() != kCells || r.color_ram.size() != kCells)
        return std::unexpected{wrong_size_error("PETSCII")};

    // PETSCII EncodeResult: screen_ram = char index, color_ram = fg.
    std::vector<std::uint8_t> image_data;
    image_data.reserve(1000 + 1000 + 1);
    image_data.insert(image_data.end(), r.screen_ram.begin(),
                      r.screen_ram.end());
    for (auto c : r.color_ram) image_data.push_back(c & 0x0F);
    image_data.push_back(r.bg_color & 0x0F);

    return build_display_prg(prg_detail::petscii_displayer,
                             sizeof(prg_detail::petscii_displayer),
                             image_data, 0x2000);
}

// ---------------------------------------------------------------------------

Result<EncodeResult> unpack_pipeline_raw(
    amiga::Mode mode,
    const std::vector<std::uint8_t>& raw,
    std::uint8_t bg_color) {
    using amiga::Mode;
    std::size_t bitmap_size = 0, screen_size = 0, color_size = 0;
    switch (mode) {
    case Mode::c64_multicolor:
    case Mode::c64_hires:
        bitmap_size = 8000;
        screen_size = 1000;
        color_size = (mode == Mode::c64_multicolor) ? 1000 : 0;
        break;
    case Mode::c64_fli:
        bitmap_size = 8000;
        screen_size = 8000;
        color_size = 1000;
        break;
    case Mode::c64_afli:
        bitmap_size = 8000;
        screen_size = 8000;
        color_size = 0;
        break;
    case Mode::c64_petscii:
        bitmap_size = 0;
        screen_size = 1000;
        color_size = 1000;
        break;
    default:
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "PRG export not supported for this c64 mode",
        }};
    }
    if (raw.size() < bitmap_size + screen_size + color_size) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "raw_frame too small for c64 PRG unpack",
        }};
    }
    EncodeResult out;
    out.bg_color = bg_color;
    out.bitmap.assign(raw.begin(),
                      raw.begin() + static_cast<std::ptrdiff_t>(bitmap_size));
    out.screen_ram.assign(
        raw.begin() + static_cast<std::ptrdiff_t>(bitmap_size),
        raw.begin() + static_cast<std::ptrdiff_t>(bitmap_size + screen_size));
    out.color_ram.assign(
        raw.begin() + static_cast<std::ptrdiff_t>(bitmap_size + screen_size),
        raw.begin() + static_cast<std::ptrdiff_t>(
            bitmap_size + screen_size + color_size));
    return out;
}

Result<PrgData> from_mode(amiga::Mode mode, const EncodeResult& r) {
    using amiga::Mode;
    switch (mode) {
    case Mode::c64_multicolor: return koala(r);
    case Mode::c64_hires:      return hires_bitmap(r);
    case Mode::c64_fli:        return fli(r);
    case Mode::c64_afli:       return afli(r);
    case Mode::c64_petscii:    return petscii_text(r);
    default:
        return std::unexpected{Error{
            ErrorCode::unsupported_mode,
            "PRG export not supported for this mode (charset modes "
            "deferred until mc1/mc2 globals are surfaced)",
        }};
    }
}

Result<void> write(std::string_view path, const PrgData& data) {
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to open: " + std::string(path),
        }};
    }
    out.write(reinterpret_cast<const char*>(data.bytes.data()),
              static_cast<std::streamsize>(data.bytes.size()));
    return {};
}

}  // namespace png2amiga::c64::prg
