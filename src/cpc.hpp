#pragma once

// Amstrad CPC encoder — classic Gate Array (27-color palette) and CPC Plus
// ASIC (4096-color RGB444 palette registers).
//
//   mode 0: 160×200, 16 inks, 2 px/byte
//   mode 1: 320×200,  4 inks, 4 px/byte
//   mode 2: 640×200,  2 inks, 8 px/byte
//
// Screen: 16 KB at &C000, 80 bytes per line. Byte offset of line y is
// (y / 8) * 80 + (y % 8) * 2048; the 48 bytes at the end of each 2 KB
// block are unused (left zero).
//
// Pixel packing (bit 7 = leftmost). Source: MAME
// src/mame/amstrad/amstrad_m.cpp amstrad_init_lookups() — the Gate Array
// shifts the byte left once per pixel and reads:
//   mode 2: pen bit0 = byte bit 7
//   mode 1: pen bit0 = bit 7, bit1 = bit 3            (pixel n: 7-n, 3-n)
//   mode 0: pen bit0 = bit 7, bit1 = bit 3, bit2 = bit 5, bit3 = bit 1
//           (pixel 1: bits 6, 2, 4, 0)

#include "amiga.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace png2amiga::cpc {

constexpr std::size_t kScreenBytes = 16384;
constexpr std::size_t kBytesPerLine = 80;
constexpr std::size_t kLines = 200;
constexpr std::uint16_t kScreenAddress = 0xC000;
constexpr std::size_t kAmsdosHeaderBytes = 128;

// Firmware color f (0..26): green level f/9, red level (f/3)%3, blue
// level f%3, each 0 / half / full. Gate Array hardware color number for
// firmware color f (the value written to the Gate Array is 0x40 | hw).
// Source: MAME amstrad_palette[] (indexed by hardware number) cross-
// checked against the CPC firmware color names.
constexpr std::array<std::uint8_t, 27> kFirmwareToHardware = {
    0x14, 0x04, 0x15, 0x1C, 0x18, 0x1D, 0x0C, 0x05, 0x0D,  //  0- 8
    0x16, 0x06, 0x17, 0x1E, 0x00, 0x1F, 0x0E, 0x07, 0x0F,  //  9-17
    0x12, 0x02, 0x13, 0x1A, 0x19, 0x1B, 0x0A, 0x03, 0x0B,  // 18-26
};

// sRGB level per channel step. 0x00 / 0x80 / 0xFF are the values the CPC
// wiki "CPC Palette" page lists (0 % / 50 % / 100 %).
constexpr std::array<std::uint8_t, 3> kLevels = {0x00, 0x80, 0xFF};

// Linear RGB of firmware color f.
Color3f firmware_color(std::size_t f) noexcept;

struct EncodeResult {
    Image rendered;                    // decoded from screen + inks
    std::vector<std::uint8_t> screen;  // 16384 bytes, &C000 layout
    std::vector<Color3f> inks;         // linear RGB, 16 / 4 / 2 entries
    // Classic CPC: firmware color number per ink (0..26). Empty on Plus.
    std::vector<std::uint8_t> firmware;
    // CPC Plus: 12-bit ink words 0x0GRB (ASIC palette RAM stores them LE:
    // byte 0 = R<<4 | B, byte 1 = G). Empty on the classic CPC.
    std::vector<std::uint16_t> plus_grb;
    float total_error = 0.0f;
};

// `image` must be at the mode's buffer size (160/320/640 × 200).
Result<EncodeResult> encode(const Image& image, amiga::Mode mode, const dither::Settings& settings);

// Pack a W×200 index buffer into the 16 KB screen.
std::vector<std::uint8_t> pack_screen(int screen_mode,
                                      std::span<const std::uint8_t> indices,
                                      std::size_t width);

// Unpack the 16 KB screen back to W×200 ink indices.
std::vector<std::uint8_t> unpack_screen(int screen_mode, std::span<const std::uint8_t> screen);

// Companion .pal bytes: classic = one Gate Array color byte per ink
// (0x40 | hardware number, as written to port &7Fxx); Plus = 2 bytes per
// ink in ASIC palette RAM order (R<<4|B, G).
std::vector<std::uint8_t> pal_bytes(amiga::Mode mode, std::span<const Color3f> inks);

// Firmware color number (0..26) nearest to each ink.
std::vector<std::uint8_t> firmware_numbers(std::span<const Color3f> inks);

// Plus ink words 0x0GRB.
std::vector<std::uint16_t> plus_words(std::span<const Color3f> inks);

// 128-byte AMSDOS header for a binary file (type 2). `name` is the 8.3
// name (upper-cased, space padded); checksum = sum of bytes 0..66.
std::array<std::uint8_t, kAmsdosHeaderBytes> amsdos_header(std::string_view name,
                                                           std::uint16_t load_address,
                                                           std::uint16_t length);

}  // namespace png2amiga::cpc
