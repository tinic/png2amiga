#include "dither.hpp"
#include "color_space.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace png2amiga::dither {

namespace {

// ===========================================================================
// Ordered dither matrices (normalized to [-0.5, 0.5])
// ===========================================================================

constexpr auto make_bayer2x2() noexcept {
    constexpr std::array<std::array<int, 2>, 2> raw = {{
        {{0, 2}},
        {{3, 1}},
    }};
    std::array<std::array<float, 2>, 2> m{};
    for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 4.0f - 0.5f;
    return m;
}

constexpr auto make_bayer4x4() noexcept {
    constexpr std::array<std::array<int, 4>, 4> raw = {{
        {{ 0,  8,  2, 10}},
        {{12,  4, 14,  6}},
        {{ 3, 11,  1,  9}},
        {{15,  7, 13,  5}},
    }};
    std::array<std::array<float, 4>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 16.0f - 0.5f;
    return m;
}

// Dispersed-dot matrices for non-power-of-2 sizes. Bayer's recursive
// construction only gives 2ⁿ; for 3/5/6/7 we use hand-tuned magic-square
// permutations that scatter consecutive thresholds far apart in the cell
// (the property that makes Bayer "blue-ish"). Different spectral
// character from the 4×/8× Bayer ladder — breaks the rigid grid look on
// integer-scaled pixel art.

constexpr auto make_bayer3x3() noexcept {
    constexpr std::array<std::array<int, 3>, 3> raw = {{
        {{0, 7, 3}},
        {{6, 5, 1}},
        {{4, 2, 8}},
    }};
    std::array<std::array<float, 3>, 3> m{};
    for (std::size_t y = 0; y < 3; ++y)
        for (std::size_t x = 0; x < 3; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 9.0f - 0.5f;
    return m;
}

constexpr auto make_bayer5x5() noexcept {
    constexpr std::array<std::array<int, 5>, 5> raw = {{
        {{ 0, 14,  3, 17,  6}},
        {{10, 21,  7, 24, 12}},
        {{ 4, 18,  1, 15,  9}},
        {{20,  8, 23, 11, 22}},
        {{ 2, 16,  5, 19, 13}},
    }};
    std::array<std::array<float, 5>, 5> m{};
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 5; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 25.0f - 0.5f;
    return m;
}

constexpr auto make_bayer6x6() noexcept {
    constexpr std::array<std::array<int, 6>, 6> raw = {{
        {{ 0, 22,  6, 28, 12, 33}},
        {{18,  9, 25, 15, 31,  3}},
        {{ 5, 27, 14, 35, 20,  8}},
        {{23, 11, 32, 17, 26,  1}},
        {{ 7, 30, 19,  4, 29, 13}},
        {{34, 16, 24, 10, 21,  2}},
    }};
    std::array<std::array<float, 6>, 6> m{};
    for (std::size_t y = 0; y < 6; ++y)
        for (std::size_t x = 0; x < 6; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 36.0f - 0.5f;
    return m;
}

constexpr auto make_bayer7x7() noexcept {
    constexpr std::array<std::array<int, 7>, 7> raw = {{
        {{ 0, 30,  6, 36, 12, 42, 18}},
        {{24, 11, 38, 17, 44, 23,  4}},
        {{ 7, 33, 20, 27, 14, 47, 31}},
        {{40, 15, 46,  2, 28, 10, 35}},
        {{19, 25,  8, 41, 22, 37,  1}},
        {{43,  3, 32, 13, 48,  5, 26}},
        {{ 9, 39, 21, 34, 16, 29, 45}},
    }};
    std::array<std::array<float, 7>, 7> m{};
    for (std::size_t y = 0; y < 7; ++y)
        for (std::size_t x = 0; x < 7; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 49.0f - 0.5f;
    return m;
}

constexpr auto make_bayer8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{ 0, 32,  8, 40,  2, 34, 10, 42}},
        {{48, 16, 56, 24, 50, 18, 58, 26}},
        {{12, 44,  4, 36, 14, 46,  6, 38}},
        {{60, 28, 52, 20, 62, 30, 54, 22}},
        {{ 3, 35, 11, 43,  1, 33,  9, 41}},
        {{51, 19, 59, 27, 49, 17, 57, 25}},
        {{15, 47,  7, 39, 13, 45,  5, 37}},
        {{63, 31, 55, 23, 61, 29, 53, 21}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 64.0f - 0.5f;
    return m;
}

constexpr auto make_checker() noexcept {
    return std::array<std::array<float, 2>, 2>{{
        {{-0.25f,  0.25f}},
        {{ 0.25f, -0.25f}},
    }};
}

constexpr auto make_h2x4() noexcept {
    // 2x4 Bayer — at 2:1 pixel ratio, tiles as perceptually square 4x4 block
    constexpr std::array<std::array<int, 2>, 4> raw = {{
        {{0, 4}},
        {{6, 2}},
        {{1, 5}},
        {{7, 3}},
    }};
    std::array<std::array<float, 2>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto make_clustered_dot() noexcept {
    // 4x4 clustered dot — pixels form round dots at 2:1 pixel ratio,
    // cluster grows from center outward
    constexpr std::array<std::array<int, 4>, 4> raw = {{
        {{12,  5,  6, 13}},
        {{ 4,  0,  1,  7}},
        {{11,  3,  2,  8}},
        {{15, 10,  9, 14}},
    }};
    std::array<std::array<float, 4>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 16.0f - 0.5f;
    return m;
}

constexpr auto make_line2() noexcept {
    // 1x2 alternating rows
    return std::array<std::array<float, 1>, 2>{{
        {{-0.25f}},
        {{ 0.25f}},
    }};
}

constexpr auto make_vline2() noexcept {
    // 2x1 alternating columns (vertical lines — transpose of line2)
    return std::array<std::array<float, 2>, 1>{{
        {{-0.25f, 0.25f}},
    }};
}

constexpr auto make_vline_checker() noexcept {
    // 2x2 column-biased: columns have large threshold difference,
    // rows get subtle offset — produces vertical lines with pixel variation
    // (transpose of line_checker)
    return std::array<std::array<float, 2>, 2>{{
        {{-0.35f,  0.15f}},
        {{-0.15f,  0.35f}},
    }};
}

constexpr auto make_vline4() noexcept {
    // 4x1 vertical gradient (transpose of line4)
    return std::array<std::array<float, 4>, 1>{{
        {{-0.375f, -0.125f, 0.125f, 0.375f}},
    }};
}

constexpr auto make_vline8() noexcept {
    // 8x1 finest vertical gradient (transpose of line8)
    constexpr std::array<int, 8> raw = {0, 4, 2, 6, 1, 5, 3, 7};
    std::array<std::array<float, 8>, 1> m{};
    for (std::size_t x = 0; x < 8; ++x)
        m[0][x] = (static_cast<float>(raw[x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto make_line_checker() noexcept {
    // 2x2 line-biased: rows have large threshold difference,
    // columns get subtle offset — produces horizontal lines with pixel variation
    return std::array<std::array<float, 2>, 2>{{
        {{-0.35f, -0.15f}},
        {{ 0.15f,  0.35f}},
    }};
}

constexpr auto make_line4() noexcept {
    // 1x4 smooth gradient
    return std::array<std::array<float, 1>, 4>{{
        {{-0.375f}},
        {{-0.125f}},
        {{ 0.125f}},
        {{ 0.375f}},
    }};
}

constexpr auto make_line8() noexcept {
    // 1x8 finest horizontal gradient
    constexpr std::array<int, 8> raw = {0, 4, 2, 6, 1, 5, 3, 7};
    std::array<std::array<float, 1>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        m[y][0] = (static_cast<float>(raw[y]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

// Niklasson surface-stable fractal dither — hierarchical 16×16 tile
// where every 2× subdivision is itself a valid threshold matrix. Used
// in *Return of the Obra Dinn*: the recursive structure means the same
// pattern reads at multiple zoom levels. Built by Bayer-style
// recursion (B[i,j] = 4·B[i/2,j/2] + B2[i%2, j%2]) up to 16×16, so
// 256 distinct thresholds. Quieter, more organic-feeling dither than
// standard 8×8 Bayer.
constexpr auto make_fractal16() noexcept {
    constexpr std::array<std::array<int, 2>, 2> b2 = {{ {{0, 2}}, {{3, 1}} }};
    std::array<std::array<int, 16>, 16> raw{};
    // Recursively double: B_{2N}[i,j] = 4·B_N[i/2,j/2] + b2[i%2, j%2].
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            int v = 0;
            std::size_t yy = y, xx = x;
            for (int level = 0; level < 4; ++level) {
                v = v * 4 + b2[yy & 1][xx & 1];
                yy >>= 1;
                xx >>= 1;
            }
            // Reverse the level order so finest level is least-significant.
            int rev = 0, vv = v;
            for (int i = 0; i < 4; ++i) { rev = rev * 4 + (vv & 3); vv >>= 2; }
            raw[y][x] = rev;
        }
    }
    std::array<std::array<float, 16>, 16> m{};
    for (std::size_t y = 0; y < 16; ++y)
        for (std::size_t x = 0; x < 16; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 256.0f - 0.5f;
    return m;
}

constexpr auto bayer2 = make_bayer2x2();
constexpr auto bayer4 = make_bayer4x4();
constexpr auto bayer8 = make_bayer8x8();
constexpr auto fractal16_mat = make_fractal16();

// Aseprite "old" 4×4 — hand-edited by pixel artists for clean reading on
// integer-scaled LCD displays. Biases on-pixels into a checker phase
// rather than the Bayer "diagonal sweep". Source: aseprite/src/render/
// ordered_dither.h (BSD-2 licensed).
constexpr auto make_aseprite_old() noexcept {
    constexpr std::array<std::array<int, 4>, 4> raw = {{
        {{ 0,  8,  2, 10}},
        {{14,  6, 12,  4}},
        {{ 3, 11,  1,  9}},
        {{15,  7, 13,  5}},
    }};
    std::array<std::array<float, 4>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 16.0f - 0.5f;
    return m;
}

// libcaca hand-tuned dispersed-dot matrices (Hocevar's terminal-phosphor
// study). Different spectral character than recursive Bayer constructions.
constexpr auto make_libcaca_3x3() noexcept {
    constexpr std::array<std::array<int, 3>, 3> raw = {{
        {{0, 7, 3}},
        {{6, 4, 1}},
        {{2, 8, 5}},
    }};
    std::array<std::array<float, 3>, 3> m{};
    for (std::size_t y = 0; y < 3; ++y)
        for (std::size_t x = 0; x < 3; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 9.0f - 0.5f;
    return m;
}

constexpr auto make_libcaca_6x6() noexcept {
    constexpr std::array<std::array<int, 6>, 6> raw = {{
        {{ 0, 28,  4, 24, 14, 32}},
        {{20,  8, 30, 12, 22,  2}},
        {{ 6, 26, 16, 34, 10, 18}},
        {{31, 13, 23,  3, 29,  5}},
        {{15, 35,  9, 19,  7, 25}},
        {{21,  1, 27, 11, 33, 17}},
    }};
    std::array<std::array<float, 6>, 6> m{};
    for (std::size_t y = 0; y < 6; ++y)
        for (std::size_t x = 0; x < 6; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 36.0f - 0.5f;
    return m;
}

// Pegasus / REXPaint 8×8 mosaic — biases on-pixels toward each tile's
// centre, producing a "tiled glyph" look that reads on character-cell
// displays where each pixel is a separate visual unit.
constexpr auto make_pegasus_8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{ 0, 48,  8, 40,  2, 50, 10, 42}},
        {{56, 24, 32, 16, 58, 26, 34, 18}},
        {{12, 44,  4, 36, 14, 46,  6, 38}},
        {{28, 60, 20, 52, 30, 62, 22, 54}},
        {{ 3, 51, 11, 43,  1, 49,  9, 41}},
        {{59, 27, 35, 19, 57, 25, 33, 17}},
        {{15, 47,  7, 39, 13, 45,  5, 37}},
        {{31, 63, 23, 55, 29, 61, 21, 53}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 64.0f - 0.5f;
    return m;
}

constexpr auto aseprite_old_mat = make_aseprite_old();
constexpr auto libcaca_3x3_mat = make_libcaca_3x3();
constexpr auto libcaca_6x6_mat = make_libcaca_6x6();
constexpr auto pegasus_8x8_mat = make_pegasus_8x8();
constexpr auto bayer3 = make_bayer3x3();
constexpr auto bayer5 = make_bayer5x5();
constexpr auto bayer6 = make_bayer6x6();
constexpr auto bayer7 = make_bayer7x7();
constexpr auto checker_mat = make_checker();
constexpr auto h2x4_mat = make_h2x4();

// V 4x2: vertical bias (4 wide, 2 tall) — good for hires 1:2 tall pixels
constexpr auto make_v4x2() noexcept {
    return std::array<std::array<float, 4>, 2>{{
        {{-0.35f, -0.15f,  0.15f,  0.35f}},
        {{ 0.25f,  0.05f, -0.05f, -0.25f}},
    }};
}

// Bayer 4x2: standard Bayer ordering in 4 wide x 2 tall
constexpr auto make_bayer4x2() noexcept {
    constexpr std::array<std::array<int, 4>, 2> raw = {{
        {{0, 4, 1, 5}},
        {{6, 2, 7, 3}},
    }};
    std::array<std::array<float, 4>, 2> m{};
    for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

// Bayer 2x4: standard Bayer ordering in 2 wide x 4 tall
constexpr auto make_bayer2x4() noexcept {
    constexpr std::array<std::array<int, 2>, 4> raw = {{
        {{0, 6}},
        {{4, 2}},
        {{1, 7}},
        {{5, 3}},
    }};
    std::array<std::array<float, 2>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto v4x2_mat = make_v4x2();
constexpr auto bayer4x2_mat = make_bayer4x2();
constexpr auto bayer2x4_mat = make_bayer2x4();
constexpr auto clustered_mat = make_clustered_dot();
constexpr auto line2_mat = make_line2();
constexpr auto vline2_mat = make_vline2();
constexpr auto vline_checker_mat = make_vline_checker();
constexpr auto vline4_mat = make_vline4();
constexpr auto vline8_mat = make_vline8();
constexpr auto line_checker_mat = make_line_checker();
constexpr auto line4_mat = make_line4();
constexpr auto line8_mat = make_line8();

// 45-degree halftone 8x8 (newspaper/print look, 32 gray levels)
constexpr auto make_halftone8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{13,  7,  8, 14, 17, 21, 22, 18}},
        {{ 6,  1,  3,  9, 28, 31, 29, 23}},
        {{ 5,  2,  4, 10, 27, 32, 30, 24}},
        {{16, 12, 11, 15, 20, 26, 25, 19}},
        {{17, 21, 22, 18, 13,  7,  8, 14}},
        {{28, 31, 29, 23,  6,  1,  3,  9}},
        {{27, 32, 30, 24,  5,  2,  4, 10}},
        {{20, 26, 25, 19, 16, 12, 11, 15}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) - 0.5f) / 32.0f - 0.5f;
    return m;
}

// Spiral 5x5 (organic dot growth outward from center)
constexpr auto make_spiral5x5() noexcept {
    constexpr std::array<std::array<int, 5>, 5> raw = {{
        {{20, 21, 22, 23, 24}},
        {{19,  6,  7,  8,  9}},
        {{18,  5,  0,  1, 10}},
        {{17,  4,  3,  2, 11}},
        {{16, 15, 14, 13, 12}},
    }};
    std::array<std::array<float, 5>, 5> m{};
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 5; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 25.0f - 0.5f;
    return m;
}

// Non-rectangular hexagonal tiling 8x8 (breaks Bayer cross-hatch)
constexpr auto make_hex8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{3, 4, 2, 7, 1, 6, 0, 5}},
        {{6, 0, 5, 3, 4, 2, 7, 1}},
        {{2, 7, 1, 6, 0, 5, 3, 4}},
        {{5, 3, 4, 2, 7, 1, 6, 0}},
        {{1, 6, 0, 5, 3, 4, 2, 7}},
        {{4, 2, 7, 1, 6, 0, 5, 3}},
        {{0, 5, 3, 4, 2, 7, 1, 6}},
        {{7, 1, 6, 0, 5, 3, 4, 2}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

// Void-and-cluster 64×64 blue noise (Ulichney 1993). Generated by
// tools/gen_void_cluster.mjs. Per-rank optimal: every threshold
// percentile is itself blue-noise distributed.
constexpr auto make_void_cluster64() noexcept {
    #include "void_cluster_64.inc"
    std::array<std::array<float, 64>, 64> m{};
    for (std::size_t y = 0; y < 64; ++y)
        for (std::size_t x = 0; x < 64; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 4096.0f - 0.5f;
    return m;
}

// Cluster blue noise 64×64 — same algorithm with a wider Gaussian
// (sigma=2.8) so on-pixels form larger clusters. Coarser, more
// film-grainy texture than the standard void-and-cluster — good for
// CRT phosphor look and very small palettes.
constexpr auto make_cluster_noise64() noexcept {
    #include "cluster_noise_64.inc"
    std::array<std::array<float, 64>, 64> m{};
    for (std::size_t y = 0; y < 64; ++y)
        for (std::size_t x = 0; x < 64; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 4096.0f - 0.5f;
    return m;
}

// Blue noise 64x64 — generated from Interleaved Gradient Noise formula
// fract(52.9829189 * fract(0.06711056*x + 0.00583715*y))
// Rank-ordered to produce 4096 threshold levels. No visible pattern.
constexpr auto make_blue_noise64() noexcept {
    std::array<std::array<float, 64>, 64> m{};
    // Generate IGN values and use directly as thresholds
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            auto fx = static_cast<float>(x);
            auto fy = static_cast<float>(y);
            float v = 52.9829189f * (0.06711056f * fx + 0.00583715f * fy);
            v = v - static_cast<float>(static_cast<int>(v)); // fract
            if (v < 0.0f) v += 1.0f;
            v = 52.9829189f * v;
            v = v - static_cast<float>(static_cast<int>(v)); // fract
            if (v < 0.0f) v += 1.0f;
            m[y][x] = v - 0.5f;  // center around 0
        }
    }
    return m;
}

// Diagonal clustered dot 8x8 (newspaper halftone, 64 levels)
constexpr auto make_diagonal8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{24, 10, 12, 26, 35, 47, 49, 37}},
        {{ 8,  0,  2, 14, 45, 59, 61, 51}},
        {{22,  6,  4, 16, 43, 57, 63, 53}},
        {{30, 20, 18, 28, 33, 41, 55, 39}},
        {{34, 46, 48, 36, 25, 11, 13, 27}},
        {{44, 58, 60, 50,  9,  1,  3, 15}},
        {{42, 56, 62, 52, 23,  7,  5, 17}},
        {{32, 40, 54, 38, 31, 21, 19, 29}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 64.0f - 0.5f;
    return m;
}

// Non-rectangular slanted square 5x5
constexpr auto make_hex5x5() noexcept {
    constexpr std::array<std::array<int, 5>, 5> raw = {{
        {{4, 3, 0, 1, 2}},
        {{0, 1, 2, 4, 3}},
        {{2, 4, 3, 0, 1}},
        {{3, 0, 1, 2, 4}},
        {{1, 2, 4, 3, 0}},
    }};
    std::array<std::array<float, 5>, 5> m{};
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 5; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 5.0f - 0.5f;
    return m;
}

constexpr auto halftone8x8_mat = make_halftone8x8();
constexpr auto diagonal8x8_mat = make_diagonal8x8();
constexpr auto spiral5x5_mat = make_spiral5x5();
constexpr auto hex8x8_mat = make_hex8x8();
constexpr auto hex5x5_mat = make_hex5x5();
constexpr auto blue_noise_mat = make_blue_noise64();
constexpr auto void_cluster_mat = make_void_cluster64();
constexpr auto cluster_noise_mat = make_cluster_noise64();

// ===========================================================================
// OKLab arithmetic helpers
// ===========================================================================

using OKLab = color_space::OKLab;

constexpr OKLab oklab_add(OKLab a, OKLab b) noexcept {
    return {a.L + b.L, a.a + b.a, a.b + b.b};
}

constexpr OKLab oklab_sub(OKLab a, OKLab b) noexcept {
    return {a.L - b.L, a.a - b.a, a.b - b.b};
}

constexpr OKLab oklab_scale(OKLab v, float s) noexcept {
    return {v.L * s, v.a * s, v.b * s};
}

constexpr OKLab oklab_clamp(OKLab e, float max_mag) noexcept {
    return {
        std::clamp(e.L, -max_mag, max_mag),
        std::clamp(e.a, -max_mag, max_mag),
        std::clamp(e.b, -max_mag, max_mag),
    };
}

// ===========================================================================
// Precompute palette in OKLab space
// ===========================================================================

std::vector<OKLab> precompute_palette_lab(std::span<const Color3f> palette) {
    std::vector<OKLab> lab(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i) {
        lab[i] = color_space::linear_to_oklab(palette[i]);
    }
    return lab;
}

// Find nearest palette color in OKLab space.
// Returns palette index and the OKLab value of the chosen color.
struct NearestResult {
    std::size_t index;
    OKLab color_lab;
    float dist_sq;
};

NearestResult find_nearest_oklab(OKLab pixel_lab,
                                 std::span<const OKLab> palette_lab) {
    float best_dist = std::numeric_limits<float>::max();
    std::size_t best_idx = 0;
    OKLab best_lab{};

    for (std::size_t i = 0; i < palette_lab.size(); ++i) {
        auto& cl = palette_lab[i];
        float dL = pixel_lab.L - cl.L;
        float da = pixel_lab.a - cl.a;
        float db = pixel_lab.b - cl.b;
        float dist = dL * dL + da * da + db * db;

        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
            best_lab = cl;
        }
    }

    return {best_idx, best_lab, best_dist};
}

// ===========================================================================
// Ordered dithering
// ===========================================================================

// Find nearest + second-nearest palette entries.
struct NearestPair {
    std::size_t idxA;    // nearest
    std::size_t idxB;    // second-nearest
    float distA;          // OKLab ΔE² to nearest
    float distB;          // OKLab ΔE² to second-nearest
};

NearestPair find_nearest_pair(OKLab pixel,
                               std::span<const OKLab> palette_lab) noexcept {
    float best = std::numeric_limits<float>::max();
    float second = std::numeric_limits<float>::max();
    std::size_t bi = 0, si = 0;
    for (std::size_t k = 0; k < palette_lab.size(); ++k) {
        float dL = pixel.L - palette_lab[k].L;
        float da = pixel.a - palette_lab[k].a;
        float db = pixel.b - palette_lab[k].b;
        float d = dL * dL + da * da + db * db;
        if (d < best) {
            second = best; si = bi;
            best = d; bi = k;
        } else if (d < second) {
            second = d; si = k;
        }
    }
    return {bi, si, best, second};
}

template <std::size_t W, std::size_t H>
DitherResult apply_ordered(const Image& image,
                           const std::array<std::array<float, W>, H>& matrix,
                           std::span<const OKLab> palette_lab,
                           float strength) {
    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    // Perturb each pixel's OKLab by the Bayer threshold before finding the
    // nearest palette color. Previous binary A-vs-B threshold only averaged
    // two nearest palette entries and produced visibly muted dithering on
    // small palettes; perturbation lets a pixel "reach" distant colors when
    // the bias shifts it significantly, giving the classic punchy ordered-
    // dither look that matches tools like GIMP / ImageMagick.
    //
    // Perturbation magnitude is tuned so strength=1 produces clearly visible
    // dithering without drowning the original colors.
    constexpr float kPerturbL = 0.12f;
    constexpr float kPerturbAB = 0.06f;

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto pixel_lab = color_space::linear_to_oklab(image[x, y]);

            // Bayer value in [-0.5, +0.5).
            float thr = matrix[y % H][x % W];
            pixel_lab.L += thr * strength * kPerturbL;
            pixel_lab.a += thr * strength * kPerturbAB;
            pixel_lab.b += thr * strength * kPerturbAB;

            std::size_t best = 0;
            float best_d = std::numeric_limits<float>::infinity();
            for (std::size_t i = 0; i < palette_lab.size(); ++i) {
                float dL = pixel_lab.L - palette_lab[i].L;
                float da = pixel_lab.a - palette_lab[i].a;
                float db = pixel_lab.b - palette_lab[i].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best = i; }
            }
            auto chosen = palette_lab[best];
            // Report error against the (unperturbed) target for metric consistency.
            auto orig = color_space::linear_to_oklab(image[x, y]);
            float dL = orig.L - chosen.L;
            float da = orig.a - chosen.a;
            float db = orig.b - chosen.b;
            result.indices[y * w + x] = static_cast<std::uint8_t>(best);
            result.total_error += dL * dL + da * da + db * db;
        }
    }

    return result;
}

// ===========================================================================
// Error diffusion
// ===========================================================================

// DiffusionEntry is declared publicly in dither.hpp.

// Floyd-Steinberg (1976) — canonical 7/3/5/1 over 16. Sweep over a 5⁴
// kernel grid against the 15 example images at lores depth 5 confirmed
// these weights as PSNR rank 1/81 in OKLab perceptual space.
//        * 7/16
//  3/16 5/16 1/16
constexpr std::array floyd_steinberg_kernel = {
    DiffusionEntry{ 1, 0, 7.0f / 16.0f},
    DiffusionEntry{-1, 1, 3.0f / 16.0f},
    DiffusionEntry{ 0, 1, 5.0f / 16.0f},
    DiffusionEntry{ 1, 1, 1.0f / 16.0f},
};

// Atkinson: 6-cell error diffusion. Originally distributed only 75% of
// error (6 × 1/8 = 0.75) — Bill Atkinson's 1985 deliberate aesthetic
// choice for 1-bit Macintosh displays where under-distribution gave
// "softer" black/white output.
//
// We ship OKLab-tuned weights instead: a 729-kernel × 15-image sweep at
// lores depth 5 found canonical Atkinson at rank 286/729 (mean PSNR
// 33.13 dB vs the optimum's 33.61 dB — +0.48 dB perceptual gain). The
// winning shape preserves energy (sum=1.0), puts 0.25 on each of
// right / right2 / bottom-left / bottom, and zeros bottom-right +
// bottom2 — still recognisably "Atkinson-shape" but rebalanced for
// multi-colour palettes in OKLab.
//
//      * 0.25 0.25
// 0.25 0.25 0
//      0
//
constexpr std::array atkinson_kernel = {
    DiffusionEntry{ 1, 0, 0.25f},   // c1: right
    DiffusionEntry{ 2, 0, 0.25f},   // c2: right2
    DiffusionEntry{-1, 1, 0.25f},   // c3: bottom-left
    DiffusionEntry{ 0, 1, 0.25f},   // c4: bottom
    DiffusionEntry{ 1, 1, 0.00f},   // c5: bottom-right (was 1/8)
    DiffusionEntry{ 0, 2, 0.00f},   // c6: bottom2     (was 1/8)
};

// Sierra Lite — Frankie Sierra 1990 "Filter Lite". Kernel-shape sweep
// (576 combos × 5 images × 2 modes) found apparent +0.7 dB winners
// at sum≈0.925, but disambiguation showed they were a strength effect
// in disguise: normalised-winner shape × strength sweep matches
// canonical-shape × strength sweep within 0.02 dB. Strength tuning,
// not kernel reshaping, is the real lever — see dither_tuning.cpp
// where sierra-lite's strength was raised from 0.85 to 0.90.
constexpr std::array sierra_lite_kernel = {
    DiffusionEntry{ 1, 0, 2.0f / 4.0f},
    DiffusionEntry{-1, 1, 1.0f / 4.0f},
    DiffusionEntry{ 0, 1, 1.0f / 4.0f},
};

// Stucki: 12-cell wide kernel. Original 1981 weights (8 4 2 4 8 4 2 / 1 2 4
// 2 1 over /42, sum=1.0) ranked 2155/6561 in our 8-axis × 3-step OKLab
// sweep — mean PSNR 33.30 dB vs the optimum's 33.72 dB (+0.41 dB win).
//
// The OKLab-tuned shape redistributes weights to push more energy
// straight down (centre column +0.05) and to the immediate right
// (+0.05), away from the row-2 corners (which go slightly negative).
// 12-cell sum still ≈ 1.0 (energy preserving). Mirror-symmetric: cells
// (1,1)/(-1,1) share weight 0.095, (2,1)/(-2,1) share 0.048, etc.
//
//                  *      0.240  0.045
//   -0.026 0.048  0.240  0.048 -0.026
//   -0.026 0.048  0.145  0.048 -0.026 ?? row 2 — wait, see code below
//
constexpr std::array stucki_kernel = {
    DiffusionEntry{ 1, 0,  0.240f},   // c1: right        (was 8/42 ≈ 0.190)
    DiffusionEntry{ 2, 0,  0.045f},   // c2: right2       (was 4/42 ≈ 0.095)
    DiffusionEntry{-2, 1,  0.048f},   // c3: row1 |±2|    (was 2/42 ≈ 0.048)
    DiffusionEntry{-1, 1,  0.095f},   // c4: row1 |±1|    (was 4/42 ≈ 0.095)
    DiffusionEntry{ 0, 1,  0.240f},   // c5: row1 centre  (was 8/42 ≈ 0.190)
    DiffusionEntry{ 1, 1,  0.095f},   // c4 mirror
    DiffusionEntry{ 2, 1,  0.048f},   // c3 mirror
    DiffusionEntry{-2, 2, -0.026f},   // c6: row2 |±2|    (was 1/42 ≈ 0.024) — NEGATIVE
    DiffusionEntry{-1, 2,  0.048f},   // c7: row2 |±1|    (was 2/42 ≈ 0.048)
    DiffusionEntry{ 0, 2,  0.145f},   // c8: row2 centre  (was 4/42 ≈ 0.095)
    DiffusionEntry{ 1, 2,  0.048f},   // c7 mirror
    DiffusionEntry{ 2, 2, -0.026f},   // c6 mirror
};

// Jarvis-Judice-Ninke: 12-cell wide kernel, originally 1976 weights
// (7/5/3/5/7 — 1/3/5/3/1 over /48, sum=1.0). Rank 1555/6561 in our
// 6561-kernel × 15-image OKLab sweep — mean PSNR 33.26 dB vs the
// optimum's 33.59 dB (+0.33 dB perceptual gain).
//
// OKLab-tuned shape rebalances toward the immediate right and row-1
// |±2| corners, lightens row-1/2 |±1| and the row-2 centre, and pushes
// row-2 corners slightly negative — same family pattern as the Atkinson
// and Stucki re-tunes. 12-cell sum ≈ 0.90 (gentle under-distribution).
//
constexpr std::array jarvis_kernel = {
    DiffusionEntry{ 1, 0,  0.196f},   // c1: right          (was 7/48 ≈ 0.146)
    DiffusionEntry{ 2, 0,  0.104f},   // c2: right2         (≈ canonical 5/48)
    DiffusionEntry{-2, 1,  0.113f},   // c3: row1 |±2|      (was 3/48 ≈ 0.063)
    DiffusionEntry{-1, 1,  0.054f},   // c4: row1 |±1|      (was 5/48 ≈ 0.104)
    DiffusionEntry{ 0, 1,  0.146f},   // c5: row1 centre    (≈ canonical 7/48)
    DiffusionEntry{ 1, 1,  0.054f},   // c4 mirror
    DiffusionEntry{ 2, 1,  0.113f},   // c3 mirror
    DiffusionEntry{-2, 2, -0.029f},   // c6: row2 |±2|      (was 1/48 ≈ 0.021) — NEGATIVE
    DiffusionEntry{-1, 2,  0.062f},   // c7: row2 |±1|      (≈ canonical 3/48)
    DiffusionEntry{ 0, 2,  0.054f},   // c8: row2 centre    (was 5/48 ≈ 0.104)
    DiffusionEntry{ 1, 2,  0.062f},   // c7 mirror
    DiffusionEntry{ 2, 2, -0.029f},   // c6 mirror
};

DitherResult apply_error_diffusion(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val,
    bool serpentine,
    std::span<const DiffusionEntry> kernel) {

    auto w = image.width();
    auto h = image.height();

    float ec = error_clamp_val;

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    // Precompute image in OKLab
    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);
        }
    }

    // Error buffer (OKLab)
    std::vector<OKLab> error_buf(w * h);

    for (std::size_t y = 0; y < h; ++y) {
        bool reverse = serpentine && (y % 2 == 1);

        for (std::size_t step = 0; step < w; ++step) {
            std::size_t x = reverse ? (w - 1 - step) : step;
            auto buf_idx = y * w + x;

            // Add accumulated error to the original pixel
            auto clamped_error = oklab_clamp(error_buf[buf_idx], ec);
            auto adjusted = oklab_add(image_lab[buf_idx], clamped_error);

            // Find nearest palette color
            auto [idx, chosen_lab, dist_sq] =
                find_nearest_oklab(adjusted, palette_lab);
            result.indices[buf_idx] = static_cast<std::uint8_t>(idx);
            result.total_error += dist_sq;

            // Compute quantization error and distribute
            auto quant_error =
                oklab_scale(oklab_sub(adjusted, chosen_lab), strength);

            for (auto& [kdx, kdy, weight] : kernel) {
                int actual_dx = reverse ? -kdx : kdx;
                auto nx = static_cast<int>(x) + actual_dx;
                auto ny = static_cast<int>(y) + kdy;

                if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                    ny >= 0 && static_cast<std::size_t>(ny) < h) {
                    auto nidx = static_cast<std::size_t>(ny) * w +
                                static_cast<std::size_t>(nx);
                    error_buf[nidx] = oklab_clamp(
                        oklab_add(error_buf[nidx],
                                  oklab_scale(quant_error, weight)),
                        ec);
                }
            }
        }
    }

    return result;
}

// ===========================================================================
// Ostromoukhov variable-coefficient error diffusion.
// Uses 3 coefficients that vary with the "threshold" level — the fractional
// position between the two nearest palette colors.  The coefficients are
// from Ostromoukhov's 2001 paper, simplified to a smooth interpolation
// between three regimes (near-black, midtone, near-white).
// ===========================================================================

// ===========================================================================
// Gilbert-curve error diffusion
//
// Instead of scanning row-by-row, walk a generalized Hilbert (Červený's
// "Gilbert") space-filling curve that covers any W×H rectangle. At each
// step, quantize the current pixel and diffuse the error to the NEXT
// few pixels on the curve. Because the curve has O(1) spatial locality
// everywhere, the error propagates along a locally-coherent path and
// the directional hatching artifacts you see with raster-order FS on
// flat regions disappear.
//
// Curve generation follows Červený's recursive subdivision
// (https://github.com/jakubcerveny/gilbert).  We store the visit order
// as a flat list of (x, y) pairs, then walk it linearly for diffusion.
// ===========================================================================

namespace {

inline int sgn(int v) noexcept { return (v > 0) - (v < 0); }

// Recursive subdivision. (x, y) is the starting cell; a = (ax, ay) is the
// major axis vector; b = (bx, by) is the minor axis vector. Each emitted
// coordinate is pushed onto `out`.
void gilbert_recurse(int x, int y, int ax, int ay, int bx, int by,
                     std::vector<std::pair<int, int>>& out);

void gilbert_emit_row(int x, int y, int ax, int ay,
                      std::vector<std::pair<int, int>>& out) {
    int dax = sgn(ax), day = sgn(ay);
    int len = std::abs(ax) + std::abs(ay);
    for (int i = 0; i < len; ++i) {
        out.emplace_back(x, y);
        x += dax; y += day;
    }
}

void gilbert_recurse(int x, int y, int ax, int ay, int bx, int by,
                     std::vector<std::pair<int, int>>& out) {
    int w = std::abs(ax) + std::abs(ay);
    int h = std::abs(bx) + std::abs(by);

    if (h == 0) return;
    if (h == 1) { gilbert_emit_row(x, y, ax, ay, out); return; }
    if (w == 1) { gilbert_emit_row(x, y, bx, by, out); return; }

    int dax = sgn(ax), day = sgn(ay);
    int dbx = sgn(bx), dby = sgn(by);

    int ax2 = ax / 2, ay2 = ay / 2;
    int bx2 = bx / 2, by2 = by / 2;
    int w2 = std::abs(ax2) + std::abs(ay2);
    int h2 = std::abs(bx2) + std::abs(by2);

    if (2 * w > 3 * h) {
        // Prefer horizontal split
        if ((w2 % 2) && w > 2) { ax2 += dax; ay2 += day; }
        gilbert_recurse(x, y, ax2, ay2, bx, by, out);
        gilbert_recurse(x + ax2, y + ay2, ax - ax2, ay - ay2, bx, by, out);
    } else {
        // Prefer vertical split
        if ((h2 % 2) && h > 2) { bx2 += dbx; by2 += dby; }
        gilbert_recurse(x, y, bx2, by2, ax2, ay2, out);
        gilbert_recurse(x + bx2, y + by2, ax, ay,
                        bx - bx2, by - by2, out);
        gilbert_recurse(x + (ax - dax) + (bx2 - dbx),
                        y + (ay - day) + (by2 - dby),
                        -bx2, -by2, -(ax - ax2), -(ay - ay2), out);
    }
}

std::vector<std::pair<int, int>> gilbert_curve(int w, int h) {
    std::vector<std::pair<int, int>> out;
    out.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    if (w >= h) gilbert_recurse(0, 0, w, 0, 0, h, out);
    else        gilbert_recurse(0, 0, 0, h, w, 0, out);
    return out;
}

} // namespace

DitherResult apply_gilbert(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val) {

    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    auto curve = gilbert_curve(static_cast<int>(w), static_cast<int>(h));

    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    // Flatness map: 1.0 in smooth regions, 0.0 at edges. Error diffusion
    // on a space-filling curve tends to produce curve-following patterns
    // in flat regions (because the error doesn't have anywhere to "escape"
    // laterally). Adding blue-noise perturbation in flat regions breaks
    // those patterns without disturbing edges where accuracy matters most.
    //
    // Gradient is the sum of |L - neighbor.L| over 4-neighbors, in the
    // OKLab L channel only (banding is primarily a luminance phenomenon).
    // Threshold 0.03 (≈ 8 8-bit levels) — above this, the region is
    // treated as edge and gets no noise injection.
    std::vector<float> flatness(w * h, 1.0f);
    {
        constexpr float grad_thresh = 0.015f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float L = image_lab[y * w + x].L;
                float g = 0.0f;
                if (x > 0)     g += std::abs(L - image_lab[y * w + (x - 1)].L);
                if (x + 1 < w) g += std::abs(L - image_lab[y * w + (x + 1)].L);
                if (y > 0)     g += std::abs(L - image_lab[(y - 1) * w + x].L);
                if (y + 1 < h) g += std::abs(L - image_lab[(y + 1) * w + x].L);
                float f = 1.0f - std::min(g / grad_thresh, 1.0f);
                flatness[y * w + x] = f;
            }
        }
    }

    // Blue-noise amplitude scales with palette spacing. For a 32-color
    // palette the average OKLab nearest-neighbor distance is ~0.05; we
    // inject up to ±(amp * 0.5) of perturbation in flat regions, enough
    // to shift the nearest-color decision occasionally but not dominate.
    float noise_amp = 0.0f;
    if (palette_lab.size() >= 2) {
        // Rough average nearest-neighbor distance
        float total_nn = 0.0f;
        for (std::size_t i = 0; i < palette_lab.size(); ++i) {
            float best = std::numeric_limits<float>::max();
            for (std::size_t j = 0; j < palette_lab.size(); ++j) {
                if (i == j) continue;
                float dL = palette_lab[i].L - palette_lab[j].L;
                float da = palette_lab[i].a - palette_lab[j].a;
                float db = palette_lab[i].b - palette_lab[j].b;
                float d = dL * dL + da * da + db * db;
                if (d < best) best = d;
            }
            total_nn += std::sqrt(best);
        }
        noise_amp = 0.15f * (total_nn / static_cast<float>(palette_lab.size()));
    }

    // Error accumulator per pixel index
    std::vector<OKLab> error_buf(w * h);

    // Diffusion weights along the curve: most of the error goes to the
    // immediate next cell (spatially adjacent on the curve), with a small
    // tail to a few more. FS's pyramidal weights assume a 2D kernel shape,
    // which is a poor fit for 1D curve diffusion. Biasing toward the
    // next cell keeps the error local (where it belongs) and avoids
    // spreading error across distant, unrelated pixels when the curve
    // loops around.
    //
    // 12/16 + 3/16 + 1/16 = 16/16 = 1.0 (same total as FS).
    constexpr std::array<float, 3> weights = {
        12.0f / 16.0f, 3.0f / 16.0f, 1.0f / 16.0f,
    };

    float ec = error_clamp_val;

    for (std::size_t i = 0; i < curve.size(); ++i) {
        auto [x, y] = curve[i];
        auto idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x);

        auto clamped = oklab_clamp(error_buf[idx], ec);
        auto target = oklab_add(image_lab[idx], clamped);

        // Blue-noise perturbation in flat regions only. The noise is
        // applied to the NEAREST-COLOR LOOKUP target but NOT to the
        // propagated error — otherwise noise accumulates along the
        // curve instead of cancelling out. This is the key insight from
        // nQuant's GilbertCurve + BlueNoise combination.
        auto lookup = target;
        if (noise_amp > 0.0f && flatness[idx] > 0.0f) {
            auto ux = static_cast<std::size_t>(x) & 63;
            auto uy = static_cast<std::size_t>(y) & 63;
            float n = blue_noise_mat[uy][ux] * 2.0f * flatness[idx] * noise_amp;
            lookup.L += n;
            lookup.a += n;
            lookup.b += n;
        }

        // Find nearest palette entry (using perturbed lookup)
        float best_d = std::numeric_limits<float>::max();
        std::size_t best_k = 0;
        OKLab best_lab{};
        for (std::size_t k = 0; k < palette_lab.size(); ++k) {
            float dL = lookup.L - palette_lab[k].L;
            float da = lookup.a - palette_lab[k].a;
            float db = lookup.b - palette_lab[k].b;
            float d = dL * dL + da * da + db * db;
            if (d < best_d) { best_d = d; best_k = k; best_lab = palette_lab[k]; }
        }
        result.indices[idx] = static_cast<std::uint8_t>(best_k);
        result.total_error += best_d;

        // Error propagation uses the UNPERTURBED target, so blue noise
        // doesn't compound through the diffusion chain.
        auto err = oklab_sub(target, best_lab);
        auto scaled = oklab_scale(err, strength);

        // Distribute to next 4 cells along the curve
        for (std::size_t k = 0; k < weights.size() && i + 1 + k < curve.size(); ++k) {
            auto [nx, ny] = curve[i + 1 + k];
            auto nidx = static_cast<std::size_t>(ny) * w +
                        static_cast<std::size_t>(nx);
            auto w_e = oklab_scale(scaled, weights[k]);
            error_buf[nidx] = oklab_clamp(oklab_add(error_buf[nidx], w_e), ec);
        }
    }

    return result;
}

