#include "ssimulacra2.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// SIMD backend selection — same gates as src/quantize.cpp.
// x86_64 → AVX2+FMA (256-bit, 8 lanes); AArch64 → NEON (128-bit, 4 lanes);
// Emscripten → WASM SIMD (128-bit, 4 lanes). No scalar fallback.
#if defined(__wasm_simd128__)
    #include <wasm_simd128.h>
    #define PNG2AMIGA_BACKEND_WASM_SIMD 1
    #define PNG2AMIGA_BACKEND_AVX2      0
    #define PNG2AMIGA_BACKEND_NEON      0
#elif defined(__AVX2__)
    #include <immintrin.h>
    #define PNG2AMIGA_BACKEND_AVX2      1
    #define PNG2AMIGA_BACKEND_WASM_SIMD 0
    #define PNG2AMIGA_BACKEND_NEON      0
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define PNG2AMIGA_BACKEND_NEON      1
    #define PNG2AMIGA_BACKEND_AVX2      0
    #define PNG2AMIGA_BACKEND_WASM_SIMD 0
#else
    #error "ssimulacra2.cpp requires AVX2 (x86), NEON (ARM64), or WASM SIMD."
#endif

namespace png2amiga::ssimulacra2 {

namespace {

// libjxl opsin transform: linear sRGB → mixed LMS → cube-root with bias →
// (X = (L'-M')/2, Y = (L'+M')/2, B = S').
// Constants pulled from libjxl/lib/jxl/opsin_params.cc; identical to
// what enc_xyb.cc applies before SSIMULACRA2 sees the data.
struct XYB {
    float x{}, y{}, b{};
};

constexpr float kBias = 0.0037930732552754493f;
inline float kBiasCbrt = std::cbrt(kBias);  // computed once at first use

inline XYB linear_to_xyb(const Color3f& c) noexcept {
    // L,M,S mix (libjxl OpsinAbsorbance) on linear sRGB.
    float L = 0.30f * c.r + 0.622f * c.g + 0.078f * c.b;
    float M = 0.23f * c.r + 0.692f * c.g + 0.078f * c.b;
    float S = 0.243422f * c.r + 0.204162f * c.g + 0.552416f * c.b;
    // cbrt with bias (perceptually uniform compression).
    float Lp = std::cbrt(L + kBias) - kBiasCbrt;
    float Mp = std::cbrt(M + kBias) - kBiasCbrt;
    float Sp = std::cbrt(S + kBias) - kBiasCbrt;
    return {(Lp - Mp) * 0.5f, (Lp + Mp) * 0.5f, Sp};
}

// Range-shift XYB to be non-negative and roughly 0..1 per channel —
// matches MakePositiveXYB in the reference.
inline void make_positive(XYB& v) noexcept {
    v.b = (v.b - v.y) + 0.55f;
    v.x = v.x * 14.f + 0.42f;
    v.y += 0.01f;
}

// Separable Gaussian blur, σ=1.5, half-width=6 (kernel size 13).
// Zero-pad boundary (out-of-bounds samples contribute 0 and are NOT
// renormalised), matching libjxl FastGaussian's "zero-pad boundary
// handling" — important because the EdgeDiff map's per-pixel
// (1+|x-mu|) ratio is sensitive to whether the local mean dips at the
// border, and the border-region edge maps carry some of the highest
// calibrated weights in the 108-weight aggregation. Mirror-edge here
// drifted internal vs external SSIMULACRA2 enough to flip the ranking
// of HAM6 + --best vs baseline on FS-dithered content.
constexpr int kBlurHalf = 6;
constexpr int kBlurSize = 2 * kBlurHalf + 1;

const std::array<float, kBlurSize>& blur_kernel() {
    static const std::array<float, kBlurSize> k = []() {
        std::array<float, kBlurSize> kernel{};
        constexpr float sigma = 1.5f;
        float sum = 0.0f;
        for (int i = 0; i < kBlurSize; ++i) {
            float x = static_cast<float>(i - kBlurHalf);
            kernel[static_cast<std::size_t>(i)] =
                std::exp(-0.5f * x * x / (sigma * sigma));
            sum += kernel[static_cast<std::size_t>(i)];
        }
        for (auto& v : kernel) v /= sum;
        return kernel;
    }();
    return k;
}

// Separable Gaussian, σ=1.5, kernel size 13. Hand-SIMDed (AVX2 8-lane,
// WASM SIMD 4-lane). Boundary handling: zero-pad with no renormalisation,
// matching libjxl FastGaussian semantics. The horizontal pass splits each
// row into left-boundary / interior / right-boundary so the interior runs
// branch-free at full SIMD throughput; the vertical pass is row-uniform
// (every output column at row y uses the same set of source rows) so the
// boundary check moves to the outer loop.
//
// `tmp` is a caller-owned scratch buffer (size ≥ w*h). It's threaded
// through so compute() can allocate it ONCE per call and reuse it
// across all scales/channels — AMD uProf showed 72% of CPU time was
// in ucrtbase.dll heap mgmt before this change, dominated by tmp's
// per-call vector<float>(w*h) allocation × 108 calls/compute().
void gaussian_blur(const std::vector<float>& in,
                   std::vector<float>& out,
                   std::vector<float>& tmp,
                   std::size_t w, std::size_t h) {
    auto& k = blur_kernel();
    if (tmp.size() < w * h) tmp.resize(w * h);
    if (out.size() < w * h) out.resize(w * h);
    const int W = static_cast<int>(w);
    const int H = static_cast<int>(h);

    // ---- Horizontal pass ----
    const std::size_t bx_lo = std::min<std::size_t>(kBlurHalf, w);
    const std::size_t bx_hi = (w >= static_cast<std::size_t>(kBlurHalf))
                                  ? (w - kBlurHalf) : bx_lo;
    for (std::size_t y = 0; y < h; ++y) {
        const float* row  = in.data()  + y * w;
        float*       trow = tmp.data() + y * w;
        // Left boundary: scalar with bounds check.
        for (std::size_t x = 0; x < bx_lo; ++x) {
            float s = 0.0f;
            for (int i = 0; i < kBlurSize; ++i) {
                int sx = static_cast<int>(x) + i - kBlurHalf;
                if (sx < 0 || sx >= W) continue;
                s += k[static_cast<std::size_t>(i)] *
                     row[static_cast<std::size_t>(sx)];
            }
            trow[x] = s;
        }
        // Interior: branch-free SIMD across 8 columns (or 4×2 for WASM).
        std::size_t x = bx_lo;
        const std::size_t simd_end = (bx_hi > bx_lo)
                                         ? (bx_lo + ((bx_hi - bx_lo) & ~7u))
                                         : bx_lo;
        for (; x < simd_end; x += 8) {
#if PNG2AMIGA_BACKEND_AVX2
            __m256 acc = _mm256_setzero_ps();
            for (int i = 0; i < kBlurSize; ++i) {
                __m256 kv = _mm256_set1_ps(k[static_cast<std::size_t>(i)]);
                __m256 v  = _mm256_loadu_ps(row + x + i - kBlurHalf);
                acc = _mm256_fmadd_ps(kv, v, acc);
            }
            _mm256_storeu_ps(trow + x, acc);
#elif PNG2AMIGA_BACKEND_NEON
            for (std::size_t lane = 0; lane < 8; lane += 4) {
                float32x4_t acc = vdupq_n_f32(0.0f);
                for (int i = 0; i < kBlurSize; ++i) {
                    float32x4_t kv = vdupq_n_f32(k[static_cast<std::size_t>(i)]);
                    float32x4_t v  = vld1q_f32(row + x + lane + i - kBlurHalf);
                    acc = vfmaq_f32(acc, kv, v);
                }
                vst1q_f32(trow + x + lane, acc);
            }
#else  // WASM SIMD
            for (std::size_t lane = 0; lane < 8; lane += 4) {
                v128_t acc = wasm_f32x4_const_splat(0.0f);
                for (int i = 0; i < kBlurSize; ++i) {
                    v128_t kv = wasm_f32x4_splat(k[static_cast<std::size_t>(i)]);
                    v128_t v  = wasm_v128_load(row + x + lane + i - kBlurHalf);
                    acc = wasm_f32x4_add(acc, wasm_f32x4_mul(kv, v));
                }
                wasm_v128_store(trow + x + lane, acc);
            }
#endif
        }
        // Interior tail (< 8 left).
        for (; x < bx_hi; ++x) {
            float s = 0.0f;
            for (int i = 0; i < kBlurSize; ++i) {
                s += k[static_cast<std::size_t>(i)] *
                     row[x + static_cast<std::size_t>(i)
                           - static_cast<std::size_t>(kBlurHalf)];
            }
            trow[x] = s;
        }
        // Right boundary.
        for (; x < w; ++x) {
            float s = 0.0f;
            for (int i = 0; i < kBlurSize; ++i) {
                int sx = static_cast<int>(x) + i - kBlurHalf;
                if (sx < 0 || sx >= W) continue;
                s += k[static_cast<std::size_t>(i)] *
                     row[static_cast<std::size_t>(sx)];
            }
            trow[x] = s;
        }
    }

    // ---- Vertical pass ----
    // No zero-init: the SIMD and tail loops below cover every output
    // cell. Skipping the wasted `out.assign(w*h, 0.0f)` was worth ~3%
    // wall on the 320×213 bench (one full memset per blur × 108
    // blurs/call).
    const std::size_t simd_w_end = w & ~7u;
    for (std::size_t y = 0; y < h; ++y) {
        const int sy_lo = static_cast<int>(y) - kBlurHalf;
        const int sy_hi = static_cast<int>(y) + kBlurHalf;
        const bool clip = (sy_lo < 0) || (sy_hi >= H);
        float* orow = out.data() + y * w;
        std::size_t x = 0;
        for (; x < simd_w_end; x += 8) {
#if PNG2AMIGA_BACKEND_AVX2
            __m256 acc = _mm256_setzero_ps();
            for (int i = 0; i < kBlurSize; ++i) {
                int sy = static_cast<int>(y) + i - kBlurHalf;
                if (clip && (sy < 0 || sy >= H)) continue;
                __m256 kv = _mm256_set1_ps(k[static_cast<std::size_t>(i)]);
                __m256 v  = _mm256_loadu_ps(tmp.data() +
                                static_cast<std::size_t>(sy) * w + x);
                acc = _mm256_fmadd_ps(kv, v, acc);
            }
            _mm256_storeu_ps(orow + x, acc);
#elif PNG2AMIGA_BACKEND_NEON
            for (std::size_t lane = 0; lane < 8; lane += 4) {
                float32x4_t acc = vdupq_n_f32(0.0f);
                for (int i = 0; i < kBlurSize; ++i) {
                    int sy = static_cast<int>(y) + i - kBlurHalf;
                    if (clip && (sy < 0 || sy >= H)) continue;
                    float32x4_t kv = vdupq_n_f32(k[static_cast<std::size_t>(i)]);
                    float32x4_t v  = vld1q_f32(tmp.data() +
                                static_cast<std::size_t>(sy) * w + x + lane);
                    acc = vfmaq_f32(acc, kv, v);
                }
                vst1q_f32(orow + x + lane, acc);
            }
#else  // WASM SIMD
            for (std::size_t lane = 0; lane < 8; lane += 4) {
                v128_t acc = wasm_f32x4_const_splat(0.0f);
                for (int i = 0; i < kBlurSize; ++i) {
                    int sy = static_cast<int>(y) + i - kBlurHalf;
                    if (clip && (sy < 0 || sy >= H)) continue;
                    v128_t kv = wasm_f32x4_splat(k[static_cast<std::size_t>(i)]);
                    v128_t v  = wasm_v128_load(tmp.data() +
                                static_cast<std::size_t>(sy) * w + x + lane);
                    acc = wasm_f32x4_add(acc, wasm_f32x4_mul(kv, v));
                }
                wasm_v128_store(orow + x + lane, acc);
            }
#endif
        }
        // Tail across x (< 8 columns left).
        for (; x < w; ++x) {
            float s = 0.0f;
            for (int i = 0; i < kBlurSize; ++i) {
                int sy = static_cast<int>(y) + i - kBlurHalf;
                if (clip && (sy < 0 || sy >= H)) continue;
                s += k[static_cast<std::size_t>(i)] *
                     tmp[static_cast<std::size_t>(sy) * w + x];
            }
            orow[x] = s;
        }
    }
}

// Box-average 2× downsample on linear-RGB planes. Matches reference
// Downsample(in, 2, 2) in linear space.
void downsample_2x_linear(const std::vector<Color3f>& src,
                          std::size_t sw, std::size_t sh,
                          std::vector<Color3f>& dst,
                          std::size_t& dw, std::size_t& dh) {
    dw = (sw + 1) / 2;
    dh = (sh + 1) / 2;
    if (dst.size() < dw * dh) dst.resize(dw * dh);
    // Fully overwritten by the loop below — skip zero-init.
    for (std::size_t y = 0; y < dh; ++y) {
        for (std::size_t x = 0; x < dw; ++x) {
            float r = 0, g = 0, b = 0;
            for (std::size_t iy = 0; iy < 2; ++iy) {
                for (std::size_t ix = 0; ix < 2; ++ix) {
                    auto sx = std::min(x * 2 + ix, sw - 1);
                    auto sy = std::min(y * 2 + iy, sh - 1);
                    auto& c = src[sy * sw + sx];
                    r += c.r; g += c.g; b += c.b;
                }
            }
            dst[y * dw + x] = {r * 0.25f, g * 0.25f, b * 0.25f};
        }
    }
}

// Build per-plane XYB float buffers (X, Y, B) from linear-RGB pixels.
// Caller pre-sizes X/Y/B (compute() arena). The loop below writes
// every cell, so no zero-init needed.
void to_xyb_planes(const std::vector<Color3f>& src,
                   std::size_t w, std::size_t h,
                   std::vector<float>& X, std::vector<float>& Y,
                   std::vector<float>& B) {
    auto n = w * h;
    if (X.size() < n) X.resize(n);
    if (Y.size() < n) Y.resize(n);
    if (B.size() < n) B.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto p = linear_to_xyb(src[i]);
        make_positive(p);
        X[i] = p.x;
        Y[i] = p.y;
        B[i] = p.b;
    }
}

inline double tothe4th(double x) noexcept {
    x *= x;
    x *= x;
    return x;
}

constexpr float kC2 = 0.0009f;

// SSIM' map (no double-gamma correction term, see reference comment).
// Returns {1-norm, 4-norm} aggregated over the plane.
struct AvgPair { double mean1; double mean4; };

AvgPair ssim_plane(const std::vector<float>& mu1,
                   const std::vector<float>& mu2,
                   const std::vector<float>& s11,
                   const std::vector<float>& s22,
                   const std::vector<float>& s12,
                   std::size_t w, std::size_t h) {
    double inv_n = 1.0 / static_cast<double>(w * h);
    double sum1 = 0.0, sum4 = 0.0;
    for (std::size_t i = 0; i < w * h; ++i) {
        float m1 = mu1[i], m2 = mu2[i];
        float m11 = m1 * m1, m22 = m2 * m2, m12 = m1 * m2;
        float num_m = 1.0f - (m1 - m2) * (m1 - m2);
        float num_s = 2.0f * (s12[i] - m12) + kC2;
        float denom_s = (s11[i] - m11) + (s22[i] - m22) + kC2;
        double d = 1.0 - static_cast<double>(num_m * num_s / denom_s);
        d = std::max(d, 0.0);
        sum1 += d;
        sum4 += tothe4th(d);
    }
    return {inv_n * sum1, std::sqrt(std::sqrt(inv_n * sum4))};
}

// Edge-diff: ringing (distorted has edge where original is smooth) and
// blurring (original has edge where distorted is smooth). Asymmetric.
struct EdgeDiff { AvgPair ring; AvgPair blur; };

EdgeDiff edgediff_plane(const std::vector<float>& img1,
                        const std::vector<float>& mu1,
                        const std::vector<float>& img2,
                        const std::vector<float>& mu2,
                        std::size_t w, std::size_t h) {
    double inv_n = 1.0 / static_cast<double>(w * h);
    double r1 = 0, r4 = 0, b1 = 0, b4 = 0;
    for (std::size_t i = 0; i < w * h; ++i) {
        double d1 = (1.0 + std::abs(static_cast<double>(img2[i] - mu2[i]))) /
                    (1.0 + std::abs(static_cast<double>(img1[i] - mu1[i]))) -
                    1.0;
        double artifact = std::max(d1, 0.0);
        r1 += artifact;
        r4 += tothe4th(artifact);
        double detail_lost = std::max(-d1, 0.0);
        b1 += detail_lost;
        b4 += tothe4th(detail_lost);
    }
    return {{inv_n * r1, std::sqrt(std::sqrt(inv_n * r4))},
            {inv_n * b1, std::sqrt(std::sqrt(inv_n * b4))}};
}

// Multiply two float planes elementwise. Caller pre-sizes `out` (we
// avoid the resize here to keep the per-call allocator pressure off
// the hot path — see compute_one_scale's scratch arena).
void multiply(const std::vector<float>& a,
              const std::vector<float>& b,
              std::vector<float>& out) {
    if (out.size() < a.size()) out.resize(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) out[i] = a[i] * b[i];
}

// One scale's 18 raw norms: 3 channels × {ssim, ringing, blurring} × {1n, 4n}.
struct ScaleScores {
    std::array<double, 6>  ssim_pair{};      // [c*2 + n] for c=0..2, n=0..1
    std::array<double, 12> edgediff_pair{};  // [c*4 + {ring1n,ring4n,blur1n,blur4n}]
};

// Working buffers for one compute() call. Pre-sized to the largest
// scale (scale 0 = source dims) so subsequent scales reuse the same
// allocation (smaller scales just use less of each buffer). Eliminates
// the per-call vector<float>(w*h) churn AMD uProf identified as 72%
// of CPU time on the EPYC AVX2 build.
struct Scratch {
    std::vector<float> mu1, mu2, s11, s22, s12, prod, tmp;
    void reserve_to(std::size_t n) {
        if (mu1.size() < n) mu1.resize(n);
        if (mu2.size() < n) mu2.resize(n);
        if (s11.size() < n) s11.resize(n);
        if (s22.size() < n) s22.resize(n);
        if (s12.size() < n) s12.resize(n);
        if (prod.size() < n) prod.resize(n);
        if (tmp.size()  < n) tmp.resize(n);
    }
};

ScaleScores compute_one_scale(const std::vector<float>& X1,
                              const std::vector<float>& Y1,
                              const std::vector<float>& B1,
                              const std::vector<float>& X2,
                              const std::vector<float>& Y2,
                              const std::vector<float>& B2,
                              std::size_t w, std::size_t h,
                              Scratch& sc) {
    ScaleScores out{};
    std::array<const std::vector<float>*, 3> p1{&X1, &Y1, &B1};
    std::array<const std::vector<float>*, 3> p2{&X2, &Y2, &B2};
    sc.reserve_to(w * h);
    for (std::size_t c = 0; c < 3; ++c) {
        auto& a = *p1[c];
        auto& b = *p2[c];
        gaussian_blur(a, sc.mu1, sc.tmp, w, h);
        gaussian_blur(b, sc.mu2, sc.tmp, w, h);
        multiply(a, a, sc.prod);
        gaussian_blur(sc.prod, sc.s11, sc.tmp, w, h);
        multiply(b, b, sc.prod);
        gaussian_blur(sc.prod, sc.s22, sc.tmp, w, h);
        multiply(a, b, sc.prod);
        gaussian_blur(sc.prod, sc.s12, sc.tmp, w, h);
        auto ssim = ssim_plane(sc.mu1, sc.mu2, sc.s11, sc.s22, sc.s12, w, h);
        out.ssim_pair[c * 2 + 0] = ssim.mean1;
        out.ssim_pair[c * 2 + 1] = ssim.mean4;
        auto ed = edgediff_plane(a, sc.mu1, b, sc.mu2, w, h);
        out.edgediff_pair[c * 4 + 0] = ed.ring.mean1;
        out.edgediff_pair[c * 4 + 1] = ed.ring.mean4;
        out.edgediff_pair[c * 4 + 2] = ed.blur.mean1;
        out.edgediff_pair[c * 4 + 3] = ed.blur.mean4;
    }
    return out;
}

// 108 calibrated weights from cloudinary/ssimulacra2 (April 2023).
// Layout: for each c (X,Y,B), for each scale (0..5), for each n (0,1):
//   weight[i++] *= |scale[scale].ssim_pair[c*2 + n]|
//   weight[i++] *= |scale[scale].edgediff_pair[c*4 + n]|     (ringing)
//   weight[i++] *= |scale[scale].edgediff_pair[c*4 + n + 2]| (blurring)
constexpr std::array<double, 108> kWeights{
    0.0,
    0.0007376606707406586,
    0.0,
    0.0,
    0.0007793481682867309,
    0.0,
    0.0,
    0.0004371155730107379,
    0.0,
    1.1041726426657346,
    0.00066284834129271,
    0.00015231632783718752,
    0.0,
    0.0016406437456599754,
    0.0,
    1.8422455520539298,
    11.441172603757666,
    0.0,
    0.0007989109436015163,
    0.000176816438078653,
    0.0,
    1.8787594979546387,
    10.94906990605142,
    0.0,
    0.0007289346991508072,
    0.9677937080626833,
    0.0,
    0.00014003424285435884,
    0.9981766977854967,
    0.00031949755934435053,
    0.0004550992113792063,
    0.0,
    0.0,
    0.0013648766163243398,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    7.466890328078848,
    0.0,
    17.445833984131262,
    0.0006235601634041466,
    0.0,
    0.0,
    6.683678146179332,
    0.00037724407979611296,
    1.027889937768264,
    225.20515300849274,
    0.0,
    0.0,
    19.213238186143016,
    0.0011401524586618361,
    0.001237755635509985,
    176.39317598450694,
    0.0,
    0.0,
    24.43300999870476,
    0.28520802612117757,
    0.0004485436923833408,
    0.0,
    0.0,
    0.0,
    34.77906344483772,
    44.835625328877896,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0008680556573291698,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0005313191874358747,
    0.0,
    0.00016533814161379112,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0004179171803251336,
    0.0017290828234722833,
    0.0,
    0.0020827005846636437,
    0.0,
    0.0,
    8.826982764996862,
    23.19243343998926,
    0.0,
    95.1080498811086,
    0.9863978034400682,
    0.9834382792465353,
    0.0012286405048278493,
    171.2667255897307,
    0.9807858872435379,
    0.0,
    0.0,
    0.0,
    0.0005130064588990679,
    0.0,
    0.00010854057858411537,
};

// Combine all-scale norms with the calibrated weights and apply the
// reference's cubic warp + power anchor.
double final_score(const std::vector<ScaleScores>& scales) {
    double ssim = 0.0;
    std::size_t i = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t s = 0; s < scales.size(); ++s) {
            for (std::size_t n = 0; n < 2; ++n) {
                ssim += kWeights[i++] *
                        std::abs(scales[s].ssim_pair[c * 2 + n]);
                ssim += kWeights[i++] *
                        std::abs(scales[s].edgediff_pair[c * 4 + n]);
                ssim += kWeights[i++] *
                        std::abs(scales[s].edgediff_pair[c * 4 + n + 2]);
            }
        }
    }
    ssim *= 0.9562382616834844;
    ssim = 2.326765642916932 * ssim
           - 0.020884521182843837 * ssim * ssim
           + 6.248496625763138e-05 * ssim * ssim * ssim;
    if (ssim > 0)
        ssim = 100.0 - 10.0 * std::pow(ssim, 0.6276336467831387);
    else
        ssim = 100.0;
    return ssim;
}

}  // namespace

