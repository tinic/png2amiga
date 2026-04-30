// Standalone audit: does the project's fast_cbrt cause linear_to_ocs to
// return a different OCS code than std::cbrt would?
//
// Replicates the relevant pieces from src/color_space.hpp + src/palette.hpp:
//   - sRGB -> linear (canonical)
//   - linear -> OKLab via LMS matrices, with cbrt pluggable (fast vs std)
//   - OcsOklabTable for all 4096 codes, built with each cbrt
//   - linear_to_ocs full 4096-candidate brute-force search
//
// Sweeps every 8-bit sRGB triple (16.7M colors), counts how many disagree
// between the fast-cbrt path and the std::cbrt path, and reports the worst
// per-nibble drift.
//
// Build: g++-15 -std=c++26 -O3 -march=native -fopenmp tools/cbrt_audit.cpp \
//        -o /tmp/cbrt_audit && /tmp/cbrt_audit

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

using f32x4 [[gnu::vector_size(16)]] = float;
using u32x4 [[gnu::vector_size(16)]] = std::uint32_t;

static inline float srgb_to_linear_f(float s) noexcept {
    return s <= 0.04045f ? s / 12.92f
                         : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

[[gnu::always_inline]]
static inline f32x4 fast_cbrt4(f32x4 x) noexcept {
    u32x4 sign_mask = {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u};
    u32x4 xbits = std::bit_cast<u32x4>(x);
    u32x4 sign = xbits & sign_mask;
    u32x4 absbits = xbits & ~sign_mask;
    u32x4 three = {3u, 3u, 3u, 3u};
    u32x4 off = {0x2a508bfeu, 0x2a508bfeu, 0x2a508bfeu, 0x2a508bfeu};
    u32x4 i = absbits / three + off;
    f32x4 absx = std::bit_cast<f32x4>(absbits);
    f32x4 y = std::bit_cast<f32x4>(i);
    f32x4 y3 = y * y * y;
    f32x4 two = {2.0f, 2.0f, 2.0f, 2.0f};
    y *= (y3 + two * absx) / (two * y3 + absx);
    u32x4 nonzero_mask = (absbits != 0);
    y = std::bit_cast<f32x4>(std::bit_cast<u32x4>(y) & nonzero_mask);
    return std::bit_cast<f32x4>(std::bit_cast<u32x4>(y) | sign);
}

// Same as fast_cbrt4 but applies TWO Halley iterations.
[[gnu::always_inline]]
static inline f32x4 fast_cbrt4_2step(f32x4 x) noexcept {
    u32x4 sign_mask = {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u};
    u32x4 xbits = std::bit_cast<u32x4>(x);
    u32x4 sign = xbits & sign_mask;
    u32x4 absbits = xbits & ~sign_mask;
    u32x4 three = {3u, 3u, 3u, 3u};
    u32x4 off = {0x2a508bfeu, 0x2a508bfeu, 0x2a508bfeu, 0x2a508bfeu};
    u32x4 i = absbits / three + off;
    f32x4 absx = std::bit_cast<f32x4>(absbits);
    f32x4 y = std::bit_cast<f32x4>(i);
    f32x4 two = {2.0f, 2.0f, 2.0f, 2.0f};
    f32x4 y3 = y * y * y;
    y *= (y3 + two * absx) / (two * y3 + absx);
    y3 = y * y * y;
    y *= (y3 + two * absx) / (two * y3 + absx);
    u32x4 nonzero_mask = (absbits != 0);
    y = std::bit_cast<f32x4>(std::bit_cast<u32x4>(y) & nonzero_mask);
    return std::bit_cast<f32x4>(std::bit_cast<u32x4>(y) | sign);
}

static inline f32x4 std_cbrt4(f32x4 x) noexcept {
    return f32x4{std::cbrt(x[0]), std::cbrt(x[1]), std::cbrt(x[2]), 0.0f};
}

struct OKLab { float L, a, b; };

template <auto Cbrt>
[[gnu::always_inline]]
static inline OKLab linear_to_oklab(float r, float g, float b) noexcept {
    f32x4 lms =
        f32x4{0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f} * r +
        f32x4{0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f} * g +
        f32x4{0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f} * b;
    f32x4 lms_ = Cbrt(lms);
    return {
        0.2104542553f * lms_[0] + 0.7936177850f * lms_[1] - 0.0040720468f * lms_[2],
        1.9779984951f * lms_[0] - 2.4285922050f * lms_[1] + 0.4505937099f * lms_[2],
        0.0259040371f * lms_[0] + 0.7827717662f * lms_[1] - 0.8086757660f * lms_[2],
    };
}

template <auto Cbrt>
static std::array<OKLab, 4096> build_ocs_table() {
    std::array<OKLab, 4096> tab{};
    for (int code = 0; code < 4096; ++code) {
        int r4 = (code >> 8) & 0xF;
        int g4 = (code >> 4) & 0xF;
        int b4 = code & 0xF;
        int r8 = (r4 << 4) | r4;
        int g8 = (g4 << 4) | g4;
        int b8 = (b4 << 4) | b4;
        float r = srgb_to_linear_f(r8 / 255.0f);
        float g = srgb_to_linear_f(g8 / 255.0f);
        float b = srgb_to_linear_f(b8 / 255.0f);
        tab[code] = linear_to_oklab<Cbrt>(r, g, b);
    }
    return tab;
}

template <auto Cbrt>
[[gnu::always_inline]]
static inline std::uint16_t linear_to_ocs(float r, float g, float b,
                                          const std::array<OKLab, 4096>& tab) noexcept {
    OKLab t = linear_to_oklab<Cbrt>(r, g, b);
    std::uint16_t best = 0;
    float best_d = std::numeric_limits<float>::infinity();
    for (int code = 0; code < 4096; ++code) {
        const OKLab& e = tab[code];
        float dL = t.L - e.L;
        float da = t.a - e.a;
        float db = t.b - e.b;
        float d = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best = static_cast<std::uint16_t>(code); }
    }
    return best;
}

