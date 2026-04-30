// Standalone audit: does linear_to_ocs's ±2-nibble seed-window search
// always return the same OCS code as a full 4096-candidate brute-force
// OKLab search?
//
// Both paths use std::cbrt (matches current production), so any
// disagreement is purely due to the seed window missing cases where the
// perceptually-nearest OCS code lies outside ±2 nibbles of the
// sRGB-rounded seed in some channel.
//
// Build: g++-15 -std=c++26 -O3 -march=native -fopenmp \
//        tools/seed_window_audit.cpp -o /tmp/seed_audit && /tmp/seed_audit

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <vector>

using f32x4 [[gnu::vector_size(16)]] = float;

static inline float srgb_to_linear_f(float s) noexcept {
    return s <= 0.04045f ? s / 12.92f
                         : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

static inline float linear_to_srgb_f(float l) noexcept {
    return l <= 0.0031308f ? l * 12.92f
                           : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

static inline f32x4 cbrt4(f32x4 x) noexcept {
    return f32x4{std::cbrt(x[0]), std::cbrt(x[1]), std::cbrt(x[2]), 0.0f};
}

struct OKLab { float L, a, b; };

[[gnu::always_inline]]
static inline OKLab linear_to_oklab(float r, float g, float b) noexcept {
    f32x4 lms =
        f32x4{0.4122214708f, 0.2119034982f, 0.0883024619f, 0.0f} * r +
        f32x4{0.5363325363f, 0.6806995451f, 0.2817188376f, 0.0f} * g +
        f32x4{0.0514459929f, 0.1073969566f, 0.6299787005f, 0.0f} * b;
    f32x4 lms_ = cbrt4(lms);
    return {
        0.2104542553f * lms_[0] + 0.7936177850f * lms_[1] - 0.0040720468f * lms_[2],
        1.9779984951f * lms_[0] - 2.4285922050f * lms_[1] + 0.4505937099f * lms_[2],
        0.0259040371f * lms_[0] + 0.7827717662f * lms_[1] - 0.8086757660f * lms_[2],
    };
}

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
        tab[code] = linear_to_oklab(r, g, b);
    }
    return tab;
}

// Production algorithm: ±2-nibble seed window (5×5×5 = 125 candidates).
[[gnu::always_inline]]
static inline std::uint16_t linear_to_ocs_seed(
        float r, float g, float b,
        const std::array<OKLab, 4096>& tab) noexcept {
    OKLab t = linear_to_oklab(r, g, b);
    float sr = std::clamp(linear_to_srgb_f(r), 0.0f, 1.0f);
    float sg = std::clamp(linear_to_srgb_f(g), 0.0f, 1.0f);
    float sb = std::clamp(linear_to_srgb_f(b), 0.0f, 1.0f);
    int seed_r = std::clamp(static_cast<int>(sr * 15.0f + 0.5f), 0, 15);
    int seed_g = std::clamp(static_cast<int>(sg * 15.0f + 0.5f), 0, 15);
    int seed_b = std::clamp(static_cast<int>(sb * 15.0f + 0.5f), 0, 15);
    constexpr int kRadius = 2;
    int rlo = std::max(0, seed_r - kRadius);
    int rhi = std::min(15, seed_r + kRadius);
    int glo = std::max(0, seed_g - kRadius);
    int ghi = std::min(15, seed_g + kRadius);
    int blo = std::max(0, seed_b - kRadius);
    int bhi = std::min(15, seed_b + kRadius);

    std::uint16_t best = 0;
    float best_d = std::numeric_limits<float>::infinity();
    for (int r4 = rlo; r4 <= rhi; ++r4) {
        for (int g4 = glo; g4 <= ghi; ++g4) {
            int row = (r4 << 8) | (g4 << 4);
            for (int b4 = blo; b4 <= bhi; ++b4) {
                auto code = static_cast<std::uint16_t>(row | b4);
                const OKLab& e = tab[code];
                float dL = t.L - e.L;
                float da = t.a - e.a;
                float db = t.b - e.b;
                float d = dL * dL + da * da + db * db;
                if (d < best_d) { best_d = d; best = code; }
            }
        }
    }
    return best;
}

