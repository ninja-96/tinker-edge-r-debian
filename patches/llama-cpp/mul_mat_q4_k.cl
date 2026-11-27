// Use aligned word loads: dynamic byte loads miscompile on Panfrost/Rusticl 25.0.7.
inline uint packed_byte(__global const uint *p, uint i) {
    return (p[i / 4] >> ((i % 4) * 8)) & 255u;
}
// GGML MUL_MAT, contiguous Q4_K rows times F32 columns. One group per output.
__kernel void mul_mat_q4_k(__global const uint *weights, __global const float *input,
                           __global float *output, uint blocks, __local float *partial,
                           uint weight_offset, uint input_offset, uint output_offset) {
    weights += weight_offset;
    input += input_offset;
    output += output_offset;
    uint lane = get_local_id(0), width = get_local_size(0);
    uint row = get_group_id(0), col = get_global_id(1);
    float sum = 0;
    for (uint b = 0; b < blocks; ++b) {
        __global const uint *p = weights + (row * blocks + b) * 36;
        // OpenCL 1.2 vload_half converts binary16 storage to float without fp16 arithmetic.
        float d = vload_half(0, (__global const half *)p);
        float m = vload_half(1, (__global const half *)p);
        __global const uint *s = p + 1;
        for (uint i = lane; i < 256; i += width) {
            uint g = i / 32;
            uint scale = g < 4 ? packed_byte(s, g) & 63 : (packed_byte(s, g+4) & 15) | ((packed_byte(s, g-4) >> 6) << 4);
            uint minval = g < 4 ? packed_byte(s, g+4) & 63 : (packed_byte(s, g+4) >> 4) | ((packed_byte(s, g) >> 6) << 4);
            uint q = packed_byte(p, 16 + (i / 64) * 32 + i % 32);
            q = g & 1 ? q >> 4 : q & 15;
            sum += (d * scale * q - m * minval) * input[(col * blocks + b) * 256 + i];
        }
    }

    partial[lane] = sum;
    for (uint step = width / 2; step; step /= 2) {
        barrier(CLK_LOCAL_MEM_FENCE);
        if (lane < step) partial[lane] += partial[lane + step];
    }
    if (!lane) output[col * get_num_groups(0) + row] = partial[0];
}
