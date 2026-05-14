#pragma once

// Optional Metal-accelerated palette quantizer (Apple GPU).
//
// The implementation lives in quantize_metal.cpp, compiled with
// apple-clang (it pulls in metal-cpp which uses Apple-clang-only
// extensions like ObjC blocks in some headers). The main project
// is built with gcc-15 (Homebrew libstdc++) on macOS; mixing
// libstdc++ and libc++ across object files would break ABI for
// any shared STL type, so this header exposes a strict C-pure
// surface — only PODs cross the language barrier.
//
// Result on DIV2K-100 + Kodak-24 vs pngquant:
//   K=4..256  mean ΔPSNR -0.2..-0.4 dB,  mean ΔS2 +2.6..+3.4
// Win rate (more S2) is 73-94% across K. The PSNR loss is the
// expected OKLab-vs-RGB-MSE trade-off; SSIMULACRA2 is the
// perceptual metric we care about.

#include "types.hpp"

#include <cstddef>
#include <cstdint>

namespace png2amiga::quantize {

// True iff Metal compute is available on this build (compiled with
// PNG2AMIGA_HAVE_METAL) AND a Metal device exists at runtime that
// supports atomic_float (Apple GPU family 7+ — A14 / M1 and later).
//
// Cached after first probe. Cheap to call from a hot path.
[[nodiscard]] bool metal_available() noexcept;

// Run k-means with parallel restarts on GPU. Returns the chosen
// palette (linear RGB) sorted by perceptual luminance. On failure
// (Metal unavailable, GPU error, image empty) returns
// std::unexpected; caller should fall back to median_cut.
[[nodiscard]] Result<Palette> gpu_restart_quantize(const Image& image,
                                                   std::size_t max_colors,
                                                   int restarts = 32,
                                                   int iterations = 20,
                                                   std::uint32_t seed = 0xC0FFEEu) noexcept;

}  // namespace png2amiga::quantize

// ---------------------------------------------------------------------------
// C-pure boundary for the apple-clang implementation TU. Don't call
// these directly — go through metal_available() / gpu_restart_quantize().
// ---------------------------------------------------------------------------
extern "C" {

bool png2amiga_metal_available_c() noexcept;

// pixels_rgb is float[n_pixels * 3] linear RGB.
// out_palette_rgb must be float[max_colors * 3] caller-allocated.
// On success returns 0 and writes *out_palette_size palette entries
// (sorted by OKLab L). Non-zero return = caller falls back.
int png2amiga_quantize_metal_c(const float* pixels_rgb,
                               std::size_t n_pixels,
                               std::size_t max_colors,
                               int restarts,
                               int iterations,
                               std::uint32_t seed,
                               float* out_palette_rgb,
                               std::size_t* out_palette_size) noexcept;

}  // extern "C"
