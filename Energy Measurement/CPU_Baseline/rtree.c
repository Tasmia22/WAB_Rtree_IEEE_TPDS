#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include "rtree.h"
#include <stdatomic.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#ifndef ENABLE_ROOFLINE
#define ENABLE_ROOFLINE 0
#endif

static _Atomic int shared_index = 0;

static inline double sec_since(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static int get_repeat_count(void)
{
    const char *env = getenv("REPEAT_COUNT");
    if (env == NULL || env[0] == '\0')
        return 5;

    char *endp = NULL;
    errno = 0;
    long parsed = strtol(env, &endp, 10);
    if (errno == ERANGE || endp == env || *endp != '\0' || parsed <= 0 || parsed > INT_MAX)
    {
        fprintf(stderr, "Invalid REPEAT_COUNT=%s, falling back to 5.\n", env);
        return 5;
    }

    return (int)parsed;
}

static int choose_chunk(int numQ, int numThreads)
{
    int c = numQ / (numThreads * 8);
    if (c < 256)
        c = 256;
    if (c > 16384)
        c = 16384;
    return c;
}

static void *aligned_alloc_zeroed(size_t alignment, size_t size)
{
    void *ptr = NULL;
    int rc = posix_memalign(&ptr, alignment, size);
    if (rc != 0)
    {
        fprintf(stderr, "posix_memalign failed: %s\n", strerror(rc));
        exit(EXIT_FAILURE);
    }

    memset(ptr, 0, size);
    return ptr;
}

#if ENABLE_ROOFLINE
static void print_roofline_summary(const char *label, const SearchRooflineMetrics *metrics, double time_s)
{
    double intensity = 0.0;
    double throughput = 0.0;
    double mib = 0.0;

    if (metrics->bytes_accessed > 0)
        intensity = (double)metrics->overlap_tests / (double)metrics->bytes_accessed;
    if (time_s > 0.0)
        throughput = (double)metrics->overlap_tests / time_s;
    mib = (double)metrics->bytes_accessed / (1024.0 * 1024.0);

    printf("[%s Roofline] Work = %llu overlap tests, Bytes = %llu (%.2f MiB), OI = %.6f tests/byte, Throughput = %.2f Mtests/s\n",
           label,
           metrics->overlap_tests,
           metrics->bytes_accessed,
           mib,
           intensity,
           throughput / 1e6);
}
#endif

// static inline int run_query_search(Node *root, Rect query, int query_index, SearchRooflineMetrics *metrics)
// {
// #if ENABLE_ROOFLINE
//     return searchRTree_iter_with_metrics(root, query, query_index, metrics);
// #else
//     (void)metrics;
//     return searchRTree_iter(root, query, query_index);
// #endif
// }

static inline int run_query_search(Node *root, Rect query, int query_index, SearchRooflineMetrics *metrics)
{
#if ENABLE_ROOFLINE
    return searchRTree_with_metrics(root, query, query_index, metrics);
#else
    (void)metrics;
    return searchRTree(root, query, query_index);
#endif
}

typedef struct
{
    int thread_id;
    Rect *queries;
    int *results;
    Node *root;
    int numQuery;
    int chunk_size;
#if ENABLE_ROOFLINE
    SearchRooflineMetrics *metrics;
#endif
} ThreadArgs __attribute__((aligned(64)));

void *thread_worker_dynamic(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;

    while (1)
    {
        int start = atomic_fetch_add(&shared_index, args->chunk_size);
        if (start >= args->numQuery)
            break;

        int end = start + args->chunk_size;
        if (end > args->numQuery)
            end = args->numQuery;

        for (int i = start; i < end; i++)
        {
#if ENABLE_ROOFLINE
            args->results[i] = run_query_search(args->root, args->queries[i], i, args->metrics);
#else
            args->results[i] = run_query_search(args->root, args->queries[i], i, NULL);
#endif
        }
    }

    return NULL;
}

void run_thread_pool_query_dynamic(Rect *query_rects, int *results, Node *root, int numQuery, int numThreads,
                                   int chunk_size, SearchRooflineMetrics *aggregate_metrics)
{
    if (chunk_size <= 0)
        chunk_size = choose_chunk(numQuery, numThreads);

    pthread_t *threads = malloc((size_t)numThreads * sizeof(pthread_t));
    ThreadArgs *args = aligned_alloc_zeroed(64, (size_t)numThreads * sizeof(ThreadArgs));
#if ENABLE_ROOFLINE
    SearchRooflineMetrics *thread_metrics = aligned_alloc_zeroed(64, (size_t)numThreads * sizeof(SearchRooflineMetrics));
#endif
    if (!threads)
    {
        perror("malloc threads");
        exit(EXIT_FAILURE);
    }

#if ENABLE_ROOFLINE
    memset(thread_metrics, 0, (size_t)numThreads * sizeof(SearchRooflineMetrics));
    if (aggregate_metrics)
        *aggregate_metrics = (SearchRooflineMetrics){0};
#else
    (void)aggregate_metrics;
#endif

    atomic_store(&shared_index, 0);

    for (int t = 0; t < numThreads; t++)
    {
        args[t] = (ThreadArgs){
            .thread_id = t,
            .queries = query_rects,
            .results = results,
            .root = root,
            .numQuery = numQuery,
            .chunk_size = chunk_size,
#if ENABLE_ROOFLINE
            .metrics = &thread_metrics[t],
#endif
        };
        int rc = pthread_create(&threads[t], NULL, thread_worker_dynamic, &args[t]);
        if (rc != 0)
        {
            fprintf(stderr, "pthread_create failed for thread %d: %s\n", t, strerror(rc));
            exit(EXIT_FAILURE);
        }
    }

    for (int t = 0; t < numThreads; t++)
    {
        int rc = pthread_join(threads[t], NULL);
        if (rc != 0)
        {
            fprintf(stderr, "pthread_join failed for thread %d: %s\n", t, strerror(rc));
            exit(EXIT_FAILURE);
        }
#if ENABLE_ROOFLINE
        if (aggregate_metrics)
        {
            aggregate_metrics->overlap_tests += thread_metrics[t].overlap_tests;
            aggregate_metrics->bytes_accessed += thread_metrics[t].bytes_accessed;
        }
#endif
    }

#if ENABLE_ROOFLINE
    free(thread_metrics);
#endif
    free(threads);
    free(args);
}

int main()
{
    struct timespec t0, t1, t2, t3, t4, t5;
    static const int thread_counts[] = {18, 32};
    const size_t parallel_run_count = sizeof(thread_counts) / sizeof(thread_counts[0]);
    double rtree_construction_time = 0.0;
    double seq_time = 0.0;
    bool all_parallel_results_match = true;
    int numRects = 0;
    int numQuery = 0;
    int dataset_option = 0;
    int exit_code = EXIT_FAILURE;
    Rect *rects = NULL;
    Rect *query_rects = NULL;
    Node *root = NULL;
    int *cpu_overlap_count = NULL;
    
printf("\nWhich dataset do you want to work with? Choose option:"
       "\n\t1. Sports"
       "\n\t2. Lakes"
       "\n\t3. Parks"
       "\n\t4. Synthetic"
       "\n\t5. Buildings\n");

printf("\nEnter your option: ");

if (scanf(" %d", &dataset_option) != 1)
{
    fprintf(stderr, "Invalid input. Exiting.\n");
    goto cleanup;
}

    rects = selectDataDataset(&numRects, dataset_option);
    if (!rects)
    {
        fprintf(stderr, "Failed to read points.\n");
        goto cleanup;
    }

    printf("Read %d rects successfully.\n", numRects);
    printf("Total dataset size: %.2f MB\n", (numRects * sizeof(Rect)) / (1024.0 * 1024.0));

    clock_gettime(CLOCK_MONOTONIC, &t0);
    root = createRTree_STR(rects, 0, numRects - 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!root)
    {
        fprintf(stderr, "Failed to construct R-tree.\n");
        goto cleanup;
    }

    rtree_construction_time = sec_since(t0, t1);
    printf("\nR-tree construction time = %.2f s\n", rtree_construction_time);
    printRTreeStats(root);

    query_rects = selectQueryDataset(&numQuery, dataset_option);
    if (!query_rects)
    {
        fprintf(stderr, "Failed to read Query Rectangles.\n");
        goto cleanup;
    }

    printf("Read %d query rects. Query data size: %.2f MB\n", numQuery, (numQuery * sizeof(Rect)) / (1024.0 * 1024.0));

    cpu_overlap_count = calloc((size_t)numQuery, sizeof(int));
    if (!cpu_overlap_count)
    {
        perror("Failed to allocate overlap count buffer");
        goto cleanup;
    }

    long long found_seq = 0;
#if ENABLE_ROOFLINE
    SearchRooflineMetrics seq_metrics = {0};
#endif
    clock_gettime(CLOCK_MONOTONIC, &t2);
    for (int i = 0; i < numQuery; i++)
    {
#if ENABLE_ROOFLINE
        int result = run_query_search(root, query_rects[i], i, &seq_metrics);
#else
        int result = run_query_search(root, query_rects[i], i, NULL);
#endif
        cpu_overlap_count[i] = result;
        found_seq += (long long)result;
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);
    seq_time = sec_since(t2, t3);
    printf("\n[Sequential] Overlaps = %lld, Time = %.2f s\n", found_seq, seq_time);
#if ENABLE_ROOFLINE
    print_roofline_summary("Sequential", &seq_metrics, seq_time);
#endif

    const int repeat_count = get_repeat_count();
    for (int rep = 0; rep < repeat_count; ++rep)
    {
        printf("\nParallel search timings (rep %d/%d):\n", rep + 1, repeat_count);
        for (size_t run = 0; run < parallel_run_count; run++)
        {
            const int numThreads = thread_counts[run];
            double par_time = 0.0;
            long long found_par = 0;
#if ENABLE_ROOFLINE
            SearchRooflineMetrics par_metrics = {0};
#endif

            memset(cpu_overlap_count, 0, (size_t)numQuery * sizeof(int));
            clock_gettime(CLOCK_MONOTONIC, &t4);

#if ENABLE_ROOFLINE
            run_thread_pool_query_dynamic(query_rects, cpu_overlap_count, root, numQuery, numThreads, 0, &par_metrics);
#else
            run_thread_pool_query_dynamic(query_rects, cpu_overlap_count, root, numQuery, numThreads, 0, NULL);
#endif

            clock_gettime(CLOCK_MONOTONIC, &t5);
            par_time = sec_since(t4, t5);

            for (int i = 0; i < numQuery; i++)
            {
                found_par += (long long)cpu_overlap_count[i];
            }

            printf("[Parallel %zu/%zu] Threads = %d, Overlaps = %lld, Time = %.2f s, Speedup = %.2fx\n",
                   run + 1, parallel_run_count,
                   numThreads,
                   found_par,
                   par_time,
                   seq_time / par_time);
#if ENABLE_ROOFLINE
            print_roofline_summary("Parallel", &par_metrics, par_time);
#endif

            if (found_seq != found_par)
            {
                all_parallel_results_match = false;
                printf("Result check: mismatch between sequential and parallel results for %d threads.\n", numThreads);
            }

        }

    }

    if (all_parallel_results_match)
    {
        printf("Result check: all parallel runs matched the sequential results.\n");
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    free(cpu_overlap_count);
    free(query_rects);
    destroyRTree(root);
    free(rects);

    return exit_code;
}
