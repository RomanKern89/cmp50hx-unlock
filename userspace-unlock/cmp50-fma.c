/* cmp50-fma — measure FP32 FFMA throughput through the CUDA driver API.
 *
 * The CMP 50HX lock clamps FFMA (and DP4A) by roughly 1/32, so this number
 * separates "speed selectors applied" from "stock": ~0.4 TFLOP/s locked versus
 * ~13 TFLOP/s unlocked. No CUDA toolkit is needed: the kernel is PTX generated
 * here and JIT-compiled by the driver.
 *
 * The loop body is 64 back-to-back FMAs over 16 independent accumulators, so
 * the instruction stream is ~98% FFMA and the measurement follows the FP32
 * pipe rather than issue overhead.
 *
 *   ./cmp50-fma [iterations] [device]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ACC 16
#define UNROLL 4                    /* FMAs per accumulator per iteration */
#define MFMA (ACC * UNROLL)         /* 64 FMAs per loop iteration */

static char *build_ptx(void)
{
    size_t cap = 65536;
    char *p = malloc(cap);
    size_t n = 0;
    int a, u;

    if (!p)
        return NULL;
    n += snprintf(p + n, cap - n,
                  ".version 7.0\n.target sm_75\n.address_size 64\n"
                  ".visible .entry fma_bench(.param .u64 p_out, .param .f32 pb,\n"
                  "                          .param .f32 pc, .param .u32 p_iters)\n{\n"
                  "    .reg .f32 %%f<40>;\n    .reg .b32 %%r<4>;\n"
                  "    .reg .b64 %%rd<2>;\n    .reg .pred %%p<2>;\n"
                  "    ld.param.u64 %%rd1, [p_out];\n"
                  "    ld.param.f32 %%f1, [pb];\n"
                  "    ld.param.f32 %%f2, [pc];\n"
                  "    ld.param.u32 %%r1, [p_iters];\n");
    for (a = 0; a < ACC; a++)
        n += snprintf(p + n, cap - n, "    mov.f32 %%f%d, 0f3F800000;\n", 3 + a);
    n += snprintf(p + n, cap - n, "    mov.u32 %%r2, 0;\nLOOP:\n"
                                  "    setp.ge.u32 %%p1, %%r2, %%r1;\n"
                                  "    @%%p1 bra DONE;\n");
    for (u = 0; u < UNROLL; u++)
        for (a = 0; a < ACC; a++)
            n += snprintf(p + n, cap - n, "    fma.rn.f32 %%f%d, %%f%d, %%f1, %%f2;\n",
                          3 + a, 3 + a);
    n += snprintf(p + n, cap - n, "    add.u32 %%r2, %%r2, 1;\n    bra LOOP;\nDONE:\n"
                                  "    setp.eq.f32 %%p1, %%f3, 0f461C4000;\n"
                                  "    @%%p1 st.global.f32 [%%rd1], %%f3;\n"
                                  "    ret;\n}\n");
    return p;
}

#define SYM(v, s) do { \
        void *sym_ = dlsym(h, s); \
        if (!sym_) { fprintf(stderr, "missing %s\n", s); return 1; } \
        memcpy(&(v), &sym_, sizeof(sym_)); \
    } while (0)

#define CUDA_SUCCESS 0

