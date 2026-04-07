#pragma once

#include "types.hpp"

namespace png2amiga::preprocess {

struct Settings {
    float brightness = 0.0f;  // [-1, 1] additive in OKLab L
    float contrast   = 1.0f;  // [0, 3] multiplicative around L=0.5
    float saturation = 1.0f;  // [0, 3] chroma scaling
    float gamma      = 1.0f;  // [0.1, 8] power curve
    float hue_shift  = 0.0f;  // [-180, 180] degrees rotation in OKLab a/b plane
    float sharpen    = 0.0f;  // [-1, 2] negative = blur, positive = sharpen
    float black_point = 0.0f; // [0, 0.5] clip darkest fraction
    float white_point = 0.0f; // [0, 0.5] clip brightest fraction
};

void apply(Image& image, const Settings& settings);

// Remap the image's OKLab extent to fit a palette's OKLab extent.
void match_palette_range(Image& image, const Palette& palette,
                         float percentile = 0.01f, float margin = 0.05f);

} // namespace png2amiga::preprocess
