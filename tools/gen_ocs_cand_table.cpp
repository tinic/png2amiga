// One-shot generator for the per-seed OCS candidate table.
//
// For each of the 4096 sRGB-rounded seed nibble triples we emit the small
// list of 12-bit OCS codes that can ever be the OKLab-nearest for any
// input whose seed is that triple. The list is found by densely sweeping
// the sRGB cube at N=1024 samples per channel and recording the actual
// OKLab-nearest code (using std::cbrt for exactness).
//
// Output: src/ocs_cand_table.cpp (auto-generated, do not edit by hand).
//
// Build/run: g++-15 -std=c++26 -O3 -march=native -fopenmp \
//   tools/gen_ocs_cand_table.cpp -o /tmp/gen_ocs && /tmp/gen_ocs
//
// Generation takes ~10–15 minutes single-machine (parallel). The exact
// audit (tools/seed_window_audit.cpp) verifies zero mismatches across all
// 16.7M 8-bit sRGB inputs at N=1024.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <vector>

using f32x4 [[gnu::vector_size(16)]] = float;

static inline float srgb_to_linear_f(float s) noexcept {
    return s <= 0.04045f ? s / 12.92f
                         : std::pow((s + 0.055f) / 1.055f, 2.4f);
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
        tab[static_cast<std::size_t>(code)] = linear_to_oklab(r, g, b);
    }
    return tab;
}

static std::uint16_t nearest_ocs(const OKLab& t,
                                 const std::array<OKLab, 4096>& tab) noexcept {
    std::uint16_t best = 0;
    float best_d = std::numeric_limits<float>::infinity();
    for (int code = 0; code < 4096; ++code) {
        const OKLab& e = tab[static_cast<std::size_t>(code)];
        float dL = t.L - e.L, da = t.a - e.a, db = t.b - e.b;
        float d  = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best = static_cast<std::uint16_t>(code); }
    }
    return best;
}

int main(int argc, char** argv) {
    int N = 1024;
    if (argc > 1) N = std::atoi(argv[1]);
    const char* out_path = argc > 2 ? argv[2]
                                    : "src/ocs_cand_table.cpp";
    std::printf("Building OCS candidate table at N=%d -> %s\n", N, out_path);

    auto t0 = std::chrono::steady_clock::now();
    auto tab = build_ocs_table();

    std::array<std::vector<std::uint16_t>, 4096> per_seed;
    #pragma omp parallel
    {
        std::array<std::vector<std::uint16_t>, 4096> local;
        #pragma omp for schedule(static) nowait
        for (int ir = 0; ir < N; ++ir) {
            float sr = (ir + 0.5f) / static_cast<float>(N);
            float rl = srgb_to_linear_f(sr);
            int seed_r = std::clamp(static_cast<int>(sr * 15.0f + 0.5f), 0, 15);
            for (int ig = 0; ig < N; ++ig) {
                float sg = (ig + 0.5f) / static_cast<float>(N);
                float gl = srgb_to_linear_f(sg);
                int seed_g = std::clamp(static_cast<int>(sg * 15.0f + 0.5f), 0, 15);
                for (int ib = 0; ib < N; ++ib) {
                    float sb = (ib + 0.5f) / static_cast<float>(N);
                    float bl = srgb_to_linear_f(sb);
                    int seed_b = std::clamp(
                        static_cast<int>(sb * 15.0f + 0.5f), 0, 15);
                    int seed = (seed_r << 8) | (seed_g << 4) | seed_b;
                    OKLab t = linear_to_oklab(rl, gl, bl);
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

    std::vector<std::uint16_t> codes;
    std::array<std::uint32_t, 4097> offsets{};
    codes.reserve(4096 * 16);
    for (int s = 0; s < 4096; ++s) {
        auto& v = per_seed[static_cast<std::size_t>(s)];
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        for (auto c : v) codes.push_back(c);
        offsets[static_cast<std::size_t>(s + 1)] =
            static_cast<std::uint32_t>(codes.size());
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("Built table in %.1fs: %zu codes, max-per-seed=%u\n",
                secs, codes.size(),
                *std::max_element(offsets.begin() + 1, offsets.end()) -
                *std::max_element(offsets.begin(), offsets.end() - 1));

    std::ofstream out(out_path);
    if (!out) { std::perror(out_path); return 1; }
    out << "// AUTO-GENERATED by tools/gen_ocs_cand_table.cpp at N="
        << N << ". DO NOT EDIT.\n";
    out << "// Per-seed OCS candidate table. Verified by\n";
    out << "// tools/seed_window_audit.cpp: zero mismatches across all 16.7M\n";
    out << "// 8-bit sRGB inputs vs full 4096-candidate brute-force search.\n";
    out << "//\n";
    out << "// Layout: codes for seed s live in [offsets[s], offsets[s+1]).\n";
    out << "// Total codes: " << codes.size() << ", offsets: 4097.\n\n";
    out << "#include \"palette.hpp\"\n\n";
    out << "namespace png2amiga::palette::detail {\n\n";

    out << "constexpr std::uint16_t kOcsCandCodes["
        << codes.size() << "] = {\n";
    for (std::size_t i = 0; i < codes.size(); ++i) {
        if ((i % 16) == 0) out << "    ";
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%03X", codes[i]);
        out << buf;
        if (i + 1 != codes.size()) out << ",";
        if ((i % 16) == 15 || i + 1 == codes.size()) out << "\n";
        else out << " ";
    }
    out << "};\n\n";

    out << "constexpr std::uint32_t kOcsCandOffsets[4097] = {\n";
    for (std::size_t i = 0; i < 4097; ++i) {
        if ((i % 12) == 0) out << "    ";
        out << offsets[i];
        if (i + 1 != 4097) out << ",";
        if ((i % 12) == 11 || i + 1 == 4097) out << "\n";
        else out << " ";
    }
    out << "};\n\n";

    out << "const OcsCandTable& ocs_cand_table() noexcept {\n";
    out << "    static constexpr OcsCandTable t{kOcsCandOffsets,\n";
    out << "                                    kOcsCandCodes,\n";
    out << "                                    " << codes.size() << "};\n";
    out << "    return t;\n";
    out << "}\n\n";
    out << "}  // namespace png2amiga::palette::detail\n";
    out.close();
    std::printf("Wrote %s\n", out_path);
    return 0;
}