int main(int argc, char **argv)
{
    void *h;
    int (*cu_init)(unsigned);
    int (*cu_dev_count)(int *);
    int (*cu_dev_get)(int *, int);
    int (*cu_ctx_create)(void **, unsigned, int);
    int (*cu_ctx_sync)(void);
    int (*cu_module_load)(void **, const void *);
    int (*cu_module_get_fn)(void **, void *, const char *);
    int (*cu_mem_alloc)(uint64_t *, size_t);
    int (*cu_mem_free)(uint64_t);
    int (*cu_launch)(void *, unsigned, unsigned, unsigned, unsigned, unsigned,
                     unsigned, unsigned, void *, void **, void **);
    int (*cu_get_err)(int, const char **);
    int ndev = 0, dev = 0, rc;
    void *ctx = NULL, *mod = NULL, *fn = NULL;
    uint64_t dptr = 0;
    uint32_t iters = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 20000;
    int devsel = argc > 2 ? atoi(argv[2]) : 0;
    unsigned grid = 1024, block = 256;
    float b = 1.000001f, c = 0.000001f;
    void *args[4];
    char *ptx;
    double secs, flops;
    struct timespec t0, t1;

    setvbuf(stdout, NULL, _IONBF, 0);
    h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "libcuda.so.1: %s\n", dlerror());
        return 1;
    }
    SYM(cu_init, "cuInit");
    SYM(cu_dev_count, "cuDeviceGetCount");
    SYM(cu_dev_get, "cuDeviceGet");
    SYM(cu_ctx_create, "cuCtxCreate_v2");
    SYM(cu_ctx_sync, "cuCtxSynchronize");
    SYM(cu_module_load, "cuModuleLoadData");
    SYM(cu_module_get_fn, "cuModuleGetFunction");
    SYM(cu_mem_alloc, "cuMemAlloc_v2");
    SYM(cu_mem_free, "cuMemFree_v2");
    SYM(cu_launch, "cuLaunchKernel");
    SYM(cu_get_err, "cuGetErrorString");

    ptx = build_ptx();
    if (!ptx) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    rc = cu_init(0);
    if (rc != CUDA_SUCCESS) { fprintf(stderr, "cuInit: %d\n", rc); return 1; }
    rc = cu_dev_count(&ndev);
    if (rc != CUDA_SUCCESS || !ndev) { fprintf(stderr, "no CUDA device (%d)\n", rc); return 1; }
    if (devsel >= ndev) { fprintf(stderr, "device %d of %d\n", devsel, ndev); return 1; }
    rc = cu_dev_get(&dev, devsel);
    if (rc != CUDA_SUCCESS) { fprintf(stderr, "cuDeviceGet: %d\n", rc); return 1; }
    rc = cu_ctx_create(&ctx, 0, dev);
    if (rc != CUDA_SUCCESS || !ctx) { fprintf(stderr, "cuCtxCreate: %d\n", rc); return 1; }
    rc = cu_module_load(&mod, ptx);
    if (rc != CUDA_SUCCESS) {
        const char *e = "?";

        cu_get_err(rc, &e);
        fprintf(stderr, "cuModuleLoadData: %d (%s)\n", rc, e);
        return 1;
    }
    rc = cu_module_get_fn(&fn, mod, "fma_bench");
    if (rc != CUDA_SUCCESS) { fprintf(stderr, "cuModuleGetFunction: %d\n", rc); return 1; }
    rc = cu_mem_alloc(&dptr, 4096);
    if (rc != CUDA_SUCCESS) { fprintf(stderr, "cuMemAlloc: %d\n", rc); return 1; }

    args[0] = &dptr;
    args[1] = &b;
    args[2] = &c;
    args[3] = &iters;

    /* warm-up: JIT the PTX and bring the clocks up */
    cu_launch(fn, grid, 1, 1, block, 1, 1, 0, 0, args, NULL);
    cu_ctx_sync();

    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = cu_launch(fn, grid, 1, 1, block, 1, 1, 0, 0, args, NULL);
    if (rc != CUDA_SUCCESS) { fprintf(stderr, "cuLaunchKernel: %d\n", rc); return 1; }
    rc = cu_ctx_sync();
    if (rc != CUDA_SUCCESS) { fprintf(stderr, "cuCtxSynchronize: %d\n", rc); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    flops = (double)grid * block * (double)iters * MFMA * 2.0;
    printf("device %d: %.4f s, %.3f TFLOP/s FP32 (FFMA), %u iters x %u threads\n",
           devsel, secs, flops / secs / 1e12, iters, grid * block);
    cu_mem_free(dptr);
    free(ptx);
    return 0;
}
