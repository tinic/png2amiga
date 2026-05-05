// Stage C — Real Spatial Color Quantization (Bonn 1998 / scolorq).
//
// Joint argmin over palette p (K colours) and assignment a:
//
//     E(p, a) = Σ_x || I(x) - Σ_y F(y-x) p[a(y)] ||²
//
// where F is a 3×3 filter modelling spatial pooling. We use:
//
//     F = [1 2 1; 2 4 2; 1 2 1] / 16     (separable Gaussian, σ ≈ 1)
//     ||F||² = 36/256 = 0.140625
//
// **Optimal palette given a**: setting ∂E/∂p[k] = 0 produces a
// linear system M · p = b where:
//
//     M[k,k'] = Σ_x F_k(x) · F_k'(x)
//     b[k]    = Σ_x F_k(x) · I(x)
//     F_k(x)  = Σ_y F(y-x) · [a(y) = k]    (filtered indicator)
//
// **Optimal assignment given p**: the cost change for flipping
// a(x) = k → k' factorises as
//
//     ΔE = ||F||² · ||p[k']-p[k]||² - 2 (p[k']-p[k]) · g(x)
//
// where g(x) = Σ_y F(y-x) · R(y), R(y) = I(y) - F·p[a](y) is the
// current filtered residual. argmin_k' equals argmin_k' ||p[k'] - τ(x)||²
// where τ(x) = p[a(x)] + g(x)/||F||².
//
// One scolorq iteration is therefore four GPU passes:
//   1. compute_filtered_output   — out(x) = F·p[a](x)
//   2. compute_g                 — g(x) = F · (I - out)(x)   (= F * R)
//   3. assign_and_accumulate_M   — new a(x) + atomic-build M, b
//   4. CPU solves M · p = b      — push new palette back
//
// This is done per restart (host copies M+b out, solves on CPU,
// uploads new palette). At K=32 the K×K solve is microscopic.

#include <metal_stdlib>
using namespace metal;

struct ScolorqParams {
    uint width;
    uint height;
    uint num_centroids;
    uint num_restarts;
    float center_weight;     // unused (Stage A ABI compat)
    float neighbor_weight;   // unused
};

// 3×3 filter weights (normalised to sum 1). Bonn paper uses the
// separable Gaussian [1,2,1;2,4,2;1,2,1]/16 (||F||² = 36/256 ≈ 0.141).
constant float kF[9] = {
    1.0f/16, 2.0f/16, 1.0f/16,
    2.0f/16, 4.0f/16, 2.0f/16,
    1.0f/16, 2.0f/16, 1.0f/16
};
constant float kFNormSq = 36.0f / 256.0f;

inline int reflect(int i, int n) {
    if (i < 0)      return -i;
    if (i >= n)     return 2 * n - i - 2;
    return i;
}

// Sample filter neighbour indices for pixel (x, y). Returns the
// linear pixel index for tap_idx 0..8 (row-major within 3×3).
inline uint nb_idx(uint x, uint y, uint tap, uint W, uint H) {
    int dx = int(tap % 3) - 1;
    int dy = int(tap / 3) - 1;
    int nx = reflect(int(x) + dx, int(W));
    int ny = reflect(int(y) + dy, int(H));
    return uint(ny) * W + uint(nx);
}

// -----------------------------------------------------------------
// Kernel 1: out(x) = F · p[a](x).
// Used by both kernels 2 (residual = I - out) and SSE accounting.
// -----------------------------------------------------------------
kernel void scolorq_filtered_output(
    device       float4*  out_buf      [[ buffer(0) ]],   // R*W*H (float4, w pad)
    device const float4*  centroids    [[ buffer(1) ]],   // R*K
    device const uint*    indices      [[ buffer(2) ]],   // R*W*H
    constant ScolorqParams& params     [[ buffer(3) ]],
    constant uint&        restart      [[ buffer(4) ]],
    uint2                 pos          [[ thread_position_in_grid ]])
{
    if (pos.x >= params.width || pos.y >= params.height) return;
    const uint W = params.width, H = params.height, K = params.num_centroids;
    const uint pi = pos.y * W + pos.x;
    const uint ibase = restart * W * H;
    const uint cbase = restart * K;

    float4 acc = float4(0.0f);
    for (uint t = 0; t < 9; ++t) {
        uint nb = nb_idx(pos.x, pos.y, t, W, H);
        uint k = indices[ibase + nb];
        acc += kF[t] * centroids[cbase + k];
    }
    out_buf[ibase + pi] = acc;
}

