// gcc-built wrapper for the apple-clang Metal TU.
//
// quantize_metal.cpp lives behind a C boundary (see quantize_metal.hpp)
// because gcc-15 (Homebrew libstdc++) and apple-clang (libc++) have
// incompatible STL ABIs on macOS. This wrapper marshals between the
// project's libstdc++ Result<Palette>/Image and the C interface,
// staying inside libstdc++ on this side.

#include "quantize_metal.hpp"

#include <cstring>

namespace png2amiga::quantize {

bool metal_available() noexcept {
    return png2amiga_metal_available_c();
}

Result<Palette> gpu_restart_quantize(
    const Image& image, std::size_t max_colors,
    int restarts, int iterations, std::uint32_t seed) noexcept {

    if (image.width() == 0 || image.height() == 0) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
                                      "empty image"}};
    }
    if (!png2amiga_metal_available_c()) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
                                      "Metal device unavailable"}};
    }
    if (max_colors == 0) max_colors = 1;
    if (max_colors > 256) max_colors = 256;

    const auto pixels = image.pixels();
    const std::size_t n = pixels.size();

    // Flatten Color3f span into the C-safe float array layout that
    // quantize_metal.cpp expects.
    std::vector<float> rgb(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        rgb[i*3+0] = pixels[i].r;
        rgb[i*3+1] = pixels[i].g;
        rgb[i*3+2] = pixels[i].b;
    }

    std::vector<float> pal_rgb(max_colors * 3);
    std::size_t out_size = 0;
    int rc = png2amiga_quantize_metal_c(
        rgb.data(), n, max_colors, restarts, iterations, seed,
        pal_rgb.data(), &out_size);
    if (rc != 0) {
        return std::unexpected{Error{ErrorCode::unsupported_mode,
            "gpu_restart_quantize: GPU error " + std::to_string(rc)}};
    }

    Palette out;
    out.name = "gpu-restart";
    out.colors.reserve(out_size);
    for (std::size_t k = 0; k < out_size; ++k) {
        out.colors.push_back(Color3f{
            pal_rgb[k*3+0], pal_rgb[k*3+1], pal_rgb[k*3+2]});
    }
    return out;
}

} // namespace png2amiga::quantize