// ===========================================================================
// Riemersma dither — Thiadmer Riemersma 1998
// (https://www.compuphase.com/riemer.htm)
//
// Walks the same Gilbert/Hilbert space-filling curve as apply_gilbert but
// uses Riemersma's canonical exponential-decay error queue: the last 16
// pixels' errors all influence the current target, weighted so the most
// recent contributes most and the 16-back ago has weight 1/16.
//
// Difference from apply_gilbert: gilbert spreads error to the NEXT few
// cells with FS-like fixed weights; Riemersma propagates BACKWARDS by
// reading the queue of past errors at the current cell. Mathematically
// equivalent in steady-state but Riemersma gives a softer, more
// painterly result on smooth gradients.
// ===========================================================================

DitherResult apply_riemersma(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val) {

    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    auto curve = gilbert_curve(static_cast<int>(w), static_cast<int>(h));

    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    // Riemersma's canonical queue: 16 entries with exponential decay.
    // ratio = (1/16)^(1/15) so weight_i = ratio^(QSIZE-1-i) gives oldest
    // entry weight = 1/16, newest entry weight = 1.0. Sum is normalised
    // implicitly by error propagation — accumulated error converges.
    constexpr std::size_t QSIZE = 16;
    std::array<OKLab, QSIZE> queue{};  // ring buffer of last QSIZE errors
    std::array<float, QSIZE> weights{};
    {
        const float ratio = std::pow(1.0f / 16.0f, 1.0f / 15.0f);
        float w_acc = 1.0f;
        for (std::size_t i = QSIZE; i-- > 0; ) {
            weights[i] = w_acc;
            w_acc *= ratio;
        }
        // Normalise so sum = 1.
        float total = 0.0f;
        for (float wt : weights) total += wt;
        for (float& wt : weights) wt /= total;
    }
    std::size_t qhead = 0;  // next slot to overwrite

    float ec = error_clamp_val;

    for (std::size_t i = 0; i < curve.size(); ++i) {
        auto [x, y] = curve[i];
        auto idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x);

        // Sum the queue with exponential weights — newest at qhead-1.
        OKLab carry{};
        for (std::size_t k = 0; k < QSIZE; ++k) {
            std::size_t age = (qhead + QSIZE - 1 - k) % QSIZE;
            carry = oklab_add(carry, oklab_scale(queue[age], weights[k]));
        }
        carry = oklab_clamp(carry, ec);
        auto target = oklab_add(image_lab[idx], carry);

        // Find nearest palette entry.
        float best_d = std::numeric_limits<float>::max();
        std::size_t best_k = 0;
        OKLab best_lab{};
        for (std::size_t k = 0; k < palette_lab.size(); ++k) {
            float dL = target.L - palette_lab[k].L;
            float da = target.a - palette_lab[k].a;
            float db = target.b - palette_lab[k].b;
            float d = dL * dL + da * da + db * db;
            if (d < best_d) { best_d = d; best_k = k; best_lab = palette_lab[k]; }
        }

        result.indices[idx] = static_cast<std::uint8_t>(best_k);

        // Push new error onto queue, dropping oldest.
        auto err = oklab_sub(target, best_lab);
        queue[qhead] = oklab_scale(err, strength);
        qhead = (qhead + 1) % QSIZE;

        float dL = image_lab[idx].L - best_lab.L;
        float da = image_lab[idx].a - best_lab.a;
        float db = image_lab[idx].b - best_lab.b;
        result.total_error += dL * dL + da * da + db * db;
    }

    return result;
}

