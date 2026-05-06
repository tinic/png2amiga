// Stage A — parallel Lloyd k-means in OKLab on Apple GPU.
//
// One dispatch per restart per Lloyd iteration:
//   1. assign_and_accumulate: 1 thread/pixel; argmin against the
//      restart's K centroids; atomic_float-add into per-restart
//      sums + per-cluster count + per-restart SSE.
//   2. finalize_centroids: K threads; new_centroid = sum / count
//      (skips empty clusters).
//
// atomic_float requires Apple GPU family 7+ (A14/M1+). Fixed-point
// alternative (atomic_int over ×scale encoding) overflowed int32
// at typical resolutions — kept on the cutting-room floor.

#include <metal_stdlib>
using namespace metal;

struct Params {
    uint num_pixels;
    uint num_centroids;     // K
    uint num_restarts;      // R
    uint pixels_stride;     // = num_pixels (kept for future packed layouts)
};

// Per-pixel OKLab values, layout: pixels[N] (float3, but stored as float4 for alignment)
// Per-restart centroids: centroids[R*K] (float4 OKLab, padding in .w)
// Per-restart sums: sums[R*K*4]  (atomic_int — L,a,b,count — fixed-point)
// Per-restart per-pixel SSE:    sse[R]    (atomic_uint — fixed-point sum of dist²)
// Per-pixel-per-restart index:  idx[R*N]  (uint8 packed as uint via byte writes)

// -----------------------------------------------------------------
// Kernel 1: assign each pixel to nearest centroid AND accumulate
// the new sums + counts atomically. Combines two passes into one
// for fewer dispatches.
//
// Grid: pixels    Threadgroup: 256
// One restart per dispatch; caller sets restart_index in params.
// -----------------------------------------------------------------
kernel void assign_and_accumulate(
    device const float4*      pixels       [[ buffer(0) ]],
    device const float4*      centroids    [[ buffer(1) ]],  // R*K
    device atomic_float*      sums_f       [[ buffer(2) ]],  // R*K*3 (L,a,b)
    device atomic_uint*       counts_u     [[ buffer(3) ]],  // R*K
    device atomic_float*      sse_f        [[ buffer(4) ]],  // R
    device uint*              indices      [[ buffer(5) ]],  // R*N
    constant Params&          params       [[ buffer(6) ]],
    constant uint&            restart      [[ buffer(7) ]],
    uint                      gid          [[ thread_position_in_grid ]])
{
    if (gid >= params.num_pixels) return;

    const float4 px = pixels[gid];
    const uint K   = params.num_centroids;
    const uint cbase = restart * K;

    // Linear nearest-centroid search.
    float best_d = INFINITY;
    uint  best_k = 0;
    for (uint k = 0; k < K; ++k) {
        float4 c = centroids[cbase + k];
        float dL = px.x - c.x;
        float da = px.y - c.y;
        float db = px.z - c.z;
        float d  = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best_k = k; }
    }

    indices[restart * params.num_pixels + gid] = best_k;

    // Accumulate sums (atomic_float) + count (atomic_uint) + SSE.
    const uint sbase = restart * K * 3 + best_k * 3;
    atomic_fetch_add_explicit(&sums_f[sbase + 0], px.x,
                               memory_order_relaxed);
    atomic_fetch_add_explicit(&sums_f[sbase + 1], px.y,
                               memory_order_relaxed);
    atomic_fetch_add_explicit(&sums_f[sbase + 2], px.z,
                               memory_order_relaxed);
    atomic_fetch_add_explicit(&counts_u[restart * K + best_k], 1u,
                               memory_order_relaxed);
    atomic_fetch_add_explicit(&sse_f[restart], best_d,
                               memory_order_relaxed);
}

// -----------------------------------------------------------------
// Kernel 2: divide accumulated sums by counts → new centroids.
// Empty clusters are left unchanged (caller can reseed in CPU
// post-pass if desired; for Stage A we accept rare empty clusters
// as a Lloyd quirk).
//
// Grid: K centroids for the active restart.
// -----------------------------------------------------------------
kernel void finalize_centroids(
    device float4*            centroids    [[ buffer(0) ]],  // R*K
    device atomic_float*      sums_f       [[ buffer(1) ]],  // R*K*3
    device atomic_uint*       counts_u     [[ buffer(2) ]],  // R*K
    constant Params&          params       [[ buffer(3) ]],
    constant uint&            restart      [[ buffer(4) ]],
    uint                      k            [[ thread_position_in_grid ]])
{
    if (k >= params.num_centroids) return;

    const uint K = params.num_centroids;
    const uint sbase = restart * K * 3 + k * 3;

    float sL = atomic_exchange_explicit(&sums_f[sbase + 0], 0.0f,
                                         memory_order_relaxed);
    float sa = atomic_exchange_explicit(&sums_f[sbase + 1], 0.0f,
                                         memory_order_relaxed);
    float sb = atomic_exchange_explicit(&sums_f[sbase + 2], 0.0f,
                                         memory_order_relaxed);
    uint  cnt = atomic_exchange_explicit(&counts_u[restart * K + k],
                                          0u, memory_order_relaxed);

    if (cnt > 0) {
        float inv = 1.0f / float(cnt);
        centroids[restart * K + k] = float4(sL * inv, sa * inv,
                                             sb * inv, 0.0f);
    }
    // else: keep prior centroid; rare for K << N.
}

// -----------------------------------------------------------------
// Kernel 3: assign_only — same argmin as assign_and_accumulate but
// writes only the per-pixel cluster index. No atomic_float sums, so
// the result is bit-deterministic across runs (atomic_float on Metal
// is order-of-arrival-dependent, which depends on GPU thread
// scheduling). Caller does the sum on CPU in a fixed sequential
// order. Costs an extra GPU↔CPU readback per Lloyd iteration but
// makes the whole gpu_restart_quantize pipeline reproducible —
// required for byte-equality between repeated runs (e.g. between
// CLI invocations under the api-equiv ctest harness).
//
// Grid: pixels    Threadgroup: 256
// One restart per dispatch; caller sets restart_index in params.
// -----------------------------------------------------------------
kernel void assign_only(
    device const float4*      pixels       [[ buffer(0) ]],
    device const float4*      centroids    [[ buffer(1) ]],  // R*K
    device uint*              indices      [[ buffer(2) ]],  // R*N
    constant Params&          params       [[ buffer(3) ]],
    constant uint&            restart      [[ buffer(4) ]],
    uint                      gid          [[ thread_position_in_grid ]])
{
    if (gid >= params.num_pixels) return;

    const float4 px = pixels[gid];
    const uint K   = params.num_centroids;
    const uint cbase = restart * K;

    float best_d = INFINITY;
    uint  best_k = 0;
    for (uint k = 0; k < K; ++k) {
        float4 c = centroids[cbase + k];
        float dL = px.x - c.x;
        float da = px.y - c.y;
        float db = px.z - c.z;
        float d  = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best_k = k; }
    }

    indices[restart * params.num_pixels + gid] = best_k;
}
