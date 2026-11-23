// astc_kernel_test: structure tests of the ASTC encoder's tables and SIMD
// kernels (ctest "astc-kernels"). Output: key=value lines, exit code 0 when
// every check passes.
//
// Encoder output cannot be pinned by hash across compilers (floating point
// differs), so this tests what the speed work relies on instead:
//   - every SIMD kernel against its scalar reference (the same explicit
//     multiply-add, see astc_simd.hpp), bit for bit, on random input;
//   - the decimation solver table against a full elimination of the
//     augmented system;
//   - the duplicate-partition table against a direct comparison.

#include "astc_impl.inc"

#include <cstdio>
#include <cstring>
#include <vector>

namespace png2amiga::astc {
namespace {

struct Rng {
    std::uint32_t s = 0x2545F491u;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    // Uniform in [lo, hi), 24 random bits.
    float uniform(float lo, float hi) {
        return lo + (hi - lo) * (float(next() >> 8) * (1.f / 16777216.f));
    }
};

constexpr int kFootprints[14][2] = {{4, 4},  {5, 4},  {5, 5},  {6, 5},   {6, 6},
                                    {8, 5},  {8, 6},  {8, 8},  {10, 5},  {10, 6},
                                    {10, 8}, {10, 10}, {12, 10}, {12, 12}};

// SIMD arm == scalar arm for every length that mixes vector and tail lanes.
int check_axpy() {
    Rng rng;
    int bad = 0, cases = 0;
    for (int n = 0; n <= 70; ++n) {
        for (int rep = 0; rep < 50; ++rep) {
            float a[70], y0[70], ys[70], yv[70];
            for (int i = 0; i < 70; ++i) {
                a[i] = rng.uniform(-2.f, 2.f);
                y0[i] = rng.uniform(-64.f, 64.f);
            }
            if (rep % 5 == 0) a[rng.next() % 70] = 0.f;
            const float x = rep % 7 == 0 ? 0.f : rng.uniform(-64.f, 64.f);
            std::memcpy(ys, y0, sizeof(y0));
            std::memcpy(yv, y0, sizeof(y0));
            simd::axpy_sub_scalar(ys, a, x, n);
            simd::axpy_sub(yv, a, x, n);
            if (std::memcmp(ys, yv, sizeof(ys)) != 0) ++bad;
            ++cases;
        }
    }
    std::printf("axpy_cases=%d\naxpy_simd_vs_scalar_mismatch=%d\n", cases, bad);
    return bad;
}

// The elimination the template encoders ran per block before the solver
// table: normal matrix and right-hand side together.
void solve_reference(const BilinearTexel* texels, int tn, int gn, const float* w_ideal,
                     float* atb) {
    const auto n = std::size_t(gn);
    std::vector<float> ata(n * n, 0.f);
    for (std::size_t i = 0; i < n; ++i) atb[i] = 0.f;
    for (int j = 0; j < tn; ++j) {
        const BilinearTexel& t = texels[j];
        float T_j = w_ideal[j] * 64.f;
        for (int k = 0; k < 4; ++k) {
            float ck = float(t.weight[k]) * (1.f / 16.f);
            if (ck <= 0.f) continue;
            const std::size_t gi = t.grid[k];
            atb[gi] += ck * T_j;
            for (int kk = 0; kk < 4; ++kk) {
                float cc = float(t.weight[kk]) * (1.f / 16.f);
                if (cc <= 0.f) continue;
                ata[gi * n + t.grid[kk]] += ck * cc;
            }
        }
    }
    constexpr float kRidge = 1e-3f;
    for (std::size_t i = 0; i < n; ++i) ata[i * n + i] += kRidge;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t piv = i;
        float bestmag = std::abs(ata[i * n + i]);
        for (std::size_t r = i + 1; r < n; ++r) {
            float m = std::abs(ata[r * n + i]);
            if (m > bestmag) { bestmag = m; piv = r; }
        }
        if (bestmag < 1e-9f) continue;
        if (piv != i) {
            for (std::size_t c = 0; c < n; ++c) std::swap(ata[i * n + c], ata[piv * n + c]);
            std::swap(atb[i], atb[piv]);
        }
        float pinv = 1.f / ata[i * n + i];
        for (std::size_t c = 0; c < n; ++c) ata[i * n + c] *= pinv;
        atb[i] *= pinv;
        for (std::size_t r = 0; r < n; ++r) {
            if (r == i) continue;
            float f = ata[r * n + i];
            if (f == 0.f) continue;
            for (std::size_t c = 0; c < n; ++c) ata[r * n + c] -= f * ata[i * n + c];
            atb[r] -= f * atb[i];
        }
    }
}

// Solver table == full elimination, for every footprint and weight grid.
// Values must be equal; the sign of a zero may differ (see DecimSolve).
int check_decim_solve() {
    Rng rng;
    int bad = 0, grids = 0, zero_sign = 0;
    for (const auto& fp : kFootprints) {
        const int tw = fp[0], th = fp[1], tn = tw * th;
        for (int gw = 2; gw <= tw; ++gw)
            for (int gh = 2; gh <= th; ++gh) {
                const int gn = gw * gh;
                if (gn > 64) continue;
                const GridMapRT gm = build_grid_map(tw, th, gw, gh);
                BilinearTexel texels[kMaxTexels];
                for (int j = 0; j < tn; ++j)
                    for (int k = 0; k < 4; ++k) {
                        texels[j].grid[k] = gm.tg[j][k];
                        texels[j].weight[k] = gm.tc[j][k];
                    }
                const DecimSolve sv = build_decim_solve(texels, tn, gn);
                ++grids;
                for (int rep = 0; rep < 8; ++rep) {
                    float w_ideal[kMaxTexels], ref[64], atb[64] = {};
                    for (int j = 0; j < tn; ++j)
                        w_ideal[j] = rep == 0   ? 0.f
                                     : rep == 1 ? 1.f
                                     : rep == 2 ? float(j & 1)
                                                : rng.uniform(0.f, 1.f);
                    solve_reference(texels, tn, gn, w_ideal, ref);
                    for (int j = 0; j < tn; ++j) {
                        float T_j = w_ideal[j] * 64.f;
                        for (int k = 0; k < 4; ++k) {
                            float ck = float(texels[j].weight[k]) * (1.f / 16.f);
                            if (ck <= 0.f) continue;
                            atb[texels[j].grid[k]] += ck * T_j;
                        }
                    }
                    decim_solve_apply(sv, atb);
                    for (int i = 0; i < gn; ++i) {
                        if (!(atb[i] == ref[i])) ++bad;
                        else if (std::memcmp(&atb[i], &ref[i], sizeof(float)) != 0) ++zero_sign;
                    }
                }
            }
    }
    std::printf("decim_solve_grids=%d\ndecim_solve_mismatch=%d\ndecim_solve_zero_sign=%d\n",
                grids, bad, zero_sign);
    return bad;
}

template <int N, int TexW, int TexH>
int check_partition_table(int& distinct) {
    const auto& table = partition_table_np<N, TexW, TexH>();
    int bad = 0;
    for (int pi = 0; pi < 1024; ++pi) {
        int first = pi;
        for (int q = 0; q < pi; ++q)
            if (std::memcmp(table.assign[q], table.assign[pi], std::size_t(TexW * TexH)) == 0) {
                first = q;
                break;
            }
        if (table.first_same[pi] != first) ++bad;
        if (first == pi) ++distinct;
    }
    // same_partitions on a list with repeats.
    int pis[32], same[32];
    for (int k = 0; k < 32; ++k) pis[k] = (k * 37 + (k % 3 == 0 ? 0 : 11)) % 1024;
    pis[7] = pis[2];
    same_partitions<N, TexW, TexH>(pis, 32, same);
    for (int k = 0; k < 32; ++k) {
        int want = k;
        for (int q = 0; q < k; ++q)
            if (std::memcmp(table.assign[pis[q]], table.assign[pis[k]],
                            std::size_t(TexW * TexH)) == 0) {
                want = q;
                break;
            }
        if (same[k] != want) ++bad;
    }
    return bad;
}

int check_partitions() {
    int bad = 0, d = 0;
    bad += check_partition_table<2, 4, 4>(d);
    std::printf("partitions_distinct_2p_4x4=%d\n", d);
    d = 0;
    bad += check_partition_table<3, 4, 4>(d);
    std::printf("partitions_distinct_3p_4x4=%d\n", d);
    d = 0;
    bad += check_partition_table<2, 6, 6>(d);
    std::printf("partitions_distinct_2p_6x6=%d\n", d);
    d = 0;
    bad += check_partition_table<3, 8, 8>(d);
    std::printf("partitions_distinct_3p_8x8=%d\n", d);
    d = 0;
    bad += check_partition_table<2, 12, 12>(d);
    std::printf("partitions_distinct_2p_12x12=%d\n", d);
    std::printf("partition_table_mismatch=%d\n", bad);
    return bad;
}

}  // namespace
}  // namespace png2amiga::astc

int main() {
    using namespace png2amiga::astc;
    int bad = 0;
    bad += check_axpy();
    bad += check_decim_solve();
    bad += check_partitions();
    std::printf("simd_kernels=%d\nsimd_fused=%d\n", simd::kKernels ? 1 : 0, simd::kFused ? 1 : 0);
    std::printf("status=%s\n", bad == 0 ? "ok" : "fail");
    return bad == 0 ? 0 : 1;
}
