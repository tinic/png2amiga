#pragma once

// Thomson TO7/70 + TO8 encoders. Five modes total:
//
//   forme-couleur (320×200, 2 colors per 8×1-pixel attribute cell):
//     thomson_to7_320x16 — TO7/70 FIXED 16-color palette
//     thomson_to8_320x16 — TO8 programmable 16-from-4096 palette
//   bitmap (no per-cell color constraint, TO8 programmable palette):
//     thomson_to8_160x16 — 4bpp nibbles, 16 colors
//     thomson_to8_320x4  — 2 bitplanes, 4 colors
//     thomson_to8_640x2  — 1bpp, 2 colors
//
// Thomson VRAM is two 8 KB pages. The encoders return pageA + pageB byte
// arrays in the native hardware layout (transcribed from the theodore
// emulator's Decode* functions) plus a rendered preview Image and, for the
// programmable (TO8) modes, the chosen palette as 4-bit (r,g,b) channel
// indices into the EF9369 intens[] gamma LUT.

#include "amiga.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <cstdint>
#include <vector>

namespace png2amiga::thomson {

// Per-color 4-bit channel indices into the EF9369 intens[] LUT. For the
// fixed TO7/70 palette these are the hardware power-on values; for the TO8
// programmable modes they are the snapped quantizer output.
struct PaletteEntry {
    std::uint8_t r;  // 0..15
    std::uint8_t g;  // 0..15
    std::uint8_t b;  // 0..15
};

// forme-couleur pair-selection tunables (see encode_formecouleur in
// thomson.cpp for what each term does). Defaults are the Kodak-24-tuned
// shipping values; --best sweeps a grid around them per image.
struct FormeCouleurParams {
    float mix_noise_lambda = 0.1875f;  // λ on the Bernoulli mix-variance term
    float chroma_weight = 3.0f;        // a,b axis weight in pair distance
    float coherence_bonus = 0.01f;     // left/above same-pair discount
};

struct EncodeResult {
    Image rendered;                    // preview at the logical buffer size
    std::vector<std::uint8_t> page_a;  // 8000 bytes (forme-couleur: couleur;
                                       //   bitmap4: plane-lo; 160×16/640×2:
                                       //   low byte of the 16-bit word)
    std::vector<std::uint8_t> page_b;  // 8000 bytes (forme-couleur: shape;
                                       //   bitmap4: plane-hi; 160×16/640×2:
                                       //   high byte of the 16-bit word)
    std::vector<PaletteEntry> palette;  // empty for TO7/70 (fixed); 16/4/2
                                        //   entries for the TO8 modes.
    float total_error = 0.0f;           // OKLab² sum source vs rendered
};

// Encode an image already at the mode's native buffer dimensions
// (320×200 / 160×200 / 640×200). `mode` selects the layout; `pal_size`
// is implied by the mode. The dither settings drive the error-diffusion
// pass (forme-couleur uses per-cell candidate dithering; bitmap modes
// dither against the full snapped palette).
Result<EncodeResult> encode(const Image& image,
                            amiga::Mode mode,
                            const dither::Settings& settings = {},
                            const FormeCouleurParams& fc = {});

}  // namespace png2amiga::thomson