// ===========================================================================
// Yliluoma / Knoll palette-aware pattern dither
// (https://bisqwit.iki.fi/story/howto/dither/jy/)
//
// At each pixel, *devise a mixing plan* of 64 palette indices whose
// average colour matches the target. Sort by luma, then index into the
// plan with the standard 8×8 Bayer threshold. Result: every pixel's
// output is one of 64 colours pre-selected to make the right *averaged*
// colour at viewing distance — far better than single-threshold ordered
// dither when the palette is small (CGA, EHB, 4–16 colours).
//
// Greedy plan construction (Yliluoma method 1): at step k, try each
// palette colour, pick the one whose addition to the plan minimises
// distance from target. O(N×P) per pixel.
// ===========================================================================

} // namespace (anon closes — the next two are public API)

// Forward decls so pick_yliluoma_family_index can dispatch.
std::uint8_t pick_yliluoma_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y,
    bool mode2, float strength);

std::uint8_t pick_yliluoma_family_index(
    Method method,
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y, float strength) {

    switch (method) {
    case Method::opt_checker:
        return pick_opt_checker_index(target, palette_lab, x, y, strength);
    case Method::opt_line:
        return pick_opt_line_index(target, palette_lab, x, y, strength);
    case Method::opt_line_checker:
        return pick_opt_line_checker_index(target, palette_lab, x, y, strength);
    case Method::knoll:
        return pick_knoll_index(target, palette_lab, x, y, strength);
    case Method::tri_tone:
        return pick_tri_tone_index(target, palette_lab, x, y, strength);
    case Method::yliluoma1:
        return pick_yliluoma1_index(target, palette_lab, x, y, strength);
    case Method::yliluoma2:
        return pick_yliluoma_index(target, palette_lab, x, y, true, strength);
    default:  // yliluoma (alg 2 greedy) — also fallback for non-family methods
        return pick_yliluoma_index(target, palette_lab, x, y, false, strength);
    }
}

