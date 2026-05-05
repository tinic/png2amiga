// GPU palette-quantization experiment — Stage A.
//
// Loads an RGB PNG, converts to OKLab on CPU, runs R parallel-restart
// Lloyd k-means runs on Apple GPU (one restart per dispatch loop, max
// kIters iterations each), picks the restart with lowest OKLab SSE,
// converts the winning palette back to sRGB, writes a quantized RGB
// PNG (rendered = palette[indices], not indexed-PNG — the bench
// scores quality via png2amiga --score-vs which doesn't care about
// the PNG storage format).
//
// Usage:
//   ./quant <input.png> <output.png> [--colors 256] [--restarts 32]
//                                    [--iters 20]
//
// Build: ./build.sh

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include "../../third_party/stb_image.h"
#include "../../third_party/stb_image_write.h"

#include "colorspace.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string in_path;
    std::string out_path;
    int colors = 256;
    int restarts = 32;
    int iters = 20;           // Lloyd iters
    int scolorq_iters = 0;    // 0 = pure-Lloyd (Stage A); >0 = Stage B
    int width = 0;            // overridden after image load
    int height = 0;
    std::uint32_t seed = 0xC0FFEEu;
};

[[nodiscard]] bool parse_args(int argc, char** argv, Args& a) {
    if (argc < 3) return false;
    a.in_path  = argv[1];
    a.out_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](int& dst) {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs value\n", s.c_str()); std::exit(2); }
            dst = std::atoi(argv[++i]);
        };
        if (s == "--colors")        next(a.colors);
        else if (s == "--restarts") next(a.restarts);
        else if (s == "--iters")    next(a.iters);
        else if (s == "--scolorq")  next(a.scolorq_iters);
        else if (s == "--seed") {
            if (i + 1 >= argc) std::exit(2);
            a.seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", s.c_str());
            return false;
        }
    }
    return true;
}

// k-means++ initial seeding: pick first centroid uniformly at random,
// each subsequent one with probability proportional to its squared
// distance from the nearest already-chosen centroid.
//
// Runs on a uniform random subsample (~kSampleSize pixels) — full
// image isn't needed and the O(K * N) loop dominates init time at
// 1.5 MP × 256 K. The downstream Lloyd loop then refines on the
// full image, so the only cost of subsampling is in the seed quality.
constexpr std::size_t kInitSampleSize = 8192;

std::vector<expq::OKLab> kmeanspp_init(
    std::span<const expq::OKLab> pixels, int K, std::mt19937& rng)
{
    std::vector<expq::OKLab> sample;
    if (pixels.size() <= kInitSampleSize) {
        sample.assign(pixels.begin(), pixels.end());
    } else {
        sample.reserve(kInitSampleSize);
        std::uniform_int_distribution<std::size_t> pick(0, pixels.size() - 1);
        for (std::size_t i = 0; i < kInitSampleSize; ++i)
            sample.push_back(pixels[pick(rng)]);
    }

    std::vector<expq::OKLab> out;
    out.reserve(K);

    std::uniform_int_distribution<std::size_t> first(0, sample.size() - 1);
    out.push_back(sample[first(rng)]);

    std::vector<float> nearest_d2(sample.size(), 1e30f);

    while (int(out.size()) < K) {
        const auto& last = out.back();
        for (std::size_t i = 0; i < sample.size(); ++i) {
            float d = expq::dist_sq(sample[i], last);
            if (d < nearest_d2[i]) nearest_d2[i] = d;
        }
        std::discrete_distribution<std::size_t> pick(
            nearest_d2.begin(), nearest_d2.end());
        out.push_back(sample[pick(rng)]);
    }
    return out;
}

