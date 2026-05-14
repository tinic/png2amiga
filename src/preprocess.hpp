#pragma once

#include "types.hpp"

namespace png2amiga::preprocess {

struct Settings {
    float brightness = 0.0f;   // [-1, 1] additive in OKLab L
    float contrast = 1.0f;     // [0, 3] multiplicative around L=0.5
    float saturation = 1.0f;   // [0, 3] chroma scaling
    float gamma = 1.0f;        // [0.1, 8] power curve
    float hue_shift = 0.0f;    // [-180, 180] degrees rotation in OKLab a/b plane
    float sharpen = 0.0f;      // [-1, 2] negative = blur, positive = sharpen
    float black_point = 0.0f;  // [0, 0.5] clip darkest fraction
    float white_point = 0.0f;  // [0, 0.5] clip brightest fraction
};

void apply(Image& image, const Settings& settings);

// Hue-preserving chroma stretch onto the palette's 3D OKLab convex
// hull: every pixel gets scaled per-(L, hue) so the source's chroma
// range at each hue slice fills the palette's reachable extent. Hue
// and luminance preserved; out-of-gamut excess clipped. The old
// axis-aligned bounding-box version that this replaced rotated hues
// and over-stretched diagonals — `percentile` and `margin` are
// retained for ABI but unused.
void match_palette_range(Image& image,
                         const Palette& palette,
                         float percentile = 0.01f,
                         float margin = 0.05f);

}  // namespace png2amiga::preprocess
