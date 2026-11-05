// FP16 tensor-core GEMM (cuBLAS, 8192^3, FP32/FP16 accumulate) and device-memory bandwidth.
//   nvcc -O2 -ccbin g++-13 -arch=sm_75 hgemm-bench.cu -lcublas -o hgemm-bench
//   ./hgemm-bench <device>
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include <cuda_fp16.h>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
    fprintf(stderr, "%s:%d %s: %s\n", __FILE__, __LINE__, #x, cudaGetErrorString(e_)); exit(1); } } while (0)
#define CB(x) do { cublasStatus_t s_ = (x); if (s_ != CUBLAS_STATUS_SUCCESS) { \
    fprintf(stderr, "%s:%d %s: cublas status %d\n", __FILE__, __LINE__, #x, (int)s_); exit(1); } } while (0)

// Non-zero inputs in [-1, 1): an all-zero GEMM draws less power than real data.
__global__ void fill(__half *p, size_t n, unsigned seed)
{
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) {
        unsigned x = (unsigned)i * 2654435761u ^ seed;
        x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
        p[i] = __float2half((float)(x & 0xffff) / 32768.0f - 1.0f);
    }
}

static double gemm_tflops(cublasHandle_t h, int n, cublasComputeType_t ct, const __half *A, const __half *B, __half *C, int reps)
{
    const float a32 = 1.f, b32 = 0.f;
    const __half a16 = __float2half(1.f), b16 = __float2half(0.f);
    const void *alpha = ct == CUBLAS_COMPUTE_16F ? (const void *)&a16 : (const void *)&a32;
    const void *beta = ct == CUBLAS_COMPUTE_16F ? (const void *)&b16 : (const void *)&b32;
    cudaEvent_t e0, e1;
    CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
    for (int i = 0; i <= reps; i++) {          // iteration 0 is the warm-up
        if (i == 1) CK(cudaEventRecord(e0));
        CB(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, n, n, n, alpha, A, CUDA_R_16F, n, B, CUDA_R_16F, n,
                        beta, C, CUDA_R_16F, n, ct, CUBLAS_GEMM_DEFAULT));
    }
    CK(cudaEventRecord(e1)); CK(cudaEventSynchronize(e1));
    float ms = 0; CK(cudaEventElapsedTime(&ms, e0, e1));
    CK(cudaEventDestroy(e0)); CK(cudaEventDestroy(e1));
    return 2.0 * n * (double)n * n * reps / (ms / 1e3) / 1e12;
}

int main(int argc, char **argv)
{
    int ndev = 0; CK(cudaGetDeviceCount(&ndev));
    int dev = argc > 1 ? atoi(argv[1]) : 0;
    if (dev < 0 || dev >= ndev) { fprintf(stderr, "device %d out of range (0..%d)\n", dev, ndev - 1); return 1; }
    CK(cudaSetDevice(dev));
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, dev));

    const int n = 8192; const size_t elems = (size_t)n * n;
    __half *A, *B, *C;
    CK(cudaMalloc(&A, elems * 2)); CK(cudaMalloc(&B, elems * 2)); CK(cudaMalloc(&C, elems * 2));
    fill<<<1024, 256>>>(A, elems, 1u); fill<<<1024, 256>>>(B, elems, 2u);
    CK(cudaGetLastError()); CK(cudaDeviceSynchronize());

    cublasHandle_t h; CB(cublasCreate(&h));
    const double t32 = gemm_tflops(h, n, CUBLAS_COMPUTE_32F, A, B, C, 20);
    const double t16 = gemm_tflops(h, n, CUBLAS_COMPUTE_16F, A, B, C, 20);
    CB(cublasDestroy(h));
    CK(cudaFree(A)); CK(cudaFree(B)); CK(cudaFree(C));

    // device memory: 1 GiB device-to-device copy (each byte read once, written once)
    const size_t cb = 1ull << 30; void *X, *Y;
    CK(cudaMalloc(&X, cb)); CK(cudaMalloc(&Y, cb)); CK(cudaMemset(X, 1, cb));
    cudaEvent_t e0, e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
    CK(cudaMemcpy(Y, X, cb, cudaMemcpyDeviceToDevice));
    CK(cudaEventRecord(e0));
    for (int i = 0; i < 10; i++) CK(cudaMemcpy(Y, X, cb, cudaMemcpyDeviceToDevice));
    CK(cudaEventRecord(e1)); CK(cudaEventSynchronize(e1));
    float ms = 0; CK(cudaEventElapsedTime(&ms, e0, e1));
    const double bw = 2.0 * cb * 10 / (ms / 1e3) / 1e9;

    printf("device %d %s: FP16 tensor GEMM %.1f TFLOP/s (FP32 acc), %.1f TFLOP/s (FP16 acc); "
           "device memory %.0f GB/s (read + write)\n", dev, p.name, t32, t16, bw);
    return 0;
}