// Per-pixel Yliluoma quantizer — exposed so callers can use it on a
// per-row basis (e.g., copper.cpp's CAP loop) with the absolute y index
// for Bayer rotation. Bypassing dither::apply on a 1-row sub-image was
// the root of the "vertical line bias" bug — bayer8[0][x%8] is the
// same row of thresholds for every scanline.
std::uint8_t pick_yliluoma_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y,
    bool mode2, float strength) {

    constexpr std::size_t PLAN_SIZE = 64;
    const std::size_t P = palette_lab.size();
    if (P == 0) return 0;

    std::array<std::size_t, PLAN_SIZE> plan{};
    std::array<float, PLAN_SIZE> plan_luma{};
    std::array<std::size_t, PLAN_SIZE> sorted{};

    OKLab sum{};
    for (std::size_t step = 0; step < PLAN_SIZE; ++step) {
        float best_err = std::numeric_limits<float>::max();
        std::size_t best_k = 0;
        float n_inv = 1.0f / static_cast<float>(step + 1);
        for (std::size_t k = 0; k < P; ++k) {
            OKLab avg = {
                (sum.L + palette_lab[k].L) * n_inv,
                (sum.a + palette_lab[k].a) * n_inv,
                (sum.b + palette_lab[k].b) * n_inv,
            };
            float dL = avg.L - target.L;
            float da = avg.a - target.a;
            float db = avg.b - target.b;
            float err = mode2
                ? (4.0f * dL * dL + da * da + db * db)
                : (dL * dL + da * da + db * db);
            if (err < best_err) { best_err = err; best_k = k; }
        }
        plan[step] = best_k;
        sum = oklab_add(sum, palette_lab[best_k]);
    }

    for (std::size_t i = 0; i < PLAN_SIZE; ++i) {
        plan_luma[i] = palette_lab[plan[i]].L;
        sorted[i] = i;
    }
    for (std::size_t i = 1; i < PLAN_SIZE; ++i) {
        std::size_t j = i;
        while (j > 0 && plan_luma[sorted[j - 1]] > plan_luma[sorted[j]]) {
            std::swap(sorted[j], sorted[j - 1]);
            --j;
        }
    }

    int b = static_cast<int>((bayer8[y % 8][x % 8] + 0.5f) * 64.0f);
    if (b < 0) b = 0;
    if (b >= static_cast<int>(PLAN_SIZE)) b = static_cast<int>(PLAN_SIZE) - 1;
    int median = static_cast<int>(PLAN_SIZE) / 2;
    int adjusted = static_cast<int>(std::round(static_cast<float>(median) + static_cast<float>(b - median) * strength));
    if (adjusted < 0) adjusted = 0;
    if (adjusted >= static_cast<int>(PLAN_SIZE)) adjusted = static_cast<int>(PLAN_SIZE) - 1;
    return static_cast<std::uint8_t>(plan[sorted[static_cast<std::size_t>(adjusted)]]);
}

// Per-pixel Knoll pattern dither — Thomas Knoll's Photoshop "Pattern" mode,
// US patent 6,606,166 (filed 2001, expired 2019). Same greedy mixing-plan
// construction as Yliluoma method 1 / 2 but with the patent's original
// parameters: N=16 plan size laid out on a 4×4 Bayer matrix. The shorter
// plan is faster than Yliluoma's 8×8/N=64 setup and gives the
// characteristic "Photoshop pattern" look.
std::uint8_t pick_knoll_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y,
    float strength) {

    constexpr std::size_t PLAN_SIZE = 16;
    const std::size_t P = palette_lab.size();
    if (P == 0) return 0;

    std::array<std::size_t, PLAN_SIZE> plan{};
    std::array<float, PLAN_SIZE> plan_luma{};
    std::array<std::size_t, PLAN_SIZE> sorted{};

    color_space::OKLab sum{};
    for (std::size_t step = 0; step < PLAN_SIZE; ++step) {
        float best_err = std::numeric_limits<float>::max();
        std::size_t best_k = 0;
        float n_inv = 1.0f / static_cast<float>(step + 1);
        for (std::size_t k = 0; k < P; ++k) {
            color_space::OKLab avg = {
                (sum.L + palette_lab[k].L) * n_inv,
                (sum.a + palette_lab[k].a) * n_inv,
                (sum.b + palette_lab[k].b) * n_inv,
            };
            float dL = avg.L - target.L;
            float da = avg.a - target.a;
            float db = avg.b - target.b;
            float err = dL * dL + da * da + db * db;
            if (err < best_err) { best_err = err; best_k = k; }
        }
        plan[step] = best_k;
        sum.L += palette_lab[best_k].L;
        sum.a += palette_lab[best_k].a;
        sum.b += palette_lab[best_k].b;
    }

    // Sort plan entries by luma so Bayer's "low" threshold maps to darker.
    for (std::size_t i = 0; i < PLAN_SIZE; ++i) {
        plan_luma[i] = palette_lab[plan[i]].L;
        sorted[i] = i;
    }
    for (std::size_t i = 1; i < PLAN_SIZE; ++i) {
        std::size_t j = i;
        while (j > 0 && plan_luma[sorted[j - 1]] > plan_luma[sorted[j]]) {
            std::swap(sorted[j], sorted[j - 1]);
            --j;
        }
    }

    // Bayer4 threshold ∈ [-0.5, 0.5) → b ∈ [0, 16) indexes the 16-step plan.
    int b = static_cast<int>((bayer4[y % 4][x % 4] + 0.5f) * 16.0f);
    if (b < 0) b = 0;
    if (b >= static_cast<int>(PLAN_SIZE)) b = static_cast<int>(PLAN_SIZE) - 1;
    int median = static_cast<int>(PLAN_SIZE) / 2;
    int adjusted = static_cast<int>(std::round(static_cast<float>(median) + static_cast<float>(b - median) * strength));
    if (adjusted < 0) adjusted = 0;
    if (adjusted >= static_cast<int>(PLAN_SIZE)) adjusted = static_cast<int>(PLAN_SIZE) - 1;
    return static_cast<std::uint8_t>(plan[sorted[static_cast<std::size_t>(adjusted)]]);
}

// Tri-tone — Yliluoma's 2×2 / 3-colour preset. Greedy plan of 4 entries
// (palette repeats allowed) sorted by luma, indexed by the standard
// Bayer 2×2 phase. When the 4-step plan picks the same colour twice,
// you naturally get the "one at 50% + two at 25%" pattern Yliluoma
// describes.
std::uint8_t pick_tri_tone_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y, float strength) {

    constexpr std::size_t PLAN_SIZE = 4;
    const std::size_t P = palette_lab.size();
    if (P == 0) return 0;

    std::array<std::size_t, PLAN_SIZE> plan{};
    std::array<float, PLAN_SIZE> plan_luma{};
    std::array<std::size_t, PLAN_SIZE> sorted{};

    color_space::OKLab sum{};
    for (std::size_t step = 0; step < PLAN_SIZE; ++step) {
        float best_err = std::numeric_limits<float>::max();
        std::size_t best_k = 0;
        float n_inv = 1.0f / static_cast<float>(step + 1);
        for (std::size_t k = 0; k < P; ++k) {
            color_space::OKLab avg = {
                (sum.L + palette_lab[k].L) * n_inv,
                (sum.a + palette_lab[k].a) * n_inv,
                (sum.b + palette_lab[k].b) * n_inv,
            };
            float dL = avg.L - target.L;
            float da = avg.a - target.a;
            float db = avg.b - target.b;
            float err = dL * dL + da * da + db * db;
            if (err < best_err) { best_err = err; best_k = k; }
        }
        plan[step] = best_k;
        sum.L += palette_lab[best_k].L;
        sum.a += palette_lab[best_k].a;
        sum.b += palette_lab[best_k].b;
    }

    for (std::size_t i = 0; i < PLAN_SIZE; ++i) {
        plan_luma[i] = palette_lab[plan[i]].L;
        sorted[i] = i;
    }
    for (std::size_t i = 1; i < PLAN_SIZE; ++i) {
        std::size_t j = i;
        while (j > 0 && plan_luma[sorted[j - 1]] > plan_luma[sorted[j]]) {
            std::swap(sorted[j], sorted[j - 1]);
            --j;
        }
    }

    int b = static_cast<int>((bayer2[y % 2][x % 2] + 0.5f) * 4.0f);
    if (b < 0) b = 0;
    if (b >= static_cast<int>(PLAN_SIZE)) b = static_cast<int>(PLAN_SIZE) - 1;
    int median = static_cast<int>(PLAN_SIZE) / 2;
    int adjusted = static_cast<int>(std::round(static_cast<float>(median) + static_cast<float>(b - median) * strength));
    if (adjusted < 0) adjusted = 0;
    if (adjusted >= static_cast<int>(PLAN_SIZE)) adjusted = static_cast<int>(PLAN_SIZE) - 1;
    return static_cast<std::uint8_t>(plan[sorted[static_cast<std::size_t>(adjusted)]]);
}