float compute(std::span<const Color3f> orig,
              std::span<const Color3f> distorted,
              std::size_t width,
              std::size_t height) {
    auto n = width * height;
    if (n == 0 || orig.size() < n || distorted.size() < n) return 0.0f;

    // Reference loops 6 scales: 1:1, 1:2, 1:4, 1:8, 1:16, 1:32.
    // At each scale the input is downsampled in LINEAR sRGB, then converted
    // to XYB. We mirror that exactly.
    constexpr int kNumScales = 6;
    std::vector<Color3f> lin1(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(n));
    std::vector<Color3f> lin2(distorted.begin(),
                              distorted.begin() + static_cast<std::ptrdiff_t>(n));
    std::size_t w = width, h = height;
    std::vector<ScaleScores> per_scale;
    per_scale.reserve(kNumScales);
    // All scratch buffers live for the duration of compute(); each
    // scale reuses the same allocations. AMD uProf showed the original
    // implementation spent 72% of CPU in the heap allocator (every
    // gaussian_blur / multiply / to_xyb call freshly allocated a
    // 273KB-at-scale-0 vector<float>). The scratch arena drops that to
    // a one-shot allocation per call.
    Scratch sc;
    sc.reserve_to(w * h);
    std::vector<float> X1, Y1, B1, X2, Y2, B2;
    X1.resize(w * h); Y1.resize(w * h); B1.resize(w * h);
    X2.resize(w * h); Y2.resize(w * h); B2.resize(w * h);
    std::vector<Color3f> next1, next2;
    for (int scale = 0; scale < kNumScales; ++scale) {
        if (w < 8 || h < 8) break;
        if (scale > 0) {
            std::size_t nw = 0, nh = 0;
            downsample_2x_linear(lin1, w, h, next1, nw, nh);
            downsample_2x_linear(lin2, w, h, next2, nw, nh);
            lin1.swap(next1);
            lin2.swap(next2);
            w = nw;
            h = nh;
        }
        to_xyb_planes(lin1, w, h, X1, Y1, B1);
        to_xyb_planes(lin2, w, h, X2, Y2, B2);
        per_scale.push_back(compute_one_scale(X1, Y1, B1, X2, Y2, B2, w, h, sc));
    }
    return static_cast<float>(final_score(per_scale));
}

}  // namespace png2amiga::ssimulacra2
