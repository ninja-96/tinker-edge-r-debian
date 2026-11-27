// All offsets and strides are in bytes. Device buffers on T860 are below 2 GiB.
uint4 coords(uint i, uint4 shape) {
    uint4 c;
    c.x = i % shape.x; i /= shape.x;
    c.y = i % shape.y; i /= shape.y;
    c.z = i % shape.z; c.w = i / shape.z;
    return c;
}
uint address(uint4 c, uint4 stride) {
    uint4 offset = c * stride;
    return offset.x + offset.y + offset.z + offset.w;
}
void copy_value(__global const uchar *src, uint in_bytes, __global uchar *dst, uint out_bytes) {
    if (in_bytes == out_bytes) {
        if (in_bytes == 4) *(__global uint *)dst = *(__global const uint *)src;
        else *(__global ushort *)dst = *(__global const ushort *)src;
    } else if (out_bytes == 2) {
        vstore_half_rte(*(__global const float *)src, 0, (__global half *)dst);
    } else {
        *(__global float *)dst = vload_half(0, (__global const half *)src);
    }
}
#define TENSOR(t) __global uchar *t, uint t##_off, uint4 t##_shape, uint4 t##_stride
__kernel void copy_tensor(TENSOR(src), TENSOR(dst)) {
    uint i = get_global_id(0);
    copy_value(src + src_off + address(coords(i,src_shape),src_stride), src_stride.x,
               dst + dst_off + address(coords(i,dst_shape),dst_stride), dst_stride.x);
}
__kernel void set_rows(TENSOR(src), TENSOR(dst), TENSOR(ids)) {
    uint4 c = coords(get_global_id(0),src_shape);
    uint4 index = (uint4)(c.y,c.z % ids_shape.y,c.w % ids_shape.z,0);
    uint off = ids_off + address(index,ids_stride);
    // Valid GGML row indices fit in uint even with an I64 index tensor.
    uint row = *(__global const uint *)(ids + off);
    uint source = src_off + address(c,src_stride);
    c.y = row;
    copy_value(src + source,src_stride.x,dst + dst_off + address(c,dst_stride),dst_stride.x);
}

// Four adjacent Q6 values can start two bytes into a word.
inline uint q6_word(__global const uint *p, uint offset) {
    uint lo = p[offset / 4];
    return (offset & 2) ? ((lo >> 16) | (p[offset / 4 + 1] << 16)) : lo;
}
// Q6_K and Q8_0 blocks can start at a half-word boundary.
__kernel void mul_mat_packed(__global const uint *weights, __global const float *input,
                            __global float *output, uint blocks, __local float *partial,
                            uint weight_offset, uint input_offset, uint output_offset,
                            uint q6) {
    uint lane = get_local_id(0), row = get_group_id(0), col = get_global_id(1);
    uint size = q6 ? 210 : 34;
    float sum = 0;
    for (uint b = 0; b < blocks; ++b) {
        uint off = weight_offset * 4 + (row * blocks + b) * size;
        float d = vload_half((off + (q6 ? 208 : 0))/2, (__global const half *)weights);
        if (q6) {
            uint quarter = lane / 8, byte = (lane % 8) * 4;
            const uint4 shifts = (uint4)(0, 8, 16, 24);
            for (uint half_block = 0; half_block < 2; ++half_block) {
                uint i = half_block*128 + quarter*32 + byte;
                uint lo = q6_word(weights, off + half_block*64 + (quarter%2)*32 + byte);
                uint hi = q6_word(weights, off + 128 + half_block*32 + byte);
                uint packed = ((lo >> ((quarter/2)*4)) & 0x0f0f0f0fu)
                            | (((hi >> (quarter*2)) & 0x03030303u) << 4);
                float4 values = convert_float4(((uint4)(packed) >> shifts) & 255u) - 32.0f;
                int scale = ((int)packed_byte(weights, off + 192 + i/16) ^ 128) - 128;
                sum += (d * scale) * dot(values, vload4(0,input + input_offset + (col*blocks+b)*256+i));
            }
        } else {
            int v = ((int)packed_byte(weights, off + 2 + lane) ^ 128) - 128;
            sum += d * v * input[input_offset + (col*blocks+b)*32 + lane];
        }
    }
    partial[lane] = sum;
    for (uint step = get_local_size(0)/2; step; step /= 2) {
        barrier(CLK_LOCAL_MEM_FENCE);
        if (lane < step) partial[lane] += partial[lane+step];
    }
    if (!lane) output[output_offset + col*get_num_groups(0)+row] = partial[0];
}