// Yliluoma Algorithm 1 — exhaustive (i, j, ratio) search. For each
// unique palette pair (i ≤ j) and each mixing ratio r ∈ {1..N-1} on a
// 4×4 Bayer cell (N=16 ratio levels), compute avg = r/N · p[i] +
// (N-r)/N · p[j] in OKLab and pick the (i, j, r) minimising distance
// to target. Per Yliluoma's "Improvement to Algorithm 1", a
// luminance-difference cutoff trims the pair list — pairs with
// ΔL > strength·0.6 are skipped, which kills millions of unhelpful
// far-apart pairs and keeps the search tractable on 32+ colour
// palettes. Output for the current pixel: bayer threshold ∈ [0, N) is
// compared to r — below r → palette[i], else palette[j].
std::uint8_t pick_yliluoma1_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y, float strength) {

    constexpr int N = 16;  // 4×4 Bayer cell area
    const std::size_t P = palette_lab.size();
    if (P == 0) return 0;
    if (P == 1) return 0;

    // Luma cutoff scales linearly with strength up to a moderate cap.
    // At strength=0 only same-colour "pairs" (i==j) survive → nearest-
    // colour, no dither. At strength=1 cutoff is 0.3 in OKLab L — wide
    // enough for genuine pair averaging but tight enough to keep the
    // search from picking visually-correct-on-average but per-pixel-
    // extreme pairs (the failure mode of unbounded Algorithm 1).
    float s = std::clamp(strength, 0.0f, 1.0f);
    float luma_cutoff = s * 0.30f;

    float best_err = std::numeric_limits<float>::max();
    std::size_t best_i = 0, best_j = 0;
    int best_r = N / 2;
    for (std::size_t i = 0; i < P; ++i) {
        for (std::size_t j = i; j < P; ++j) {
            float dl = std::abs(palette_lab[i].L - palette_lab[j].L);
            if (dl > luma_cutoff) continue;
            for (int r = 0; r <= N; ++r) {
                float w_i = static_cast<float>(r) / static_cast<float>(N);
                float w_j = 1.0f - w_i;
                float aL = palette_lab[i].L * w_i + palette_lab[j].L * w_j;
                float aa = palette_lab[i].a * w_i + palette_lab[j].a * w_j;
                float ab = palette_lab[i].b * w_i + palette_lab[j].b * w_j;
                float dL = aL - target.L;
                float da = aa - target.a;
                float db = ab - target.b;
                float err = dL * dL + da * da + db * db;
                if (err < best_err) {
                    best_err = err; best_i = i; best_j = j; best_r = r;
                }
            }
        }
    }

    // Bayer4 threshold ∈ [-0.5, 0.5) → b ∈ [0, N) compared against r.
    int b = static_cast<int>((bayer4[y % 4][x % 4] + 0.5f) * static_cast<float>(N));
    if (b < 0) b = 0;
    if (b >= N) b = N - 1;
    return static_cast<std::uint8_t>((b < best_r) ? best_i : best_j);
}

bool is_yliluoma(Method method) {
    return method == Method::yliluoma || method == Method::yliluoma2 ||
           method == Method::opt_checker || method == Method::knoll ||
           method == Method::tri_tone || method == Method::yliluoma1 ||
           method == Method::opt_line || method == Method::opt_line_checker;
}

// Per-pixel optimal-pair quantizer for the 2×2 checker variant.
// Brute-force search over all palette pairs (i, j) — including i==j —
// picks the pair whose linear-OKLab AVERAGE most closely matches target.
// Output index = sorted-by-luma pair[(x+y) & 1]. The 2×2 checker reads
// well on CRTs because the phosphor + scanline blur averages adjacent
// pixels into the intended midtone with near-zero spatial frequency.
//
// O(P²) per pixel where P = palette size. For our typical 16/32-colour
// palettes this is hundreds of pairs per pixel, well under 100M ops on
// a 320×200 image. Cheap.
// Shared core for Optimal Checker / Line / Line-Checker: greedy
// Yliluoma N=2 pair search anchored on the nearest-colour, then pick
// `lo` (luma-sorted darker) or `hi` based on the per-method `phase` (0
// or 1). Strength controls a separation penalty in the partner search.
static std::uint8_t opt_pair_pick(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    int phase, float strength) {

    const std::size_t P = palette_lab.size();
    if (P == 0) return 0;
    if (P == 1) return 0;

    float s = std::clamp(strength, 0.0f, 1.0f);

    // Step 0: nearest-colour pick over the full palette. This anchors
    // the pair so step 1 can't run off into wildly distant colours —
    // unlike a free brute-force pair search, which at strength=1 would
    // happily pick (black, white) for a midgrey target.
    std::size_t A = 0;
    float best_a = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k < P; ++k) {
        float dL = palette_lab[k].L - target.L;
        float da = palette_lab[k].a - target.a;
        float db = palette_lab[k].b - target.b;
        float d = dL * dL + da * da + db * db;
        if (d < best_a) { best_a = d; A = k; }
    }

    // Strength=0 short-circuit — never call the algorithm "no dither"
    // when the user asks for none. Returns plain nearest-colour.
    if (s == 0.0f) return static_cast<std::uint8_t>(A);

    // Step 1: pick partner B such that (palette[A] + palette[B]) / 2
    // is closest to target. Yliluoma method 1 is greedy: B = A is a
    // valid candidate (gives baseline best_a) and naturally wins when
    // target is on-palette → no visible dither there. A separation
    // penalty modulated by strength biases the search toward closer
    // partners as strength drops, so the checker contrast tapers
    // smoothly toward zero rather than slamming to "no dither" at a
    // hard threshold.
    //
    //   sep_w(s) interpolates exponentially in [0.005, 5.0]:
    //     s=1.0 → 0.005 (mild — full Yliluoma 1 averaging)
    //     s=0.5 → 0.158
    //     s→0   → 5.0 (forces B == A)
    constexpr float MIN_W = 0.005f;
    constexpr float MAX_W = 5.0f;
    float sep_w = MIN_W * std::pow(MAX_W / MIN_W, 1.0f - s);

    std::size_t B = A;
    float best_b = best_a;  // baseline: pair (A, A), no improvement
    for (std::size_t k = 0; k < P; ++k) {
        if (k == A) continue;
        float aL = (palette_lab[A].L + palette_lab[k].L) * 0.5f;
        float aa = (palette_lab[A].a + palette_lab[k].a) * 0.5f;
        float ab = (palette_lab[A].b + palette_lab[k].b) * 0.5f;
        float dL = aL - target.L;
        float da = aa - target.a;
        float db = ab - target.b;
        float avg_err = dL * dL + da * da + db * db;
        float sL = palette_lab[A].L - palette_lab[k].L;
        float sa = palette_lab[A].a - palette_lab[k].a;
        float sb = palette_lab[A].b - palette_lab[k].b;
        float sep = sL * sL + sa * sa + sb * sb;
        float err = avg_err + sep_w * sep;
        if (err < best_b) { best_b = err; B = k; }
    }

    // If B collapsed back to A (no candidate beat the same-colour
    // baseline), no checker — return A for both phases.
    if (B == A) return static_cast<std::uint8_t>(A);

    // Sort by luma so the "low" phase always picks the darker.
    std::size_t lo = A, hi = B;
    if (palette_lab[hi].L < palette_lab[lo].L) std::swap(lo, hi);
    return static_cast<std::uint8_t>(phase ? hi : lo);
}

std::uint8_t pick_opt_checker_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y, float strength) {
    return opt_pair_pick(target, palette_lab,
                         static_cast<int>((x + y) & 1u), strength);
}

std::uint8_t pick_opt_line_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y, float strength) {
    (void)x;
    return opt_pair_pick(target, palette_lab,
                         static_cast<int>(y & 1u), strength);
}

std::uint8_t pick_opt_line_checker_index(
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y, float strength) {
    // 4-step greedy plan (tri-tone-style) with line_checker phase index:
    // line_checker_mat has 4 distinct thresholds per 2×2 cell (-0.35,
    // -0.15, +0.15, +0.35). Map to plan indices 0..3 by the threshold
    // ordering — line dominance (rows) plus subtle column variation
    // gives a 4-colour line-tinted pattern, not just a 2-tone line.
    constexpr std::size_t PLAN_SIZE = 4;
    const std::size_t P = palette_lab.size();
    if (P == 0) return 0;

    std::array<std::size_t, PLAN_SIZE> plan{};
    std::array<float, PLAN_SIZE> plan_luma{};
    std::array<std::size_t, PLAN_SIZE> sorted{};

    color_space::OKLab sum{};
    for (std::size_t step = 0; step < PLAN_SIZE; ++step) {
        float best_err = std::numeric_limits<float>::max();
        std::size_t best_k = 0;
        float n_inv = 1.0f / static_cast<float>(step + 1);
        for (std::size_t k = 0; k < P; ++k) {
            color_space::OKLab avg = {
                (sum.L + palette_lab[k].L) * n_inv,
                (sum.a + palette_lab[k].a) * n_inv,
                (sum.b + palette_lab[k].b) * n_inv,
            };
            float dL = avg.L - target.L;
            float da = avg.a - target.a;
            float db = avg.b - target.b;
            float err = dL * dL + da * da + db * db;
            if (err < best_err) { best_err = err; best_k = k; }
        }
        plan[step] = best_k;
        sum.L += palette_lab[best_k].L;
        sum.a += palette_lab[best_k].a;
        sum.b += palette_lab[best_k].b;
    }
    for (std::size_t i = 0; i < PLAN_SIZE; ++i) {
        plan_luma[i] = palette_lab[plan[i]].L;
        sorted[i] = i;
    }
    for (std::size_t i = 1; i < PLAN_SIZE; ++i) {
        std::size_t j = i;
        while (j > 0 && plan_luma[sorted[j - 1]] > plan_luma[sorted[j]]) {
            std::swap(sorted[j], sorted[j - 1]);
            --j;
        }
    }
    // Map line_checker threshold (-0.35..+0.35) → plan index 0..3.
    float thr = line_checker_mat[y % 2][x % 2];
    int b = static_cast<int>((thr + 0.5f) * static_cast<float>(PLAN_SIZE));
    if (b < 0) b = 0;
    if (b >= static_cast<int>(PLAN_SIZE)) b = static_cast<int>(PLAN_SIZE) - 1;
    int median = static_cast<int>(PLAN_SIZE) / 2;
    int adjusted = static_cast<int>(std::round(static_cast<float>(median) + static_cast<float>(b - median) * strength));
    if (adjusted < 0) adjusted = 0;
    if (adjusted >= static_cast<int>(PLAN_SIZE)) adjusted = static_cast<int>(PLAN_SIZE) - 1;
    return static_cast<std::uint8_t>(plan[sorted[static_cast<std::size_t>(adjusted)]]);
}

namespace { // reopen anon namespace for the apply_* helpers below

// `mode2` selects Yliluoma method 2: weights candidates by closeness to
// target's luma rather than its full OKLab vector during plan
// construction. Tends to produce smoother gradients with less colour
// drift on small palettes than method 1.
DitherResult apply_yliluoma(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength,
    bool mode2 = false) {

    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    // Plan size = Bayer matrix area (8×8 = 64).
    constexpr std::size_t PLAN_SIZE = 64;
    const std::size_t P = palette_lab.size();
    if (P == 0) return result;

    // Per-pixel scratch — reused across the loop.
    std::array<std::size_t, PLAN_SIZE> plan{};
    std::array<float, PLAN_SIZE> plan_luma{};
    std::array<std::size_t, PLAN_SIZE> sorted{};

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto target = color_space::linear_to_oklab(image[x, y]);

            // Build the plan greedily: at each step pick the palette
            // entry that minimises the average's distance from target.
            OKLab sum{};
            for (std::size_t step = 0; step < PLAN_SIZE; ++step) {
                float best_err = std::numeric_limits<float>::max();
                std::size_t best_k = 0;
                float n_inv = 1.0f / static_cast<float>(step + 1);
                for (std::size_t k = 0; k < P; ++k) {
                    OKLab avg = {
                        (sum.L + palette_lab[k].L) * n_inv,
                        (sum.a + palette_lab[k].a) * n_inv,
                        (sum.b + palette_lab[k].b) * n_inv,
                    };
                    float dL = avg.L - target.L;
                    float da = avg.a - target.a;
                    float db = avg.b - target.b;
                    // Method 1: full Lab distance.
                    // Method 2: weight luma 4× heavier than chroma — keeps
                    // gradients smooth at the cost of slightly worse hue
                    // matching, the canonical Yliluoma-2 trade-off.
                    float err = mode2
                        ? (4.0f * dL * dL + da * da + db * db)
                        : (dL * dL + da * da + db * db);
                    if (err < best_err) { best_err = err; best_k = k; }
                }
                plan[step] = best_k;
                sum = oklab_add(sum, palette_lab[best_k]);
            }

            // Sort plan entries by OKLab luma so Bayer's "low threshold"
            // → darker palette pick. Insertion sort, plan size = 64.
            for (std::size_t i = 0; i < PLAN_SIZE; ++i) {
                plan_luma[i] = palette_lab[plan[i]].L;
                sorted[i] = i;
            }
            for (std::size_t i = 1; i < PLAN_SIZE; ++i) {
                std::size_t j = i;
                while (j > 0 && plan_luma[sorted[j - 1]] > plan_luma[sorted[j]]) {
                    std::swap(sorted[j], sorted[j - 1]);
                    --j;
                }
            }

            // Bayer8 threshold gives 0..63, indexes the sorted plan.
            // Strength scales toward the *median* plan entry so a low
            // strength collapses to nearest-only.
            int b = static_cast<int>((bayer8[y % 8][x % 8] + 0.5f) * 64.0f);
            if (b < 0) b = 0;
            if (b >= static_cast<int>(PLAN_SIZE)) b = static_cast<int>(PLAN_SIZE) - 1;
            int median = static_cast<int>(PLAN_SIZE) / 2;
            int adjusted = static_cast<int>(std::round(static_cast<float>(median) + static_cast<float>(b - median) * strength));
            if (adjusted < 0) adjusted = 0;
            if (adjusted >= static_cast<int>(PLAN_SIZE)) adjusted = static_cast<int>(PLAN_SIZE) - 1;
            std::size_t pick = plan[sorted[static_cast<std::size_t>(adjusted)]];

            result.indices[y * w + x] = static_cast<std::uint8_t>(pick);
            float dL = target.L - palette_lab[pick].L;
            float da = target.a - palette_lab[pick].a;
            float db = target.b - palette_lab[pick].b;
            result.total_error += dL * dL + da * da + db * db;
        }
    }

    return result;
}

// ===========================================================================
// Structure-aware FS — Laplacian-modulated Floyd-Steinberg
// (https://github.com/dalpil/structure-aware-dithering)
//
// Plain FS produces "worm" artifacts in flat regions and blurs detail.
// This variant computes the source image's Laplacian (edge detector)
// and a sliding-window stddev, then biases the per-pixel dither
// threshold by K · Laplacian where K scales with local detail. Effect:
// in flat regions the bias is ~zero (FS behaves normally); near edges
// the bias pulls the quantization toward the source's local feature so
// detail is preserved instead of smeared by error diffusion.
//
// Adapted from dalpil's bi-level Python reference; here the bias is
// applied as an OKLab-L offset before nearest-pair selection so it
// works with N-colour palettes too.
// ===========================================================================

// Bias mode chooses how the per-pixel threshold is modulated.
enum class StructureBias { laplacian, contrast, zhoufang };

