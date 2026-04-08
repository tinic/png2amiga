#include "degas.hpp"
#include "palette.hpp"

#include <fstream>
#include <format>

namespace png2amiga::degas {

// Degas Elite file format:
//   Byte 0-1:     Resolution (0=low 320x200x16, 1=med 640x200x4, 2=hi 640x400 mono)
//   Byte 2-33:    Palette (16 entries, 2 bytes each, big-endian)
//   Byte 34-32033: Bitplane data (32000 bytes, word-interleaved)
// Total: 32034 bytes always.

// STE palette word: 4-bit-per-channel with "twisted" bit order.
// Hardware stores each nibble as: bit3=LSB, bits2-0=upper 3 bits.
// So value 0bABCD becomes 0bBCDA in hardware register.
std::uint16_t to_ste_palette_word(Color3f color) {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r4 = static_cast<unsigned>(srgb.r * 15.0f + 0.5f) & 0xF;
    auto g4 = static_cast<unsigned>(srgb.g * 15.0f + 0.5f) & 0xF;
    auto b4 = static_cast<unsigned>(srgb.b * 15.0f + 0.5f) & 0xF;
    // Twist: ABCD -> BCDA
    auto twist = [](unsigned v) -> unsigned {
        return ((v & 0x7) << 1) | ((v >> 3) & 1);
    };
    return static_cast<std::uint16_t>(
        (twist(r4) << 8) | (twist(g4) << 4) | twist(b4));
}

std::uint16_t to_stf_palette_word(Color3f color) {
    auto srgb = color_space::linear_to_srgb(color).clamped();
    auto r3 = static_cast<unsigned>(srgb.r * 7.0f + 0.5f) & 0x7;
    auto g3 = static_cast<unsigned>(srgb.g * 7.0f + 0.5f) & 0x7;
    auto b3 = static_cast<unsigned>(srgb.b * 7.0f + 0.5f) & 0x7;
    return static_cast<std::uint16_t>((r3 << 8) | (g3 << 4) | b3);
}

Result<std::vector<std::uint8_t>> encode(
    const bitplane::BitplaneData& planes,
    std::span<const Color3f> palette,
    amiga::Mode mode) {

    if (!amiga::is_atari(mode)) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            "Degas output requires an Atari ST/STE mode"}};
    }

    if (planes.layout != bitplane::Layout::word_interleaved) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "Degas output requires word-interleaved bitplane data"}};
    }

    bool is_med = (mode == amiga::Mode::stf_med || mode == amiga::Mode::ste_med);
    bool is_stf = amiga::is_stf(mode);

    std::vector<std::uint8_t> out(32034, 0);

    // Resolution word (big-endian)
    out[0] = 0;
    out[1] = is_med ? 1 : 0;

    // Palette: 16 entries, 2 bytes each (big-endian)
    for (std::size_t i = 0; i < 16 && i < palette.size(); ++i) {
        auto word = is_stf
            ? to_stf_palette_word(palette[i])
            : to_ste_palette_word(palette[i]);
        out[2 + i * 2]     = static_cast<std::uint8_t>(word >> 8);
        out[2 + i * 2 + 1] = static_cast<std::uint8_t>(word & 0xFF);
    }

    // Bitplane data: copy up to 32000 bytes
    auto copy_size = std::min(planes.data.size(), std::size_t{32000});
    std::copy_n(planes.data.begin(), copy_size, out.begin() + 34);

    return out;
}

Result<void> save(std::string_view path,
                  const bitplane::BitplaneData& planes,
                  std::span<const Color3f> palette,
                  amiga::Mode mode) {
    auto data = encode(planes, palette, mode);
    if (!data) return std::unexpected{data.error()};

    auto path_str = std::string(path);
    std::ofstream file(path_str, std::ios::binary);
    if (!file) {
        return std::unexpected{Error{ErrorCode::write_failed,
            "Failed to open file: " + path_str}};
    }
    file.write(reinterpret_cast<const char*>(data->data()),
               static_cast<std::streamsize>(data->size()));
    return {};
}

} // namespace png2amiga::degas
