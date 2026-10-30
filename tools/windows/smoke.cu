/* Compile with nvcc -arch=sm_121a; no model or large allocations. */
#include <cuda_runtime.h>
#include <stdio.h>
#if !defined(_M_ARM64) && !defined(__aarch64__)
#error This smoke check requires Windows ARM64 host output
#endif
#define CHECK(call) do { cudaError_t e = (call); if (e != cudaSuccess) { \
    fprintf(stderr, "%s: %s\n", #call, cudaGetErrorString(e)); return 1; } } while (0)
__global__ void add(int *out) { out[threadIdx.x] = (int)threadIdx.x + 17; }
int main(void) {
    int count = 0, host[32], *device = NULL;
    CHECK(cudaGetDeviceCount(&count));
    if (count < 1) { fprintf(stderr, "No CUDA device\n"); return 1; }
    cudaDeviceProp prop;
    CHECK(cudaGetDeviceProperties(&prop, 0));
    printf("CUDA device: %s compute=%d.%d\n", prop.name, prop.major, prop.minor);
    CHECK(cudaMalloc((void **)&device, sizeof(host)));
    add<<<1, 32>>>(device);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
    CHECK(cudaMemcpy(host, device, sizeof(host), cudaMemcpyDeviceToHost));
    CHECK(cudaFree(device));
    for (int i = 0; i < 32; ++i) if (host[i] != i + 17) return 2;
    cudaError_t invalid = cudaSetDevice(count);
    if (invalid != cudaErrorInvalidDevice) {
        fprintf(stderr, "Unexpected invalid-device result: %s\n", cudaGetErrorString(invalid));
        return 3;
    }
    (void)cudaGetLastError();
    puts("ARM64 sm_121a CUDA smoke PASS: kernel values and error propagation");
    return 0;
}