DitherResult apply_structure_fs(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val,
    bool serpentine,
    StructureBias bias_mode = StructureBias::laplacian) {

    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    // Source as OKLab.
    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    // Per-pixel structure signal. For Laplacian mode this is the 4-conn
    // discrete Laplacian of the OKLab L channel; for contrast mode it's
    // (local_max - local_min) on a 3×3 window; for zhoufang it's the
    // raw intensity (used as a multiplicative modulator on noise).
    std::vector<float> lap(w * h, 0.0f);
    if (bias_mode == StructureBias::laplacian) {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float L = image_lab[y * w + x].L;
                float Lx0 = (x > 0)     ? image_lab[y * w + (x - 1)].L : L;
                float Lx1 = (x + 1 < w) ? image_lab[y * w + (x + 1)].L : L;
                float Ly0 = (y > 0)     ? image_lab[(y - 1) * w + x].L : L;
                float Ly1 = (y + 1 < h) ? image_lab[(y + 1) * w + x].L : L;
                float v = 4.0f * L - Lx0 - Lx1 - Ly0 - Ly1;
                if (v < -0.5f) v = -0.5f;
                if (v > 0.5f)  v = 0.5f;
                lap[y * w + x] = v;
            }
        }
    } else if (bias_mode == StructureBias::contrast) {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float lo = 1.0f, hi = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto yy = static_cast<std::ptrdiff_t>(y) + dy;
                        auto xx = static_cast<std::ptrdiff_t>(x) + dx;
                        if (yy < 0 || yy >= static_cast<std::ptrdiff_t>(h)) continue;
                        if (xx < 0 || xx >= static_cast<std::ptrdiff_t>(w)) continue;
                        float L = image_lab[static_cast<std::size_t>(yy) * w +
                                            static_cast<std::size_t>(xx)].L;
                        if (L < lo) lo = L;
                        if (L > hi) hi = L;
                    }
                }
                float L = image_lab[y * w + x].L;
                float center = 0.5f * (lo + hi);
                lap[y * w + x] = (hi - lo) * ((L > center) ? 1.0f : -1.0f);
            }
        }
    } else { // zhoufang: signal = intensity-noise-amplitude (used below)
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float L = image_lab[y * w + x].L;
                // Zhou-Fang's modulator: noise amplitude peaks near the
                // problematic 25%/75% intensity bands where worms appear,
                // tapers off at extremes. Triangular weighting around 0.5.
                float dist = std::abs(L - 0.5f);
                lap[y * w + x] = (1.0f - 2.0f * dist) * 0.3f;
            }
        }
    }

    // 5×5 windowed stddev — tells us "is this a detailed region?".
    // We build it cheaply via separable mean + mean-of-squares.
    std::vector<float> stddev(w * h, 0.0f);
    constexpr int R = 2;  // 5×5 window
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float mean = 0.0f, sq = 0.0f;
            int count = 0;
            for (int dy = -R; dy <= R; ++dy) {
                for (int dx = -R; dx <= R; ++dx) {
                    auto yy = static_cast<std::ptrdiff_t>(y) + dy;
                    auto xx = static_cast<std::ptrdiff_t>(x) + dx;
                    if (yy < 0 || yy >= static_cast<std::ptrdiff_t>(h)) continue;
                    if (xx < 0 || xx >= static_cast<std::ptrdiff_t>(w)) continue;
                    float L = image_lab[static_cast<std::size_t>(yy) * w +
                                        static_cast<std::size_t>(xx)].L;
                    mean += L;
                    sq += L * L;
                    ++count;
                }
            }
            mean /= static_cast<float>(count);
            sq /= static_cast<float>(count);
            float var = sq - mean * mean;
            stddev[y * w + x] = (var > 0.0f) ? std::sqrt(var) : 0.0f;
        }
    }

    // Floyd-Steinberg-style error buffer.
    std::vector<OKLab> error_buf(w * h);
    constexpr std::array<DiffusionEntry, 4> kernel = {{
        {1, 0, 7.0f / 16.0f},
        {-1, 1, 3.0f / 16.0f},
        {0, 1, 5.0f / 16.0f},
        {1, 1, 1.0f / 16.0f},
    }};

    // Structure scale: how aggressively the Laplacian modulates the
    // dither. 0.4 chosen empirically — lower preserves more "FS look",
    // higher makes structure dominate.
    constexpr float K_SCALE = 0.4f;

    for (std::size_t y = 0; y < h; ++y) {
        bool reverse = serpentine && (y % 2 == 1);
        for (std::size_t step = 0; step < w; ++step) {
            std::size_t x = reverse ? (w - 1 - step) : step;
            auto idx = y * w + x;

            auto err = oklab_clamp(error_buf[idx], error_clamp_val);
            auto target = oklab_add(image_lab[idx], err);

            // Structure bias: shape depends on mode. Laplacian/contrast
            // scale by local stddev so flat regions stay neutral. ZF uses
            // its modulator as a noise amplitude — sample a hashed value.
            float bias = 0.0f;
            if (bias_mode == StructureBias::zhoufang) {
                auto seed = static_cast<std::uint32_t>(y * 65537u + x);
                seed = (seed ^ 61u) ^ (seed >> 16u);
                seed *= 9u;
                seed ^= seed >> 4u;
                seed *= 0x27d4eb2du;
                seed ^= seed >> 15u;
                float r = static_cast<float>(seed & 0xFFFFu) / 65536.0f - 0.5f;
                bias = r * lap[idx];  // lap[idx] holds the modulator here
            } else {
                bias = K_SCALE * lap[idx] * (stddev[idx] / 0.1f);
                if (bias < -0.2f) bias = -0.2f;
                if (bias > 0.2f)  bias = 0.2f;
            }
            target.L += bias;

            // Find nearest palette entry.
            float best_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            OKLab best_lab{};
            for (std::size_t k = 0; k < palette_lab.size(); ++k) {
                float dL = target.L - palette_lab[k].L;
                float da = target.a - palette_lab[k].a;
                float db = target.b - palette_lab[k].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best_k = k; best_lab = palette_lab[k]; }
            }

            result.indices[idx] = static_cast<std::uint8_t>(best_k);

            // Diffuse the *unbiased* error (don't compound the structure bias).
            auto e = oklab_sub(image_lab[idx], best_lab);
            auto scaled = oklab_scale(e, strength);
            for (auto& kr : kernel) {
                int dx = kr.dx;
                if (reverse) dx = -dx;
                auto nx = static_cast<std::ptrdiff_t>(x) + dx;
                auto ny = static_cast<std::ptrdiff_t>(y) + kr.dy;
                if (nx < 0 || nx >= static_cast<std::ptrdiff_t>(w)) continue;
                if (ny < 0 || ny >= static_cast<std::ptrdiff_t>(h)) continue;
                auto nidx = static_cast<std::size_t>(ny) * w +
                            static_cast<std::size_t>(nx);
                error_buf[nidx] = oklab_add(error_buf[nidx],
                                            oklab_scale(scaled, kr.weight));
            }

            float dL = image_lab[idx].L - best_lab.L;
            float da = image_lab[idx].a - best_lab.a;
            float db = image_lab[idx].b - best_lab.b;
            result.total_error += dL * dL + da * da + db * db;
        }
    }

    return result;
}

DitherResult apply_ostromoukhov(
    const Image& image,
    std::span<const OKLab> palette_lab,
    float strength, float error_clamp_val,
    bool serpentine) {

    auto w = image.width();
    auto h = image.height();

    float ec = error_clamp_val;

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    std::vector<OKLab> error_buf(w * h);

    for (std::size_t y = 0; y < h; ++y) {
        bool reverse = serpentine && (y % 2 == 1);
        for (std::size_t step = 0; step < w; ++step) {
            std::size_t x = reverse ? (w - 1 - step) : step;
            auto buf_idx = y * w + x;

            auto clamped_error = oklab_clamp(error_buf[buf_idx], ec);
            auto adjusted = oklab_add(image_lab[buf_idx], clamped_error);

            // Find nearest AND second-nearest to compute threshold level
            float best_d = std::numeric_limits<float>::max();
            float second_d = std::numeric_limits<float>::max();
            std::size_t best_k = 0;
            OKLab best_lab{};
            for (std::size_t k = 0; k < palette_lab.size(); ++k) {
                float dL = adjusted.L - palette_lab[k].L;
                float da = adjusted.a - palette_lab[k].a;
                float db = adjusted.b - palette_lab[k].b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) {
                    second_d = best_d;
                    best_d = d;
                    best_k = k;
                    best_lab = palette_lab[k];
                } else if (d < second_d) {
                    second_d = d;
                }
            }
            result.indices[buf_idx] = static_cast<std::uint8_t>(best_k);
            result.total_error += best_d;

            // Threshold: 0 = pixel is exactly on nearest, 1 = equidistant
            float threshold = 0.0f;
            if (second_d > 1e-12f) {
                float sqrt_best = std::sqrt(best_d);
                float sqrt_second = std::sqrt(second_d);
                threshold = sqrt_best / (sqrt_best + sqrt_second);
            }

            // Variable coefficients: at threshold=0 (near palette color),
            // distribute less error (pixel is well-served). At threshold=0.5
            // (equidistant), distribute more aggressively.
            // F-S base: right=7/16, bottom-left=3/16, bottom=5/16, bottom-right=1/16
            // Scale by threshold: more aggressive diffusion for uncertain pixels
            float scale = 0.6f + 0.8f * threshold;  // 0.6 to 1.4
            float w0 = (7.0f / 16.0f) * scale;
            float w1 = (3.0f / 16.0f) * scale;
            float w2 = (5.0f / 16.0f) * scale;
            float w3 = (1.0f / 16.0f) * scale;

            auto quant_error =
                oklab_scale(oklab_sub(adjusted, best_lab), strength);

            struct { int dx, dy; float wt; } entries[] = {
                {1, 0, w0}, {-1, 1, w1}, {0, 1, w2}, {1, 1, w3}};
            for (auto& [kdx, kdy, wt] : entries) {
                int actual_dx = reverse ? -kdx : kdx;
                auto nx = static_cast<int>(x) + actual_dx;
                auto ny = static_cast<int>(y) + kdy;
                if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                    ny >= 0 && static_cast<std::size_t>(ny) < h) {
                    auto nidx = static_cast<std::size_t>(ny) * w +
                                static_cast<std::size_t>(nx);
                    error_buf[nidx] = oklab_clamp(
                        oklab_add(error_buf[nidx],
                                  oklab_scale(quant_error, wt)),
                        ec);
                }
            }
        }
    }
    return result;
}

// ===========================================================================
// No-dither fallback (plain nearest-color mapping)
// ===========================================================================

DitherResult apply_none(const Image& image,
                        std::span<const OKLab> palette_lab) {
    auto w = image.width();
    auto h = image.height();

    DitherResult result;
    result.indices.resize(w * h);
    result.total_error = 0.0f;

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto pixel_lab = color_space::linear_to_oklab(image[x, y]);
            auto [idx, chosen, dist_sq] =
                find_nearest_oklab(pixel_lab, palette_lab);
            result.indices[y * w + x] = static_cast<std::uint8_t>(idx);
            result.total_error += dist_sq;
        }
    }

    return result;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