struct Stats {
    std::atomic<std::size_t> mismatch{0};
    std::atomic<int> max_dr{0}, max_dg{0}, max_db{0}, max_sum{0};
    std::array<std::atomic<std::size_t>, 5> hist{};
};

static void bump_max(std::atomic<int>& a, int v) {
    int cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v,
            std::memory_order_relaxed)) {}
}

static void report(const char* label, std::size_t total, Stats& s) {
    auto M = s.mismatch.load();
    std::printf("\n=== %s vs std::cbrt ===\n", label);
    std::printf("  Mismatches: %zu (%.6f%%)\n",
                M, 100.0 * static_cast<double>(M) / static_cast<double>(total));
    std::printf("  Worst per-channel nibble delta: dr=%d dg=%d db=%d (sum %d)\n",
                s.max_dr.load(), s.max_dg.load(), s.max_db.load(),
                s.max_sum.load());
    std::printf("  Histogram by max-channel nibble delta:\n");
    for (int i = 1; i <= 4; ++i)
        std::printf("    delta=%d: %zu\n", i,
                    s.hist[static_cast<std::size_t>(i)].load());
}

template <auto Cbrt>
static void sweep_one(const char* label,
                      const std::array<float, 256>& lin,
                      const std::array<OKLab, 4096>& tab_test,
                      const std::array<OKLab, 4096>& tab_ref,
                      std::size_t total) {
    Stats s;
    #pragma omp parallel for collapse(2) schedule(static)
    for (int r8 = 0; r8 < 256; ++r8) {
        for (int g8 = 0; g8 < 256; ++g8) {
            float rl = lin[r8];
            float gl = lin[g8];
            std::size_t local_mismatch = 0;
            std::array<std::size_t, 5> local_hist{};
            int local_dr = 0, local_dg = 0, local_db = 0, local_sum = 0;
            for (int b8 = 0; b8 < 256; ++b8) {
                float bl = lin[b8];
                auto test = linear_to_ocs<Cbrt>(rl, gl, bl, tab_test);
                auto ref  = linear_to_ocs<std_cbrt4>(rl, gl, bl, tab_ref);
                if (test != ref) {
                    ++local_mismatch;
                    int dr = std::abs(((test >> 8) & 0xF) - ((ref >> 8) & 0xF));
                    int dg = std::abs(((test >> 4) & 0xF) - ((ref >> 4) & 0xF));
                    int db = std::abs((test & 0xF) - (ref & 0xF));
                    int m = std::max({dr, dg, db});
                    int t = dr + dg + db;
                    if (m > 4) m = 4;
                    local_hist[static_cast<std::size_t>(m)]++;
                    if (dr > local_dr) local_dr = dr;
                    if (dg > local_dg) local_dg = dg;
                    if (db > local_db) local_db = db;
                    if (t > local_sum) local_sum = t;
                }
            }
            s.mismatch.fetch_add(local_mismatch, std::memory_order_relaxed);
            for (int i = 0; i < 5; ++i)
                s.hist[static_cast<std::size_t>(i)].fetch_add(
                    local_hist[static_cast<std::size_t>(i)],
                    std::memory_order_relaxed);
            bump_max(s.max_dr, local_dr);
            bump_max(s.max_dg, local_dg);
            bump_max(s.max_db, local_db);
            bump_max(s.max_sum, local_sum);
        }
    }
    report(label, total, s);
}