// Reference: full brute force over all 4096 OCS codes.
[[gnu::always_inline]]
static inline std::uint16_t linear_to_ocs_full(
        float r, float g, float b,
        const std::array<OKLab, 4096>& tab) noexcept {
    OKLab t = linear_to_oklab(r, g, b);
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

// ---------------------------------------------------------------------------
// Per-seed candidate table.
//
// For each of the 4096 sRGB-rounded seed nibble triples we precompute the
// (small) set of OCS codes that can ever be the OKLab-nearest for any
// input whose seed is that triple. Built once by densely sweeping the
// sRGB cube and recording the actual nearest code for each sample.
//
// At runtime the search visits only those candidates — typically 1-4 per
// seed instead of 125 (±2 window) or 4096 (full brute force).
// ---------------------------------------------------------------------------

struct CandTab {
    std::vector<std::uint16_t>      codes;
    std::array<std::uint32_t, 4097> offsets;
};

static std::uint16_t nearest_ocs(
        const OKLab& t,
        const std::array<OKLab, 4096>& tab) noexcept {
    std::uint16_t best = 0;
    float         best_d = std::numeric_limits<float>::infinity();
    for (int code = 0; code < 4096; ++code) {
        const OKLab& e = tab[code];
        float dL = t.L - e.L, da = t.a - e.a, db = t.b - e.b;
        float d  = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best = static_cast<std::uint16_t>(code); }
    }
    return best;
}

static CandTab build_candidate_table(const std::array<OKLab, 4096>& tab,
                                     int N) {
    std::array<std::vector<std::uint16_t>, 4096> per_seed;

    #pragma omp parallel
    {
        std::array<std::vector<std::uint16_t>, 4096> local;

        #pragma omp for schedule(static) nowait
        for (int ir = 0; ir < N; ++ir) {
            float sr     = (ir + 0.5f) / N;
            float rl     = srgb_to_linear_f(sr);
            int   seed_r = std::clamp(static_cast<int>(sr * 15.0f + 0.5f),
                                      0, 15);
            for (int ig = 0; ig < N; ++ig) {
                float sg     = (ig + 0.5f) / N;
                float gl     = srgb_to_linear_f(sg);
                int   seed_g = std::clamp(static_cast<int>(sg * 15.0f + 0.5f),
                                          0, 15);
                for (int ib = 0; ib < N; ++ib) {
                    float sb     = (ib + 0.5f) / N;
                    float bl     = srgb_to_linear_f(sb);
                    int   seed_b = std::clamp(
                        static_cast<int>(sb * 15.0f + 0.5f), 0, 15);

                    int   seed = (seed_r << 8) | (seed_g << 4) | seed_b;
                    OKLab t    = linear_to_oklab(rl, gl, bl);
                    local[static_cast<std::size_t>(seed)].push_back(
                        nearest_ocs(t, tab));
                }
            }
        }

        #pragma omp critical
        for (int s = 0; s < 4096; ++s)
            per_seed[static_cast<std::size_t>(s)].insert(
                per_seed[static_cast<std::size_t>(s)].end(),
                local[static_cast<std::size_t>(s)].begin(),
                local[static_cast<std::size_t>(s)].end());
    }

    CandTab out;
    out.offsets[0] = 0;
    out.codes.reserve(4096 * 4);
    for (int s = 0; s < 4096; ++s) {
        auto& v = per_seed[static_cast<std::size_t>(s)];
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        for (auto c : v) out.codes.push_back(c);
        out.offsets[static_cast<std::size_t>(s + 1)] =
            static_cast<std::uint32_t>(out.codes.size());
    }
    return out;
}

[[gnu::always_inline]]
static inline std::uint16_t linear_to_ocs_exact(
        float r, float g, float b,
        const std::array<OKLab, 4096>& tab,
        const CandTab& ct) noexcept {
    OKLab t = linear_to_oklab(r, g, b);
    int sr = std::clamp(
        static_cast<int>(std::clamp(linear_to_srgb_f(r), 0.f, 1.f) * 15.f + 0.5f),
        0, 15);
    int sg = std::clamp(
        static_cast<int>(std::clamp(linear_to_srgb_f(g), 0.f, 1.f) * 15.f + 0.5f),
        0, 15);
    int sb = std::clamp(
        static_cast<int>(std::clamp(linear_to_srgb_f(b), 0.f, 1.f) * 15.f + 0.5f),
        0, 15);
    int seed = (sr << 8) | (sg << 4) | sb;

    std::uint16_t best = 0;
    float best_d = std::numeric_limits<float>::infinity();
    auto* p   = ct.codes.data() + ct.offsets[static_cast<std::size_t>(seed)];
    auto* end = ct.codes.data() + ct.offsets[static_cast<std::size_t>(seed + 1)];
    for (; p != end; ++p) {
        const OKLab& e = tab[*p];
        float dL = t.L - e.L, da = t.a - e.a, db = t.b - e.b;
        float d = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best = *p; }
    }
    return best;
}

