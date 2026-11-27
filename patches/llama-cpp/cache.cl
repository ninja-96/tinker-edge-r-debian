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

// Q6_K and Q8_0 blocks can start at a half-word boundary.
__kernel void mul_mat_packed(__global const uint *weights, __global const float *input,
                            __global float *output, uint blocks, __local float *partial,
                            uint weight_offset, uint input_offset, uint output_offset,
                            uint q6) {
    uint lane = get_local_id(0), row = get_group_id(0), col = get_global_id(1);
    uint n = q6 ? 256 : 32, size = q6 ? 210 : 34;
    float sum = 0;
    for (uint b = 0; b < blocks; ++b) {
        uint off = weight_offset * 4 + (row * blocks + b) * size;
        float d = vload_half((off + (q6 ? 208 : 0))/2, (__global const half *)weights);
        for (uint i = lane; i < n; i += get_local_size(0)) {
            int v;
            if (q6) {
                uint quarter = (i / 32) % 4;
                uint lo = packed_byte(weights, off + i/128*64 + (quarter%2)*32 + i%32);
                uint hi = packed_byte(weights, off + 128 + i/128*32 + i%32);
                int q = ((lo >> ((quarter/2)*4) & 15) | ((hi >> (quarter*2) & 3) << 4));
                int scale = ((int)packed_byte(weights, off + 192 + i/16) ^ 128) - 128;
                v = (q - 32) * scale;
            } else {
                v = ((int)packed_byte(weights, off + 2 + i) ^ 128) - 128;
            }
            sum += d * v * input[input_offset + (col*blocks+b)*n+i];
        }
    }
    partial[lane] = sum;
    for (uint step = get_local_size(0)/2; step; step /= 2) {
        barrier(CLK_LOCAL_MEM_FENCE);
        if (lane < step) partial[lane] += partial[lane+step];
    }
    if (!lane) output[output_offset + col*get_num_groups(0)+row] = partial[0];
}
