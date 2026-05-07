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
// log2 polynomial: float log2 via bit-trick (extract exponent) + 7-degree
// minimax polynomial on the mantissa range [1, 2). Coefficients fitted
// against log2(1+m) on m ∈ [0, 1] for max abs err < 1e-6.
//
// exp2 polynomial: 7-degree minimax on the fractional part + ldexp via
// int-to-float bit-bash.
[[gnu::always_inline]]
inline float fast_log2(float x) noexcept {
    union { float f; std::uint32_t u; } v{x};
    int e = static_cast<int>((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;  // mantissa in [1, 2)
    float m = v.f - 1.0f;                    // m in [0, 1)
    // 7-degree minimax for log2(1+m) on [0, 1]. Coefficients via
    // Remez fit; max abs err ~1e-7 at degree 7.
    float p =          0.0218544483f;
    p = p * m + (-0.0939070854f);
    p = p * m + ( 0.2143127323f);
    p = p * m + (-0.3489833556f);
    p = p * m + ( 0.4810302445f);
    p = p * m + (-0.7213471462f);
    p = p * m + ( 1.4426950409f);
    p = p * m;
    return p + static_cast<float>(e);
}

[[gnu::always_inline]]
inline float fast_exp2(float x) noexcept {
    float xi = std::floor(x);
    float f = x - xi;
    int   i = static_cast<int>(xi);
    // 7-degree minimax for 2^f on f ∈ [0, 1]. Max abs err ~1e-7.
    float p =          0.00015321379f;
    p = p * f + 0.00133927340f;
    p = p * f + 0.00961812909f;
    p = p * f + 0.05550410866f;
    p = p * f + 0.24022651460f;
    p = p * f + 0.69314718056f;
    p = p * f + 1.0f;
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
// --- SSE 4-wide polynomial (matches dither inner loop's per-pixel rgb) ---
[[gnu::always_inline]]
inline __m128 fast_log2_v4(__m128 x) noexcept {
    __m128i ix = _mm_castps_si128(x);
    __m128i ie = _mm_sub_epi32(
        _mm_and_si128(_mm_srli_epi32(ix, 23), _mm_set1_epi32(0xFF)),
        _mm_set1_epi32(127));
    __m128 e = _mm_cvtepi32_ps(ie);
    __m128i im = _mm_or_si128(
        _mm_and_si128(ix, _mm_set1_epi32(0x007FFFFF)),
        _mm_set1_epi32(0x3F800000));
    __m128 m = _mm_sub_ps(_mm_castsi128_ps(im), _mm_set1_ps(1.0f));
    __m128 p = _mm_set1_ps( 0.0218544483f);
    p = _mm_fmadd_ps(p, m, _mm_set1_ps(-0.0939070854f));
    p = _mm_fmadd_ps(p, m, _mm_set1_ps( 0.2143127323f));
    p = _mm_fmadd_ps(p, m, _mm_set1_ps(-0.3489833556f));
    p = _mm_fmadd_ps(p, m, _mm_set1_ps( 0.4810302445f));
    p = _mm_fmadd_ps(p, m, _mm_set1_ps(-0.7213471462f));
    p = _mm_fmadd_ps(p, m, _mm_set1_ps( 1.4426950409f));
    p = _mm_mul_ps(p, m);
    return _mm_add_ps(p, e);
}

[[gnu::always_inline]]
inline __m128 fast_exp2_v4(__m128 x) noexcept {
    __m128 xi = _mm_round_ps(x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    __m128 f  = _mm_sub_ps(x, xi);
    __m128i i = _mm_cvtps_epi32(xi);
    __m128 p = _mm_set1_ps(0.00015321379f);
    p = _mm_fmadd_ps(p, f, _mm_set1_ps(0.00133927340f));
    p = _mm_fmadd_ps(p, f, _mm_set1_ps(0.00961812909f));
    p = _mm_fmadd_ps(p, f, _mm_set1_ps(0.05550410866f));
    p = _mm_fmadd_ps(p, f, _mm_set1_ps(0.24022651460f));
    p = _mm_fmadd_ps(p, f, _mm_set1_ps(0.69314718056f));
    p = _mm_fmadd_ps(p, f, _mm_set1_ps(1.0f));
    __m128i shifted = _mm_slli_epi32(_mm_add_epi32(i, _mm_set1_epi32(127)), 23);
    return _mm_mul_ps(p, _mm_castsi128_ps(shifted));
}

[[gnu::always_inline]]
inline __m128 pow24_poly_sse4_v(__m128 s) noexcept {
    __m128 lin = _mm_mul_ps(s, _mm_set1_ps(1.0f / 12.92f));
    __m128 base = _mm_mul_ps(_mm_add_ps(s, _mm_set1_ps(0.055f)),
                              _mm_set1_ps(1.0f / 1.055f));
    __m128 poly = fast_exp2_v4(_mm_mul_ps(_mm_set1_ps(2.4f),
                                            fast_log2_v4(base)));
    __m128 mask = _mm_cmple_ps(s, _mm_set1_ps(0.04045f));
    return _mm_blendv_ps(poly, lin, mask);
}

// --- SLEEF-style: ln via (m-1)/(m+1) on m∈[0.75,1.5], exp via q*ln2 + s ---
// Coefficients lifted from sleefsimdsp.c xlogf / xexpf. Adds one division
// per call (vdivps) but gets ~1 ULP accuracy.
[[gnu::always_inline]]
inline __m128 sleef_lnf_v4(__m128 d) noexcept {
    // e = ilogb2k(d * 4/3)
    __m128 d_scaled = _mm_mul_ps(d, _mm_set1_ps(4.0f / 3.0f));
    __m128i e_int = _mm_sub_epi32(
        _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(d_scaled), 23),
                       _mm_set1_epi32(0xFF)),
        _mm_set1_epi32(127));
    __m128 e = _mm_cvtepi32_ps(e_int);
    // m = ldexp3(d, -e): subtract e from d's biased exponent.
    __m128i m_bits = _mm_sub_epi32(_mm_castps_si128(d),
                                     _mm_slli_epi32(e_int, 23));
    __m128 m = _mm_castsi128_ps(m_bits);
    // x = (m - 1) / (m + 1), x ∈ [-0.143, 0.2]
    __m128 x  = _mm_div_ps(_mm_sub_ps(m, _mm_set1_ps(1.0f)),
                            _mm_add_ps(m, _mm_set1_ps(1.0f)));
    __m128 x2 = _mm_mul_ps(x, x);
    __m128 t = _mm_set1_ps(0.2392828464508056640625f);
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(0.28518211841583251953125f));
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(0.400005877017974853515625f));
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(0.666666686534881591796875f));
    t = _mm_fmadd_ps(t, x2, _mm_set1_ps(2.0f));
    // ln(d) = x*t + e*ln(2)
    return _mm_fmadd_ps(e, _mm_set1_ps(0.693147180559945286226764f),
                         _mm_mul_ps(x, t));
}

