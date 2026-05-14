#pragma once

// Console / retro-display color quanta — a single home for the per-channel
// bit-depth tricks shared across Sega Genesis (BGR333), SNES (BGR555 +
// Direct-Color RGB443 pixel byte), Master System / NES / PCE if they land
// later. Each console's quirks are encoded as parameters, not duplicated
// files.
//
// All quantisation happens in sRGB space (the DAC's nominal input) with a
// linear → sRGB → quantise → expand → linear round-trip so the dither
// pipeline scores against the perceptually-correct quantised palette in
// OKLab without surprises at the gamma boundary.

#include "color_space.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstdint>

namespace png2amiga::console_color {

// ---------------------------------------------------------------------------
// Per-channel sRGB quantisation
//
// `bits` is the precision per channel that the hardware actually stores; the
// returned float is in [0, 1] with bit-replication expansion so saturated
// inputs round-trip cleanly to 0xFF (same trick used by OCS / EGA / etc.).
// ---------------------------------------------------------------------------
constexpr float quantise_srgb_to_bits(float srgb_v, int bits) noexcept {
    if (srgb_v < 0.0f) srgb_v = 0.0f;
    if (srgb_v > 1.0f) srgb_v = 1.0f;
    int max_val = (1 << bits) - 1;
    int q = static_cast<int>(srgb_v * static_cast<float>(max_val) + 0.5f);
    if (q < 0) q = 0;
    if (q > max_val) q = max_val;
    // Bit-replicate the b-bit value up to 8 bits.
    int out8 = 0;
    int shift = 8 - bits;
    out8 = (q << shift);
    int filled = bits;
    while (filled < 8) {
        int take = std::min(8 - filled, bits);
        out8 |= q >> (bits - take) << (8 - filled - take);
        filled += take;
    }
    return static_cast<float>(out8) / 255.0f;
}

// Integer quantum (no bit-replication). Useful when packing into hardware
// palette words / pixel bytes.
constexpr int quantise_srgb_to_int(float srgb_v, int bits) noexcept {
    if (srgb_v < 0.0f) srgb_v = 0.0f;
    if (srgb_v > 1.0f) srgb_v = 1.0f;
    int max_val = (1 << bits) - 1;
    int q = static_cast<int>(srgb_v * static_cast<float>(max_val) + 0.5f);
    if (q < 0) q = 0;
    if (q > max_val) q = max_val;
    return q;
}

// ---------------------------------------------------------------------------
// Snap a linear-RGB sample to a per-channel-bit grid in sRGB space.
// Returns the snapped color in linear RGB.
// ---------------------------------------------------------------------------
inline Color3f quantise_per_channel(Color3f linear, int r_bits, int g_bits, int b_bits) noexcept {
    auto srgb = color_space::linear_to_srgb(linear);
    Color3f q{
        quantise_srgb_to_bits(srgb.r, r_bits),
        quantise_srgb_to_bits(srgb.g, g_bits),
        quantise_srgb_to_bits(srgb.b, b_bits),
    };
    return color_space::srgb_to_linear(q);
}

// Convenience wrappers — symmetric depths (most common case).
inline Color3f quantise_uniform(Color3f linear, int bits_per_channel) noexcept {
    return quantise_per_channel(linear, bits_per_channel, bits_per_channel, bits_per_channel);
}

// ---------------------------------------------------------------------------
// Pack a linear color into a hardware palette word.
//
// Layout: bits packed (B << (r_bits + g_bits)) | (G << r_bits) | R.
// This matches both Sega Genesis CRAM (BGR333 → 0BBB0GGG0RRR layout
// produced by left-shifting each 3-bit value by 1 to fit the BGR mask) and
// SNES BGR555.
//
// `pad_zero_lsb` adds one zero bit between channels — set to true for
// Genesis CRAM (each channel sits in the upper 3 bits of a 4-bit nibble),
// false for SNES BGR555 (channels packed end-to-end).
// ---------------------------------------------------------------------------
inline std::uint16_t pack_word_bgr(
    Color3f linear, int r_bits, int g_bits, int b_bits, bool pad_zero_lsb = false) noexcept {
    auto srgb = color_space::linear_to_srgb(linear);
    int r = quantise_srgb_to_int(srgb.r, r_bits);
    int g = quantise_srgb_to_int(srgb.g, g_bits);
    int b = quantise_srgb_to_int(srgb.b, b_bits);
    if (pad_zero_lsb) {
        // Each channel left-shifted by 1 (Genesis: 0bbb0ggg0rrr in a u16).
        return static_cast<std::uint16_t>(((b << 1) << ((r_bits + 1) + (g_bits + 1))) |
                                          ((g << 1) << (r_bits + 1)) | (r << 1));
    }
    return static_cast<std::uint16_t>((b << (r_bits + g_bits)) | (g << r_bits) | r);
}

// ---------------------------------------------------------------------------
// Convenience: SNES Mode 7 named entry points.
// Existing call sites in api.cpp / main.cpp can use these names; new code
// should prefer the parameterised primitives above.
// ---------------------------------------------------------------------------
inline Color3f bgr555_quantize(Color3f linear) noexcept {
    return quantise_uniform(linear, 5);
}

inline Color3f rgb443_quantize(Color3f linear) noexcept {
    return quantise_per_channel(linear, 4, 4, 3);
}

// SNES Mode 7 Direct Color: pixel byte is BBGGGRRR (3+3+2 bits = 256
// effective colors). Mode 7 tilemap has no per-cell palette field, so
// the `ppp` low-bit boost is unavailable — the gamut really is 256.
inline Color3f rgb332_quantize(Color3f linear) noexcept {
    return quantise_per_channel(linear, 3, 3, 2);
}

inline std::uint16_t to_bgr555_word(Color3f linear) noexcept {
    return pack_word_bgr(linear, 5, 5, 5, /*pad_zero_lsb=*/false);
}

// SNES Direct-Color pixel byte: low 3 bits of R, low 3 bits of G, low 2
// bits of B → packed as bbgggrrr. Hardware sub-palette supplies the high
// bits; we only emit the per-pixel low bits.
inline std::uint8_t pack_rgb443_byte(Color3f linear) noexcept {
    auto srgb = color_space::linear_to_srgb(linear);
    int r4 = quantise_srgb_to_int(srgb.r, 4);
    int g4 = quantise_srgb_to_int(srgb.g, 4);
    int b3 = quantise_srgb_to_int(srgb.b, 3);
    int b_lo2 = b3 & 0x03;
    int g_lo3 = g4 & 0x07;
    int r_lo3 = r4 & 0x07;
    return static_cast<std::uint8_t>((b_lo2 << 6) | (g_lo3 << 3) | r_lo3);
}

// ---------------------------------------------------------------------------
// Sega Genesis BGR333 — 3 bits per channel in CRAM, packed 0BBB0GGG0RRR.
// 9 bits of color information across 12 bits of word storage; the 0-bits
// are not meaningful color data but the hardware reads the word as 16-bit.
// ---------------------------------------------------------------------------
inline Color3f bgr333_quantize(Color3f linear) noexcept {
    return quantise_uniform(linear, 3);
}

inline std::uint16_t to_bgr333_word(Color3f linear) noexcept {
    return pack_word_bgr(linear, 3, 3, 3, /*pad_zero_lsb=*/true);
}

}  // namespace png2amiga::console_color
