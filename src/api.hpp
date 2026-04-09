#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace png2amiga::api {

// Lock a palette slot to a specific color (sRGB 0-255), pre-quantization.
// The quantizer fills the remaining slots, and the locked color is reachable
// by the dither phase. Has no effect on HAM modes (palette is dynamic) or
// copper modes (per-scanline palettes).
struct LockSpec {
    int index;       // palette slot, 0..max_colors-1
    int r, g, b;     // sRGB 0-255
};

// Pin a palette slot to whatever color the source pixel at (x, y) ends up
// at after quantization+dithering. Implemented as a post-dither index swap:
// the index pixel(x,y) holds is swapped (palette + index map) with `index`.
// Pin targets must NOT be locked. Has no effect on HAM/copper modes.
struct PinSpec {
    int index;       // target palette slot
    int x, y;        // source pixel coordinates
};

struct Options {
    std::string mode = "lores";         // lores, hires, ham4, ham5, ham6, ham7, ham8, ehb
    std::string chipset;                // "ocs", "aga", or "" for auto
    int depth = 5;                      // bitplane depth (1-8, mode-dependent)
    bool interlace = false;             // set LACE bit in CAMG
    float gamma = 1.0f;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float hue_shift = 0.0f;
    float sharpen = 0.0f;
    float black_point = 0.0f;
    float white_point = 0.0f;
    bool match_range = false;
    int width = 0;                      // override output width (0 = mode default)
    int height = 0;                     // override output height (0 = mode default)

    // Dithering
    std::string dither = "floyd-steinberg"; // none, bayer2x2, bayer4x4, bayer8x8,
                                            // checker, h2x4, clustered-dot,
                                            // line2, line-checker, line4, line8,
                                            // floyd-steinberg, atkinson, sierra-lite,
                                            // stucki, jarvis
    float dither_strength = 1.0f;       // 0.0 = no dither, 1.0 = full
    float error_clamp = 0.12f;          // max error per OKLab channel

    // Palette
    std::string palette_file;           // load palette from file (empty = auto)
    std::vector<std::uint8_t> palette_data; // inline palette data (empty = auto)

    // HAM encoding
    std::string ham_quality = "optimal"; // fast, optimal
    int ham_beam = 16;                   // beam width for DP mode (1-256)

    // Copper palette (per-scanline palette changes)
    bool copper = false;                // use per-scanline copper palettes
    int copper_changes = 0;             // override changes/line (0 = auto)

    // Transparency
    float alpha_threshold = 0.0f;       // offset from 0.5 midpoint (-0.5..0.5)
    std::string alpha_dither;           // alpha dither method (empty = hard threshold)
    float alpha_dither_strength = 1.0f; // dither pattern intensity

    // C header output
    std::string symbol_name;            // base name for C symbols (default: "image")

    // Mask export
    bool mask_invert = false;           // invert mask polarity (default: 1=opaque, 0=transparent)

    // Cropping
    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;                     // 0 = no crop
    int crop_h = 0;
    bool crop_auto = false;             // auto-crop to mode aspect ratio (center)

    // Palette index manipulation (lores/hires/EHB/Atari only)
    std::vector<LockSpec> locks;
    std::vector<PinSpec>  pins;
};

struct ConvertResult {
    std::vector<std::uint8_t> data;     // output file bytes (IFF or PNG)
    int width{};
    int height{};
    int depth{};                        // bitplane depth
    int colors{};                       // number of palette colors
    float copperChanges{};              // avg actual color changes per line (0 if no copper)
    int totalColors{};                  // unique colors in rendered output
    int planeBytes{};                   // raw bitplane data size
    int copperBytes{};                  // copper list size (0 if no copper)
    float quantError{};                 // perceptual encoding error
    bool hasTransparency{};             // source image had alpha channel
    std::string error;                  // error message (empty on success)
};

// Convert raw image data (PNG/JPEG bytes) to Amiga format.
// Returns PNG bytes of the converted (preview) image.
ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options);

// Convert raw image data and return IFF ILBM bytes.
ConvertResult convert_iff(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// Convert raw image data and return C header (.h) as UTF-8 text bytes.
ConvertResult convert_cheader(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options);

// Convert raw image data and return Degas .PI1/.PI2 file bytes (Atari ST/STE).
ConvertResult convert_degas(const std::uint8_t* input_data,
                            std::size_t input_size,
                            const Options& options);

// Convert raw image data and return standalone viewer .cpp source (UTF-8 text).
ConvertResult convert_viewer(const std::uint8_t* input_data,
                             std::size_t input_size,
                             const Options& options);

// Convert raw image data and return RGBA pixel data (for live preview).
ConvertResult convert_rgba(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options);

// Convert raw image data and return raw bitplane bytes (interleaved, no header).
ConvertResult convert_raw(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// Convert raw image data and return OCS 12-bit palette bytes
// (2 bytes per color, big-endian 0x0RGB).
ConvertResult convert_palette(const std::uint8_t* input_data,
                              std::size_t input_size,
                              const Options& options);

// Convert raw image data and return the 1-bit transparency mask.
// Output format depends on caller:
//   - convert_mask: returns PNG bytes (1-bit B/W)
//   - convert_mask_raw: returns raw 1-bitplane data (word-aligned)
//   - convert_mask_iff: returns IFF ILBM (1 bitplane, B/W palette)
// White (1) = opaque, black (0) = transparent (or inverted with mask_invert).
// Returns empty data if source has no transparency.
ConvertResult convert_mask(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options);

ConvertResult convert_mask_raw(const std::uint8_t* input_data,
                               std::size_t input_size,
                               const Options& options);

ConvertResult convert_mask_iff(const std::uint8_t* input_data,
                               std::size_t input_size,
                               const Options& options);

} // namespace png2amiga::api
