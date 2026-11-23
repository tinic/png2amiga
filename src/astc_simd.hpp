#pragma once

// SIMD kernels of the ASTC encoder.
//
// Every kernel is element-wise: lane i of the result depends on lane i of
// the inputs only, and each lane runs the same operations in the same order
// as the scalar arm. There are no reductions here.
//
// Multiply-add and why the kernels are not on everywhere
// ------------------------------------------------------
// The encoder is compiled with -ffp-contract=fast (/fp:contract on MSVC):
// the compiler may fuse a * b + c into one FMA instruction, and it decides
// per expression. That decision is part of the encoder's output (a fused
// and a separate multiply-add round differently, and one changed bit can
// flip a candidate choice). An intrinsic kernel fixes the decision, so it
// only keeps the output of a build unchanged where it fixes it the way the
// compiler does for the scalar loop it replaces:
//   - no FMA instruction in the target (baseline x86-64 build, WASM): the
//     scalar code cannot be fused, the kernel multiplies and adds. Exact by
//     construction.
//   - clang on AArch64: the kernels are fused. Checked with whole encodes:
//     the files of the kernel build and of the scalar code it replaced are
//     byte-identical on a 74-case matrix (all footprints).
//   - every other compiler with an FMA target (x86-64 AVX2+FMA with GCC,
//     clang or MSVC; GCC on AArch64): not checked. There the kernels are
//     off by default and the encoder runs plain scalar loops, which the
//     compiler contracts by its own rules. To try the kernels:
//     -DPNG2AMIGA_ASTC_SIMD_KERNELS=1, then compare the encoder's files
//     against a build without it. (Separate multiply and add intrinsics
//     are no way to an unfused kernel on these targets: the compiler
//     contracts them like scalar code.)
// A kernel must not replace a loop-carried sum (a dot product turned into
// column updates): clang on AArch64 fuses such a sum in most instantiations
// but not in all (found at 10x8), so no fixed choice reproduces it.
// -DPNG2AMIGA_ASTC_SIMD_KERNELS=0 turns the kernels off on every target.
//
// Backends, as in color_space.hpp / oklab_simd.hpp:
//   NEON        AArch64
//   AVX2 + FMA  x86-64 default build
//   SSE2        x86-64 baseline build (PNG2AMIGA_BASELINE_SIMD)
//   WASM SIMD   the -msimd128 module
//   scalar      everything else (the no-SIMD WASM module)
// tools/astc_kernel_test.cpp checks each kernel against a scalar reference
// with the same explicit multiply-add, bit for bit, on random inputs.

#include <cmath>
#include <cstddef>

#if !defined(PNG2AMIGA_ASTC_SIMD_KERNELS)
#if defined(__wasm__) || defined(__EMSCRIPTEN__)
#define PNG2AMIGA_ASTC_SIMD_KERNELS 1
#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(__FMA__)
#define PNG2AMIGA_ASTC_SIMD_KERNELS 1
#elif defined(__clang__) && defined(__aarch64__)
#define PNG2AMIGA_ASTC_SIMD_KERNELS 1
#else
#define PNG2AMIGA_ASTC_SIMD_KERNELS 0
#endif
#endif

#if PNG2AMIGA_ASTC_SIMD_KERNELS
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define PNG2AMIGA_ASTC_SIMD_WASM 1
#elif defined(__wasm__) || defined(__EMSCRIPTEN__)
// no-SIMD WASM module: scalar arm
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define PNG2AMIGA_ASTC_SIMD_NEON 1
#elif defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#define PNG2AMIGA_ASTC_SIMD_AVX2 1
#elif defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define PNG2AMIGA_ASTC_SIMD_SSE2 1
#endif
#endif

// 1: the kernels fuse multiply-add (NEON, AVX2 + FMA). 0: the target has no
// FMA instruction and nothing can be fused.
#if defined(PNG2AMIGA_ASTC_SIMD_NEON) || defined(PNG2AMIGA_ASTC_SIMD_AVX2)
#define PNG2AMIGA_ASTC_FUSED 1
#else
#define PNG2AMIGA_ASTC_FUSED 0
#endif

namespace png2amiga::astc::simd {

inline constexpr bool kKernels = PNG2AMIGA_ASTC_SIMD_KERNELS != 0;
inline constexpr bool kFused = PNG2AMIGA_ASTC_FUSED != 0;

// Scalar references with the kernels' explicit multiply-add (for the test
// and for the vector tails). madd(a, x, y) = y + a * x.
inline float madd(float a, float x, float y) noexcept {
#if PNG2AMIGA_ASTC_FUSED
    return std::fma(a, x, y);
#else
    // No FMA instruction in these targets, so the compiler cannot fuse.
    return y + a * x;
#endif
}
inline void axpy_sub_scalar(float* y, const float* a, float x, int n) noexcept {
    for (int i = 0; i < n; ++i)
        y[i] = madd(-a[i], x, y[i]);
}

// y[i] -= a[i] * x for i in [0, n).
inline void axpy_sub(float* y, const float* a, float x, int n) noexcept {
    int i = 0;
#if defined(PNG2AMIGA_ASTC_SIMD_NEON)
    const float32x4_t vx = vdupq_n_f32(x);
    for (; i + 4 <= n; i += 4) {
        vst1q_f32(y + i, vfmsq_f32(vld1q_f32(y + i), vld1q_f32(a + i), vx));
    }
#elif defined(PNG2AMIGA_ASTC_SIMD_AVX2)
    const __m256 vx = _mm256_set1_ps(x);
    for (; i + 8 <= n; i += 8) {
        _mm256_storeu_ps(y + i,
                         _mm256_fnmadd_ps(_mm256_loadu_ps(a + i), vx, _mm256_loadu_ps(y + i)));
    }
#elif defined(PNG2AMIGA_ASTC_SIMD_SSE2)
    const __m128 vx = _mm_set1_ps(x);
    for (; i + 4 <= n; i += 4)
        _mm_storeu_ps(y + i, _mm_sub_ps(_mm_loadu_ps(y + i), _mm_mul_ps(_mm_loadu_ps(a + i), vx)));
#elif defined(PNG2AMIGA_ASTC_SIMD_WASM)
    const v128_t vx = wasm_f32x4_splat(x);
    for (; i + 4 <= n; i += 4)
        wasm_v128_store(
            y + i,
            wasm_f32x4_sub(wasm_v128_load(y + i), wasm_f32x4_mul(wasm_v128_load(a + i), vx)));
#endif
    for (; i < n; ++i)
        y[i] = madd(-a[i], x, y[i]);
}

}  // namespace png2amiga::astc::simd
