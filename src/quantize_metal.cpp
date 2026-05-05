// Metal-accelerated palette quantizer — apple-clang TU.
//
// IMPORTANT: This file is compiled with apple-clang (not gcc-15)
// because metal-cpp pulls in Apple-clang-only extensions (ObjC
// blocks in NSNotification / NSProcessInfo headers). The main
// project uses gcc-15 + Homebrew libstdc++; mixing libstdc++ and
// libc++ object files in one binary breaks ABI for any STL type
// passed across. So the boundary is strict C — see the
// extern "C" declarations in quantize_metal.hpp.

// Match the experiment harness flags. -fno-objc-arc avoids ObjC
// reference counting because metal-cpp manages lifetimes manually.
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#if PNG2AMIGA_HAVE_METAL
    #include <Foundation/Foundation.hpp>
    #include <Metal/Metal.hpp>
    #include "quantize_metal_metallib.h"   // generated: byte array
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <vector>

#if !PNG2AMIGA_HAVE_METAL

extern "C" {
bool png2amiga_metal_available_c() noexcept { return false; }
int png2amiga_quantize_metal_c(const float*, std::size_t, std::size_t,
                                int, int, std::uint32_t,
                                float*, std::size_t*) noexcept { return 1; }
}

#else  // ============================================================

// ---- Local OKLab conversion (self-contained; mirrors src/color_space.hpp
// minus the GCC-specific constexpr LUT path). Apple-clang's libc++ does
// not have constexpr std::pow yet, so the sRGB LUT initialises lazily
// at first use.