[[nodiscard]] int run(const Args& args) {
    // -------- Load PNG (RGB f32) ----------
    int w = 0, h = 0, nch = 0;
    unsigned char* raw = stbi_load(args.in_path.c_str(), &w, &h, &nch, 3);
    if (!raw) {
        std::fprintf(stderr, "stbi_load failed: %s\n", args.in_path.c_str());
        return 1;
    }
    const std::size_t N = std::size_t(w) * std::size_t(h);
    const_cast<Args&>(args).width  = w;
    const_cast<Args&>(args).height = h;

    // float4 (xyz=L,a,b; w=pad) for GPU buffer alignment
    std::vector<expq::OKLab> oklab(N);
    for (std::size_t i = 0; i < N; ++i) {
        auto rgb = expq::srgb_u8_to_linear(raw[i*3+0], raw[i*3+1], raw[i*3+2]);
        oklab[i] = expq::linear_to_oklab(rgb);
    }

    // -------- Metal init ----------
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) { std::fprintf(stderr, "no Metal device\n"); return 1; }
    std::printf("device: %s\n", device->name()->utf8String());

    NS::Error* err = nullptr;
    auto path = NS::String::string(
        "experiments/gpu_quantize/quant.metallib", NS::UTF8StringEncoding);
    MTL::Library* lib = device->newLibrary(path, &err);
    if (!lib) {
        std::fprintf(stderr, "newLibrary: %s\n",
                     err ? err->localizedDescription()->utf8String() : "?");
        return 1;
    }
    auto fn1_name = NS::String::string("assign_and_accumulate", NS::UTF8StringEncoding);
    auto fn2_name = NS::String::string("finalize_centroids", NS::UTF8StringEncoding);
    auto fn3_name = NS::String::string("scolorq_filtered_output", NS::UTF8StringEncoding);
    auto fn4_name = NS::String::string("scolorq_compute_g", NS::UTF8StringEncoding);
    auto fn5_name = NS::String::string("scolorq_assign_only", NS::UTF8StringEncoding);
    auto fn6_name = NS::String::string("scolorq_build_Mb", NS::UTF8StringEncoding);
    MTL::ComputePipelineState* pso_assign =
        device->newComputePipelineState(lib->newFunction(fn1_name), &err);
    MTL::ComputePipelineState* pso_finalize =
        device->newComputePipelineState(lib->newFunction(fn2_name), &err);
    MTL::ComputePipelineState* pso_sc_filt =
        device->newComputePipelineState(lib->newFunction(fn3_name), &err);
    MTL::ComputePipelineState* pso_sc_g =
        device->newComputePipelineState(lib->newFunction(fn4_name), &err);
    MTL::ComputePipelineState* pso_sc_assign =
        device->newComputePipelineState(lib->newFunction(fn5_name), &err);
    MTL::ComputePipelineState* pso_sc_build =
        device->newComputePipelineState(lib->newFunction(fn6_name), &err);
    if (!pso_assign || !pso_finalize || !pso_sc_filt
        || !pso_sc_g || !pso_sc_assign || !pso_sc_build) {
        std::fprintf(stderr, "PSO build failed: %s\n",
                     err ? err->localizedDescription()->utf8String() : "?");
        return 1;
    }
    MTL::CommandQueue* queue = device->newCommandQueue();

    const int K = args.colors;
    const int R = args.restarts;

    // -------- GPU buffers ----------
    // pixels: float4 per pixel
    auto* pixel_buf = device->newBuffer(N * 4 * sizeof(float),
                                         MTL::ResourceStorageModeShared);
    auto* px = static_cast<float*>(pixel_buf->contents());
    for (std::size_t i = 0; i < N; ++i) {
        px[i*4+0] = oklab[i].L;
        px[i*4+1] = oklab[i].a;
        px[i*4+2] = oklab[i].b;
        px[i*4+3] = 0.0f;
    }

    auto* cent_buf   = device->newBuffer(R * K * 4 * sizeof(float),
                                          MTL::ResourceStorageModeShared);
    auto* sums_buf   = device->newBuffer(R * K * 3 * sizeof(float),
                                          MTL::ResourceStorageModeShared);
    auto* counts_buf = device->newBuffer(R * K * sizeof(unsigned),
                                          MTL::ResourceStorageModeShared);
    auto* sse_buf    = device->newBuffer(R * sizeof(float),
                                          MTL::ResourceStorageModeShared);
    auto* idx_buf    = device->newBuffer(R * N * sizeof(unsigned),
                                          MTL::ResourceStorageModeShared);
    // Second index buffer for ICM ping-pong: scolorq reads from
    // indices_in (the previous iteration's assignment) and writes
    // to indices_out so neighbour lookups don't see partial updates
    // mid-pass. Same memory shape as idx_buf.
    auto* idx_buf2   = device->newBuffer(R * N * sizeof(unsigned),
                                          MTL::ResourceStorageModeShared);

    // Zero atomic accumulators (host-visible shared memory).
    std::memset(sums_buf->contents(),   0, R * K * 3 * sizeof(float));
    std::memset(counts_buf->contents(), 0, R * K * sizeof(unsigned));
    std::memset(sse_buf->contents(),    0, R * sizeof(float));

    struct Params {
        unsigned num_pixels, num_centroids, num_restarts, pixels_stride;
    } params{ unsigned(N), unsigned(K), unsigned(R), unsigned(N) };

    // -------- k-means++ init for each restart on CPU ----------
    std::mt19937 master_rng(args.seed);
    auto* cent = static_cast<float*>(cent_buf->contents());
    auto t_init0 = std::chrono::steady_clock::now();
    for (int r = 0; r < R; ++r) {
        std::mt19937 rng(master_rng());
        auto seeds = kmeanspp_init(oklab, K, rng);
        for (int k = 0; k < K; ++k) {
            cent[r*K*4 + k*4 + 0] = seeds[k].L;
            cent[r*K*4 + k*4 + 1] = seeds[k].a;
            cent[r*K*4 + k*4 + 2] = seeds[k].b;
            cent[r*K*4 + k*4 + 3] = 0.0f;
        }
    }
    auto t_init1 = std::chrono::steady_clock::now();
    std::printf("kmeans++ init (CPU, %d restarts × %d centroids): %.1f ms\n",
                R, K, std::chrono::duration<double, std::milli>(
                    t_init1 - t_init0).count());

    // -------- Lloyd loop on GPU ----------
    auto t_loop0 = std::chrono::steady_clock::now();
    for (int r = 0; r < R; ++r) {
        for (int it = 0; it < args.iters; ++it) {
            // Reset SSE counter for this restart.
            auto* sse_host = static_cast<float*>(sse_buf->contents());
            sse_host[r] = 0.0f;

            MTL::CommandBuffer* cmd = queue->commandBuffer();

            // Pass 1: assign + accumulate.
            {
                auto* enc = cmd->computeCommandEncoder();
                enc->setComputePipelineState(pso_assign);
                enc->setBuffer(pixel_buf,  0, 0);
                enc->setBuffer(cent_buf,   0, 1);
                enc->setBuffer(sums_buf,   0, 2);
                enc->setBuffer(counts_buf, 0, 3);
                enc->setBuffer(sse_buf,    0, 4);
                enc->setBuffer(idx_buf,    0, 5);
                enc->setBytes(&params, sizeof(params), 6);
                unsigned restart_u = unsigned(r);
                enc->setBytes(&restart_u, sizeof(unsigned), 7);
                NS::UInteger tg = pso_assign->maxTotalThreadsPerThreadgroup();
                if (tg > 256) tg = 256;
                enc->dispatchThreads(MTL::Size(N, 1, 1), MTL::Size(tg, 1, 1));
                enc->endEncoding();
            }
            // Pass 2: finalize.
            {
                auto* enc = cmd->computeCommandEncoder();
                enc->setComputePipelineState(pso_finalize);
                enc->setBuffer(cent_buf,   0, 0);
                enc->setBuffer(sums_buf,   0, 1);
                enc->setBuffer(counts_buf, 0, 2);
                enc->setBytes(&params, sizeof(params), 3);
                unsigned restart_u = unsigned(r);
                enc->setBytes(&restart_u, sizeof(unsigned), 4);
                enc->dispatchThreads(MTL::Size(K, 1, 1), MTL::Size(K, 1, 1));
                enc->endEncoding();
            }

            cmd->commit();
            cmd->waitUntilCompleted();
        }
    }
    auto t_loop1 = std::chrono::steady_clock::now();
    std::printf("Lloyd (GPU, %d restarts × %d iters × %zu px × %d K): %.1f ms\n",
                R, args.iters, N, K,
                std::chrono::duration<double, std::milli>(t_loop1 - t_loop0).count());

    // -------- Stage C: real scolorq joint optimization ----------
    // Lloyd seed sits in idx_buf. Allocate scratch for filtered
    // output, g(x), and the per-restart (M, b) linear-system pair.
    // CPU does the K×K Gauss-Seidel solve each iteration; GPU does
    // the per-pixel kernels.
    if (args.scolorq_iters > 0) {
        auto* out_buf = device->newBuffer(R * N * 4 * sizeof(float),
                                            MTL::ResourceStorageModeShared);
        auto* g_buf   = device->newBuffer(R * N * 4 * sizeof(float),
                                            MTL::ResourceStorageModeShared);
        auto* M_buf   = device->newBuffer(R * K * K * sizeof(float),
                                            MTL::ResourceStorageModeShared);
        auto* b_buf   = device->newBuffer(R * K * 3 * sizeof(float),
                                            MTL::ResourceStorageModeShared);

        struct ScolorqParams {
            unsigned width, height, K, R;
            float center_weight, neighbor_weight;
        } sp{
            unsigned(args.width), unsigned(args.height),
            unsigned(K), unsigned(R), 0.0f, 0.0f,
        };

        auto t_sc0 = std::chrono::steady_clock::now();
        for (int r = 0; r < R; ++r) {
            // Ping-pong index buffers: read from idx_buf, write to idx_buf2.
            std::memcpy(static_cast<unsigned*>(idx_buf2->contents()) + r * N,
                        static_cast<unsigned*>(idx_buf->contents())  + r * N,
                        N * sizeof(unsigned));

            auto* idx_in  = idx_buf;
            auto* idx_out = idx_buf2;

            for (int it = 0; it < args.scolorq_iters; ++it) {
                // Reset M, b, sse for this restart.
                std::memset(static_cast<float*>(M_buf->contents()) + r*K*K,
                            0, K * K * sizeof(float));
                std::memset(static_cast<float*>(b_buf->contents()) + r*K*3,
                            0, K * 3 * sizeof(float));
                static_cast<float*>(sse_buf->contents())[r] = 0.0f;

                MTL::CommandBuffer* cmd = queue->commandBuffer();
                unsigned restart_u = unsigned(r);

                // Pass 1: out(x) = F · p[a](x).
                {
                    auto* enc = cmd->computeCommandEncoder();
                    enc->setComputePipelineState(pso_sc_filt);
                    enc->setBuffer(out_buf,   0, 0);
                    enc->setBuffer(cent_buf,  0, 1);
                    enc->setBuffer(idx_in,    0, 2);
                    enc->setBytes(&sp, sizeof(sp), 3);
                    enc->setBytes(&restart_u, sizeof(unsigned), 4);
                    enc->dispatchThreads(
                        MTL::Size(args.width, args.height, 1),
                        MTL::Size(16, 16, 1));
                    enc->endEncoding();
                }
                // Pass 2: g(x) = F · (I - out)(x).
                {
                    auto* enc = cmd->computeCommandEncoder();
                    enc->setComputePipelineState(pso_sc_g);
                    enc->setBuffer(g_buf,     0, 0);
                    enc->setBuffer(pixel_buf, 0, 1);
                    enc->setBuffer(out_buf,   0, 2);
                    enc->setBytes(&sp, sizeof(sp), 3);
                    enc->setBytes(&restart_u, sizeof(unsigned), 4);
                    enc->dispatchThreads(
                        MTL::Size(args.width, args.height, 1),
                        MTL::Size(16, 16, 1));
                    enc->endEncoding();
                }
                // Pass 3a: assign only (writes idx_out from idx_in,
                // current palette, and g).
                {
                    auto* enc = cmd->computeCommandEncoder();
                    enc->setComputePipelineState(pso_sc_assign);
                    enc->setBuffer(idx_out,   0, 0);
                    enc->setBuffer(idx_in,    0, 1);
                    enc->setBuffer(cent_buf,  0, 2);
                    enc->setBuffer(g_buf,     0, 3);
                    enc->setBytes(&sp, sizeof(sp), 4);
                    enc->setBytes(&restart_u, sizeof(unsigned), 5);
                    enc->dispatchThreads(
                        MTL::Size(args.width, args.height, 1),
                        MTL::Size(16, 16, 1));
                    enc->endEncoding();
                }
                // Pass 3b: build M, b using the JUST-WRITTEN
                // assignments (idx_out is the current state).
                {
                    auto* enc = cmd->computeCommandEncoder();
                    enc->setComputePipelineState(pso_sc_build);
                    enc->setBuffer(idx_out,   0, 0);
                    enc->setBuffer(pixel_buf, 0, 1);
                    enc->setBuffer(M_buf,     0, 2);
                    enc->setBuffer(b_buf,     0, 3);
                    enc->setBytes(&sp, sizeof(sp), 4);
                    enc->setBytes(&restart_u, sizeof(unsigned), 5);
                    enc->dispatchThreads(
                        MTL::Size(args.width, args.height, 1),
                        MTL::Size(16, 16, 1));
                    enc->endEncoding();
                }
                cmd->commit();
                cmd->waitUntilCompleted();

                // CPU: solve K×K linear system M · p = b. The
                // proper scolorq palette update is the unique p
                // satisfying M·p = b — direct Cholesky/LDLT on a
                // 32×32 PSD matrix is trivial (~5K ops). Earlier
                // Gauss-Seidel diverged on poorly-conditioned M.
                auto* M_in = static_cast<const float*>(M_buf->contents()) + r*K*K;
                auto* b_in = static_cast<const float*>(b_buf->contents()) + r*K*3;
                auto* P    = static_cast<float*>(cent_buf->contents()) + r*K*4;
                constexpr float kRegEps = 1e-4f;  // PSD ridge

                // Copy M (with ridge) and rhs into local working buffers.
                std::vector<float> A(K * K);
                std::vector<float> rhs(K * 3);
                for (int i = 0; i < K; ++i) {
                    for (int j = 0; j < K; ++j) A[i*K+j] = M_in[i*K+j];
                    A[i*K+i] += kRegEps;
                    rhs[i*3+0] = b_in[i*3+0];
                    rhs[i*3+1] = b_in[i*3+1];
                    rhs[i*3+2] = b_in[i*3+2];
                }

                // LDLT (no pivoting) — A is symmetric PSD by
                // construction so this is stable up to the ridge.
                std::vector<float> L(K * K, 0.0f);
                std::vector<float> D(K, 0.0f);
                bool solver_ok = true;
                for (int j = 0; j < K; ++j) {
                    float d = A[j*K+j];
                    for (int p = 0; p < j; ++p)
                        d -= L[j*K+p] * L[j*K+p] * D[p];
                    D[j] = d;
                    L[j*K+j] = 1.0f;
                    if (std::abs(d) < 1e-12f) { solver_ok = false; break; }
                    for (int i = j+1; i < K; ++i) {
                        float s = A[i*K+j];
                        for (int p = 0; p < j; ++p)
                            s -= L[i*K+p] * L[j*K+p] * D[p];
                        L[i*K+j] = s / d;
                    }
                }
                if (solver_ok) {
                    // Solve L · y = b (forward), D · z = y (diag),
                    // L^T · x = z (back) for each of 3 RHS columns.
                    for (int ch = 0; ch < 3; ++ch) {
                        std::vector<float> y(K), z(K), x(K);
                        for (int i = 0; i < K; ++i) {
                            float s = rhs[i*3+ch];
                            for (int p = 0; p < i; ++p) s -= L[i*K+p] * y[p];
                            y[i] = s;
                        }
                        for (int i = 0; i < K; ++i) z[i] = y[i] / D[i];
                        for (int i = K-1; i >= 0; --i) {
                            float s = z[i];
                            for (int p = i+1; p < K; ++p) s -= L[p*K+i] * x[p];
                            x[i] = s;
                        }
                        for (int i = 0; i < K; ++i) P[i*4+ch] = x[i];
                    }
                }

                std::swap(idx_in, idx_out);
            }
            // Final assignment lives in idx_in (after the swap).
            // Make sure idx_buf is the "current" copy for downstream
            // rendering.
            if (idx_in != idx_buf) {
                std::memcpy(static_cast<unsigned*>(idx_buf->contents()) + r * N,
                            static_cast<const unsigned*>(idx_in->contents()) + r * N,
                            N * sizeof(unsigned));
            }
        }
        auto t_sc1 = std::chrono::steady_clock::now();
        std::printf("scolorq (GPU+CPU solve, %d restarts × %d iters): %.1f ms\n",
                    R, args.scolorq_iters,
                    std::chrono::duration<double, std::milli>(t_sc1 - t_sc0).count());

        out_buf->release();
        g_buf->release();
        M_buf->release();
        b_buf->release();
    }

    // -------- Pick best restart ----------
    // Re-run one final assign pass per restart to get fresh SSE
    // (the last update changed centroids; the SSE we accumulated
    // was against the centroids of the iteration BEFORE the last
    // finalize). For Stage A simplicity we just re-score on CPU —
    // it's a one-time cost.
    int best_r = 0;
    double best_sse = 1e30;
    auto* idx = static_cast<const unsigned*>(idx_buf->contents());
    auto* cent_final = static_cast<const float*>(cent_buf->contents());
    for (int r = 0; r < R; ++r) {
        double sse = 0;
        for (std::size_t i = 0; i < N; ++i) {
            unsigned k = idx[r * N + i];
            float dL = oklab[i].L - cent_final[r*K*4 + k*4 + 0];
            float da = oklab[i].a - cent_final[r*K*4 + k*4 + 1];
            float db = oklab[i].b - cent_final[r*K*4 + k*4 + 2];
            sse += dL*dL + da*da + db*db;
        }
        if (sse < best_sse) { best_sse = sse; best_r = r; }
    }
    std::printf("best restart: %d   sse=%.4f\n", best_r, best_sse);

    // -------- Build palette + render output ----------
    std::vector<unsigned char> rgb_out(N * 3);
    for (std::size_t i = 0; i < N; ++i) {
        unsigned k = idx[best_r * N + i];
        expq::OKLab lab{
            cent_final[best_r*K*4 + k*4 + 0],
            cent_final[best_r*K*4 + k*4 + 1],
            cent_final[best_r*K*4 + k*4 + 2],
        };
        auto lin = expq::oklab_to_linear(lab);
        rgb_out[i*3+0] = expq::linear_to_srgb_u8(lin.r);
        rgb_out[i*3+1] = expq::linear_to_srgb_u8(lin.g);
        rgb_out[i*3+2] = expq::linear_to_srgb_u8(lin.b);
    }
    if (!stbi_write_png(args.out_path.c_str(), w, h, 3,
                         rgb_out.data(), w * 3)) {
        std::fprintf(stderr, "stbi_write_png failed\n");
        return 1;
    }
    stbi_image_free(raw);

    pool->release();
    std::printf("wrote: %s (%dx%d, %d colors)\n",
                args.out_path.c_str(), w, h, K);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, a)) {
        std::fprintf(stderr,
            "usage: quant <input.png> <output.png>"
            " [--colors N] [--restarts N] [--iters N] [--seed N]\n");
        return 2;
    }
    return run(a);
}
