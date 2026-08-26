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
// Global shared index (atomic)
static _Atomic int shared_index = 0;

// --- timing helpers ---
static inline double sec_since(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

// Heuristic chunk chooser (keeps chunks reasonably large to amortize search cost)
static int choose_chunk(int numQ, int numThreads)
{
    int c = numQ / (numThreads * 8);
    if (c < 256)
        c = 256;
    if (c > 16384)
        c = 16384;
    return c;
}

typedef struct
{
    int thread_id;
    Rect *queries;
    int *results;
    Node *root;
    int numQuery;
    int chunk_size;
    SearchCounters counters;
   // char pad[20]; // prevent false sharing
} ThreadArgs __attribute__((aligned(64)));

// Worker function with dynamic scheduling
void *thread_worker_dynamic(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    SearchWorkspace workspace;
    if (!initSearchWorkspace(&workspace, FANOUT + 1u)) {
        perror("search workspace allocation failed");
        exit(EXIT_FAILURE);
    }
    resetSearchCounters(&args->counters);

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
                        /* use iterative search variant to avoid recursion overhead */
                        args->results[i] = searchRTree_iter(args->root, args->queries[i], i,
                                                           &args->counters, &workspace);
        }
    }

    freeSearchWorkspace(&workspace);
    return NULL;
}

 void run_thread_pool_query_dynamic(Rect *query_rects, int *results, Node *root, int numQuery, int numThreads, int chunk_size, SearchCounters *out_counters)
{
    if (numThreads <= 0)
    {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        numThreads = (ncpu > 0) ? (int)ncpu : 4;
    }

    if (chunk_size <= 0)
        chunk_size = choose_chunk(numQuery, numThreads);

    pthread_t *threads = malloc((size_t)numThreads * sizeof(pthread_t));
    size_t args_bytes = (size_t)numThreads * sizeof(ThreadArgs);
    args_bytes = (args_bytes + 63u) & ~((size_t)63u);
    ThreadArgs *args = aligned_alloc(64, args_bytes);
    if (!threads || !args)
    {
        perror("allocation failed");
        exit(EXIT_FAILURE);
    }

    // Initialize shared index
    atomic_store(&shared_index, 0);

    for (int t = 0; t < numThreads; t++)
    {
        args[t] = (ThreadArgs){
            .thread_id = t,
            .queries = query_rects,
            .results = results,
            .root = root,
            .numQuery = numQuery,
            .chunk_size = chunk_size};
        resetSearchCounters(&args[t].counters);
        pthread_create(&threads[t], NULL, thread_worker_dynamic, &args[t]);
    }

    resetSearchCounters(out_counters);
    for (int t = 0; t < numThreads; t++)
    {
        pthread_join(threads[t], NULL);
        out_counters->query_count += args[t].counters.query_count;
        out_counters->node_visits += args[t].counters.node_visits;
        out_counters->leaf_visits += args[t].counters.leaf_visits;
        out_counters->rect_tests += args[t].counters.rect_tests;
        out_counters->child_pushes += args[t].counters.child_pushes;
    }

    free(threads);
    free(args); // Free dynamically allocated thread arguments here
}

