// Compare two images using OKLab perceptual distance (same metric as png2amiga)
// Usage: compare <original.png> <converted.png>
// Outputs: total OKLab ΔE² (sum over all pixels), same scale as png2amiga's error

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

struct OKLab { float L, a, b; };

static float srgb_to_linear(float s) {
    if (s <= 0.04045f) return s / 12.92f;
    return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

static float fast_cbrt(float x) {
    if (x == 0.0f) return 0.0f;
    float sign = x < 0 ? -1.0f : 1.0f;
    x = std::abs(x);
    float y = std::cbrt(x);
    return sign * y;
}

static OKLab linear_to_oklab(float r, float g, float b) {
    float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;
    float l_ = fast_cbrt(l);
    float m_ = fast_cbrt(m);
    float s_ = fast_cbrt(s);
    return {
        0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
    };
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <original.png> <converted.png>\n", argv[0]);
        return 1;
    }

    int w1, h1, c1, w2, h2, c2;
    auto* img1 = stbi_load(argv[1], &w1, &h1, &c1, 3);
    auto* img2 = stbi_load(argv[2], &w2, &h2, &c2, 3);

    if (!img1) { std::fprintf(stderr, "Failed to load %s\n", argv[1]); return 1; }
    if (!img2) { std::fprintf(stderr, "Failed to load %s\n", argv[2]); return 1; }

    // Handle size mismatch: compare the overlapping region
    int w = w1 < w2 ? w1 : w2;
    int h = h1 < h2 ? h1 : h2;
    if (w1 != w2 || h1 != h2) {
        std::fprintf(stderr, "Warning: size mismatch %dx%d vs %dx%d, comparing %dx%d\n",
                     w1, h1, w2, h2, w, h);
    }

    double total_error = 0.0;   // OKLab ΔE² sum
    double sq_sum_8bit = 0.0;   // for PSNR in 8-bit sRGB
    long count = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx1 = (y * w1 + x) * 3;
            int idx2 = (y * w2 + x) * 3;

            float r1 = srgb_to_linear(img1[idx1 + 0] / 255.0f);
            float g1 = srgb_to_linear(img1[idx1 + 1] / 255.0f);
            float b1 = srgb_to_linear(img1[idx1 + 2] / 255.0f);

            float r2 = srgb_to_linear(img2[idx2 + 0] / 255.0f);
            float g2 = srgb_to_linear(img2[idx2 + 1] / 255.0f);
            float b2 = srgb_to_linear(img2[idx2 + 2] / 255.0f);

            auto lab1 = linear_to_oklab(r1, g1, b1);
            auto lab2 = linear_to_oklab(r2, g2, b2);

            float dL = lab1.L - lab2.L;
            float da = lab1.a - lab2.a;
            float db = lab1.b - lab2.b;
            total_error += static_cast<double>(dL * dL + da * da + db * db);

            int dr = img1[idx1 + 0] - img2[idx2 + 0];
            int dg = img1[idx1 + 1] - img2[idx2 + 1];
            int dbb = img1[idx1 + 2] - img2[idx2 + 2];
            sq_sum_8bit += dr*dr + dg*dg + dbb*dbb;
            ++count;
        }
    }

    double mse = sq_sum_8bit / (count * 3.0);
    double psnr = (mse < 1e-12) ? 99.99 : 10.0 * std::log10(255.0 * 255.0 / mse);
    std::printf("error: %.4f PSNR: %.2f\n", total_error, psnr);

    stbi_image_free(img1);
    stbi_image_free(img2);
    return 0;
}
