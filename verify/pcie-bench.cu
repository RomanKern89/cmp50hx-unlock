// Host<->device transfer bandwidth probe for the CMP 50HX Gen2 unlock.
// Pinned-memory bidirectional copies: the number that doubles when the
// link retrains from Gen1 x4 (~0.8 GB/s) to Gen2 x4 (~1.6 GB/s).
#include <chrono>
#include <cstdio>
#include <cstdlib>

using clk = std::chrono::steady_clock;

static double bench_copy(void *dst, const void *src, size_t bytes, int reps)
{
    double best = 0.0;
    // one warmup
    if (cudaMemcpy(dst, src, bytes, cudaMemcpyDefault) != cudaSuccess) {
        std::fprintf(stderr, "cudaMemcpy failed\n");
        std::exit(1);
    }
    for (int r = 0; r < reps; r++) {
        auto t0 = clk::now();
        if (cudaMemcpy(dst, src, bytes, cudaMemcpyDefault) != cudaSuccess) {
            std::fprintf(stderr, "cudaMemcpy failed\n");
            std::exit(1);
        }
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        double gbs = (double)bytes / s / (1000.0 * 1000.0 * 1000.0);
        if (gbs > best)
            best = gbs;
    }
    return best;
}

int main(void)
{
    const size_t bytes = 256u * 1024u * 1024u; // 256 MiB
    const int reps = 5;
    char *dev = nullptr;
    char *host = nullptr;

    if (cudaMalloc(&dev, bytes) != cudaSuccess ||
        cudaMallocHost(&host, bytes) != cudaSuccess) {
        std::fprintf(stderr, "allocation failed\n");
        return 1;
    }
    std::fill(host, host + bytes, 0x5a);

    int dev_i = 0;
    if (cudaGetDevice(&dev_i) != cudaSuccess)
        return 1;
    cudaDeviceProp prop = {};
    cudaGetDeviceProperties(&prop, dev_i);
    std::printf("device  : %s\n", prop.name);

    std::printf("H2D     : %.2f GB/s (best of %d, %zu MiB pinned)\n",
                bench_copy(dev, host, bytes, reps), reps, bytes >> 20);
    std::printf("D2H     : %.2f GB/s\n", bench_copy(host, dev, bytes, reps));
    std::printf("D2D     : %.2f GB/s (reference, no PCIe)\n",
                bench_copy(dev, dev, bytes, reps));

    cudaFreeHost(host);
    cudaFree(dev);
    return 0;
}