int main(void)
{
    struct timespec t0, t1, t2, t3, t4,t5;
    double rtree_construction_time;
    int numRects, numQuery, dataset_option = 0;
    printf("\nHow many data you want to work with? Choose option: \n\t1. 6M\n\t2. Sports(999k)\n\t3. Sports(1.7M) \n\t4. parks(9.9M)\n\t5. cemetery(168k)\n\t6. Lakes(8M)\n\t7. Uniform(16M)\n\t8. Buildings\n");
    printf("\nEnter your option: ");

    if (scanf(" %d", &dataset_option) != 1)
    {
        fprintf(stderr, "Invalid input. Exiting.\n");
        return EXIT_FAILURE;
    }
    Rect *rects = selectDataDataset(&numRects, dataset_option);
    if (!rects)
    {
        printf("Failed to read points.\n");
        return -1;
    }

    printf("Read %d rects successfully.\n", numRects);
    printf("Total dataset size: %.2f MB\n", (numRects * sizeof(Rect)) / (1024.0 * 1024.0));
    // R-tree construction (sequential)
    clock_gettime(CLOCK_MONOTONIC, &t0);
   Node *root = createRTree_STR(rects, 0, numRects - 1);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    rtree_construction_time = sec_since(t0,t1);
    printf("\nR-tree construction time = %.2f s\n", rtree_construction_time);
    printRTreeStats(root);
    // Load queries
    Rect *query_rects = selectQueryDataset(&numQuery, dataset_option);

    if (!query_rects)
    {
        printf("Failed to read Query Rectangles.\n");
        return -1;
    }
    
    printf("Read %d query rects. Query data size: %.2f MB\n", numQuery, (numQuery * sizeof(Rect)) / (1024.0 * 1024.0));

    // Allocate result array
    int *cpu_overlap_count = calloc(numQuery, sizeof(int));

    // === Sequential Query Search ===
    long long found_seq = 0;
    SearchCounters seqCounters;
    SearchWorkspace seqWorkspace;
    if (!initSearchWorkspace(&seqWorkspace, FANOUT + 1u)) {
        perror("search workspace allocation failed");
        return EXIT_FAILURE;
    }
    resetSearchCounters(&seqCounters);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    for (int i = 0; i < numQuery; i++)
    {
        int result = searchRTree_iter(root, query_rects[i], i, &seqCounters,
                                      &seqWorkspace);
        cpu_overlap_count[i] = (long long)result;
        found_seq += (long long)result;
    }

    clock_gettime(CLOCK_MONOTONIC, &t3);
    freeSearchWorkspace(&seqWorkspace);
    double seq_time = sec_since(t2,t3);
    printf("\n[Sequential] Overlaps = %lld, Time = %.2f s\n", found_seq, seq_time);
    printSearchStats("Sequential", &seqCounters, seq_time);

    // === Parallel Query Search (Thread Pool) ===
    SearchCounters parCounters;
    clock_gettime(CLOCK_MONOTONIC, &t4);
    long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int numThreads = online_cpus > 0 ? (int)online_cpus : 1;
    const char *thread_env = getenv("RTREE_THREADS");
    if (thread_env && *thread_env) {
        char *end = NULL;
        long requested = strtol(thread_env, &end, 10);
        if (*end != '\0' || requested <= 0 || requested > INT_MAX) {
            fprintf(stderr, "Invalid RTREE_THREADS value: %s\n", thread_env);
            return EXIT_FAILURE;
        }
        numThreads = (int)requested;
    }
    memset(cpu_overlap_count, 0, numQuery * sizeof(int));
    
    // Use automatic chunk sizing instead of a hard-coded value.
    run_thread_pool_query_dynamic(query_rects, cpu_overlap_count, root, numQuery, numThreads, 0, &parCounters);

    long long found_par = 0;
    for (int i = 0; i < numQuery; i++)
    {
        found_par += (long long)cpu_overlap_count[i];
    }
    clock_gettime(CLOCK_MONOTONIC, &t5);
    double par_time = sec_since(t4,t5);
    double speedup = seq_time / par_time;

    printf("[Parallel]   Overlaps = %lld, Time = %.2f s (Threads: %d)\n", found_par, par_time, numThreads);
    printf("⚡ Speedup = %.2fx\n", speedup);
    printSearchStats("Parallel", &parCounters, par_time);

    //  Result check
    if (found_seq != found_par)
    {
        printf("❌ Mismatch between sequential and parallel results!\n");
    }
    else
    {
        printf("✅ Results match between sequential and parallel runs.\n");
    }
    if (seqCounters.query_count != parCounters.query_count ||
        seqCounters.node_visits != parCounters.node_visits ||
        seqCounters.leaf_visits != parCounters.leaf_visits ||
        seqCounters.rect_tests != parCounters.rect_tests ||
        seqCounters.child_pushes != parCounters.child_pushes) {
        fprintf(stderr, "❌ Search counters differ between sequential and parallel runs.\n");
        return EXIT_FAILURE;
    }
    printf("✅ Search counters match between sequential and parallel runs.\n");
    // === Write timing results to file ===
    writeTimingLog(numRects, numQuery, numThreads, seq_time, par_time, &seqCounters, &parCounters);

    // Cleanup
    free(cpu_overlap_count);
    free(rects);
    free(query_rects);

    return 0;
}
