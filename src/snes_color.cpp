#include "snes_color.hpp"
#include "color_space.hpp"

#include <algorithm>

namespace png2amiga::snes_color {

namespace {

// Quantise an sRGB value (0..1 float) to N bits and expand back via bit
// replication — same trick used by other reduced-precision modes (OCS,
// EGA, etc.). Replication ensures full sRGB white snaps to 0xFF rather
// than (255 - 1<<(8-N)).
constexpr float quantise_to_bits(float srgb_v, int bits) noexcept {
    if (srgb_v < 0.0f) srgb_v = 0.0f;
    if (srgb_v > 1.0f) srgb_v = 1.0f;
    int max_val = (1 << bits) - 1;
    int q = static_cast<int>(srgb_v * static_cast<float>(max_val) + 0.5f);
    if (q < 0) q = 0;
    if (q > max_val) q = max_val;
    // Bit replication to reach 8 bits: for 5-bit q, output = (q<<3) | (q>>2)
    int out8 = 0;
    int shift = 8 - bits;
    out8 = (q << shift);
    // Fill the lower bits by replicating the high bits of q
    int filled = bits;
    while (filled < 8) {
        int take = std::min(8 - filled, bits);
        out8 |= q >> (bits - take) << (8 - filled - take);
        filled += take;
    }
    return static_cast<float>(out8) / 255.0f;
}

// Quantise the integer 5/5/5 (or 4/4/3) component used to form the pixel
// byte / palette word — returns the integer quantum, no bit replication.
constexpr int quantise_to_int(float srgb_v, int bits) noexcept {
    if (srgb_v < 0.0f) srgb_v = 0.0f;
    if (srgb_v > 1.0f) srgb_v = 1.0f;
    int max_val = (1 << bits) - 1;
    int q = static_cast<int>(srgb_v * static_cast<float>(max_val) + 0.5f);
    if (q < 0) q = 0;
    if (q > max_val) q = max_val;
    return q;
}

} // namespace

Color3f bgr555_quantize(Color3f linear) noexcept {
    auto srgb = color_space::linear_to_srgb(linear);
    Color3f quantised{
        quantise_to_bits(srgb.r, 5),
        quantise_to_bits(srgb.g, 5),
        quantise_to_bits(srgb.b, 5),
    };
    return color_space::srgb_to_linear(quantised);
}

Color3f rgb443_quantize(Color3f linear) noexcept {
    auto srgb = color_space::linear_to_srgb(linear);
    Color3f quantised{
        quantise_to_bits(srgb.r, 4),
        quantise_to_bits(srgb.g, 4),
        quantise_to_bits(srgb.b, 3),
    };
    return color_space::srgb_to_linear(quantised);
}

std::uint16_t to_bgr555_word(Color3f linear) noexcept {
    auto srgb = color_space::linear_to_srgb(linear);
    int r5 = quantise_to_int(srgb.r, 5);
    int g5 = quantise_to_int(srgb.g, 5);
    int b5 = quantise_to_int(srgb.b, 5);
    return static_cast<std::uint16_t>((b5 << 10) | (g5 << 5) | r5);
}

std::uint8_t pack_rgb443_byte(Color3f linear) noexcept {
    auto srgb = color_space::linear_to_srgb(linear);
    int r4 = quantise_to_int(srgb.r, 4);
    int g4 = quantise_to_int(srgb.g, 4);
    int b3 = quantise_to_int(srgb.b, 3);
    // Low bits only: pixel byte = bbgggrrr (B: top 2 bits = bits 1-0 of b3,
    // G: middle 3 bits = bits 2-0 of g4, R: low 3 bits = bits 2-0 of r4).
    int b_lo2 = b3 & 0x03;
    int g_lo3 = g4 & 0x07;
    int r_lo3 = r4 & 0x07;
    return static_cast<std::uint8_t>((b_lo2 << 6) | (g_lo3 << 3) | r_lo3);
}

} // namespace png2amiga::snes_color
