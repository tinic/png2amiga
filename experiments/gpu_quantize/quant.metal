// Hello-world MSL kernel — proves the toolchain end-to-end.
// Adds 1.0 to each input element. Replace once parallel-Lloyd lands.
#include <metal_stdlib>
using namespace metal;

kernel void add_one(device const float* in  [[ buffer(0) ]],
                    device       float* out [[ buffer(1) ]],
                    uint gid [[ thread_position_in_grid ]]) {
    out[gid] = in[gid] + 1.0f;
}