namespace {

struct OKLab { float L, a, b; };

float srgb_to_linear_f(float s) noexcept {
    return s <= 0.04045f ? s / 12.92f
                          : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb_f(float l) noexcept {
    return l <= 0.0031308f ? l * 12.92f
                            : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

OKLab linear_to_oklab(float r, float g, float b) noexcept {
    float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;
    l = std::cbrt(l); m = std::cbrt(m); s = std::cbrt(s);
    return {
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
    };
}

void oklab_to_linear_rgb(OKLab lab, float& r, float& g, float& b) noexcept {
    float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;
    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;
    r =  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

float oklab_dist_sq(OKLab a, OKLab b) noexcept {
    float dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
    return dL*dL + da*da + db*db;
}

// k-means++ init on a uniform random subsample of the pixel set.
// Subsampling is essential — full-image init was 28.9 s for 32
// restarts at K=256; subsampled is ~160 ms with statistically
// indistinguishable seed quality for downstream Lloyd.
constexpr std::size_t kInitSampleSize = 8192;

std::vector<OKLab> kmeanspp_init(
    std::span<const OKLab> pixels, int K, std::mt19937& rng)
{
    std::vector<OKLab> sample;
    if (pixels.size() <= kInitSampleSize) {
        sample.assign(pixels.begin(), pixels.end());
    } else {
        sample.reserve(kInitSampleSize);
        std::uniform_int_distribution<std::size_t> pick(0, pixels.size() - 1);
        for (std::size_t i = 0; i < kInitSampleSize; ++i)
            sample.push_back(pixels[pick(rng)]);
    }

    std::vector<OKLab> out;
    out.reserve(static_cast<std::size_t>(K));

    std::uniform_int_distribution<std::size_t> first(0, sample.size() - 1);
    out.push_back(sample[first(rng)]);

    std::vector<float> nearest_d2(sample.size(),
                                   std::numeric_limits<float>::max());
    while (static_cast<int>(out.size()) < K) {
        const auto& last = out.back();
        for (std::size_t i = 0; i < sample.size(); ++i) {
            float d = oklab_dist_sq(sample[i], last);
            if (d < nearest_d2[i]) nearest_d2[i] = d;
        }
        std::discrete_distribution<std::size_t> pick(
            nearest_d2.begin(), nearest_d2.end());
        out.push_back(sample[pick(rng)]);
    }
    return out;
}

// ---- Singleton Metal context. Probes the device on first call,
// caches the result. Pipeline state objects are reused across
// invocations.

class MetalContext {
public:
    static MetalContext& instance() noexcept {
        static MetalContext ctx;
        return ctx;
    }
    bool ok() const noexcept { return ready_; }
    MTL::Device*               device()       const noexcept { return device_; }
    MTL::CommandQueue*         queue()        const noexcept { return queue_; }
    MTL::ComputePipelineState* pso_assign()   const noexcept { return pso_assign_; }
    MTL::ComputePipelineState* pso_finalize() const noexcept { return pso_finalize_; }
private:
    MetalContext() noexcept {
        device_ = MTL::CreateSystemDefaultDevice();
        if (!device_) return;
        if (!device_->supportsFamily(MTL::GPUFamilyApple7)) {
            device_->release(); device_ = nullptr; return;
        }
        // Wrap the embedded metallib bytes as dispatch_data_t.
        auto* data = dispatch_data_create(
            quantize_metal_metallib_data,
            quantize_metal_metallib_data_size,
            nullptr, nullptr);
        NS::Error* err = nullptr;
        lib_ = device_->newLibrary(static_cast<dispatch_data_t>(data), &err);
        if (data) dispatch_release(data);
        if (!lib_) return;

        auto fn = [&](const char* name) -> MTL::ComputePipelineState* {
            auto* nsname = NS::String::string(name, NS::UTF8StringEncoding);
            MTL::Function* f = lib_->newFunction(nsname);
            if (!f) return nullptr;
            NS::Error* perr = nullptr;
            auto* p = device_->newComputePipelineState(f, &perr);
            f->release();
            return p;
        };
        pso_assign_   = fn("assign_and_accumulate");
        pso_finalize_ = fn("finalize_centroids");
        if (!pso_assign_ || !pso_finalize_) return;

        queue_ = device_->newCommandQueue();
        if (!queue_) return;
        ready_ = true;
    }
    MetalContext(const MetalContext&) = delete;
    MetalContext& operator=(const MetalContext&) = delete;

    MTL::Device*                device_       = nullptr;
    MTL::Library*               lib_          = nullptr;
    MTL::CommandQueue*          queue_        = nullptr;
    MTL::ComputePipelineState*  pso_assign_   = nullptr;
    MTL::ComputePipelineState*  pso_finalize_ = nullptr;
    bool ready_ = false;
};

} // anon

extern "C" {

bool png2amiga_metal_available_c() noexcept {
    return MetalContext::instance().ok();
}

int png2amiga_quantize_metal_c(
    const float* pixels_rgb, std::size_t n_pixels,
    std::size_t  max_colors, int restarts, int iterations,
    std::uint32_t seed,
    float* out_palette_rgb, std::size_t* out_palette_size) noexcept
try {
    auto& ctx = MetalContext::instance();
    if (!ctx.ok())                       return 1;
    if (n_pixels == 0)                   return 2;
    if (max_colors == 0 || max_colors > 256) return 3;
    if (!pixels_rgb || !out_palette_rgb || !out_palette_size) return 4;

    if (restarts < 1)   restarts   = 1;
    if (iterations < 1) iterations = 1;
    const int K = static_cast<int>(max_colors);
    const int R = restarts;

    // sRGB-linear → OKLab f32 (CPU; cheap relative to the GPU work).
    std::vector<OKLab> oklab(n_pixels);
    for (std::size_t i = 0; i < n_pixels; ++i) {
        oklab[i] = linear_to_oklab(pixels_rgb[i*3+0],
                                    pixels_rgb[i*3+1],
                                    pixels_rgb[i*3+2]);
    }

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto* device = ctx.device();
    auto* queue  = ctx.queue();

    auto* pixel_buf = device->newBuffer(n_pixels * 4 * sizeof(float),
                                         MTL::ResourceStorageModeShared);
    auto* cent_buf  = device->newBuffer(static_cast<std::size_t>(R*K) * 4 * sizeof(float),
                                         MTL::ResourceStorageModeShared);
    auto* sums_buf  = device->newBuffer(static_cast<std::size_t>(R*K) * 3 * sizeof(float),
                                         MTL::ResourceStorageModeShared);
    auto* counts_buf= device->newBuffer(static_cast<std::size_t>(R*K) * sizeof(unsigned),
                                         MTL::ResourceStorageModeShared);
    auto* sse_buf   = device->newBuffer(static_cast<std::size_t>(R) * sizeof(float),
                                         MTL::ResourceStorageModeShared);
    auto* idx_buf   = device->newBuffer(static_cast<std::size_t>(R) * n_pixels * sizeof(unsigned),
                                         MTL::ResourceStorageModeShared);

    auto* px = static_cast<float*>(pixel_buf->contents());
    for (std::size_t i = 0; i < n_pixels; ++i) {
        px[i*4+0] = oklab[i].L;
        px[i*4+1] = oklab[i].a;
        px[i*4+2] = oklab[i].b;
        px[i*4+3] = 0.0f;
    }
    std::memset(sums_buf->contents(),   0,
                static_cast<std::size_t>(R*K) * 3 * sizeof(float));
    std::memset(counts_buf->contents(), 0,
                static_cast<std::size_t>(R*K) * sizeof(unsigned));
    std::memset(sse_buf->contents(),    0,
                static_cast<std::size_t>(R) * sizeof(float));

    // k-means++ init per restart.
    std::mt19937 master_rng(seed);
    auto* cent = static_cast<float*>(cent_buf->contents());
    std::span<const OKLab> oklab_span(oklab.data(), oklab.size());
    for (int r = 0; r < R; ++r) {
        std::mt19937 rng(master_rng());
        auto seeds = kmeanspp_init(oklab_span, K, rng);
        for (int k = 0; k < K; ++k) {
            cent[r*K*4 + k*4 + 0] = seeds[static_cast<std::size_t>(k)].L;
            cent[r*K*4 + k*4 + 1] = seeds[static_cast<std::size_t>(k)].a;
            cent[r*K*4 + k*4 + 2] = seeds[static_cast<std::size_t>(k)].b;
            cent[r*K*4 + k*4 + 3] = 0.0f;
        }
    }

    struct Params {
        unsigned num_pixels, num_centroids, num_restarts, pixels_stride;
    } params{ static_cast<unsigned>(n_pixels), static_cast<unsigned>(K),
              static_cast<unsigned>(R),       static_cast<unsigned>(n_pixels) };

    // Lloyd loop on GPU. ~3 s for 32×20 at 1.5 MP.
    for (int r = 0; r < R; ++r) {
        for (int it = 0; it < iterations; ++it) {
            static_cast<float*>(sse_buf->contents())[r] = 0.0f;
            MTL::CommandBuffer* cmd = queue->commandBuffer();
            unsigned restart_u = static_cast<unsigned>(r);
            {
                auto* enc = cmd->computeCommandEncoder();
                enc->setComputePipelineState(ctx.pso_assign());
                enc->setBuffer(pixel_buf,  0, 0);
                enc->setBuffer(cent_buf,   0, 1);
                enc->setBuffer(sums_buf,   0, 2);
                enc->setBuffer(counts_buf, 0, 3);
                enc->setBuffer(sse_buf,    0, 4);
                enc->setBuffer(idx_buf,    0, 5);
                enc->setBytes(&params, sizeof(params), 6);
                enc->setBytes(&restart_u, sizeof(unsigned), 7);
                NS::UInteger tg = ctx.pso_assign()->maxTotalThreadsPerThreadgroup();
                if (tg > 256) tg = 256;
                enc->dispatchThreads(MTL::Size(n_pixels, 1, 1),
                                      MTL::Size(tg, 1, 1));
                enc->endEncoding();
            }
            {
                auto* enc = cmd->computeCommandEncoder();
                enc->setComputePipelineState(ctx.pso_finalize());
                enc->setBuffer(cent_buf,   0, 0);
                enc->setBuffer(sums_buf,   0, 1);
                enc->setBuffer(counts_buf, 0, 2);
                enc->setBytes(&params, sizeof(params), 3);
                enc->setBytes(&restart_u, sizeof(unsigned), 4);
                enc->dispatchThreads(MTL::Size(static_cast<NS::UInteger>(K), 1, 1),
                                      MTL::Size(static_cast<NS::UInteger>(K), 1, 1));
                enc->endEncoding();
            }
            cmd->commit();
            cmd->waitUntilCompleted();
        }
    }

    // CPU re-score per restart against the FINAL centroids; pick best.
    int best_r = 0;
    double best_sse = std::numeric_limits<double>::max();
    auto* idx        = static_cast<const unsigned*>(idx_buf->contents());
    auto* cent_final = static_cast<const float*>(cent_buf->contents());
    for (int r = 0; r < R; ++r) {
        double sse = 0.0;
        for (std::size_t i = 0; i < n_pixels; ++i) {
            unsigned k = idx[static_cast<std::size_t>(r) * n_pixels + i];
            float dL = oklab[i].L - cent_final[r*K*4 + k*4 + 0];
            float da = oklab[i].a - cent_final[r*K*4 + k*4 + 1];
            float db = oklab[i].b - cent_final[r*K*4 + k*4 + 2];
            sse += static_cast<double>(dL)*dL
                 + static_cast<double>(da)*da
                 + static_cast<double>(db)*db;
        }
        if (sse < best_sse) { best_sse = sse; best_r = r; }
    }

    // Convert winning palette OKLab → linear RGB. Sort by L so the
    // ordering matches median_cut's convention (perceptual luminance).
    std::vector<OKLab> lab_pal(static_cast<std::size_t>(K));
    for (int k = 0; k < K; ++k) {
        lab_pal[static_cast<std::size_t>(k)] = OKLab{
            cent_final[best_r*K*4 + k*4 + 0],
            cent_final[best_r*K*4 + k*4 + 1],
            cent_final[best_r*K*4 + k*4 + 2],
        };
    }
    std::sort(lab_pal.begin(), lab_pal.end(),
              [](OKLab a, OKLab b) { return a.L < b.L; });

    for (int k = 0; k < K; ++k) {
        float r, g, b;
        oklab_to_linear_rgb(lab_pal[static_cast<std::size_t>(k)], r, g, b);
        out_palette_rgb[k*3+0] = std::clamp(r, 0.0f, 1.0f);
        out_palette_rgb[k*3+1] = std::clamp(g, 0.0f, 1.0f);
        out_palette_rgb[k*3+2] = std::clamp(b, 0.0f, 1.0f);
    }
    *out_palette_size = static_cast<std::size_t>(K);

    pixel_buf->release(); cent_buf->release(); sums_buf->release();
    counts_buf->release(); sse_buf->release(); idx_buf->release();
    pool->release();

    return 0;
} catch (...) {
    return 99;  // unknown error
}

} // extern "C"

#endif // PNG2AMIGA_HAVE_METAL