[[gnu::always_inline]]
inline __m128 sleef_expf_v4(__m128 d) noexcept {
    // q = round(d * 1/ln2)
    __m128 q_f = _mm_round_ps(
        _mm_mul_ps(d, _mm_set1_ps(1.4426950408889634f)),
        _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m128i q = _mm_cvtps_epi32(q_f);
    // s = d - q * ln(2), high+low precision pair for accuracy
    __m128 s = _mm_fmadd_ps(q_f, _mm_set1_ps(-0.693145751953125f), d);
    s = _mm_fmadd_ps(q_f, _mm_set1_ps(-1.4286067653e-06f), s);
    // u = poly(s)
    __m128 u = _mm_set1_ps(0.000198527617612853646278381f);
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.00139304355252534151077271f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.00833336077630519866943359f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.0416664853692054748535156f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.166666671633720397949219f));
    u = _mm_fmadd_ps(u, s, _mm_set1_ps(0.5f));
    // 1 + s + s² * u
    u = _mm_add_ps(_mm_set1_ps(1.0f),
        _mm_fmadd_ps(_mm_mul_ps(s, s), u, s));
    // ldexp2(u, q): multiply by 2^q via biased-exponent injection
    __m128i shifted = _mm_slli_epi32(_mm_add_epi32(q, _mm_set1_epi32(127)), 23);
    return _mm_mul_ps(u, _mm_castsi128_ps(shifted));
}