// -----------------------------------------------------------------
// Kernel 2: g(x) = F · R(x) where R = I - out.
// Operates in-place via two buffers (avoid feedback aliasing).
// -----------------------------------------------------------------
kernel void scolorq_compute_g(
    device       float4*  g_buf        [[ buffer(0) ]],   // R*W*H
    device const float4*  pixels       [[ buffer(1) ]],   // W*H
    device const float4*  out_buf      [[ buffer(2) ]],   // R*W*H (filtered output)
    constant ScolorqParams& params     [[ buffer(3) ]],
    constant uint&        restart      [[ buffer(4) ]],
    uint2                 pos          [[ thread_position_in_grid ]])
{
    if (pos.x >= params.width || pos.y >= params.height) return;
    const uint W = params.width, H = params.height;
    const uint pi = pos.y * W + pos.x;
    const uint ibase = restart * W * H;

    // Filter F applied to R = I - out. Same 3x3 stencil.
    float4 acc = float4(0.0f);
    for (uint t = 0; t < 9; ++t) {
        uint nb = nb_idx(pos.x, pos.y, t, W, H);
        float4 R = pixels[nb] - out_buf[ibase + nb];
        acc += kF[t] * R;
    }
    g_buf[ibase + pi] = acc;
}

// -----------------------------------------------------------------
// Kernel 3a: assign only. target(x) = p[a_old(x)] + g(x)/||F||².
// Writes indices_out; does NOT touch M/b.
// -----------------------------------------------------------------
kernel void scolorq_assign_only(
    device       uint*    indices_out  [[ buffer(0) ]],   // R*W*H
    device const uint*    indices_in   [[ buffer(1) ]],   // R*W*H
    device const float4*  centroids    [[ buffer(2) ]],   // R*K
    device const float4*  g_buf        [[ buffer(3) ]],   // R*W*H
    constant ScolorqParams& params     [[ buffer(4) ]],
    constant uint&        restart      [[ buffer(5) ]],
    uint2                 pos          [[ thread_position_in_grid ]])
{
    if (pos.x >= params.width || pos.y >= params.height) return;
    const uint W = params.width, K = params.num_centroids;
    const uint pi = pos.y * W + pos.x;
    const uint ibase = restart * params.width * params.height;
    const uint cbase = restart * K;

    const uint a_old = indices_in[ibase + pi];
    const float4 p_old = centroids[cbase + a_old];
    const float4 g = g_buf[ibase + pi];

    float4 target = p_old + g / kFNormSq;
    target.w = 0.0f;

    float best_d = INFINITY;
    uint  best_k = 0;
    for (uint k = 0; k < K; ++k) {
        float4 c = centroids[cbase + k];
        float dL = target.x - c.x;
        float da = target.y - c.y;
        float db = target.z - c.z;
        float d  = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best_k = k; }
    }
    indices_out[ibase + pi] = best_k;
}

// -----------------------------------------------------------------
// Kernel 3b: build M (K×K), b (K×3) from a fully-current
// assignment (indices_in here is the post-assign output).
// -----------------------------------------------------------------
kernel void scolorq_build_Mb(
    device const uint*    indices      [[ buffer(0) ]],   // R*W*H (current a)
    device const float4*  pixels       [[ buffer(1) ]],   // W*H
    device atomic_float*  M_buf        [[ buffer(2) ]],   // R*K*K
    device atomic_float*  b_buf        [[ buffer(3) ]],   // R*K*3
    constant ScolorqParams& params     [[ buffer(4) ]],
    constant uint&        restart      [[ buffer(5) ]],
    uint2                 pos          [[ thread_position_in_grid ]])
{
    if (pos.x >= params.width || pos.y >= params.height) return;
    const uint W = params.width, H = params.height, K = params.num_centroids;
    const uint pi = pos.y * W + pos.x;
    const uint ibase = restart * W * H;

    const float4 I_x = pixels[pi];

    // Bucket the 9 filter taps by their assignment.
    uint  k_seen[9];
    float w_seen[9];
    uint  n_seen = 0;
    for (uint t = 0; t < 9; ++t) {
        uint nb = nb_idx(pos.x, pos.y, t, W, H);
        uint k_t = indices[ibase + nb];
        bool found = false;
        for (uint i = 0; i < n_seen; ++i) {
            if (k_seen[i] == k_t) {
                w_seen[i] += kF[t];
                found = true;
                break;
            }
        }
        if (!found) {
            k_seen[n_seen] = k_t;
            w_seen[n_seen] = kF[t];
            n_seen += 1;
        }
    }

    const uint mbase = restart * K * K;
    const uint bbase = restart * K * 3;
    for (uint i = 0; i < n_seen; ++i) {
        uint k_i = k_seen[i];
        float w_i = w_seen[i];
        atomic_fetch_add_explicit(&b_buf[bbase + k_i * 3 + 0],
                                   w_i * I_x.x, memory_order_relaxed);
        atomic_fetch_add_explicit(&b_buf[bbase + k_i * 3 + 1],
                                   w_i * I_x.y, memory_order_relaxed);
        atomic_fetch_add_explicit(&b_buf[bbase + k_i * 3 + 2],
                                   w_i * I_x.z, memory_order_relaxed);
        for (uint j = 0; j < n_seen; ++j) {
            uint k_j = k_seen[j];
            atomic_fetch_add_explicit(&M_buf[mbase + k_i * K + k_j],
                                       w_i * w_seen[j],
                                       memory_order_relaxed);
        }
    }
}

