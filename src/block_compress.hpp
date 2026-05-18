#pragma once

// Generic per-block texture-compression scaffold shared by ETC2 (today)
// and ASTC (planned). The pieces here are deliberately format-agnostic:
//
//   - BlockCoord / BlockGrid<T>   : block-grid containers
//   - BlockSample<W,H>            : 1 block of source pixels in linear + OKLab
//   - score_block<M>              : templated metric (srgb_mse / oklab2)
//   - block-grid Floyd-Steinberg  : residual propagation across blocks
//                                   (analog to dither::apply_error_diffusion
//                                    but on a (W/bw)×(H/bh) grid; this is
//                                    the project's main wager over existing
//                                    ETC2/ASTC encoders, which treat blocks
//                                    independently)
//
// Concrete encoders (etc2.cpp, future astc.cpp) provide:
//   - per-mode candidate generation (encodes uint8_t[bw*bh][3])
//   - decode_block(params) → uint8_t[bw*bh][3]
//
// All math stays in OKLab where possible, mirroring the HAM + dither
// pipeline (project_ham_aware_ed.md). Block error scoring is a pure
// perceptual-distance metric (the "wins" case in
// feedback_oklab_to_srgb_migration.md), so OKLab is the right default.

#include "color_space.hpp"
#include "dither.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace png2amiga::block_compress {

// ---------------------------------------------------------------------------
// Block-grid coordinates and containers
// ---------------------------------------------------------------------------

struct BlockCoord {
    int x;
    int y;
};

template<typename T>
class BlockGrid {
    std::vector<T> data_;
    int cols_{};
    int rows_{};

public:
    BlockGrid() = default;
    BlockGrid(int cols, int rows)
        : data_(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows)),
          cols_(cols),
          rows_(rows) {}

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

    T& operator[](int cx, int cy) noexcept {
        return data_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cols_) +
                     static_cast<std::size_t>(cx)];
    }
    const T& operator[](int cx, int cy) const noexcept {
        return data_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cols_) +
                     static_cast<std::size_t>(cx)];
    }

    [[nodiscard]] std::span<T> as_span() noexcept { return data_; }
    [[nodiscard]] std::span<const T> as_span() const noexcept { return data_; }
};

// ---------------------------------------------------------------------------
// BlockSample — one block's source pixels in both linear + OKLab + sRGB8
// ---------------------------------------------------------------------------
//
// All three forms are precomputed once per block so per-candidate scoring
// only has to convert the DECODED uint8_t to OKLab (when oklab2 metric is
// active). Format-agnostic: W*H is the per-block pixel count.

template<int W, int H>
struct BlockSample {
    static constexpr int npx = W * H;
    color_space::OKLab lab[npx]{};
    Color3f linear[npx]{};       // unused today, kept for future encoders
    std::uint8_t srgb8[npx][3]{};
};

// Extract one block of source pixels. Replicate-pad at image edges so
// the encoder runs on non-multiple-of-block-size sources without a
// special-case border path.
template<int W, int H>
inline void sample_block(BlockSample<W, H>& out,
                         std::span<const Color3f> src_linear,
                         std::span<const std::uint8_t> src_srgb8,  // packed RGB rows
                         int img_w,
                         int img_h,
                         int px,
                         int py) {
    for (int dy = 0; dy < H; ++dy) {
        int sy = std::clamp(py + dy, 0, img_h - 1);
        for (int dx = 0; dx < W; ++dx) {
            int sx = std::clamp(px + dx, 0, img_w - 1);
            std::size_t idx = static_cast<std::size_t>(sy) * static_cast<std::size_t>(img_w) +
                              static_cast<std::size_t>(sx);
            Color3f c = src_linear[idx];
            int i = dy * W + dx;
            out.linear[i] = c;
            out.lab[i] = color_space::linear_to_oklab(c);
            std::size_t s = idx * 3u;
            out.srgb8[i][0] = src_srgb8[s + 0];
            out.srgb8[i][1] = src_srgb8[s + 1];
            out.srgb8[i][2] = src_srgb8[s + 2];
        }
    }
}