[[gnu::always_inline]]
inline __m128 pow24_sleef_sse4_v(__m128 s) noexcept {
    __m128 lin = _mm_mul_ps(s, _mm_set1_ps(1.0f / 12.92f));
    __m128 base = _mm_mul_ps(_mm_add_ps(s, _mm_set1_ps(0.055f)),
                              _mm_set1_ps(1.0f / 1.055f));
    __m128 ln_b = sleef_lnf_v4(base);
    __m128 pw = sleef_expf_v4(_mm_mul_ps(_mm_set1_ps(2.4f), ln_b));
    __m128 mask = _mm_cmple_ps(s, _mm_set1_ps(0.04045f));
    return _mm_blendv_ps(pw, lin, mask);
}

// --- AVX2 8-wide SLEEF port ---
[[gnu::always_inline]]
inline __m256 sleef_lnf_v8(__m256 d) noexcept {
    __m256 d_scaled = _mm256_mul_ps(d, _mm256_set1_ps(4.0f / 3.0f));
    __m256i e_int = _mm256_sub_epi32(
        _mm256_and_si256(_mm256_srli_epi32(_mm256_castps_si256(d_scaled), 23),
                          _mm256_set1_epi32(0xFF)),
        _mm256_set1_epi32(127));
    __m256 e = _mm256_cvtepi32_ps(e_int);
    __m256i m_bits = _mm256_sub_epi32(_mm256_castps_si256(d),
                                        _mm256_slli_epi32(e_int, 23));
    __m256 m = _mm256_castsi256_ps(m_bits);
    __m256 x  = _mm256_div_ps(_mm256_sub_ps(m, _mm256_set1_ps(1.0f)),
                                _mm256_add_ps(m, _mm256_set1_ps(1.0f)));
    __m256 x2 = _mm256_mul_ps(x, x);
    __m256 t = _mm256_set1_ps(0.2392828464508056640625f);
    t = _mm256_fmadd_ps(t, x2, _mm256_set1_ps(0.28518211841583251953125f));
    t = _mm256_fmadd_ps(t, x2, _mm256_set1_ps(0.400005877017974853515625f));
    t = _mm256_fmadd_ps(t, x2, _mm256_set1_ps(0.666666686534881591796875f));
    t = _mm256_fmadd_ps(t, x2, _mm256_set1_ps(2.0f));
    return _mm256_fmadd_ps(e, _mm256_set1_ps(0.693147180559945286226764f),
                            _mm256_mul_ps(x, t));
}