// Per-input ULP error of the cbrt itself (no OKLab, no OCS): how many
// ULPs separate fast_cbrt's result from std::cbrt's result?
template <auto Cbrt>
static void sweep_cbrt_ulp(const char* label) {
    std::atomic<std::uint64_t> sum_ulp{0};
    std::atomic<std::uint32_t> max_ulp{0};
    std::atomic<std::size_t> exact{0};
    constexpr std::size_t N = 1u << 20;
    // Sweep cube-root inputs in [0, 1] with 2^20 evenly-spaced samples
    // across the relevant LMS range for sRGB-linear inputs.
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i <= N; ++i) {
        float x = static_cast<float>(i) / static_cast<float>(N);
        f32x4 v{x, x, x, 0.0f};
        f32x4 a = Cbrt(v);
        float ref = std::cbrt(x);
        std::uint32_t ai = std::bit_cast<std::uint32_t>(a[0]);
        std::uint32_t ri = std::bit_cast<std::uint32_t>(ref);
        std::uint32_t u = ai >= ri ? ai - ri : ri - ai;
        if (u == 0) exact.fetch_add(1, std::memory_order_relaxed);
        sum_ulp.fetch_add(u, std::memory_order_relaxed);
        std::uint32_t cur = max_ulp.load(std::memory_order_relaxed);
        while (u > cur && !max_ulp.compare_exchange_weak(cur, u,
                std::memory_order_relaxed)) {}
    }
    std::printf("\n--- %s scalar cbrt ULP error vs std::cbrt over [0,1], %zu samples ---\n",
                label, N + 1);
    std::printf("  Exact (0 ULP):  %zu (%.4f%%)\n",
                exact.load(),
                100.0 * static_cast<double>(exact.load()) /
                static_cast<double>(N + 1));
    std::printf("  Worst ULP error: %u\n", max_ulp.load());
    std::printf("  Mean ULP error:  %.3f\n",
                static_cast<double>(sum_ulp.load()) /
                static_cast<double>(N + 1));
}

// ---------------------------------------------------------------------------
// Performance benchmark: time a stream of linear_to_oklab() calls under each
// cbrt variant. Runs single-threaded, same workload, multiple repetitions
// so the OS settles. Uses a volatile sink to defeat dead-code elimination.
// ---------------------------------------------------------------------------

#include <chrono>

template <auto Cbrt>
[[gnu::noinline]]
static double bench_oklab(const std::array<float, 256>& lin,
                          std::size_t reps) {
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    float sink_L = 0.f, sink_a = 0.f, sink_b = 0.f;
    for (std::size_t rep = 0; rep < reps; ++rep) {
        for (int r8 = 0; r8 < 256; ++r8) {
            float rl = lin[r8];
            for (int g8 = 0; g8 < 256; ++g8) {
                float gl = lin[g8];
                for (int b8 = 0; b8 < 256; ++b8) {
                    float bl = lin[b8];
                    OKLab o = linear_to_oklab<Cbrt>(rl, gl, bl);
                    sink_L += o.L;
                    sink_a += o.a;
                    sink_b += o.b;
                }
            }
        }
    }
    auto t1 = clk::now();
    // Force the sink to be observed.
    volatile float v = sink_L + sink_a + sink_b;
    (void)v;
    return std::chrono::duration<double>(t1 - t0).count();
}