// ---------------------------------------------------------------------------
// Block error metric — templated, zero-cost dispatch via if constexpr
// ---------------------------------------------------------------------------
//
// Mirrors ham.hpp's HamMetric (see project_ham_aware_ed.md). Default
// oklab2 matches HAM's v1.26.0 default; srgb_mse opt-in for headline-
// PSNR comparisons against existing encoders.

enum class BlockMetric : std::uint8_t {
    srgb_mse,
    oklab2,
};

template<BlockMetric M, int W, int H>
inline float score_block(const BlockSample<W, H>& src, const std::uint8_t (&dec_srgb8)[W * H][3]) {
    constexpr int N = W * H;
    if constexpr (M == BlockMetric::srgb_mse) {
        int acc = 0;
        for (int i = 0; i < N; ++i) {
            int dr = int(src.srgb8[i][0]) - int(dec_srgb8[i][0]);
            int dg = int(src.srgb8[i][1]) - int(dec_srgb8[i][1]);
            int db = int(src.srgb8[i][2]) - int(dec_srgb8[i][2]);
            acc += dr * dr + dg * dg + db * db;
        }
        return static_cast<float>(acc) * (1.0f / 65536.0f);  // ham.cpp normalisation
    } else {
        float acc = 0.0f;
        for (int i = 0; i < N; ++i) {
            color_space::OKLab dec = color_space::srgb8_to_oklab(dec_srgb8[i][0],
                                                                 dec_srgb8[i][1],
                                                                 dec_srgb8[i][2]);
            float dL = src.lab[i].L - dec.L;
            float da = src.lab[i].a - dec.a;
            float db_ = src.lab[i].b - dec.b;
            acc += color_space::fma_dist_sq(dL, da, db_);
        }
        return acc;
    }
}

// ---------------------------------------------------------------------------
// Block-grid error diffusion (the project's main quality wager)
// ---------------------------------------------------------------------------
//
// After a block is encoded, we know its decoded pixels and therefore the
// per-block mean residual (source_block_mean − decoded_block_mean) in OKLab.
// Existing ETC2/ASTC encoders treat blocks independently and accept the
// resulting per-block stepping at block boundaries. We treat the block
// grid as a coarse image and run an error-diffusion kernel on it: the
// per-block residual nudges neighbour blocks' target colours before
// they're encoded.
//
// The kernel comes from dither::error_diffusion_kernel(method) — same
// 58-method catalog the per-pixel dithering uses. Floyd-Steinberg is the
// default; Atkinson / Stucki / Jarvis / Sierra-Lite / etc. are all
// valid choices (a sweep across kernels is something for the bench
// harness to pick the best per content type). This obeys the HARD RULE
// in feedback_never_hardcode_fs — kernels are NOT hand-coded here.
//
// Hypothesis from project_ham_aware_ed.md: on continuous-tone content
// SSIMULACRA2 rewards spatial-correlation smoothness much more than PSNR
// does; block-grid ED should win SSIMULACRA2 with PSNR barely moving.

struct BlockGridEdOptions {
    float strength = 0.5f;     // 0 = off, 1 = full kernel strength
    float error_clamp = 4.0f;  // OKLab L/a/b shift cap per channel
    bool serpentine = true;    // alternate row direction
    dither::Method method = dither::Method::floyd_steinberg;
};

inline void propagate_block_residual(BlockGrid<color_space::OKLab>& targets,
                                     BlockCoord coord,
                                     color_space::OKLab residual,
                                     std::span<const dither::DiffusionEntry> kernel,
                                     const BlockGridEdOptions& opt,
                                     bool reverse_row) {
    auto clamp = [&](float v) { return std::clamp(v, -opt.error_clamp, opt.error_clamp); };
    for (const auto& e : kernel) {
        int sx = reverse_row ? -e.dx : e.dx;
        int cx = coord.x + sx;
        int cy = coord.y + e.dy;
        if (cx < 0 || cy < 0 || cx >= targets.cols() || cy >= targets.rows()) continue;
        float w = e.weight * opt.strength;
        targets[cx, cy].L += clamp(residual.L * w);
        targets[cx, cy].a += clamp(residual.a * w);
        targets[cx, cy].b += clamp(residual.b * w);
    }
}

}  // namespace png2amiga::block_compress