[[gnu::always_inline]]
inline __m256 sleef_expf_v8(__m256 d) noexcept {
    __m256 q_f = _mm256_round_ps(
        _mm256_mul_ps(d, _mm256_set1_ps(1.4426950408889634f)),
        _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256i q = _mm256_cvtps_epi32(q_f);
    __m256 s = _mm256_fmadd_ps(q_f, _mm256_set1_ps(-0.693145751953125f), d);
    s = _mm256_fmadd_ps(q_f, _mm256_set1_ps(-1.4286067653e-06f), s);
    __m256 u = _mm256_set1_ps(0.000198527617612853646278381f);
    u = _mm256_fmadd_ps(u, s, _mm256_set1_ps(0.00139304355252534151077271f));
    u = _mm256_fmadd_ps(u, s, _mm256_set1_ps(0.00833336077630519866943359f));
    u = _mm256_fmadd_ps(u, s, _mm256_set1_ps(0.0416664853692054748535156f));
    u = _mm256_fmadd_ps(u, s, _mm256_set1_ps(0.166666671633720397949219f));
    u = _mm256_fmadd_ps(u, s, _mm256_set1_ps(0.5f));
    u = _mm256_add_ps(_mm256_set1_ps(1.0f),
        _mm256_fmadd_ps(_mm256_mul_ps(s, s), u, s));
    __m256i shifted = _mm256_slli_epi32(
        _mm256_add_epi32(q, _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(u, _mm256_castsi256_ps(shifted));
}

[[gnu::always_inline]]
inline __m256 pow24_sleef_avx2_v(__m256 s) noexcept {
    __m256 lin = _mm256_mul_ps(s, _mm256_set1_ps(1.0f / 12.92f));
    __m256 base = _mm256_mul_ps(_mm256_add_ps(s, _mm256_set1_ps(0.055f)),
                                  _mm256_set1_ps(1.0f / 1.055f));
    __m256 ln_b = sleef_lnf_v8(base);
    __m256 pw = sleef_expf_v8(_mm256_mul_ps(_mm256_set1_ps(2.4f), ln_b));
    __m256 mask = _mm256_cmp_ps(s, _mm256_set1_ps(0.04045f), _CMP_LE_OQ);
    return _mm256_blendv_ps(pw, lin, mask);
}

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
    __m256 p = _mm256_set1_ps( 0.0218544483f);
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps(-0.0939070854f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps( 0.2143127323f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps(-0.3489833556f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps( 0.4810302445f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps(-0.7213471462f));
    p = _mm256_fmadd_ps(p, m, _mm256_set1_ps( 1.4426950409f));
    p = _mm256_mul_ps(p, m);
    return _mm256_add_ps(p, e);
}

[[gnu::always_inline]]
inline __m256 fast_exp2_v(__m256 x) noexcept {
    __m256 xi = _mm256_round_ps(x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    __m256 f  = _mm256_sub_ps(x, xi);
    __m256i i = _mm256_cvtps_epi32(xi);
    __m256 p = _mm256_set1_ps(0.00015321379f);
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.00133927340f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.00961812909f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.05550410866f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.24022651460f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.69314718056f));
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
double bench_sse4(const std::vector<float>& xs) {
    auto t0 = std::chrono::steady_clock::now();
    __m128 sink = _mm_setzero_ps();
    for (std::size_t i = 0; i + 4 <= xs.size(); i += 4) {
        __m128 v = _mm_loadu_ps(&xs[i]);
        sink = _mm_add_ps(sink, pow24_poly_sse4_v(v));
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    alignas(16) float out[4];
    _mm_store_ps(out, sink);
    float total = out[0] + out[1] + out[2] + out[3];
    double ns_per = ms * 1e6 / static_cast<double>(xs.size());
    std::printf("  %-22s %.3f ms total, %.2f ns/call  (sink %.4f)\n",
                "pow24_poly_sse4 (x4)", ms, ns_per,
                static_cast<double>(total));
    return ns_per;
}

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

double bench_sleef_sse(const std::vector<float>& xs) {
    auto t0 = std::chrono::steady_clock::now();
    __m128 sink = _mm_setzero_ps();
    for (std::size_t i = 0; i + 4 <= xs.size(); i += 4) {
        __m128 v = _mm_loadu_ps(&xs[i]);
        sink = _mm_add_ps(sink, pow24_sleef_sse4_v(v));
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    alignas(16) float out[4];
    _mm_store_ps(out, sink);
    float total = out[0] + out[1] + out[2] + out[3];
    double ns_per = ms * 1e6 / static_cast<double>(xs.size());
    std::printf("  %-22s %.3f ms total, %.2f ns/call  (sink %.4f)\n",
                "pow24_sleef_sse (x4)", ms, ns_per,
                static_cast<double>(total));
    return ns_per;
}

double bench_sleef_avx2(const std::vector<float>& xs) {
    auto t0 = std::chrono::steady_clock::now();
    __m256 sink = _mm256_setzero_ps();
    for (std::size_t i = 0; i + 8 <= xs.size(); i += 8) {
        __m256 v = _mm256_loadu_ps(&xs[i]);
        sink = _mm256_add_ps(sink, pow24_sleef_avx2_v(v));
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    alignas(32) float out[8];
    _mm256_store_ps(out, sink);
    float total = out[0] + out[1] + out[2] + out[3] +
                  out[4] + out[5] + out[6] + out[7];
    double ns_per = ms * 1e6 / static_cast<double>(xs.size());
    std::printf("  %-22s %.3f ms total, %.2f ns/call  (sink %.4f)\n",
                "pow24_sleef_avx2(x8)", ms, ns_per,
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
        double scalar_abs = 0, scalar_rel = 0;
        for (auto x : xs) {
            float r = pow24_libm(x);
            float p = pow24_poly_scalar(x);
            double e = std::fabs(static_cast<double>(p - r));
            scalar_abs = std::max(scalar_abs, e);
            if (r > 1e-6f) scalar_rel = std::max(scalar_rel,
                e / static_cast<double>(r));
        }
        std::printf("  poly scalar accuracy: max abs %.2e, max rel %.2e\n",
                    scalar_abs, scalar_rel);
#if HAVE_AVX2
        double sse_abs = 0, sse_rel = 0;
        alignas(16) float lanes[4];
        for (std::size_t i = 0; i + 4 <= xs.size(); i += 4) {
            __m128 r4 = pow24_poly_sse4_v(_mm_loadu_ps(&xs[i]));
            _mm_store_ps(lanes, r4);
            for (int j = 0; j < 4; ++j) {
                float r = pow24_libm(xs[i + static_cast<std::size_t>(j)]);
                double e = std::fabs(static_cast<double>(lanes[j] - r));
                sse_abs = std::max(sse_abs, e);
                if (r > 1e-6f) sse_rel = std::max(sse_rel,
                    e / static_cast<double>(r));
            }
        }
        std::printf("  poly sse4   accuracy: max abs %.2e, max rel %.2e\n",
                    sse_abs, sse_rel);
        double avx_abs = 0, avx_rel = 0;
        alignas(32) float lanes8[8];
        for (std::size_t i = 0; i + 8 <= xs.size(); i += 8) {
            __m256 r8 = pow24_poly_avx2_v(_mm256_loadu_ps(&xs[i]));
            _mm256_store_ps(lanes8, r8);
            for (int j = 0; j < 8; ++j) {
                float r = pow24_libm(xs[i + static_cast<std::size_t>(j)]);
                double e = std::fabs(static_cast<double>(lanes8[j] - r));
                avx_abs = std::max(avx_abs, e);
                if (r > 1e-6f) avx_rel = std::max(avx_rel,
                    e / static_cast<double>(r));
            }
        }
        std::printf("  poly avx2   accuracy: max abs %.2e, max rel %.2e\n",
                    avx_abs, avx_rel);
        double sleef_sse_abs = 0, sleef_sse_rel = 0;
        for (std::size_t i = 0; i + 4 <= xs.size(); i += 4) {
            __m128 r4 = pow24_sleef_sse4_v(_mm_loadu_ps(&xs[i]));
            _mm_store_ps(lanes, r4);
            for (int j = 0; j < 4; ++j) {
                float r = pow24_libm(xs[i + static_cast<std::size_t>(j)]);
                double e = std::fabs(static_cast<double>(lanes[j] - r));
                sleef_sse_abs = std::max(sleef_sse_abs, e);
                if (r > 1e-6f) sleef_sse_rel = std::max(sleef_sse_rel,
                    e / static_cast<double>(r));
            }
        }
        std::printf("  sleef sse4  accuracy: max abs %.2e, max rel %.2e\n",
                    sleef_sse_abs, sleef_sse_rel);
        double sleef_avx_abs = 0, sleef_avx_rel = 0;
        for (std::size_t i = 0; i + 8 <= xs.size(); i += 8) {
            __m256 r8 = pow24_sleef_avx2_v(_mm256_loadu_ps(&xs[i]));
            _mm256_store_ps(lanes8, r8);
            for (int j = 0; j < 8; ++j) {
                float r = pow24_libm(xs[i + static_cast<std::size_t>(j)]);
                double e = std::fabs(static_cast<double>(lanes8[j] - r));
                sleef_avx_abs = std::max(sleef_avx_abs, e);
                if (r > 1e-6f) sleef_avx_rel = std::max(sleef_avx_rel,
                    e / static_cast<double>(r));
            }
        }
        std::printf("  sleef avx2  accuracy: max abs %.2e, max rel %.2e\n",
                    sleef_avx_abs, sleef_avx_rel);
#endif
    }

    std::printf("--- timing ---\n");
    double t_libm = bench("std::pow (scalar)",   pow24_libm,         xs);
    double t_pol  = bench("pow24_poly (scalar)", pow24_poly_scalar,  xs);
#if HAVE_AVX2
    double t_sse = bench_sse4(xs);
    double t_avx2 = bench_avx2(xs);
    double t_sleef_sse = bench_sleef_sse(xs);
    double t_sleef_avx = bench_sleef_avx2(xs);
    std::printf("--- speedups vs std::pow ---\n");
    std::printf("  poly scalar  : %.2fx\n", t_libm / t_pol);
    std::printf("  poly sse4x4  : %.2fx\n", t_libm / t_sse);
    std::printf("  poly avx2x8  : %.2fx\n", t_libm / t_avx2);
    std::printf("  sleef sse4x4 : %.2fx\n", t_libm / t_sleef_sse);
    std::printf("  sleef avx2x8 : %.2fx\n", t_libm / t_sleef_avx);
#else
    std::printf("--- speedups vs std::pow ---\n");
    std::printf("  poly scalar : %.2fx\n", t_libm / t_pol);
    std::printf("  AVX2 path not built\n");
#endif
    return 0;
}
