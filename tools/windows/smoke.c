/* Model-independent native target and monotonic clock check. */
#include <stdio.h>
#include <windows.h>
#if !defined(_M_ARM64) && !defined(__aarch64__)
#error This smoke check requires a native Windows ARM64 target
#endif
int main(void) {
    LARGE_INTEGER frequency, start, end;
    if (sizeof(void *) != 8 || !QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&start)) return 1;
    Sleep(10);
    if (!QueryPerformanceCounter(&end) || end.QuadPart <= start.QuadPart) return 2;
    printf("ARM64 C smoke PASS: pointer=%zu QPC=%lld elapsed=%.6f s\n",
           sizeof(void *), (long long)frequency.QuadPart,
           (double)(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart);
    return 0;
}
