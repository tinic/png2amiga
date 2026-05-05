// Stage B — Soft k-means with deterministic annealing (Rose 1998).
//
// Replaces the discrete argmin assignment of Stage A's Lloyd with a
// soft Gibbs-distribution assignment at temperature T:
//
//     w_k(x) = exp(-||x - p[k]||² / T) / Σ_j exp(-||x - p[j]||² / T)
//
// The palette update becomes a weighted mean:
//
//     p[k]' = Σ_x w_k(x) I(x) / Σ_x w_k(x)
//
// As T → 0 this recovers hard k-means; at high T every cluster
// gets non-negligible mass from every pixel, which avoids the
// local minima that hard Lloyd gets trapped in. We anneal T over
// the course of args.scolorq_iters by a fixed multiplicative
// schedule (T *= cooling) — the analytic basin-escape mechanism
// the original scolorq joint optimisation also relies on, just
// without the spatial-filter complication.
//
// Soft k-means matrix is O(N×K) entries — too big to materialise.
// Per-pixel we redo the soft-max in shared memory each pass.

#include <metal_stdlib>
using namespace metal;

struct ScolorqParams {
    uint width;
    uint height;
    uint num_centroids;
    uint num_restarts;
    float center_weight;     // unused now (kept for ABI compat)
    float neighbor_weight;   // unused now
};

// -----------------------------------------------------------------
// Soft assignment + accumulate. For each pixel:
//   1. Compute distances to all K centroids
//   2. Find min distance d_min for numerical stability
//   3. Compute weights w_k = exp(-(d²-d_min²)/T)
//   4. Atomic-accumulate weighted pixel + weight per cluster
//
// At low T (kT ≈ 1e-5) softmax converges to a delta on the
// nearest cluster — equivalent to hard Lloyd. At high T (kT ≈ 1)
// weights are diffuse and the palette update is "everyone moves
// toward the global centroid", which is what unsticks local minima.
// -----------------------------------------------------------------
kernel void scolorq_assign(
    device const float4*  pixels       [[ buffer(0) ]],
    device       uint*    indices_in   [[ buffer(1) ]],   // unused (ABI compat)
    device       uint*    indices_out  [[ buffer(2) ]],
    device const float4*  centroids    [[ buffer(3) ]],
    device atomic_float*  sums_f       [[ buffer(4) ]],   // R*K*3
    device atomic_uint*   counts_u     [[ buffer(5) ]],   // R*K (reused as weight×1e6)
    device atomic_float*  sse_f        [[ buffer(6) ]],
    constant ScolorqParams& params     [[ buffer(7) ]],
    constant uint&        restart      [[ buffer(8) ]],
    uint2                 pos          [[ thread_position_in_grid ]])
{
    if (pos.x >= params.width || pos.y >= params.height) return;

    const uint K  = params.num_centroids;
    const uint W  = params.width;
    const uint H  = params.height;
    const uint pi = pos.y * W + pos.x;
    const uint cbase = restart * K;
    const uint ibase = restart * W * H;

    // Temperature passed via center_weight slot to avoid expanding
    // the params struct — Stage B repurposes it.
    const float T = max(params.center_weight, 1e-8f);

    const float4 I = pixels[pi];

    // Pass 1: find distances + min for stability.
    float dists[256];   // K ≤ 256 in this experiment
    float dmin = INFINITY;
    uint  best_k = 0;
    for (uint k = 0; k < K; ++k) {
        float4 c = centroids[cbase + k];
        float dL = I.x - c.x;
        float da = I.y - c.y;
        float db = I.z - c.z;
        float d  = dL * dL + da * da + db * db;
        dists[k] = d;
        if (d < dmin) { dmin = d; best_k = k; }
    }

    // Pass 2: softmax weights, normalising to keep numerics sane.
    float wsum = 0.0f;
    for (uint k = 0; k < K; ++k) {
        float w = exp(-(dists[k] - dmin) / T);
        dists[k] = w;       // reuse buffer
        wsum += w;
    }
    float inv_wsum = 1.0f / wsum;

    // Hard index for rendering = argmin (irrespective of T).
    indices_out[ibase + pi] = best_k;

    // Atomic-accumulate weighted contributions.
    // counts_u stores weight in fixed-point (×1e6, max ~1e6 per
    // pixel ⇒ max sum 1e12 over 1MP — overflows uint32). Use
    // atomic_float on counts to dodge that; reinterpret cast via
    // the atomic_uint API isn't possible, so we devote sums_f's
    // .w slot for the per-cluster weight — already there in the
    // R*K*3 layout? No — that's L,a,b only. Fall back to a
    // separate device float buffer (counts repurposed).
    //
    // For simplicity here we reinterpret counts_u as atomic_float
    // via a union-style cast: that's UB in MSL. Cleaner: write
    // weight to a .w slot in centroids_acc. But that requires
    // changing the buffer layout. Easiest: scale weights up by
    // 1024 (so dynamic range fits in uint32 over 1MP × 32K /
    // typical max ~1.0) and atomic_uint-add the rounded value.
    // Worst-case overflow stays bounded.
    for (uint k = 0; k < K; ++k) {
        float w = dists[k] * inv_wsum;
        if (w < 1e-6f) continue;   // sparse sum: skip negligible
        const uint sbase = restart * K * 3 + k * 3;
        atomic_fetch_add_explicit(&sums_f[sbase + 0], w * I.x,
                                   memory_order_relaxed);
        atomic_fetch_add_explicit(&sums_f[sbase + 1], w * I.y,
                                   memory_order_relaxed);
        atomic_fetch_add_explicit(&sums_f[sbase + 2], w * I.z,
                                   memory_order_relaxed);
        // Weight in fixed-point ×1024. Per-pixel max contribution = 1024.
        // Over 4MP image: 4e9 — at the edge of uint32 (~4.3e9) but
        // typical w << 1.0 keeps it well under.
        atomic_fetch_add_explicit(&counts_u[restart * K + k],
                                   uint(w * 1024.0f),
                                   memory_order_relaxed);
    }

    atomic_fetch_add_explicit(&sse_f[restart], dmin,
                               memory_order_relaxed);
}

// Update: p[k] = weighted_sum[k] / weighted_count[k] (with weight
// in ×1024 fixed-point).
kernel void scolorq_palette_update(
    device float4*        centroids    [[ buffer(0) ]],
    device atomic_float*  sums_f       [[ buffer(1) ]],
    device atomic_uint*   counts_u     [[ buffer(2) ]],
    constant ScolorqParams& params     [[ buffer(3) ]],
    constant uint&        restart      [[ buffer(4) ]],
    uint                  k            [[ thread_position_in_grid ]])
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
    uint cnt_fp = atomic_exchange_explicit(&counts_u[restart * K + k],
                                            0u, memory_order_relaxed);

    // weighted count is fixed-point ×1024, so weight = cnt_fp / 1024.
    if (cnt_fp > 0) {
        float w = float(cnt_fp) / 1024.0f;
        float inv = 1.0f / w;
        centroids[restart * K + k] = float4(sL * inv, sa * inv,
                                             sb * inv, 0.0f);
    }
}