// -----------------------------------------------------------------
// Kernel 3 (legacy combined): kept for ABI; not used by host now.
// -----------------------------------------------------------------
//
// Per pixel:
//   target(x) = p[a_old(x)] + g(x) / ||F||²
//   a_new(x)  = argmin_k ||p[k] - target(x)||²
//   accumulate M[k,k'] = F_k(x) · F_k'(x); b[k] = F_k(x) · I(x)
//
// F_k(x) is sparse: at most 9 non-zero entries per pixel (one per
// distinct filter-tap-assignment). We bucket the 9 taps in a local
// 256-slot array (indexed by k, requires K ≤ 256), then do the
// outer-product accumulation only for non-zero buckets.
// -----------------------------------------------------------------
kernel void scolorq_assign_and_build(
    device       uint*    indices_out  [[ buffer(0) ]],   // R*W*H
    device const uint*    indices_in   [[ buffer(1) ]],   // R*W*H
    device const float4*  pixels       [[ buffer(2) ]],   // W*H
    device const float4*  centroids    [[ buffer(3) ]],   // R*K
    device const float4*  g_buf        [[ buffer(4) ]],   // R*W*H
    device atomic_float*  M_buf        [[ buffer(5) ]],   // R*K*K
    device atomic_float*  b_buf        [[ buffer(6) ]],   // R*K*3
    device atomic_float*  sse_f        [[ buffer(7) ]],   // R
    constant ScolorqParams& params     [[ buffer(8) ]],
    constant uint&        restart      [[ buffer(9) ]],
    uint2                 pos          [[ thread_position_in_grid ]])
{
    if (pos.x >= params.width || pos.y >= params.height) return;
    const uint W = params.width, H = params.height, K = params.num_centroids;
    const uint pi = pos.y * W + pos.x;
    const uint ibase = restart * W * H;
    const uint cbase = restart * K;

    const float4 I_x = pixels[pi];
    const uint a_old = indices_in[ibase + pi];
    const float4 p_old = centroids[cbase + a_old];
    const float4 g = g_buf[ibase + pi];

    // 1. target = p[a_old] + g / ||F||²
    float4 target = p_old + g / kFNormSq;
    target.w = 0.0f;

    // 2. argmin_k ||p[k] - target||²
    float best_d = INFINITY;
    uint  best_k = 0;
    for (uint k = 0; k < K; ++k) {
        float4 c = centroids[cbase + k];
        float dL = target.x - c.x;
        float da = target.y - c.y;
        float db = target.z - c.z;
        float d  = dL * dL + da * da + db * db;
        if (d < best_d) { best_d = d; best_k = k; }
    }
    indices_out[ibase + pi] = best_k;

    // 3. Build sparse F_k(x): bucket the 9 filter taps by their
    //    PROVISIONAL assignment (i.e., neighbours use indices_in,
    //    centre uses best_k — we want the soon-to-be-current state).
    //    Use a thread-local hash by linear scan since at most 9
    //    distinct k values per pixel. K_local capped at 9 entries.
    uint  k_seen[9];
    float w_seen[9];
    uint  n_seen = 0;
    for (uint t = 0; t < 9; ++t) {
        uint nb = nb_idx(pos.x, pos.y, t, W, H);
        uint k_t;
        if (nb == pi) {
            k_t = best_k;
        } else {
            k_t = indices_in[ibase + nb];
        }
        // Linear search — n_seen ≤ 9.
        bool found = false;
        for (uint i = 0; i < n_seen; ++i) {
            if (k_seen[i] == k_t) {
                w_seen[i] += kF[t];
                found = true;
                break;
            }
        }
        if (!found) {
            k_seen[n_seen] = k_t;
            w_seen[n_seen] = kF[t];
            n_seen += 1;
        }
    }

    // 4. Accumulate M[k,k'] += w_k * w_k' and b[k] += w_k * I(x).
    const uint mbase = restart * K * K;
    const uint bbase = restart * K * 3;
    for (uint i = 0; i < n_seen; ++i) {
        uint k_i = k_seen[i];
        float w_i = w_seen[i];
        atomic_fetch_add_explicit(&b_buf[bbase + k_i * 3 + 0],
                                   w_i * I_x.x, memory_order_relaxed);
        atomic_fetch_add_explicit(&b_buf[bbase + k_i * 3 + 1],
                                   w_i * I_x.y, memory_order_relaxed);
        atomic_fetch_add_explicit(&b_buf[bbase + k_i * 3 + 2],
                                   w_i * I_x.z, memory_order_relaxed);
        for (uint j = 0; j < n_seen; ++j) {
            uint k_j = k_seen[j];
            atomic_fetch_add_explicit(&M_buf[mbase + k_i * K + k_j],
                                       w_i * w_seen[j],
                                       memory_order_relaxed);
        }
    }

    // SSE in the filtered-output space — we computed F·p[a] in
    // kernel 1 but the assignment changed; we approximate the
    // per-pixel cost with the current step's pre-flip value.
    // (Used only as a tie-breaker among restarts; not part of the
    // optimisation gradient.)
    atomic_fetch_add_explicit(&sse_f[restart], best_d,
                               memory_order_relaxed);
}