DitherResult apply(const Image& image,
                   std::span<const Color3f> palette,
                   const Settings& settings) {

    auto palette_lab = precompute_palette_lab(palette);
    std::span<const OKLab> pal_span{palette_lab};

    switch (settings.method) {
    case Method::none:
        return apply_none(image, pal_span);

    // Ordered dithering
    case Method::bayer2x2:
        return apply_ordered(image, bayer2, pal_span,
                             settings.strength);
    case Method::bayer4x4:
        return apply_ordered(image, bayer4, pal_span,
                             settings.strength);
    case Method::bayer8x8:
        return apply_ordered(image, bayer8, pal_span,
                             settings.strength);
    case Method::bayer3x3:
        return apply_ordered(image, bayer3, pal_span,
                             settings.strength);
    case Method::bayer5x5:
        return apply_ordered(image, bayer5, pal_span,
                             settings.strength);
    case Method::bayer6x6:
        return apply_ordered(image, bayer6, pal_span,
                             settings.strength);
    case Method::bayer7x7:
        return apply_ordered(image, bayer7, pal_span,
                             settings.strength);
    case Method::checker:
        return apply_ordered(image, checker_mat, pal_span,
                             settings.strength);
    case Method::h2x4:
        return apply_ordered(image, h2x4_mat, pal_span,
                             settings.strength);
    case Method::v4x2:
        return apply_ordered(image, v4x2_mat, pal_span,
                             settings.strength);
    case Method::bayer4x2:
        return apply_ordered(image, bayer4x2_mat, pal_span,
                             settings.strength);
    case Method::bayer2x4:
        return apply_ordered(image, bayer2x4_mat, pal_span,
                             settings.strength);
    case Method::clustered_dot:
        return apply_ordered(image, clustered_mat, pal_span,
                             settings.strength);
    case Method::line2:
        return apply_ordered(image, line2_mat, pal_span,
                             settings.strength);
    case Method::vline2:
        return apply_ordered(image, vline2_mat, pal_span,
                             settings.strength);
    case Method::vline_checker:
        return apply_ordered(image, vline_checker_mat, pal_span,
                             settings.strength);
    case Method::vline4:
        return apply_ordered(image, vline4_mat, pal_span,
                             settings.strength);
    case Method::vline8:
        return apply_ordered(image, vline8_mat, pal_span,
                             settings.strength);
    case Method::line_checker:
        return apply_ordered(image, line_checker_mat, pal_span,
                             settings.strength);
    case Method::line4:
        return apply_ordered(image, line4_mat, pal_span,
                             settings.strength);
    case Method::line8:
        return apply_ordered(image, line8_mat, pal_span,
                             settings.strength);
    case Method::halftone8x8:
        return apply_ordered(image, halftone8x8_mat, pal_span,
                             settings.strength);
    case Method::diagonal8x8:
        return apply_ordered(image, diagonal8x8_mat, pal_span,
                             settings.strength);
    case Method::spiral5x5:
        return apply_ordered(image, spiral5x5_mat, pal_span,
                             settings.strength);
    case Method::hex8x8:
        return apply_ordered(image, hex8x8_mat, pal_span,
                             settings.strength);
    case Method::hex5x5:
        return apply_ordered(image, hex5x5_mat, pal_span,
                             settings.strength);
    case Method::blue_noise:
        return apply_ordered(image, blue_noise_mat, pal_span,
                             settings.strength);
    case Method::void_cluster:
        return apply_ordered(image, void_cluster_mat, pal_span,
                             settings.strength);
    case Method::cluster_noise:
        return apply_ordered(image, cluster_noise_mat, pal_span,
                             settings.strength);
    case Method::fractal16:
        return apply_ordered(image, fractal16_mat, pal_span,
                             settings.strength);
    case Method::aseprite_old:
        return apply_ordered(image, aseprite_old_mat, pal_span,
                             settings.strength);
    case Method::libcaca_3x3:
        return apply_ordered(image, libcaca_3x3_mat, pal_span,
                             settings.strength);
    case Method::libcaca_6x6:
        return apply_ordered(image, libcaca_6x6_mat, pal_span,
                             settings.strength);
    case Method::pegasus_8x8:
        return apply_ordered(image, pegasus_8x8_mat, pal_span,
                             settings.strength);
    case Method::ign:
    case Method::ign_triangle:
    case Method::white_noise:
    case Method::r2_sequence:
    case Method::r2_triangle:
    case Method::crosshatch:
    case Method::radial:
    case Method::value_noise:
    case Method::cranley_bayer:
    case Method::quasicrystal:
    case Method::truchet: {
        // Analytical threshold methods — same "nearest vs second-nearest"
        // palette selection as matrix-based ordered dithering, but with
        // per-pixel analytical threshold (IGN, R2, white noise, etc.).
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        auto method = settings.method;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto pixel_lab = color_space::linear_to_oklab(image[x, y]);
                auto np = find_nearest_pair(pixel_lab, pal_span);
                float total = np.distA + np.distB;
                float t = (total > 1e-12f)
                    ? (std::sqrt(np.distA) /
                       (std::sqrt(np.distA) + std::sqrt(np.distB)))
                    : 0.0f;
                // ordered_threshold returns value in [-0.5, 0.5)
                float thr = ordered_threshold(method, x, y) + 0.5f;
                bool use_b = (thr < t * settings.strength);
                auto idx = use_b ? np.idxB : np.idxA;
                auto chosen = pal_span[idx];
                float dL = pixel_lab.L - chosen.L;
                float da = pixel_lab.a - chosen.a;
                float db = pixel_lab.b - chosen.b;
                r.indices[y * w + x] = static_cast<std::uint8_t>(idx);
                r.total_error += dL * dL + da * da + db * db;
            }
        }
        return r;
    }

    // Error diffusion
    case Method::floyd_steinberg:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, floyd_steinberg_kernel);
    case Method::atkinson:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, atkinson_kernel);
    case Method::sierra_lite:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, sierra_lite_kernel);
    case Method::stucki:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, stucki_kernel);
    case Method::jarvis:
        return apply_error_diffusion(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, jarvis_kernel);

    case Method::ostromoukhov:
        return apply_ostromoukhov(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine);

    case Method::gilbert:
        return apply_gilbert(
            image, pal_span,
            settings.strength, settings.error_clamp);

    case Method::riemersma:
        return apply_riemersma(
            image, pal_span,
            settings.strength, settings.error_clamp);

    case Method::yliluoma:
        return apply_yliluoma(image, pal_span, settings.strength, false);
    case Method::yliluoma2:
        return apply_yliluoma(image, pal_span, settings.strength, true);
    case Method::opt_checker: {
        // Per-pixel optimal pair, indexed by (x+y) & 1 checker phase.
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto t = color_space::linear_to_oklab(image[x, y]);
                auto idx = pick_opt_checker_index(t, pal_span, x, y, settings.strength);
                r.indices[y * w + x] = idx;
                float dL = t.L - pal_span[idx].L;
                float da = t.a - pal_span[idx].a;
                float db = t.b - pal_span[idx].b;
                r.total_error += dL * dL + da * da + db * db;
            }
        }
        return r;
    }
    case Method::knoll: {
        // Knoll pattern dither — N=16 plan on 4×4 Bayer.
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto t = color_space::linear_to_oklab(image[x, y]);
                auto idx = pick_knoll_index(t, pal_span, x, y, settings.strength);
                r.indices[y * w + x] = idx;
                float dL = t.L - pal_span[idx].L;
                float da = t.a - pal_span[idx].a;
                float db = t.b - pal_span[idx].b;
                r.total_error += dL * dL + da * da + db * db;
            }
        }
        return r;
    }
    case Method::tri_tone: {
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto t = color_space::linear_to_oklab(image[x, y]);
                auto idx = pick_tri_tone_index(t, pal_span, x, y, settings.strength);
                r.indices[y * w + x] = idx;
                float dL = t.L - pal_span[idx].L;
                float da = t.a - pal_span[idx].a;
                float db = t.b - pal_span[idx].b;
                r.total_error += dL * dL + da * da + db * db;
            }
        }
        return r;
    }
    case Method::yliluoma1: {
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto t = color_space::linear_to_oklab(image[x, y]);
                auto idx = pick_yliluoma1_index(t, pal_span, x, y, settings.strength);
                r.indices[y * w + x] = idx;
                float dL = t.L - pal_span[idx].L;
                float da = t.a - pal_span[idx].a;
                float db = t.b - pal_span[idx].b;
                r.total_error += dL * dL + da * da + db * db;
            }
        }
        return r;
    }
    case Method::opt_line: {
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto t = color_space::linear_to_oklab(image[x, y]);
                auto idx = pick_opt_line_index(t, pal_span, x, y, settings.strength);
                r.indices[y * w + x] = idx;
                float dL = t.L - pal_span[idx].L;
                float da = t.a - pal_span[idx].a;
                float db = t.b - pal_span[idx].b;
                r.total_error += dL * dL + da * da + db * db;
            }
        }
        return r;
    }
    case Method::opt_line_checker: {
        auto w = image.width();
        auto h = image.height();
        DitherResult r;
        r.indices.resize(w * h);
        r.total_error = 0.0f;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                auto t = color_space::linear_to_oklab(image[x, y]);
                auto idx = pick_opt_line_checker_index(t, pal_span, x, y, settings.strength);
                r.indices[y * w + x] = idx;
                float dL = t.L - pal_span[idx].L;
                float da = t.a - pal_span[idx].a;
                float db = t.b - pal_span[idx].b;
                r.total_error += dL * dL + da * da + db * db;
            }
        }
        return r;
    }

    case Method::structure_fs:
        return apply_structure_fs(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, StructureBias::laplacian);
    case Method::contrast_fs:
        return apply_structure_fs(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, StructureBias::contrast);
    case Method::zhoufang:
        return apply_structure_fs(
            image, pal_span,
            settings.strength, settings.error_clamp,
            settings.serpentine, StructureBias::zhoufang);
    }

    return apply_none(image, pal_span);
}

std::span<const DiffusionEntry> error_diffusion_kernel(Method method) {
    switch (method) {
    case Method::floyd_steinberg: return floyd_steinberg_kernel;
    case Method::atkinson:        return atkinson_kernel;
    case Method::sierra_lite:     return sierra_lite_kernel;
    case Method::stucki:          return stucki_kernel;
    case Method::jarvis:          return jarvis_kernel;
    case Method::ostromoukhov:    return floyd_steinberg_kernel;  // F-S base kernel
    // Structure-aware variants and Riemersma all build on F-S in CAP mode
    // — the per-pixel bias / queue is layered on top by the caller.
    // (Curve walking can't span per-scanline palette swaps cleanly.)
    case Method::structure_fs:    return floyd_steinberg_kernel;
    case Method::contrast_fs:     return floyd_steinberg_kernel;
    case Method::zhoufang:        return floyd_steinberg_kernel;
    case Method::riemersma:       return floyd_steinberg_kernel;
    default:                      return {};
    }
}

bool needs_structure_bias(Method method) {
    return method == Method::structure_fs ||
           method == Method::contrast_fs ||
           method == Method::zhoufang;
}

bool needs_riemersma_queue(Method method) {
    return method == Method::riemersma;
}

std::vector<float> compute_structure_bias(const Image& image, Method method) {
    if (!needs_structure_bias(method)) return {};
    auto w = image.width();
    auto h = image.height();
    std::vector<color_space::OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);

    std::vector<float> sig(w * h, 0.0f);
    if (method == Method::structure_fs) {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float L = image_lab[y * w + x].L;
                float Lx0 = (x > 0)     ? image_lab[y * w + (x - 1)].L : L;
                float Lx1 = (x + 1 < w) ? image_lab[y * w + (x + 1)].L : L;
                float Ly0 = (y > 0)     ? image_lab[(y - 1) * w + x].L : L;
                float Ly1 = (y + 1 < h) ? image_lab[(y + 1) * w + x].L : L;
                float v = 4.0f * L - Lx0 - Lx1 - Ly0 - Ly1;
                if (v < -0.5f) v = -0.5f;
                if (v > 0.5f)  v = 0.5f;
                sig[y * w + x] = v;
            }
        }
    } else if (method == Method::contrast_fs) {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float lo = 1.0f, hi = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto yy = static_cast<std::ptrdiff_t>(y) + dy;
                        auto xx = static_cast<std::ptrdiff_t>(x) + dx;
                        if (yy < 0 || yy >= static_cast<std::ptrdiff_t>(h)) continue;
                        if (xx < 0 || xx >= static_cast<std::ptrdiff_t>(w)) continue;
                        float L = image_lab[static_cast<std::size_t>(yy) * w +
                                            static_cast<std::size_t>(xx)].L;
                        if (L < lo) lo = L;
                        if (L > hi) hi = L;
                    }
                }
                float L = image_lab[y * w + x].L;
                float center = 0.5f * (lo + hi);
                sig[y * w + x] = (hi - lo) * ((L > center) ? 1.0f : -1.0f);
            }
        }
    } else { // zhoufang: triangular intensity-modulated noise amplitude
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                float L = image_lab[y * w + x].L;
                float dist = std::abs(L - 0.5f);
                sig[y * w + x] = (1.0f - 2.0f * dist) * 0.3f;
            }
        }
    }

    // Local stddev gate (5×5) so flat regions stay neutral.
    std::vector<float> stddev(w * h, 0.0f);
    constexpr int R = 2;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float mean = 0.0f, sq = 0.0f;
            int count = 0;
            for (int dy = -R; dy <= R; ++dy) {
                for (int dx = -R; dx <= R; ++dx) {
                    auto yy = static_cast<std::ptrdiff_t>(y) + dy;
                    auto xx = static_cast<std::ptrdiff_t>(x) + dx;
                    if (yy < 0 || yy >= static_cast<std::ptrdiff_t>(h)) continue;
                    if (xx < 0 || xx >= static_cast<std::ptrdiff_t>(w)) continue;
                    float L = image_lab[static_cast<std::size_t>(yy) * w +
                                        static_cast<std::size_t>(xx)].L;
                    mean += L;
                    sq += L * L;
                    ++count;
                }
            }
            mean /= static_cast<float>(count);
            sq /= static_cast<float>(count);
            float var = sq - mean * mean;
            stddev[y * w + x] = (var > 0.0f) ? std::sqrt(var) : 0.0f;
        }
    }

    constexpr float K_SCALE = 0.4f;
    std::vector<float> bias(w * h, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            std::size_t idx = y * w + x;
            float b;
            if (method == Method::zhoufang) {
                auto seed = static_cast<std::uint32_t>(y * 65537u + x);
                seed = (seed ^ 61u) ^ (seed >> 16u);
                seed *= 9u;
                seed ^= seed >> 4u;
                seed *= 0x27d4eb2du;
                seed ^= seed >> 15u;
                float r = static_cast<float>(seed & 0xFFFFu) / 65536.0f - 0.5f;
                b = r * sig[idx];
            } else {
                b = K_SCALE * sig[idx] * (stddev[idx] / 0.1f);
                if (b < -0.2f) b = -0.2f;
                if (b > 0.2f)  b = 0.2f;
            }
            bias[idx] = b;
        }
    }
    return bias;
}

bool uses_error_diffusion(Method method) {
    return method != Method::none &&
           !is_ordered(method) &&
           !is_yliluoma(method) &&
           !error_diffusion_kernel(method).empty();
}

float pick_palette_index_with_ostro(
    Method method,
    const color_space::OKLab& target,
    std::span<const color_space::OKLab> palette_lab,
    std::size_t x, std::size_t y, float strength,
    std::size_t k_min,
    std::size_t& chosen_index,
    color_space::OKLab& chosen_lab) {

    if (is_yliluoma(method)) {
        std::span<const color_space::OKLab> sub = (k_min == 0)
            ? palette_lab
            : std::span<const color_space::OKLab>{
                palette_lab.data() + k_min, palette_lab.size() - k_min};
        auto rel = pick_yliluoma_family_index(method, target, sub,
                                               x, y, strength);
        chosen_index = k_min + static_cast<std::size_t>(rel);
        chosen_lab = palette_lab[chosen_index];
        return 0.5f;  // yliluoma → unit ostromoukhov scale
    }

    float best_d = std::numeric_limits<float>::max();
    float second_d = std::numeric_limits<float>::max();
    std::size_t best_k = k_min;
    for (std::size_t k = k_min; k < palette_lab.size(); ++k) {
        float dL = target.L - palette_lab[k].L;
        float da = target.a - palette_lab[k].a;
        float db = target.b - palette_lab[k].b;
        float d = dL * dL + da * da + db * db;
        if (d < best_d) {
            second_d = best_d;
            best_d = d;
            best_k = k;
        } else if (d < second_d) {
            second_d = d;
        }
    }
    chosen_index = best_k;
    chosen_lab = palette_lab[best_k];
    if (second_d > 1e-12f) {
        float sb = std::sqrt(best_d);
        float ss = std::sqrt(second_d);
        return sb / (sb + ss);
    }
    return 0.5f;
}

