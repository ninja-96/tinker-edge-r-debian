// Build with c++ q4_k_opencl.cpp -lOpenCL; pass mul_mat_q4_k.cl as argv[1].
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#define CHECK(call) do { cl_int rc = (call); if (rc) { fprintf(stderr, "%s: %d\n", #call, rc); exit(1); } } while (0)
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1]);
    std::string source{std::istreambuf_iterator<char>(file), {}};
    if (source.empty()) return 2;
    cl_uint count;
    CHECK(clGetPlatformIDs(0, nullptr, &count));
    std::vector<cl_platform_id> platforms(count);
    CHECK(clGetPlatformIDs(count, platforms.data(), nullptr));
    cl_device_id device = nullptr;
    for (auto platform : platforms)
        if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, nullptr) == CL_SUCCESS) break;
    char name[256];
    CHECK(clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof name, name, nullptr));
    cl_int err;
    auto ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err); CHECK(err);
    auto queue = clCreateCommandQueue(ctx, device, 0, &err); CHECK(err);
    const char *src = source.c_str();
    auto program = clCreateProgramWithSource(ctx, 1, &src, nullptr, &err); CHECK(err);
    if (clBuildProgram(program, 1, &device, "-cl-std=CL1.2", nullptr, nullptr)) {
        size_t n;
        CHECK(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n));
        std::vector<char> log(n);
        CHECK(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, n, log.data(), nullptr));
        fprintf(stderr,"%s\n",log.data()); return 1;
    }
    auto kernel = clCreateKernel(program,"mul_mat_q4_k",&err); CHECK(err);
    constexpr unsigned blocks=2, rows=3, cols=2;
    unsigned char weights[rows*blocks*144]{};
    float x[cols*blocks*256], ref[cols*rows]{}, out[cols*rows];
    for (unsigned i=0;i<cols*blocks*256;++i) x[i]=(int(i%23)-11)/16.f;
    for (unsigned r=0;r<rows;++r) for (unsigned b=0;b<blocks;++b) {
        auto p=weights+(r*blocks+b)*144;
        p[1]=0x38; p[3]=0x34; // binary16 0.5 and 0.25
        auto s=p+4;
        for (unsigned g=0;g<8;++g) {
            unsigned scale=(37*g+13*r+b)%64, m=(23*g+31*r+b)%64;
            if (g<4) { s[g]|=scale; s[g+4]|=m; }
            else { s[g-4]|=(scale>>4)<<6; s[g]|=(m>>4)<<6; s[g+4]=(scale&15)|((m&15)<<4); }
            for (unsigned l=0;l<32;++l) {
                unsigned q=(7*l+3*g+r+b)%16;
                p[16+g/2*32+l]|=q<<(4*(g%2));
                for (unsigned c=0;c<cols;++c)
                    ref[c*rows+r]+=(0.5f*scale*q-0.25f*m)*x[(c*blocks+b)*256+g*32+l];
            }
        }
    }
    auto buf=[&](void *data,size_t n) {
        auto mem=clCreateBuffer(ctx, CL_MEM_READ_WRITE | (data ? CL_MEM_COPY_HOST_PTR : 0),n,data,&err);
        CHECK(err); return mem;
    };
    cl_mem args[]={buf(weights,sizeof weights),buf(x,sizeof x),buf(nullptr,sizeof out)};
    for (unsigned i=0;i<3;++i) CHECK(clSetKernelArg(kernel,i,sizeof(cl_mem),&args[i]));
    CHECK(clSetKernelArg(kernel,3,sizeof blocks,&blocks));
    cl_uint offset = 0;
    for (unsigned i=5;i<8;++i) CHECK(clSetKernelArg(kernel,i,sizeof offset,&offset));
    size_t local[]={32,1}, global[]={rows*32,cols};
    CHECK(clSetKernelArg(kernel,4,32*sizeof(float),nullptr));
    CHECK(clEnqueueNDRangeKernel(queue,kernel,2,nullptr,global,local,0,nullptr,nullptr));
    CHECK(clEnqueueReadBuffer(queue,args[2],CL_TRUE,0,sizeof out,out,0,nullptr,nullptr));
    for (unsigned i=0;i<rows*cols;++i) if (!(std::abs(out[i]-ref[i])<0.001f)) {
        fprintf(stderr,"out[%u]=%f expected %f\n",i,out[i],ref[i]); return 1;
    }
    printf("%s: Q4_K [3,512] x F32 [512,2] PASS\n", name);
}
