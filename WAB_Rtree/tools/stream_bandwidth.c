#include <errno.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *aligned_malloc(size_t alignment, size_t size)
{
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0)
        return NULL;
    return ptr;
}

static double seconds_now(void)
{
    return omp_get_wtime();
}

int main(int argc, char **argv)
{
    size_t n = 50000000ULL;
    int iterations = 10;
    int threads = 36;
    double scalar = 3.0;

    if (argc > 1)
        n = strtoull(argv[1], NULL, 10);
    if (argc > 2)
        iterations = atoi(argv[2]);
    if (argc > 3)
        threads = atoi(argv[3]);

    if (n == 0 || iterations <= 0 || threads <= 0)
    {
        fprintf(stderr, "Usage: %s [array_len] [iterations] [threads]\n", argv[0]);
        return 1;
    }

    omp_set_num_threads(threads);

    size_t bytes = n * sizeof(double);
    double *a = aligned_malloc(64, bytes);
    double *b = aligned_malloc(64, bytes);
    double *c = aligned_malloc(64, bytes);
    if (!a || !b || !c)
    {
        fprintf(stderr, "Allocation failed for %zu elements per array: %s\n", n, strerror(errno));
        free(a);
        free(b);
        free(c);
        return 1;
    }

#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < n; i++)
    {
        a[i] = 1.0;
        b[i] = 2.0;
        c[i] = 0.0;
    }

    double best_copy = 0.0;
    double best_scale = 0.0;
    double best_add = 0.0;
    double best_triad = 0.0;

    for (int iter = 0; iter < iterations; iter++)
    {
        double start;
        double elapsed;
        double bw;

        start = seconds_now();
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < n; i++)
            c[i] = a[i];
        elapsed = seconds_now() - start;
        bw = (2.0 * bytes) / elapsed / 1e9;
        if (bw > best_copy)
            best_copy = bw;

        start = seconds_now();
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < n; i++)
            b[i] = scalar * c[i];
        elapsed = seconds_now() - start;
        bw = (2.0 * bytes) / elapsed / 1e9;
        if (bw > best_scale)
            best_scale = bw;

        start = seconds_now();
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < n; i++)
            c[i] = a[i] + b[i];
        elapsed = seconds_now() - start;
        bw = (3.0 * bytes) / elapsed / 1e9;
        if (bw > best_add)
            best_add = bw;

        start = seconds_now();
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < n; i++)
            a[i] = b[i] + scalar * c[i];
        elapsed = seconds_now() - start;
        bw = (3.0 * bytes) / elapsed / 1e9;
        if (bw > best_triad)
            best_triad = bw;
    }

    printf("STREAM-style bandwidth benchmark\n");
    printf("Array length per vector : %zu\n", n);
    printf("Bytes per vector        : %.2f MiB\n", bytes / 1024.0 / 1024.0);
    printf("Iterations              : %d\n", iterations);
    printf("Threads                 : %d\n", threads);
    printf("Copy best bandwidth     : %.2f GB/s\n", best_copy);
    printf("Scale best bandwidth    : %.2f GB/s\n", best_scale);
    printf("Add best bandwidth      : %.2f GB/s\n", best_add);
    printf("Triad best bandwidth    : %.2f GB/s\n", best_triad);

    free(a);
    free(b);
    free(c);
    return 0;
}
