#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "host_runtime.h"


static void ensure_log_dir(const char *path)
{
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
    {
        perror("Failed to create log directory");
        exit(EXIT_FAILURE);
    }
}

int get_rtree_memory_size(Node *node)
{
    if (node == NULL)
        return 0;

    int size = (int)sizeof(Node);
    if (node->isLeaf)
    {
        size += (int)(sizeof(Rect) * (size_t)node->count);
    }
    else
    {
        size += (int)(sizeof(Node *) * (size_t)node->count);
        for (int i = 0; i < node->count; i++)
        {
            size += get_rtree_memory_size(node->children[i]);
        }
    }

    return size;
}

static const char *dataset_log_tag(int dataset_option)
{
    switch (dataset_option)
    {
    case 1:
        return "uniform16m";
    case 2:
        return "sports999k";
    case 3:
        return "sports17m";
    case 4:
        return "parks300k";
    case 5:
        return "cemetery168k";
    case 6:
        return "lakes8m";
    case 7:
        return "buildings143m";
    case 8:
        return "roads71m";
    case 9:
        return "parksfull";
    case 10:
        return "fullbuilding114m";
    default:
        return "unknown";
    }
}

int main(void)
{
    struct timespec t0, t1, t2, t3;
    time_t raw_time = time(NULL);
    struct tm *tm_info = localtime(&raw_time);
    char timestamp[32];
    char log_filename[128];

    int numRects = 0;
    int numQuery = 0;
    int dataset_option = 0;
    printf("\nHow many data you want to work with? Choose option: \n\t1. 16M\n\t2. Sports(999k)\n\t3. Sports(1.7M) \n\t4. parks(300k)\n\t5. cemetery(168k)\n\t6. Lakes(8M)\n\t7. Buildings(14.3M)\n\t8.Roads(71M)\n\t9. Full Parks\n\t10. FullBuilding\n\t11. HalfBuilding\n");
    printf("\nEnter your option: ");

    if (scanf(" %d", &dataset_option) != 1)
    {
        fprintf(stderr, "Invalid input. Exiting.\n");
        return EXIT_FAILURE;
    }

    const char *dataset_tag = dataset_log_tag(dataset_option);

    ensure_log_dir("logs");
    snprintf(log_filename, sizeof(log_filename), "logs/%s", dataset_tag);
    ensure_log_dir(log_filename);

    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
#if NR_DPUS == 2540
    snprintf(log_filename, sizeof(log_filename), "logs/%s/dpu_%s.log",
             dataset_tag, timestamp);
#else
    snprintf(log_filename, sizeof(log_filename), "logs/%s/dpu_nr%u_%s.log",
             dataset_tag, (unsigned)NR_DPUS, timestamp);
#endif

    FILE *log_file = fopen(log_filename, "w");
    if (log_file == NULL)
    {
        perror("Failed to open log file");
        return EXIT_FAILURE;
    }

    Rect *rects = selectDataDataset(&numRects, dataset_option);
    if (rects == NULL)
    {
        printf("Failed to read points.\n");
        fclose(log_file);
        return EXIT_FAILURE;
    }

    printf("Read %d rects successfully.\n", numRects);
    printf("Total dataset size: %.2f MB\n", (numRects * sizeof(Rect)) / (1024.0 * 1024.0));

    LOGF(log_file, "\n[Ablation] R-tree geometry mode: rectangle-based\n");

    clock_gettime(CLOCK_MONOTONIC, &t0);
    Node *root = createRTree_STR(rects, 0, numRects - 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double rtree_construction_time = sec_since(t0, t1);
    printf("\nRtree construction time %.6f", rtree_construction_time);
    printRTreeStats(root);

    int rtree_mem_size = get_rtree_memory_size(root);
    printf("\nActual R-tree memory size: %.2f MB\n", rtree_mem_size / (1024.0 * 1024.0));

    Rect *query_rects = selectQueryDataset(&numQuery, dataset_option);
    if (query_rects == NULL)
    {
        printf("Failed to read Query Rectangles.\n");
        free(rects);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    printf("Read %d rects successfully. with Querydata size: %.2f MB\n",
           numQuery, (numQuery * sizeof(Rect)) / (1024.0 * 1024.0));

    bool skip_cpu_verify = (getenv("SKIP_CPU_VERIFY") != NULL);

    uint32_t result_return_mode = RESULT_RETURN_MODE_SPARSE;
    const char *result_mode_env = getenv("RESULT_RETURN_MODE");
    if (result_mode_env != NULL)
    {
        if (strcmp(result_mode_env, "dense") == 0 || strcmp(result_mode_env, "1") == 0)
            result_return_mode = RESULT_RETURN_MODE_DENSE;
        else if (strcmp(result_mode_env, "sparse") != 0 && strcmp(result_mode_env, "0") != 0)
            fprintf(stderr, "Unknown RESULT_RETURN_MODE=%s, falling back to sparse.\n", result_mode_env);
    }
    const char *result_mode_name = (result_return_mode == RESULT_RETURN_MODE_DENSE) ? "dense array" : "sparse pairs";

    uint32_t replica_query_policy = REPLICA_QUERY_POLICY_BLOCK_CYCLIC;
    const char *replica_query_policy_env = getenv("REPLICA_QUERY_POLICY");
    if (replica_query_policy_env != NULL && replica_query_policy_env[0] != '\0')
    {
        if (strcmp(replica_query_policy_env, "range") == 0)
            replica_query_policy = REPLICA_QUERY_POLICY_RANGE;
        else if (strcmp(replica_query_policy_env, "block") == 0 ||
                 strcmp(replica_query_policy_env, "block-cyclic") == 0 ||
                 strcmp(replica_query_policy_env, "block_cyclic") == 0)
            replica_query_policy = REPLICA_QUERY_POLICY_BLOCK_CYCLIC;
        else
        {
            fprintf(stderr, "Invalid REPLICA_QUERY_POLICY=%s (expected range or block)\n",
                    replica_query_policy_env);
            free(query_rects);
            free(rects);
            fclose(log_file);
            return EXIT_FAILURE;
        }
    }

    uint32_t replica_query_block_queries = choose_replica_query_block_queries(numQuery);
    bool replica_query_block_auto = true;
    const char *replica_query_block_env = getenv("REPLICA_QUERY_BLOCK_QUERIES");
    if (replica_query_block_env != NULL && replica_query_block_env[0] != '\0')
    {
        char *endp = NULL;
        long parsed = strtol(replica_query_block_env, &endp, 10);
        if (endp == replica_query_block_env || *endp != '\0' || parsed <= 0)
        {
            fprintf(stderr, "Invalid REPLICA_QUERY_BLOCK_QUERIES=%s (must be >= 1)\n",
                    replica_query_block_env);
            free(query_rects);
            free(rects);
            fclose(log_file);
            return EXIT_FAILURE;
        }
        replica_query_block_queries = (uint32_t)parsed;
        replica_query_block_auto = false;
    }

    LOGF(log_file, "\n[Ablation] Result return mode: %s%s\n",
         result_mode_name,
         (result_return_mode == RESULT_RETURN_MODE_DENSE) ? " (one-off override via RESULT_RETURN_MODE)" : " (default)");
    LOGF(log_file, "[CPU-ONLY] Verification mode: unified host baseline%s\n",
         skip_cpu_verify ? " (inactive because SKIP_CPU_VERIFY=1)" : "");

    uint64_t found_count = 0;
    double search_time = -1.0;
    if (!skip_cpu_verify)
    {
        int *cpu_overlap_count = (int *)calloc((size_t)numQuery, sizeof(*cpu_overlap_count));
        if (numQuery > 0 && cpu_overlap_count == NULL)
        {
            perror("Failed to allocate CPU overlap buffer");
            free(query_rects);
            free(rects);
            fclose(log_file);
            return EXIT_FAILURE;
        }

        clock_gettime(CLOCK_MONOTONIC, &t2);
        for (int i = 0; i < numQuery; i++)
        {
            cpu_overlap_count[i] = searchRTree(root, query_rects[i], i);
        }
        clock_gettime(CLOCK_MONOTONIC, &t3);

        for (int i = 0; i < numQuery; i++)
        {
            found_count += cpu_overlap_count[i];
        }
        free(cpu_overlap_count);

        search_time = sec_since(t2, t3);
        printf("[CPU-ONLY] Overlaps=%llu and Searching Time=%.2f s \n",
               (unsigned long long)found_count, search_time);
    }
    else
    {
        printf("[CPU-ONLY] Skipped CPU reference search because SKIP_CPU_VERIFY is set.\n");
    }

    HostDpuContext dpu_ctx = setup_dpu_context(root,
                                               query_rects,
                                               numQuery,
                                               dataset_option,
                                               result_return_mode,
                                               replica_query_policy,
                                               replica_query_block_queries,
                                               replica_query_block_auto,
                                               skip_cpu_verify,
                                               found_count,
                                               rtree_construction_time,
                                               search_time,
                                               numRects,
                                               log_file);
    if (dpu_ctx.completed_with_cpu_fallback)
    {
        free(query_rects);
        free(rects);
        fclose(log_file);
        return EXIT_SUCCESS;
    }

    HostQueryStats query_stats = run_dpu_query_batches(&dpu_ctx,
                                                       query_rects,
                                                       numQuery,
                                                       dataset_option,
                                                       result_return_mode,
                                                       log_file);

    double kernel_only_time = (double)query_stats.kernel_cycles_total / DPU_FREQ_HZ;
    uint64_t cpu_overlap_for_check = skip_cpu_verify ? UINT64_MAX : (uint64_t)found_count;
    print_summary_and_check(root, log_file, dpu_ctx.nr_total, numRects, numQuery,
                            rtree_construction_time, search_time, dpu_ctx.tree_transfer_time,
                            kernel_only_time, dpu_ctx.phase_start, query_stats.phase_end,
                            cpu_overlap_for_check, query_stats.overall_dpu_overlap);

    DPU_ASSERT(dpu_free(dpu_ctx.set0));
    DPU_ASSERT(dpu_free(dpu_ctx.set1));

    free(query_rects);
    free(rects);
    fclose(log_file);
    return EXIT_SUCCESS;
}
