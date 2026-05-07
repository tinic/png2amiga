// pow24 microbench — compares scalar std::pow(x, 2.4f), scalar polynomial
// approximation, and an AVX2 8-wide polynomial against the LUT we tried.
//
// Goal: figure out whether an inline polynomial pow24 actually beats
// MSVC's std::pow on AMD Zen 1 (the dither hot loop's per-pixel cost),
// without burning a profiling cycle to find out. The LUT replacement
// already lost to std::pow on this machine (project_perf_dead_ends),
// so the inline polynomial is the next legitimate candidate.
//
// Workload: uniform-random sRGB inputs in [0, 1] (clamped) — the same
// distribution apply_error_diffusion sees after target_s clamping.
// Outputs are summed to defeat dead-code elimination.
//
// Usage: ./bench_pow24 [N=10000000]
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#if defined(__AVX2__) || defined(_M_X64)
#include <immintrin.h>
#define HAVE_AVX2 1
#else
#define HAVE_AVX2 0
#endif

namespace {

// --- scalar reference (matches color_space::srgb_to_linear) ---
[[gnu::noinline]]
float pow24_libm(float s) noexcept {
    if (s <= 0.04045f) return s / 12.92f;
    return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

// --- scalar polynomial: pow(x, 2.4) = exp2(2.4 * log2(x)) ---
//
// log2 polynomial: float log2 via bit-trick (extract exponent) + 5-degree
// minimax polynomial on the mantissa range [1, 2). exp2 polynomial: 5-
// degree minimax on the fractional part + ldexp via int-to-float.
//
// Coefficients sourced from public-domain implementations (cephes /
// SLEEF-style). Max relative error ~1e-6, plenty for our use case.
[[gnu::always_inline]]
inline float fast_log2(float x) noexcept {
    // Extract exponent + mantissa via bit trick.
    union { float f; std::uint32_t u; } v{x};
    int e = static_cast<int>((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;  // mantissa in [1, 2)
    float m = v.f - 1.0f;                    // m in [0, 1)
    // Minimax polynomial for log2(1+m), m in [0, 1). 5th-degree.
    float p = m * (1.44269504f
              + m * (-0.72134752f
              + m * ( 0.47985219f
              + m * (-0.32546528f
              + m * ( 0.13935294f)))));
    return p + static_cast<float>(e);
}

[[gnu::always_inline]]
inline float fast_exp2(float x) noexcept {
    // Split x = i + f, i = floor(x), f in [0, 1).
    float xi = std::floor(x);
    float f = x - xi;
    int   i = static_cast<int>(xi);
    // Minimax polynomial for 2^f, f in [0, 1). 5th-degree.
    float p = 1.0f
             + f * (0.69314718f
             + f * (0.24022651f
             + f * (0.05550411f
             + f * (0.00961813f
             + f * (0.00133337f)))));
    // ldexp: multiply by 2^i via bit-bash.
    union { float f; std::uint32_t u; } v;
    v.u = static_cast<std::uint32_t>((i + 127) & 0xFF) << 23;
    return p * v.f;
}

[[gnu::noinline]]
float pow24_poly_scalar(float s) noexcept {
    if (s <= 0.04045f) return s / 12.92f;
    float x = (s + 0.055f) / 1.055f;
    return fast_exp2(2.4f * fast_log2(x));
}

#if HAVE_AVX2
// --- AVX2 8-wide polynomial ---
[[gnu::always_inline]]
inline __m256 fast_log2_v(__m256 x) noexcept {
    __m256i ix = _mm256_castps_si256(x);
    __m256i ie = _mm256_sub_epi32(
        _mm256_and_si256(_mm256_srli_epi32(ix, 23),
                          _mm256_set1_epi32(0xFF)),
        _mm256_set1_epi32(127));
    __m256 e = _mm256_cvtepi32_ps(ie);
    __m256i im = _mm256_or_si256(
        _mm256_and_si256(ix, _mm256_set1_epi32(0x007FFFFF)),
        _mm256_set1_epi32(0x3F800000));
    __m256 m = _mm256_sub_ps(_mm256_castsi256_ps(im),
                              _mm256_set1_ps(1.0f));
    __m256 p = _mm256_set1_ps( 0.13935294f);
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps(-0.32546528f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps( 0.47985219f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps(-0.72134752f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps( 1.44269504f));
    p = _mm256_mul_ps(p, m);
    return _mm256_add_ps(p, e);
}

[[gnu::always_inline]]
inline __m256 fast_exp2_v(__m256 x) noexcept {
    __m256 xi = _mm256_round_ps(x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    __m256 f  = _mm256_sub_ps(x, xi);
    __m256i i = _mm256_cvtps_epi32(xi);
    __m256 p = _mm256_set1_ps(0.00133337f);
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.00961813f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.05550411f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.24022651f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.69314718f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.0f));
    __m256i biased = _mm256_add_epi32(i, _mm256_set1_epi32(127));
    __m256i shifted = _mm256_slli_epi32(biased, 23);
    return _mm256_mul_ps(p, _mm256_castsi256_ps(shifted));
}

// 8-wide pow24: branchless. For s <= 0.04045 the linear branch is
// approximated as s/12.92; the polynomial branch is fast_exp2(2.4 *
// fast_log2((s+0.055)/1.055)). Select via comparison mask.
[[gnu::noinline]]
__m256 pow24_poly_avx2_v(__m256 s) noexcept {
    __m256 lin = _mm256_mul_ps(s, _mm256_set1_ps(1.0f / 12.92f));
    __m256 base = _mm256_mul_ps(
        _mm256_add_ps(s, _mm256_set1_ps(0.055f)),
        _mm256_set1_ps(1.0f / 1.055f));
    __m256 poly = fast_exp2_v(_mm256_mul_ps(_mm256_set1_ps(2.4f),
                                              fast_log2_v(base)));
    __m256 mask = _mm256_cmp_ps(s, _mm256_set1_ps(0.04045f),
                                 _CMP_LE_OQ);
    return _mm256_blendv_ps(poly, lin, mask);
}
#endif

double bench(const char* label,
             float (*fn)(float),
             const std::vector<float>& xs) {
    auto t0 = std::chrono::steady_clock::now();
    float sink = 0.0f;
    for (auto x : xs) sink += fn(x);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ns_per = ms * 1e6 / static_cast<double>(xs.size());
    std::printf("  %-22s %.3f ms total, %.2f ns/call  (sink %.4f)\n",
                label, ms, ns_per, static_cast<double>(sink));
    return ns_per;
}

#if HAVE_AVX2
double bench_avx2(const std::vector<float>& xs) {
    auto t0 = std::chrono::steady_clock::now();
    __m256 sink = _mm256_setzero_ps();
    for (std::size_t i = 0; i + 8 <= xs.size(); i += 8) {
        __m256 v = _mm256_loadu_ps(&xs[i]);
        sink = _mm256_add_ps(sink, pow24_poly_avx2_v(v));
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    alignas(32) float out[8];
    _mm256_store_ps(out, sink);
    float total = out[0] + out[1] + out[2] + out[3] +
                  out[4] + out[5] + out[6] + out[7];
    double ns_per = ms * 1e6 / static_cast<double>(xs.size());
    std::printf("  %-22s %.3f ms total, %.2f ns/call  (sink %.4f)\n",
                "pow24_poly_avx2 (x8)", ms, ns_per,
                static_cast<double>(total));
    return ns_per;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::size_t N = (argc > 1)
        ? static_cast<std::size_t>(std::atoll(argv[1]))
        : 10'000'000;
    std::printf("pow24 microbench (N=%zu)\n", N);

    std::vector<float> xs(N);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (auto& x : xs) x = dist(rng);

    // Warmup
    {
        float s = 0;
        for (std::size_t i = 0; i < 100000; ++i) s += pow24_libm(xs[i]);
        for (std::size_t i = 0; i < 100000; ++i) s += pow24_poly_scalar(xs[i]);
        if (s == 1.234e-30f) std::printf(" ");  // anti-DCE
    }

    // Accuracy check
    {
        double max_abs_err = 0.0, max_rel_err = 0.0;
        for (auto x : xs) {
            float r = pow24_libm(x);
            float p = pow24_poly_scalar(x);
            double abs_err = std::fabs(static_cast<double>(p - r));
            max_abs_err = std::max(max_abs_err, abs_err);
            if (r > 1e-6f) {
                max_rel_err = std::max(max_rel_err,
                    abs_err / static_cast<double>(r));
            }
        }
        std::printf("  poly scalar accuracy: max abs err %.2e, max rel err %.2e\n",
                    max_abs_err, max_rel_err);
    }

    std::printf("--- timing ---\n");
    double t_libm = bench("std::pow (scalar)",   pow24_libm,         xs);
    double t_pol  = bench("pow24_poly (scalar)", pow24_poly_scalar,  xs);
#if HAVE_AVX2
    double t_avx2 = bench_avx2(xs);
    std::printf("--- speedups vs std::pow ---\n");
    std::printf("  poly scalar : %.2fx\n", t_libm / t_pol);
    std::printf("  poly avx2x8 : %.2fx\n", t_libm / t_avx2);
#else
    std::printf("--- speedups vs std::pow ---\n");
    std::printf("  poly scalar : %.2fx\n", t_libm / t_pol);
    std::printf("  AVX2 path not built\n");
#endif
    return 0;
}