// ===========================================================================
// diffuse_raw_buffer — single per-pixel ED scaffolding for raw-buffer modes
//
// Replaces nine near-identical copies of the same loop (api.cpp×3,
// ham.cpp×2, copper.cpp, scap.cpp×2, main.cpp). Centralising the
// scaffolding here also fixes drift that had crept in: ostromoukhov
// variable scaling was no-op'd in 6/9 sites, and main.cpp's loop wasn't
// serpentining.
// ===========================================================================
float diffuse_raw_buffer(const Image& image,
                         const Settings& settings,
                         const PixelPicker& pick) {
    auto w = image.width();
    auto h = image.height();

    bool is_ord  = is_ordered(settings.method) &&
                   settings.method != Method::none;
    auto kernel  = error_diffusion_kernel(settings.method);
    bool is_diff = settings.method != Method::none && !is_ord &&
                   !kernel.empty();
    bool is_ostro = (settings.method == Method::ostromoukhov);
    bool needs_riem = is_diff && needs_riemersma_queue(settings.method);

    auto bias_map = is_diff
        ? compute_structure_bias(image, settings.method)
        : std::vector<float>{};

    constexpr std::size_t RIEM_QSIZE = 16;
    std::array<color_space::OKLab, RIEM_QSIZE> riem_queue{};
    std::array<float, RIEM_QSIZE> riem_weights{};
    std::size_t riem_head = 0;
    if (needs_riem) {
        const float ratio = std::pow(1.0f / 16.0f, 1.0f / 15.0f);
        float w_acc = 1.0f;
        for (std::size_t i = RIEM_QSIZE; i-- > 0; ) {
            riem_weights[i] = w_acc;
            w_acc *= ratio;
        }
        float total = 0.0f;
        for (float wt : riem_weights) total += wt;
        for (float& wt : riem_weights) wt /= total;
    }

    std::vector<color_space::OKLab> err_buf;
    if (is_diff) err_buf.assign(w * h, color_space::OKLab{0, 0, 0});

    float total_error = 0.0f;
    auto ec = settings.error_clamp;

    for (std::size_t y = 0; y < h; ++y) {
        bool reverse = is_diff && settings.serpentine && (y & 1);

        for (std::size_t step = 0; step < w; ++step) {
            std::size_t x = reverse ? (w - 1 - step) : step;
            auto src_lab = color_space::linear_to_oklab(image[x, y]);
            color_space::OKLab target = src_lab;

            if (needs_riem) {
                color_space::OKLab carry{};
                for (std::size_t k = 0; k < RIEM_QSIZE; ++k) {
                    std::size_t age =
                        (riem_head + RIEM_QSIZE - 1 - k) % RIEM_QSIZE;
                    carry.L += riem_queue[age].L * riem_weights[k];
                    carry.a += riem_queue[age].a * riem_weights[k];
                    carry.b += riem_queue[age].b * riem_weights[k];
                }
                target.L += std::clamp(carry.L, -ec, ec);
                target.a += std::clamp(carry.a, -ec, ec);
                target.b += std::clamp(carry.b, -ec, ec);
            } else if (is_diff) {
                auto& e = err_buf[y * w + x];
                target.L += std::clamp(e.L, -ec, ec);
                target.a += std::clamp(e.a, -ec, ec);
                target.b += std::clamp(e.b, -ec, ec);
            }
            if (!bias_map.empty()) target.L += bias_map[y * w + x];
            if (is_ord) {
                float thr = ordered_threshold(settings.method, x, y);
                target.L += thr * settings.strength * 0.15f;
                target.a += thr * settings.strength * 0.03f;
                target.b += thr * settings.strength * 0.03f;
            }

            auto picked = pick(target, x, y);

            float dL = src_lab.L - picked.chosen_lab.L;
            float da = src_lab.a - picked.chosen_lab.a;
            float db = src_lab.b - picked.chosen_lab.b;
            total_error += dL * dL + da * da + db * db;

            if (is_diff) {
                color_space::OKLab qe{
                    (target.L - picked.chosen_lab.L) * settings.strength,
                    (target.a - picked.chosen_lab.a) * settings.strength,
                    (target.b - picked.chosen_lab.b) * settings.strength,
                };
                float ostro_scale = is_ostro
                    ? (0.6f + 0.8f * picked.ostro_threshold)
                    : 1.0f;
                if (needs_riem) {
                    riem_queue[riem_head] = qe;
                    riem_head = (riem_head + 1) % RIEM_QSIZE;
                } else {
                    for (auto& [kdx, kdy, kw] : kernel) {
                        auto nx = static_cast<std::ptrdiff_t>(x) +
                                  (reverse ? -kdx : kdx);
                        auto ny = static_cast<std::ptrdiff_t>(y) + kdy;
                        if (nx < 0 || ny < 0) continue;
                        if (static_cast<std::size_t>(nx) >= w) continue;
                        if (static_cast<std::size_t>(ny) >= h) continue;
                        auto& en = err_buf[
                            static_cast<std::size_t>(ny) * w +
                            static_cast<std::size_t>(nx)];
                        en.L += qe.L * kw * ostro_scale;
                        en.a += qe.a * kw * ostro_scale;
                        en.b += qe.b * kw * ostro_scale;
                    }
                }
            }
        }
    }

    return total_error;
}

float ordered_threshold(Method method, std::size_t x, std::size_t y) {
    switch (method) {
    case Method::bayer2x2:      return bayer2[y % 2][x % 2];
    case Method::bayer4x4:      return bayer4[y % 4][x % 4];
    case Method::bayer8x8:      return bayer8[y % 8][x % 8];
    case Method::bayer3x3:      return bayer3[y % 3][x % 3];
    case Method::bayer5x5:      return bayer5[y % 5][x % 5];
    case Method::bayer6x6:      return bayer6[y % 6][x % 6];
    case Method::bayer7x7:      return bayer7[y % 7][x % 7];
    case Method::checker:       return checker_mat[y % 2][x % 2];
    case Method::h2x4:          return h2x4_mat[y % 4][x % 2];
    case Method::v4x2:          return v4x2_mat[y % 2][x % 4];
    case Method::bayer4x2:      return bayer4x2_mat[y % 2][x % 4];
    case Method::bayer2x4:      return bayer2x4_mat[y % 4][x % 2];
    case Method::clustered_dot: return clustered_mat[y % 4][x % 4];
    case Method::line2:         return line2_mat[y % 2][0];
    case Method::vline2:        return vline2_mat[0][x % 2];
    case Method::vline_checker: return vline_checker_mat[y % 2][x % 2];
    case Method::vline4:        return vline4_mat[0][x % 4];
    case Method::vline8:        return vline8_mat[0][x % 8];
    case Method::line_checker:  return line_checker_mat[y % 2][x % 2];
    case Method::line4:         return line4_mat[y % 4][0];
    case Method::line8:         return line8_mat[y % 8][0];
    case Method::halftone8x8:  return halftone8x8_mat[y % 8][x % 8];
    case Method::diagonal8x8:  return diagonal8x8_mat[y % 8][x % 8];
    case Method::spiral5x5:    return spiral5x5_mat[y % 5][x % 5];
    case Method::hex8x8:       return hex8x8_mat[y % 8][x % 8];
    case Method::hex5x5:       return hex5x5_mat[y % 5][x % 5];
    case Method::blue_noise:   return blue_noise_mat[y % 64][x % 64];
    case Method::void_cluster: return void_cluster_mat[y % 64][x % 64];
    case Method::cluster_noise: return cluster_noise_mat[y % 64][x % 64];
    case Method::fractal16:    return fractal16_mat[y % 16][x % 16];
    case Method::aseprite_old: return aseprite_old_mat[y % 4][x % 4];
    case Method::libcaca_3x3:  return libcaca_3x3_mat[y % 3][x % 3];
    case Method::libcaca_6x6:  return libcaca_6x6_mat[y % 6][x % 6];
    case Method::pegasus_8x8:  return pegasus_8x8_mat[y % 8][x % 8];
    case Method::cranley_bayer: {
        // Bayer 8×8 with a deterministic per-tile random rotation
        // (Iñigo Quílez "Free dithering" — Cranley-Patterson rotation).
        // Hash the 8×8 tile index into a uniform [0,1) offset and add to
        // Bayer's threshold mod 1; breaks Bayer's regular grid into a
        // patchwork of randomly-phased Bayer tiles.
        std::size_t tx = x / 8, ty = y / 8;
        auto seed = static_cast<std::uint32_t>(tx * 73856093u ^ ty * 19349663u);
        seed ^= seed >> 13u;
        seed *= 0x5BD1E995u;
        seed ^= seed >> 15u;
        float offset = static_cast<float>(seed & 0xFFFFu) / 65536.0f;
        float v = bayer8[y % 8][x % 8] + 0.5f + offset;
        return std::fmod(v, 1.0f) - 0.5f;
    }
    case Method::quasicrystal: {
        // Sloan's quasicrystal: sum of N cosine waves at evenly-spaced
        // angles. Aperiodic without blue-noise's randomness — produces
        // a "shimmer" pattern. N=5 gives 10-fold symmetry.
        constexpr int N = 5;
        constexpr float scale = 0.40f;
        float fx = static_cast<float>(x) * scale;
        float fy = static_cast<float>(y) * scale;
        float sum = 0.0f;
        for (int k = 0; k < N; ++k) {
            float a = 3.14159265f * static_cast<float>(k) / static_cast<float>(N);
            sum += std::cos(fx * std::cos(a) + fy * std::sin(a));
        }
        // Normalize sum (range roughly [-N, N]) → [-0.5, 0.5)
        return sum / (2.0f * static_cast<float>(N));
    }
    case Method::truchet: {
        // Truchet-tile threshold: each 8×8 cell gets one of 4 rotations
        // of a quarter-arc gradient; reads as a woven/tiled texture
        // distinct from the rectangular Bayer/blue-noise look.
        std::size_t cx = x / 8, cy = y / 8;
        auto seed = static_cast<std::uint32_t>(cx * 73856093u ^ cy * 19349663u);
        seed ^= seed >> 13u; seed *= 0x5BD1E995u; seed ^= seed >> 15u;
        int rot = static_cast<int>(seed & 3u);
        float lx = static_cast<float>(x % 8) / 8.0f - 0.5f;
        float ly = static_cast<float>(y % 8) / 8.0f - 0.5f;
        // Apply rotation
        float rx = lx, ry = ly;
        switch (rot) {
        case 1: rx = -ly; ry = lx; break;
        case 2: rx = -lx; ry = -ly; break;
        case 3: rx = ly; ry = -lx; break;
        default: break;
        }
        float r = std::sqrt((rx + 0.5f) * (rx + 0.5f) + (ry + 0.5f) * (ry + 0.5f));
        return (r - 0.5f);
    }
    case Method::ign: {
        // Jimenez 2014 Interleaved Gradient Noise
        auto fx = static_cast<float>(x);
        auto fy = static_cast<float>(y);
        float v = 52.9829189f * std::fmod(0.06711056f * fx + 0.00583715f * fy, 1.0f);
        return std::fmod(v, 1.0f) - 0.5f;
    }
    case Method::ign_triangle: {
        // IGN with U(0,1) → triangle(-1,1) remap (Wronski 2016) — kills
        // the banding near 0% / 100% intensity that plain IGN exhibits
        // on dark CRT gradients.
        auto fx = static_cast<float>(x);
        auto fy = static_cast<float>(y);
        float v = 52.9829189f * std::fmod(0.06711056f * fx + 0.00583715f * fy, 1.0f);
        float u = std::fmod(v, 1.0f);
        float t;
        if (u < 0.5f) {
            t = -1.0f + std::sqrt(2.0f * u);
        } else {
            t = 1.0f - std::sqrt(2.0f - 2.0f * u);
        }
        return t * 0.5f;
    }
    case Method::white_noise: {
        // Integer hash (Wang) for pure random noise
        auto seed = static_cast<std::uint32_t>(y * 65537 + x);
        seed = (seed ^ 61u) ^ (seed >> 16u);
        seed *= 9u;
        seed ^= seed >> 4u;
        seed *= 0x27d4eb2du;
        seed ^= seed >> 15u;
        return static_cast<float>(seed & 0xFFFFu) / 65536.0f - 0.5f;
    }
    case Method::r2_sequence: {
        // Martin Roberts R2: generalized golden ratio for 2D
        constexpr float phi1 = 0.7548776662f;  // 1/plastic number
        constexpr float phi2 = 0.5698402910f;  // 1/plastic^2
        float v = std::fmod(static_cast<float>(x) * phi1 +
                            static_cast<float>(y) * phi2 + 0.5f, 1.0f);
        return v - 0.5f;
    }
    case Method::r2_triangle: {
        // R2 with U(0,1) → triangle(-1,1) remap (Wronski 2016, "Dithering
        // part three"). Triangular noise PDF removes the DC bias plain R2
        // has near 0% / 100% intensity, so dark and bright gradients
        // dither evenly instead of clipping to flat colour.
        constexpr float phi1 = 0.7548776662f;
        constexpr float phi2 = 0.5698402910f;
        float u = std::fmod(static_cast<float>(x) * phi1 +
                            static_cast<float>(y) * phi2 + 0.5f, 1.0f);
        // Remap U(0,1) → triangle(-1,1): u<0.5 → -1+sqrt(2u); u>=0.5 → 1-sqrt(2-2u)
        float t;
        if (u < 0.5f) {
            t = -1.0f + std::sqrt(2.0f * u);
        } else {
            t = 1.0f - std::sqrt(2.0f - 2.0f * u);
        }
        return t * 0.5f;  // scale back to [-0.5, 0.5)
    }
    case Method::crosshatch: {
        // Overlaid line patterns at 0°, 45°, 90°, 135° — threshold is
        // the minimum distance to any line, giving pen-and-ink texture
        auto fx = static_cast<float>(x);
        auto fy = static_cast<float>(y);
        float d0 = std::fmod(fy, 8.0f) / 8.0f;                          // horizontal
        float d1 = std::fmod(fx, 8.0f) / 8.0f;                          // vertical
        float d2 = std::fmod((fx + fy) * 0.7071f, 8.0f) / 8.0f;         // 45°
        float d3 = std::fmod((fx - fy + 512.0f) * 0.7071f, 8.0f) / 8.0f; // 135°
        // Triangle wave each to [0,1]
        d0 = 1.0f - std::abs(d0 * 2.0f - 1.0f);
        d1 = 1.0f - std::abs(d1 * 2.0f - 1.0f);
        d2 = 1.0f - std::abs(d2 * 2.0f - 1.0f);
        d3 = 1.0f - std::abs(d3 * 2.0f - 1.0f);
        // Progressive reveal: horizontal first, then +vertical, +diagonals
        float t = std::min({d0, d0 * 0.5f + d1 * 0.5f,
                            d0 * 0.3f + d1 * 0.3f + d2 * 0.4f,
                            d0 * 0.25f + d1 * 0.25f + d2 * 0.25f + d3 * 0.25f});
        return t - 0.5f;
    }
    case Method::radial: {
        // Concentric circles — threshold based on distance from center
        auto fx = static_cast<float>(x) - 160.0f;  // assume 320 wide
        auto fy = static_cast<float>(y) - 128.0f;  // assume 256 tall
        float r = std::sqrt(fx * fx + fy * fy);
        float v = std::fmod(r * 0.15f, 1.0f);
        return (1.0f - std::abs(v * 2.0f - 1.0f)) - 0.5f;
    }
    case Method::value_noise: {
        // 2D value noise: hash at integer grid + bilinear interpolation
        auto hash = [](int ix, int iy) -> float {
            auto s = static_cast<std::uint32_t>(ix * 374761393 + iy * 668265263 + 1013904223);
            s = (s ^ (s >> 13u)) * 1274126177u;
            return static_cast<float>(s & 0xFFFFu) / 65536.0f;
        };
        constexpr float scale = 0.125f;  // noise cell size = 8 pixels
        float fx = static_cast<float>(x) * scale;
        float fy = static_cast<float>(y) * scale;
        int ix = static_cast<int>(std::floor(fx));
        int iy = static_cast<int>(std::floor(fy));
        float tx = fx - static_cast<float>(ix);
        float ty = fy - static_cast<float>(iy);
        // Smoothstep for less blocky interpolation
        tx = tx * tx * (3.0f - 2.0f * tx);
        ty = ty * ty * (3.0f - 2.0f * ty);
        float v00 = hash(ix, iy), v10 = hash(ix + 1, iy);
        float v01 = hash(ix, iy + 1), v11 = hash(ix + 1, iy + 1);
        float v = v00 * (1 - tx) * (1 - ty) + v10 * tx * (1 - ty) +
                  v01 * (1 - tx) * ty + v11 * tx * ty;
        return v - 0.5f;
    }
    default:                    return 0.0f;
    }
}

} // namespace png2amiga::dither