int main() {
    auto tab = build_ocs_table();

    std::array<float, 256> lin{};
    for (int i = 0; i < 256; ++i) lin[i] = srgb_to_linear_f(i / 255.0f);

    constexpr std::size_t total = 256ull * 256ull * 256ull;
    std::printf("Total 8-bit sRGB inputs swept: %zu\n", total);

    std::atomic<std::size_t> mismatch{0};
    std::atomic<int> max_dr{0}, max_dg{0}, max_db{0}, max_sum{0};
    std::array<std::atomic<std::size_t>, 5> hist{};
    std::atomic<float> max_dist_excess{0.0f};

    // Capture up to N example mismatches with the largest nibble-sum drift,
    // for sanity-checking what kind of colors trigger the seed miss.
    struct Sample {
        int r8, g8, b8;
        std::uint16_t seed_code, full_code;
        float seed_d2, full_d2;
        int sum_delta;
    };
    constexpr std::size_t kMaxSamples = 16;
    std::vector<Sample> samples;
    std::mutex sample_mtx;

    auto bump_max_int = [](std::atomic<int>& a, int v) {
        int cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v,
                std::memory_order_relaxed)) {}
    };
    auto bump_max_f = [](std::atomic<float>& a, float v) {
        float cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v,
                std::memory_order_relaxed)) {}
    };

    #pragma omp parallel for collapse(2) schedule(static)
    for (int r8 = 0; r8 < 256; ++r8) {
        for (int g8 = 0; g8 < 256; ++g8) {
            float rl = lin[r8];
            float gl = lin[g8];
            std::size_t local_mismatch = 0;
            std::array<std::size_t, 5> local_hist{};
            int local_dr = 0, local_dg = 0, local_db = 0, local_sum = 0;
            float local_excess = 0.0f;
            for (int b8 = 0; b8 < 256; ++b8) {
                float bl = lin[b8];
                auto seed = linear_to_ocs_seed(rl, gl, bl, tab);
                auto full = linear_to_ocs_full(rl, gl, bl, tab);
                if (seed != full) {
                    ++local_mismatch;
                    int dr = std::abs(((seed >> 8) & 0xF) - ((full >> 8) & 0xF));
                    int dg = std::abs(((seed >> 4) & 0xF) - ((full >> 4) & 0xF));
                    int db = std::abs((seed & 0xF) - (full & 0xF));
                    int m = std::max({dr, dg, db});
                    int s = dr + dg + db;
                    if (m > 4) m = 4;
                    local_hist[static_cast<std::size_t>(m)]++;
                    if (dr > local_dr) local_dr = dr;
                    if (dg > local_dg) local_dg = dg;
                    if (db > local_db) local_db = db;
                    if (s > local_sum) local_sum = s;

                    OKLab t = linear_to_oklab(rl, gl, bl);
                    auto& es = tab[seed];
                    auto& ef = tab[full];
                    float ds2 = (t.L-es.L)*(t.L-es.L) +
                                (t.a-es.a)*(t.a-es.a) +
                                (t.b-es.b)*(t.b-es.b);
                    float df2 = (t.L-ef.L)*(t.L-ef.L) +
                                (t.a-ef.a)*(t.a-ef.a) +
                                (t.b-ef.b)*(t.b-ef.b);
                    float ex = ds2 - df2;
                    if (ex > local_excess) local_excess = ex;

                    if (s >= 2) {
                        std::lock_guard<std::mutex> lk(sample_mtx);
                        if (samples.size() < kMaxSamples ||
                            (!samples.empty() &&
                             s > samples.back().sum_delta)) {
                            samples.push_back({r8, g8, b8, seed, full,
                                                ds2, df2, s});
                            std::sort(samples.begin(), samples.end(),
                                [](const Sample& a, const Sample& b){
                                    return a.sum_delta > b.sum_delta;
                                });
                            if (samples.size() > kMaxSamples)
                                samples.resize(kMaxSamples);
                        }
                    }
                }
            }
            mismatch.fetch_add(local_mismatch, std::memory_order_relaxed);
            for (int i = 0; i < 5; ++i)
                hist[static_cast<std::size_t>(i)].fetch_add(
                    local_hist[static_cast<std::size_t>(i)],
                    std::memory_order_relaxed);
            bump_max_int(max_dr, local_dr);
            bump_max_int(max_dg, local_dg);
            bump_max_int(max_db, local_db);
            bump_max_int(max_sum, local_sum);
            bump_max_f(max_dist_excess, local_excess);
        }
    }

    auto M = mismatch.load();
    std::printf("\n=== seed-window (±2) vs full 4096 brute-force ===\n");
    std::printf("  Mismatches: %zu (%.6f%%)\n",
                M, 100.0 * static_cast<double>(M) / static_cast<double>(total));
    std::printf("  Worst nibble delta: dr=%d dg=%d db=%d (sum %d)\n",
                max_dr.load(), max_dg.load(), max_db.load(), max_sum.load());
    std::printf("  Worst OKLab d² penalty (seed - full): %.6e\n",
                static_cast<double>(max_dist_excess.load()));
    std::printf("  Histogram by max-channel nibble delta:\n");
    for (int i = 1; i <= 4; ++i)
        std::printf("    delta=%d: %zu\n", i,
                    hist[static_cast<std::size_t>(i)].load());

    if (!samples.empty()) {
        std::printf("\n  Worst-case examples (sRGB8 → seed_code / full_code, d²):\n");
        for (const auto& s : samples) {
            std::printf("    rgb8=(%3d,%3d,%3d) seed=0x%03X (d²=%.4e)  "
                        "full=0x%03X (d²=%.4e)  Δsum=%d\n",
                        s.r8, s.g8, s.b8, s.seed_code,
                        static_cast<double>(s.seed_d2),
                        s.full_code, static_cast<double>(s.full_d2),
                        s.sum_delta);
        }
    }

    // -----------------------------------------------------------------
    // Per-seed candidate table path.
    // -----------------------------------------------------------------
    auto build_one = [&](int N, const char* label) {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        CandTab ct = build_candidate_table(tab, N);
        auto t1 = clk::now();
        double bs = std::chrono::duration<double>(t1 - t0).count();

        std::size_t min_n = 4096, max_n = 0, total_n = 0, empty_n = 0;
        for (int s = 0; s < 4096; ++s) {
            auto n = ct.offsets[static_cast<std::size_t>(s + 1)] -
                     ct.offsets[static_cast<std::size_t>(s)];
            min_n = std::min<std::size_t>(min_n, n);
            max_n = std::max<std::size_t>(max_n, n);
            total_n += n;
            if (n == 0) ++empty_n;
        }
        std::printf("\n=== exact candidate table (sweep N=%d, %s) ===\n",
                    N, label);
        std::printf("  Build time: %.2fs\n", bs);
        std::printf("  Per-seed candidate count: min=%zu max=%zu mean=%.2f "
                    "(%zu empty seeds)\n",
                    min_n, max_n,
                    static_cast<double>(total_n) / 4096.0, empty_n);
        std::printf("  Total flat candidates: %zu (vs ±2-window: 4096*125=512000)\n",
                    total_n);

        std::atomic<std::size_t> mm{0};
        std::atomic<int> dr_max{0}, dg_max{0}, db_max{0}, sum_max{0};
        std::atomic<float> excess_max{0.0f};
        std::array<std::atomic<std::size_t>, 5> h{};

        #pragma omp parallel for collapse(2) schedule(static)
        for (int r8 = 0; r8 < 256; ++r8) {
            for (int g8 = 0; g8 < 256; ++g8) {
                float rl = lin[r8];
                float gl = lin[g8];
                std::size_t local_mm = 0;
                std::array<std::size_t, 5> local_h{};
                int local_dr = 0, local_dg = 0, local_db = 0, local_sum = 0;
                float local_ex = 0.0f;
                for (int b8 = 0; b8 < 256; ++b8) {
                    float bl = lin[b8];
                    auto exact = linear_to_ocs_exact(rl, gl, bl, tab, ct);
                    auto full  = linear_to_ocs_full (rl, gl, bl, tab);
                    if (exact != full) {
                        ++local_mm;
                        int dr = std::abs(((exact >> 8) & 0xF) - ((full >> 8) & 0xF));
                        int dg = std::abs(((exact >> 4) & 0xF) - ((full >> 4) & 0xF));
                        int db = std::abs((exact & 0xF) - (full & 0xF));
                        int m = std::max({dr, dg, db});
                        int sm = dr + dg + db;
                        if (m > 4) m = 4;
                        local_h[static_cast<std::size_t>(m)]++;
                        if (dr > local_dr) local_dr = dr;
                        if (dg > local_dg) local_dg = dg;
                        if (db > local_db) local_db = db;
                        if (sm > local_sum) local_sum = sm;

                        OKLab t = linear_to_oklab(rl, gl, bl);
                        auto& es = tab[exact];
                        auto& ef = tab[full];
                        float ds = (t.L-es.L)*(t.L-es.L) +
                                   (t.a-es.a)*(t.a-es.a) +
                                   (t.b-es.b)*(t.b-es.b);
                        float df = (t.L-ef.L)*(t.L-ef.L) +
                                   (t.a-ef.a)*(t.a-ef.a) +
                                   (t.b-ef.b)*(t.b-ef.b);
                        float ex = ds - df;
                        if (ex > local_ex) local_ex = ex;
                    }
                }
                mm.fetch_add(local_mm, std::memory_order_relaxed);
                for (int i = 0; i < 5; ++i)
                    h[static_cast<std::size_t>(i)].fetch_add(
                        local_h[static_cast<std::size_t>(i)],
                        std::memory_order_relaxed);
                bump_max_int(dr_max, local_dr);
                bump_max_int(dg_max, local_dg);
                bump_max_int(db_max, local_db);
                bump_max_int(sum_max, local_sum);
                bump_max_f(excess_max, local_ex);
            }
        }
        std::printf("  Mismatches: %zu (%.6f%%)\n",
                    mm.load(),
                    100.0 * static_cast<double>(mm.load()) /
                    static_cast<double>(total));
        if (mm.load() > 0) {
            std::printf("  Worst nibble delta: dr=%d dg=%d db=%d (sum %d)\n",
                        dr_max.load(), dg_max.load(), db_max.load(),
                        sum_max.load());
            std::printf("  Worst OKLab d² penalty: %.6e\n",
                        static_cast<double>(excess_max.load()));
            std::printf("  Histogram by max-channel nibble delta:\n");
            for (int i = 1; i <= 4; ++i)
                std::printf("    delta=%d: %zu\n", i,
                            h[static_cast<std::size_t>(i)].load());
        }
        return ct;
    };

    auto ct256  = build_one(256,  "default");
    auto ct512  = build_one(512,  "wider sweep");
    auto ct1024 = build_one(1024, "very wide sweep");

    // -----------------------------------------------------------------
    // Performance benchmark: each path on a fixed workload.
    // -----------------------------------------------------------------
    std::printf("\n=== Performance (single thread, %zu inputs) ===\n", total);
    auto bench = [&](auto&& fn, const char* label) {
        using clk = std::chrono::steady_clock;
        std::uint64_t sink = 0;
        auto t0 = clk::now();
        for (int r8 = 0; r8 < 256; ++r8) {
            float rl = lin[r8];
            for (int g8 = 0; g8 < 256; ++g8) {
                float gl = lin[g8];
                for (int b8 = 0; b8 < 256; ++b8)
                    sink += fn(rl, gl, lin[b8]);
            }
        }
        auto t1 = clk::now();
        volatile std::uint64_t v = sink; (void)v;
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::printf("  %-30s %.3fs  (%.0f ns/call)\n",
                    label, s, 1e9 * s / static_cast<double>(total));
        return s;
    };
    double ts = bench([&](float r, float g, float b){
        return linear_to_ocs_seed(r, g, b, tab); }, "seed window (±2, 125 cands)");
    double tf = bench([&](float r, float g, float b){
        return linear_to_ocs_full(r, g, b, tab); }, "full brute-force (4096)");
    double te = bench([&](float r, float g, float b){
        return linear_to_ocs_exact(r, g, b, tab, ct256); }, "exact table (N=256)");
    (void)ct512; (void)ct1024;
    std::printf("\n  exact table speedup vs seed:  %.2fx\n", ts / te);
    std::printf("  exact table speedup vs full:  %.2fx\n", tf / te);
    return 0;
}
