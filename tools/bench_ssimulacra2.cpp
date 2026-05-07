// SSIMULACRA2 microbenchmark.
//
// Prints per-call wall-clock time for `ssimulacra2::compute` at the
// representative shootout size (320x213). Used to drive CPU-side
// profiling (AMD uProf on winbuilder, Apple Instruments on macOS) so
// we can identify the hot spots inside the metric.
//
// Usage:
//     ./bench_ssimulacra2                    # default 320x213, N=200
//     ./bench_ssimulacra2 640 480 100        # custom dims + iter count
#include "ssimulacra2.hpp"
#include "types.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace png2amiga;

int main(int argc, char** argv) {
    std::size_t W = (argc > 1) ? static_cast<std::size_t>(std::atoi(argv[1])) : 320;
    std::size_t H = (argc > 2) ? static_cast<std::size_t>(std::atoi(argv[2])) : 213;
    int         N = (argc > 3) ? std::atoi(argv[3])                          : 200;

    // Synth input: random noise (orig) + small additive perturbation
    // (distorted). Real-image content would yield a different score
    // but the per-pixel + per-scale workload shape is the same.
    std::vector<Color3f> orig(W * H), dist(W * H);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> r(0, 1), perturb(-0.02f, 0.02f);
    for (std::size_t i = 0; i < W * H; ++i) {
        orig[i] = {r(rng), r(rng), r(rng)};
        dist[i] = {std::clamp(orig[i].r + perturb(rng), 0.0f, 1.0f),
                   std::clamp(orig[i].g + perturb(rng), 0.0f, 1.0f),
                   std::clamp(orig[i].b + perturb(rng), 0.0f, 1.0f)};
    }

    // Warmup so the kernel + caches are hot.
    for (int i = 0; i < 5; ++i)
        (void)ssimulacra2::compute(orig, dist, W, H);

    auto t0 = std::chrono::steady_clock::now();
    float last = 0.0f;
    for (int i = 0; i < N; ++i)
        last = ssimulacra2::compute(orig, dist, W, H);
    auto t1 = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double per_call = total_ms / N;
    std::printf("SSIMULACRA2 @ %zux%zu, %d iters: %.3f ms/call (last score %.4f)\n",
                W, H, N, per_call, static_cast<double>(last));
    std::printf("  cost @ 161-trial --best inner loop: %.3f s\n",
                161.0 * per_call / 1000.0);
    std::printf("  cost @ 51200 trials (512-pop x 100-gen): %.1f s\n",
                51200.0 * per_call / 1000.0);
    return 0;
}
