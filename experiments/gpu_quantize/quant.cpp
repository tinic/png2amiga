// GPU palette-quantization experiment — Metal scaffolding probe.
//
// Stage 0 (this file): proves the metal-cpp toolchain end-to-end on
// this machine — allocates a buffer, dispatches add_one.metal, reads
// back, asserts result. If this prints "OK", the scaffolding is good
// and we move on to the actual parallel-Lloyd kernel.
//
// Build via experiments/gpu_quantize/build.sh.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr std::size_t kN = 1024;

[[nodiscard]] int run() {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "no Metal device\n");
        return 1;
    }
    std::printf("Device: %s\n", device->name()->utf8String());

    NS::Error* err = nullptr;
    auto path = NS::String::string(
        "experiments/gpu_quantize/quant.metallib",
        NS::UTF8StringEncoding);
    MTL::Library* lib = device->newLibrary(path, &err);
    if (!lib) {
        std::fprintf(stderr, "newLibrary failed: %s\n",
                     err ? err->localizedDescription()->utf8String()
                         : "(null)");
        return 1;
    }

    auto fn_name = NS::String::string("add_one", NS::UTF8StringEncoding);
    MTL::Function* fn = lib->newFunction(fn_name);
    if (!fn) {
        std::fprintf(stderr, "newFunction(add_one) failed\n");
        return 1;
    }

    MTL::ComputePipelineState* pso =
        device->newComputePipelineState(fn, &err);
    if (!pso) {
        std::fprintf(stderr, "newComputePipelineState failed: %s\n",
                     err ? err->localizedDescription()->utf8String()
                         : "(null)");
        return 1;
    }

    MTL::CommandQueue* queue = device->newCommandQueue();

    const std::size_t bytes = kN * sizeof(float);
    MTL::Buffer* in_buf  = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
    MTL::Buffer* out_buf = device->newBuffer(bytes, MTL::ResourceStorageModeShared);

    auto* in_data = static_cast<float*>(in_buf->contents());
    for (std::size_t i = 0; i < kN; ++i) in_data[i] = static_cast<float>(i);

    MTL::CommandBuffer* cmd = queue->commandBuffer();
    MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();
    enc->setComputePipelineState(pso);
    enc->setBuffer(in_buf,  0, 0);
    enc->setBuffer(out_buf, 0, 1);

    NS::UInteger tg_size = pso->maxTotalThreadsPerThreadgroup();
    if (tg_size > kN) tg_size = kN;
    enc->dispatchThreads(MTL::Size(kN, 1, 1), MTL::Size(tg_size, 1, 1));
    enc->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();

    auto* out_data = static_cast<const float*>(out_buf->contents());
    int errors = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        const float expect = static_cast<float>(i) + 1.0f;
        if (out_data[i] != expect) {
            if (errors < 5) {
                std::fprintf(stderr, "  [%zu] got %g expected %g\n",
                             i, out_data[i], expect);
            }
            ++errors;
        }
    }

    pool->release();

    if (errors) {
        std::fprintf(stderr, "FAIL: %d mismatches\n", errors);
        return 1;
    }
    std::printf("OK: %zu elements processed on GPU\n", kN);
    return 0;
}

} // namespace

int main() { return run(); }