template <auto Cbrt>
[[gnu::noinline]]
static double bench_ocs(const std::array<float, 256>& lin,
                        const std::array<OKLab, 4096>& tab,
                        std::size_t reps) {
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    std::uint64_t sink = 0;
    for (std::size_t rep = 0; rep < reps; ++rep) {
        for (int r8 = 0; r8 < 256; ++r8) {
            float rl = lin[r8];
            for (int g8 = 0; g8 < 256; ++g8) {
                float gl = lin[g8];
                for (int b8 = 0; b8 < 256; ++b8) {
                    sink += linear_to_ocs<Cbrt>(rl, gl, lin[b8], tab);
                    (void)gl;
                }
            }
        }
    }
    auto t1 = clk::now();
    volatile std::uint64_t v = sink;
    (void)v;
    return std::chrono::duration<double>(t1 - t0).count();
}

int main() {
    auto tab_1step = build_ocs_table<fast_cbrt4>();
    auto tab_2step = build_ocs_table<fast_cbrt4_2step>();
    auto tab_prec  = build_ocs_table<std_cbrt4>();

    std::array<float, 256> lin{};
    for (int i = 0; i < 256; ++i) lin[i] = srgb_to_linear_f(i / 255.0f);

    constexpr std::size_t total = 256ull * 256ull * 256ull;
    std::printf("Total 8-bit sRGB inputs swept: %zu\n", total);

    sweep_cbrt_ulp<fast_cbrt4>("fast_cbrt4 (1 Halley)");
    sweep_cbrt_ulp<fast_cbrt4_2step>("fast_cbrt4 (2 Halley)");

    sweep_one<fast_cbrt4>("linear_to_ocs(1-step)", lin, tab_1step, tab_prec, total);
    sweep_one<fast_cbrt4_2step>("linear_to_ocs(2-step)", lin, tab_2step, tab_prec, total);

    // ---- perf benchmarks (single-threaded, fixed workload) ----
    std::printf("\n=== Performance benchmark (single thread, no OpenMP) ===\n");

    constexpr std::size_t reps_oklab = 4;
    std::printf("\nlinear_to_oklab on %zu inputs x %zu reps:\n",
                total, reps_oklab);
    // Warm-up
    (void)bench_oklab<fast_cbrt4>(lin, 1);
    double t_1 = bench_oklab<fast_cbrt4>(lin, reps_oklab);
    double t_2 = bench_oklab<fast_cbrt4_2step>(lin, reps_oklab);
    double t_s = bench_oklab<std_cbrt4>(lin, reps_oklab);
    double n = static_cast<double>(total * reps_oklab);
    std::printf("  1 Halley:  %.3fs  (%.2f ns/call,  %.1f Mops/s)\n",
                t_1, 1e9 * t_1 / n, 1e-6 * n / t_1);
    std::printf("  2 Halley:  %.3fs  (%.2f ns/call,  %.1f Mops/s)  +%.0f%%\n",
                t_2, 1e9 * t_2 / n, 1e-6 * n / t_2, 100.0 * (t_2 - t_1) / t_1);
    std::printf("  std::cbrt: %.3fs  (%.2f ns/call,  %.1f Mops/s)  +%.0f%%\n",
                t_s, 1e9 * t_s / n, 1e-6 * n / t_s, 100.0 * (t_s - t_1) / t_1);

    constexpr std::size_t reps_ocs = 1;
    std::printf("\nlinear_to_ocs (full 4096 search) on %zu inputs:\n", total);
    (void)bench_ocs<fast_cbrt4>(lin, tab_1step, 1);
    double o_1 = bench_ocs<fast_cbrt4>(lin, tab_1step, reps_ocs);
    double o_2 = bench_ocs<fast_cbrt4_2step>(lin, tab_2step, reps_ocs);
    double o_s = bench_ocs<std_cbrt4>(lin, tab_prec, reps_ocs);
    double m = static_cast<double>(total * reps_ocs);
    std::printf("  1 Halley:  %.3fs  (%.0f ns/call)\n",
                o_1, 1e9 * o_1 / m);
    std::printf("  2 Halley:  %.3fs  (%.0f ns/call)  +%.0f%%\n",
                o_2, 1e9 * o_2 / m, 100.0 * (o_2 - o_1) / o_1);
    std::printf("  std::cbrt: %.3fs  (%.0f ns/call)  +%.0f%%\n",
                o_s, 1e9 * o_s / m, 100.0 * (o_s - o_1) / o_1);

    return 0;
}
