#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_runtime.h"

#define HYBRID_TOP_K 4

typedef struct
{
    int l1_id;
    uint64_t hits;
} L1HotRank;

typedef enum
{
    WORKLOAD_SCORE_UNIFORM = 0,
    WORKLOAD_SCORE_QUERY,
    WORKLOAD_SCORE_DATA,
    WORKLOAD_SCORE_COMBINED
} WorkloadScorePolicy;

static const char *workload_score_policy_name(WorkloadScorePolicy policy)
{
    switch (policy)
    {
    case WORKLOAD_SCORE_UNIFORM:
        return "uniform";
    case WORKLOAD_SCORE_QUERY:
        return "query-only";
    case WORKLOAD_SCORE_DATA:
        return "data-only";
    case WORKLOAD_SCORE_COMBINED:
    default:
        return "combined";
    }
}

static bool parse_workload_score_policy(const char *value, WorkloadScorePolicy *out_policy)
{
    if (value == NULL || value[0] == '\0' || out_policy == NULL)
        return false;

    if (strcmp(value, "uniform") == 0 || strcmp(value, "none") == 0)
        *out_policy = WORKLOAD_SCORE_UNIFORM;
    else if (strcmp(value, "query") == 0 || strcmp(value, "query-only") == 0 || strcmp(value, "query_only") == 0)
        *out_policy = WORKLOAD_SCORE_QUERY;
    else if (strcmp(value, "data") == 0 || strcmp(value, "data-only") == 0 || strcmp(value, "data_only") == 0)
        *out_policy = WORKLOAD_SCORE_DATA;
    else if (strcmp(value, "combined") == 0 || strcmp(value, "wab") == 0)
        *out_policy = WORKLOAD_SCORE_COMBINED;
    else
        return false;

    return true;
}

static long double shard_work_score(WorkloadScorePolicy policy, long double query_hits, long double rect_sum)
{
    switch (policy)
    {
    case WORKLOAD_SCORE_UNIFORM:
        return 1.0L;
    case WORKLOAD_SCORE_QUERY:
        return query_hits;
    case WORKLOAD_SCORE_DATA:
        return rect_sum;
    case WORKLOAD_SCORE_COMBINED:
    default:
        return (query_hits + 1.0L) * (rect_sum + 1.0L);
    }
}

static int cmp_l1_hotrank_desc(const void *lhs, const void *rhs)
{
    const L1HotRank *a = (const L1HotRank *)lhs;
    const L1HotRank *b = (const L1HotRank *)rhs;

    if (b->hits > a->hits)
        return 1;
    if (b->hits < a->hits)
        return -1;
    return 0;
}

HostDpuContext setup_dpu_context(Node *root,
                                 Rect *query_rects,
                                 int numQuery,
                                 int dataset_option,
                                 uint32_t result_return_mode,
                                 uint32_t replica_query_policy,
                                 uint32_t replica_query_block_queries,
                                 bool replica_query_block_auto,
                                 bool skip_cpu_verify,
                                 uint64_t found_count,
                                 double rtree_construction_time,
                                 double search_time,
                                 int numRects,
                                 FILE *log_file)
{
    HostDpuContext ctx = {0};
    struct timespec alloc0, alloc1, t4, t5;

    clock_gettime(CLOCK_MONOTONIC, &alloc0);

    struct dpu_set_t set0, set1;
    uint32_t n0 = 0;
    uint32_t n1 = 0;
    uint32_t want0 = NR_DPUS / 2;
    uint32_t want1 = NR_DPUS - want0;

    DPU_ASSERT(dpu_alloc(want0, NULL, &set0));
    DPU_ASSERT(dpu_get_nr_dpus(set0, &n0));
    DPU_ASSERT(dpu_alloc(want1, NULL, &set1));
    DPU_ASSERT(dpu_get_nr_dpus(set1, &n1));

    uint32_t nr_total = n0 + n1;

    DPU_ASSERT(dpu_load(set0, DPU_BINARY, NULL));
    DPU_ASSERT(dpu_load(set1, DPU_BINARY, NULL));

    clock_gettime(CLOCK_MONOTONIC, &alloc1);
    double dpu_alloc_time = sec_since(alloc0, alloc1);

    printf("Allocated set0=%u DPUs, set1=%u DPUs, total=%u\n, and Allocation Time=%.6f\n",
           n0, n1, nr_total, dpu_alloc_time);

    printf("\nPassing Tree to DPUs...\n\t Step 1: Serializing the entire Rtree...");

    SerializedNode *serialized_tree = NULL;
    int total_serialized_nodes = serialize_rtree_bfs_wrapper(root, &serialized_tree);
    if (total_serialized_nodes <= 0 || serialized_tree == NULL)
    {
        fprintf(stderr, "Serialization failed.\n");
        exit(EXIT_FAILURE);
    }

    printf("\n\t Serialized %d nodes in BFS order.", total_serialized_nodes);
    printf("\n\t Step 1: Serialization done.");
    printf("\n\t Printing Serialized Tree:\n");

    clock_gettime(CLOCK_MONOTONIC, &t4);

    char replica_query_policy_log[160];
    if (replica_query_policy == REPLICA_QUERY_POLICY_RANGE)
    {
        snprintf(replica_query_policy_log, sizeof(replica_query_policy_log),
                 "per-replica contiguous query-range split");
    }
    else
    {
        snprintf(replica_query_policy_log, sizeof(replica_query_policy_log),
                 "per-replica block-cyclic query split (block=%u queries%s)",
                 replica_query_block_queries,
                 replica_query_block_auto ? ", auto-selected" : ", env override");
    }

    int top2_node_count = 0;
    int max_node_count = 0;
    {
        printf("\n\t Step 2: Get the top 2 levels and broadcast.");

        bool two_level_tree = (total_serialized_nodes > 1) &&
                              (serialized_tree[0].count > 0) &&
                              serialized_tree[1].isLeaf;
        bool synthetic_grouped_l1 = false;
        int synthetic_group_span = 0;
        int actual_l1_count = serialized_tree[0].count;
        int level2_start = 0;
        int total_level2 = 0;
        int l1_count = 0;

        SerializedNodeHdr *top2_hdrs = NULL;

        if (two_level_tree)
        {
            level2_start = 1;
            total_level2 = actual_l1_count;
            l1_count = (total_level2 + HOST_MAX_SUBTREE - 1) / HOST_MAX_SUBTREE;

            if (l1_count + 1 > MAX_TOP2)
            {
                fprintf(stderr, "Synthetic top-level overflow: need %d headers but MAX_TOP2=%d\n",
                        l1_count + 1, MAX_TOP2);
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            level2_start = 1 + actual_l1_count;

            if (level2_start < total_serialized_nodes && !serialized_tree[level2_start].isLeaf)
            {
                fprintf(stderr, "Unsupported tree depth (>3 levels) for current DPU kernel path.\n");
                exit(EXIT_FAILURE);
            }

            if (actual_l1_count + 1 > MAX_TOP2)
            {
                synthetic_grouped_l1 = true;
                synthetic_group_span = (actual_l1_count + (MAX_TOP2 - 2)) / (MAX_TOP2 - 1);
                if (synthetic_group_span < 1)
                    synthetic_group_span = 1;
                l1_count = (actual_l1_count + synthetic_group_span - 1) / synthetic_group_span;
            }
            else
            {
                int top2_node_count_raw = actual_l1_count + 1;
                if (top2_node_count_raw > total_serialized_nodes)
                {
                    fprintf(stderr, "Top-level serialization mismatch: top2=%d total_nodes=%d\n",
                            top2_node_count_raw, total_serialized_nodes);
                    exit(EXIT_FAILURE);
                }
                l1_count = actual_l1_count;
            }
        }

        top2_node_count = l1_count + 1;
        printf("\n\t   Top2 Node count=%d ", top2_node_count);
        if (two_level_tree)
        {
            printf("\n\t   Two-level tree detected; using %d synthetic L1 groups.", l1_count);
        }
        else if (synthetic_grouped_l1)
        {
            printf("\n\t   Root fanout %d exceeds MAX_TOP2-1=%d; collapsing into %d synthetic L1 groups (span=%d).",
                   actual_l1_count, MAX_TOP2 - 1, l1_count, synthetic_group_span);
        }

        top2_hdrs = (SerializedNodeHdr *)calloc((size_t)top2_node_count, sizeof(*top2_hdrs));
        if (top2_hdrs == NULL)
        {
            perror("alloc top2_hdrs");
            exit(EXIT_FAILURE);
        }

        if (two_level_tree)
        {
            top2_hdrs[0].isLeaf = 0;
            top2_hdrs[0].count = l1_count;
            top2_hdrs[0].mbr = serialized_tree[0].mbr;

            for (int l1 = 1; l1 <= l1_count; l1++)
            {
                int leaf_start = (l1 - 1) * HOST_MAX_SUBTREE;
                int leaf_count = total_level2 - leaf_start;
                if (leaf_count > HOST_MAX_SUBTREE)
                    leaf_count = HOST_MAX_SUBTREE;

                int first_idx = level2_start + leaf_start;
                if (first_idx >= total_serialized_nodes)
                {
                    fprintf(stderr, "Synthetic top-level index overflow: %d >= %d\n",
                            first_idx, total_serialized_nodes);
                    exit(EXIT_FAILURE);
                }

                MBR m = serialized_tree[first_idx].mbr;
                for (int j = 1; j < leaf_count; j++)
                {
                    int idx = first_idx + j;
                    if (idx >= total_serialized_nodes)
                    {
                        fprintf(stderr, "Synthetic top-level index overflow: %d >= %d\n",
                                idx, total_serialized_nodes);
                        exit(EXIT_FAILURE);
                    }

                    const MBR *cm = &serialized_tree[idx].mbr;
                    if (cm->xmin < m.xmin)
                        m.xmin = cm->xmin;
                    if (cm->ymin < m.ymin)
                        m.ymin = cm->ymin;
                    if (cm->xmax > m.xmax)
                        m.xmax = cm->xmax;
                    if (cm->ymax > m.ymax)
                        m.ymax = cm->ymax;
                }

                top2_hdrs[l1].isLeaf = 0;
                top2_hdrs[l1].count = leaf_count;
                top2_hdrs[l1].mbr = m;
            }
        }
        else if (synthetic_grouped_l1)
        {
            top2_hdrs[0].isLeaf = 0;
            top2_hdrs[0].count = l1_count;
            top2_hdrs[0].mbr = serialized_tree[0].mbr;

            total_level2 = 0;
            for (int l1 = 1; l1 <= l1_count; l1++)
            {
                int actual_start = 1 + (l1 - 1) * synthetic_group_span;
                int actual_end = actual_start + synthetic_group_span - 1;
                if (actual_end > actual_l1_count)
                    actual_end = actual_l1_count;

                if (actual_start > actual_l1_count)
                {
                    fprintf(stderr, "Synthetic grouped L1 start overflow: %d > %d\n",
                            actual_start, actual_l1_count);
                    exit(EXIT_FAILURE);
                }

                MBR m = serialized_tree[actual_start].mbr;
                int leaf_count = 0;
                for (int actual_l1 = actual_start; actual_l1 <= actual_end; actual_l1++)
                {
                    const MBR *cm = &serialized_tree[actual_l1].mbr;
                    if (cm->xmin < m.xmin)
                        m.xmin = cm->xmin;
                    if (cm->ymin < m.ymin)
                        m.ymin = cm->ymin;
                    if (cm->xmax > m.xmax)
                        m.xmax = cm->xmax;
                    if (cm->ymax > m.ymax)
                        m.ymax = cm->ymax;
                    leaf_count += serialized_tree[actual_l1].count;
                }

                top2_hdrs[l1].isLeaf = 0;
                top2_hdrs[l1].count = leaf_count;
                top2_hdrs[l1].mbr = m;
                total_level2 += leaf_count;
            }
        }
        else
        {
            int top2_node_count_raw = actual_l1_count + 1;
            if (top2_node_count_raw > MAX_TOP2)
            {
                fprintf(stderr, "Top-level overflow: root children=%d exceeds MAX_TOP2-1=%d\n",
                        actual_l1_count, MAX_TOP2 - 1);
                exit(EXIT_FAILURE);
            }
            if (top2_node_count_raw > total_serialized_nodes)
            {
                fprintf(stderr, "Top-level serialization mismatch: top2=%d total_nodes=%d\n",
                        top2_node_count_raw, total_serialized_nodes);
                exit(EXIT_FAILURE);
            }
            if (level2_start < total_serialized_nodes && !serialized_tree[level2_start].isLeaf)
            {
                fprintf(stderr, "Unsupported tree depth (>3 levels) for current DPU kernel path.\n");
                exit(EXIT_FAILURE);
            }

            for (int i = 0; i < top2_node_count_raw; i++)
            {
                top2_hdrs[i].isLeaf = serialized_tree[i].isLeaf;
                top2_hdrs[i].count = serialized_tree[i].count;
                top2_hdrs[i].mbr = serialized_tree[i].mbr;
            }

            for (int l1 = 1; l1 <= actual_l1_count; l1++)
            {
                total_level2 += serialized_tree[l1].count;
            }
        }

        uint64_t fanout = (uint64_t)nr_total;
        struct timespec up0, up1;
        clock_gettime(CLOCK_MONOTONIC, &up0);

        DPU_ASSERT(dpu_broadcast_to(set0, "DPU_TOP_TREE", 0, top2_hdrs,
                                    top2_node_count * sizeof(SerializedNodeHdr), DPU_XFER_DEFAULT));
        DPU_ASSERT(dpu_broadcast_to(set1, "DPU_TOP_TREE", 0, top2_hdrs,
                                    top2_node_count * sizeof(SerializedNodeHdr), DPU_XFER_DEFAULT));
        DPU_ASSERT(dpu_broadcast_to(set0, "DPU_FANOUT", 0, &fanout, sizeof(fanout), DPU_XFER_DEFAULT));
        DPU_ASSERT(dpu_broadcast_to(set1, "DPU_FANOUT", 0, &fanout, sizeof(fanout), DPU_XFER_DEFAULT));

        printf("\n\t Step 2: Broadcasting the upper level is done.\n");

        clock_gettime(CLOCK_MONOTONIC, &up1);
        printf("\n[Timing] Upper-level broadcast (TOP2+FANOUT to set0+set1): %.6f s\n",
               sec_since(up0, up1));

        printf("\n\t Step 3: Parallel Transfer lower level tree.\n");
        printf("\n\t Level2_Starts at=%d", level2_start);
        printf("\n\t Total Nodes in level 2= %d\n", total_level2);

        uint32_t actual_dpus_needed = (total_level2 < (int)nr_total) ? (uint32_t)total_level2 : nr_total;
        printf("\n\t actual_dpus_needed= %u", actual_dpus_needed);

        int *l1_level2_start = (int *)calloc((size_t)(l1_count + 1), sizeof(*l1_level2_start));
        int *l1_level2_count = (int *)calloc((size_t)(l1_count + 1), sizeof(*l1_level2_count));
        uint64_t *l1_query_hits = (uint64_t *)calloc((size_t)(l1_count + 1), sizeof(*l1_query_hits));
        uint64_t *l1_rect_sum = (uint64_t *)calloc((size_t)(l1_count + 1), sizeof(*l1_rect_sum));
        uint32_t *l1_base_shards = (uint32_t *)calloc((size_t)(l1_count + 1), sizeof(*l1_base_shards));
        if (l1_level2_start == NULL || l1_level2_count == NULL || l1_query_hits == NULL ||
            l1_rect_sum == NULL || l1_base_shards == NULL)
        {
            perror("alloc l1 metadata");
            exit(EXIT_FAILURE);
        }

        int l2_cursor = 0;
        for (int l1 = 1; l1 <= l1_count; l1++)
        {
            int c = 0;
            if (two_level_tree)
            {
                c = total_level2 - l2_cursor;
                if (c > HOST_MAX_SUBTREE)
                    c = HOST_MAX_SUBTREE;
            }
            else if (synthetic_grouped_l1)
            {
                int actual_start = 1 + (l1 - 1) * synthetic_group_span;
                int actual_end = actual_start + synthetic_group_span - 1;
                if (actual_end > actual_l1_count)
                    actual_end = actual_l1_count;

                for (int actual_l1 = actual_start; actual_l1 <= actual_end; actual_l1++)
                {
                    c += serialized_tree[actual_l1].count;
                }
            }
            else
            {
                c = serialized_tree[l1].count;
            }

            l1_level2_start[l1] = l2_cursor;
            l1_level2_count[l1] = c;

            uint64_t rect_sum = 0;
            for (int j = 0; j < c; j++)
            {
                int global_idx = level2_start + l2_cursor + j;
                if (global_idx >= total_serialized_nodes)
                {
                    fprintf(stderr, "L2 index overflow: idx=%d total_nodes=%d\n",
                            global_idx, total_serialized_nodes);
                    exit(EXIT_FAILURE);
                }
                rect_sum += (uint32_t)serialized_tree[global_idx].count;
            }
            l1_rect_sum[l1] = rect_sum;
            l2_cursor += c;
        }

        for (int q = 0; q < numQuery; q++)
        {
            const Rect *qr = &query_rects[q];
            for (int l1 = 1; l1 <= l1_count; l1++)
            {
                if (rect_overlaps_mbr_host(qr, &top2_hdrs[l1].mbr))
                    l1_query_hits[l1]++;
            }
        }

        free(top2_hdrs);

        L1HotRank *l1_ranks = (L1HotRank *)malloc((size_t)l1_count * sizeof(*l1_ranks));
        if (l1_ranks == NULL)
        {
            perror("alloc l1_ranks");
            exit(EXIT_FAILURE);
        }

        for (int l1 = 1; l1 <= l1_count; l1++)
        {
            l1_ranks[l1 - 1].l1_id = l1;
            l1_ranks[l1 - 1].hits = l1_query_hits[l1];
        }
        qsort(l1_ranks, (size_t)l1_count, sizeof(*l1_ranks), cmp_l1_hotrank_desc);

        bool *l1_is_topk = (bool *)calloc((size_t)(l1_count + 1), sizeof(*l1_is_topk));
        if (l1_is_topk == NULL)
        {
            perror("alloc l1_is_topk");
            exit(EXIT_FAILURE);
        }

        uint32_t drill_k = (uint32_t)l1_count < HYBRID_TOP_K ? (uint32_t)l1_count : HYBRID_TOP_K;
        for (uint32_t k = 0; k < drill_k; k++)
        {
            l1_is_topk[l1_ranks[k].l1_id] = true;
        }
        free(l1_ranks);

        int drilling_enabled = 0;
        const char *drilling_env = getenv("HYBRID_L2_DRILLING");
        if (drilling_env != NULL && drilling_env[0] != '\0')
        {
            char *endp = NULL;
            long parsed = strtol(drilling_env, &endp, 10);
            if (endp == drilling_env || *endp != '\0' || (parsed != 0 && parsed != 1))
            {
                fprintf(stderr, "Invalid HYBRID_L2_DRILLING env value '%s' (must be 0 or 1)\n",
                        drilling_env);
                exit(EXIT_FAILURE);
            }
            drilling_enabled = (int)parsed;
        }

        int hotspot_replication_enabled = 1;
        const char *replication_env = getenv("HOTSPOT_REPLICATION");
        if (replication_env != NULL && replication_env[0] != '\0')
        {
            char *endp = NULL;
            long parsed = strtol(replication_env, &endp, 10);
            if (endp == replication_env || *endp != '\0' || (parsed != 0 && parsed != 1))
            {
                fprintf(stderr, "Invalid HOTSPOT_REPLICATION env value '%s' (must be 0 or 1)\n",
                        replication_env);
                exit(EXIT_FAILURE);
            }
            hotspot_replication_enabled = (int)parsed;
        }

        WorkloadScorePolicy workload_score_policy = WORKLOAD_SCORE_QUERY;
        const char *workload_policy_env = getenv("WORKLOAD_SCORE_POLICY");
        if (workload_policy_env != NULL && workload_policy_env[0] != '\0')
        {
            if (!parse_workload_score_policy(workload_policy_env, &workload_score_policy))
            {
                fprintf(stderr,
                        "Invalid WORKLOAD_SCORE_POLICY env value '%s' (expected uniform, query, data, or combined)\n",
                        workload_policy_env);
                exit(EXIT_FAILURE);
            }
        }

        bool ablation_verbose = false;
        const char *ablation_verbose_env = getenv("ABLATION_VERBOSE");
        if (ablation_verbose_env != NULL && ablation_verbose_env[0] != '\0')
        {
            if (strcmp(ablation_verbose_env, "1") == 0 ||
                strcmp(ablation_verbose_env, "true") == 0 ||
                strcmp(ablation_verbose_env, "yes") == 0 ||
                strcmp(ablation_verbose_env, "on") == 0)
            {
                ablation_verbose = true;
            }
            else if (strcmp(ablation_verbose_env, "0") != 0 &&
                     strcmp(ablation_verbose_env, "false") != 0 &&
                     strcmp(ablation_verbose_env, "no") != 0 &&
                     strcmp(ablation_verbose_env, "off") != 0)
            {
                fprintf(stderr,
                        "Invalid ABLATION_VERBOSE env value '%s' (expected 0/1 or true/false)\n",
                        ablation_verbose_env);
                exit(EXIT_FAILURE);
            }
        }

        uint32_t replica_base_node_cap = REPLICA_BASE_NODE_CAP;
        const char *replica_cap_env = getenv("REPLICA_BASE_NODE_CAP");
        if (replica_cap_env != NULL && replica_cap_env[0] != '\0')
        {
            char *endp = NULL;
            long parsed = strtol(replica_cap_env, &endp, 10);
            if (endp == replica_cap_env || *endp != '\0' || parsed <= 0 || parsed > HOST_MAX_SUBTREE)
            {
                fprintf(stderr, "Invalid REPLICA_BASE_NODE_CAP env value '%s' (must be 1..%u)\n",
                        replica_cap_env, (unsigned)HOST_MAX_SUBTREE);
                exit(EXIT_FAILURE);
            }
            replica_base_node_cap = (uint32_t)parsed;
        }

        if (replica_base_node_cap == 0 || replica_base_node_cap > HOST_MAX_SUBTREE)
        {
            fprintf(stderr, "Invalid REPLICA_BASE_NODE_CAP=%u (HOST_MAX_SUBTREE=%u)\n",
                    (unsigned)replica_base_node_cap, (unsigned)HOST_MAX_SUBTREE);
            exit(EXIT_FAILURE);
        }

        uint32_t mandatory_dpus = 0;
        for (int l1 = 1; l1 <= l1_count; l1++)
        {
            uint32_t nodes = (uint32_t)l1_level2_count[l1];
            uint32_t base_shards_l1 = (nodes + replica_base_node_cap - 1u) / replica_base_node_cap;
            l1_base_shards[l1] = base_shards_l1;
            mandatory_dpus += base_shards_l1;
        }

        if (mandatory_dpus > actual_dpus_needed)
        {
            if (skip_cpu_verify)
            {
                fprintf(stderr,
                        "Insufficient DPUs for one-pass subtree placement: need=%u have=%u (HOST_MAX_SUBTREE=%u).\n"
                        "Re-run without SKIP_CPU_VERIFY to use the automatic CPU fallback, or increase subtree capacity / add multi-pass streaming.\n",
                        mandatory_dpus, actual_dpus_needed, (unsigned)HOST_MAX_SUBTREE);
                exit(EXIT_FAILURE);
            }

            LOGF(log_file, "\n[Fallback] One-pass DPU subtree placement would require %u shards, but only %u DPUs are available with HOST_MAX_SUBTREE=%u\n",
                 mandatory_dpus, actual_dpus_needed, (unsigned)HOST_MAX_SUBTREE);
            LOGF(log_file, "[Fallback] Returning the CPU-verified result for this oversized dataset run.\n");

            free(l1_level2_start);
            free(l1_level2_count);
            free(l1_query_hits);
            free(l1_rect_sum);
            free(l1_base_shards);
            free(l1_is_topk);
            free(serialized_tree);

            clock_gettime(CLOCK_MONOTONIC, &t5);
            double fallback_tree_transfer_time = sec_since(t4, t5);

            LOGF(log_file, "\n\n\t\tSUMMARY\n");
            LOGF(log_file, "DPU(s): %u\tBUNDLEFACTOR: %d\tTasklet(s): %d\n",
                 nr_total, BUNDLEFACTOR, NR_TASKLETS);
            LOGF(log_file, "Dataset size: %d\tQuery size: %d\n", numRects, numQuery);
            LOGF(log_file, "CPU R-tree construction time: %.2f s\tSearch Time in HOST: %.2f s\n",
                 rtree_construction_time, search_time);
            LOGF(log_file, "\nTotal Time In CPU (construction+search): %.2f s\n",
                 rtree_construction_time + search_time);
            LOGF(log_file, "\nR-tree distribution/setup time attempted before fallback: %.2f s\n",
                 fallback_tree_transfer_time);
            LOGF(log_file, "DPU execution skipped: oversized dataset requires %u subtree shards, but only %u DPUs fit one pass with HOST_MAX_SUBTREE=%u.\n",
                 mandatory_dpus, actual_dpus_needed, (unsigned)HOST_MAX_SUBTREE);
            LOGF(log_file, "CPU overlap total reused for this run: %" PRIu64 "\n", found_count);
            LOGF(log_file, "\nDataset-specific runtime parameters remain unchanged.\n");

            DPU_ASSERT(dpu_free(set0));
            DPU_ASSERT(dpu_free(set1));

            ctx.completed_with_cpu_fallback = true;
            return ctx;
        }

        uint32_t base_shard_count = mandatory_dpus;
        uint32_t *shard_l1 = (uint32_t *)calloc((size_t)base_shard_count, sizeof(*shard_l1));
        uint32_t *shard_l2_offset = (uint32_t *)calloc((size_t)base_shard_count, sizeof(*shard_l2_offset));
        uint32_t *shard_nodes = (uint32_t *)calloc((size_t)base_shard_count, sizeof(*shard_nodes));
        uint64_t *shard_rect_sum = (uint64_t *)calloc((size_t)base_shard_count, sizeof(*shard_rect_sum));
        uint32_t *shard_replicas = (uint32_t *)calloc((size_t)base_shard_count, sizeof(*shard_replicas));
        long double *shard_work = (long double *)calloc((size_t)base_shard_count, sizeof(*shard_work));
        uint64_t *shard_l2_query_hits = (uint64_t *)calloc((size_t)base_shard_count, sizeof(*shard_l2_query_hits));
        uint32_t *l1_total_replicas = (uint32_t *)calloc((size_t)(l1_count + 1), sizeof(*l1_total_replicas));
        if (shard_l1 == NULL || shard_l2_offset == NULL || shard_nodes == NULL ||
            shard_rect_sum == NULL || shard_replicas == NULL || shard_work == NULL ||
            shard_l2_query_hits == NULL || l1_total_replicas == NULL)
        {
            perror("alloc shard metadata");
            exit(EXIT_FAILURE);
        }

        uint32_t shard_idx = 0;
        for (int l1 = 1; l1 <= l1_count; l1++)
        {
            uint32_t shard_count_l1 = l1_base_shards[l1];
            int node_total = l1_level2_count[l1];
            if (shard_count_l1 == 0 || node_total == 0)
                continue;

            int base_nodes = node_total / (int)shard_count_l1;
            int rem_nodes = node_total % (int)shard_count_l1;
            int local_offset = l1_level2_start[l1];

            for (uint32_t s = 0; s < shard_count_l1; s++)
            {
                int this_node_count = base_nodes + ((int)s < rem_nodes ? 1 : 0);
                if (this_node_count > HOST_MAX_SUBTREE)
                {
                    fprintf(stderr, "this_node_count=%d exceeds HOST_MAX_SUBTREE=%d\n",
                            this_node_count, HOST_MAX_SUBTREE);
                    exit(EXIT_FAILURE);
                }
                if (shard_idx >= base_shard_count)
                {
                    fprintf(stderr, "Shard indexing overflow: shard_idx=%u base=%u\n",
                            shard_idx, base_shard_count);
                    exit(EXIT_FAILURE);
                }

                uint64_t this_rect_sum = 0;
                for (int j = 0; j < this_node_count; j++)
                {
                    int global_idx = level2_start + local_offset + j;
                    if (global_idx >= total_serialized_nodes)
                    {
                        fprintf(stderr, "Shard index overflow: idx=%d total_nodes=%d\n",
                                global_idx, total_serialized_nodes);
                        exit(EXIT_FAILURE);
                    }
                    this_rect_sum += (uint32_t)serialized_tree[global_idx].count;
                }

                shard_l1[shard_idx] = (uint32_t)l1;
                shard_l2_offset[shard_idx] = (uint32_t)local_offset;
                shard_nodes[shard_idx] = (uint32_t)this_node_count;
                shard_rect_sum[shard_idx] = this_rect_sum;
                shard_replicas[shard_idx] = 1u;
                shard_work[shard_idx] = shard_work_score(
                    workload_score_policy,
                    (long double)l1_query_hits[l1],
                    (long double)this_rect_sum);

                local_offset += this_node_count;
                shard_idx++;
            }

            if (local_offset != l1_level2_start[l1] + node_total)
            {
                fprintf(stderr, "L1[%d] shard split mismatch: consumed=%d expected=%d\n",
                        l1, local_offset - l1_level2_start[l1], node_total);
                exit(EXIT_FAILURE);
            }
        }

        if (shard_idx != base_shard_count)
        {
            fprintf(stderr, "Base shard count mismatch: built=%u expected=%u\n",
                    shard_idx, base_shard_count);
            exit(EXIT_FAILURE);
        }

        uint32_t hybrid_sample_rate = 10u;
        const char *sample_rate_env = getenv("HYBRID_SAMPLE_RATE");
        if (sample_rate_env != NULL && sample_rate_env[0] != '\0')
        {
            char *endp = NULL;
            long parsed = strtol(sample_rate_env, &endp, 10);
            if (endp == sample_rate_env || *endp != '\0' || parsed <= 0)
            {
                fprintf(stderr, "Invalid HYBRID_SAMPLE_RATE env value '%s' (must be >= 1)\n",
                        sample_rate_env);
                exit(EXIT_FAILURE);
            }
            hybrid_sample_rate = (uint32_t)parsed;
        }

        double hybrid_refine_time = 0.0;
        uint32_t hybrid_sampled_queries = 0;
        uint32_t hybrid_refined_shards = 0;
        for (uint32_t s = 0; s < base_shard_count; s++)
        {
            if (l1_is_topk[shard_l1[s]])
                hybrid_refined_shards++;
        }

        if (drilling_enabled && base_shard_count > 0 && numQuery > 0 && hybrid_refined_shards > 0)
        {
            struct timespec drill0, drill1;
            clock_gettime(CLOCK_MONOTONIC, &drill0);

            for (int q = 0; q < numQuery; q++)
            {
                if (((uint32_t)q % hybrid_sample_rate) != 0u)
                    continue;

                hybrid_sampled_queries++;
                const Rect *qr = &query_rects[q];

                for (uint32_t s = 0; s < base_shard_count; s++)
                {
                    uint32_t l1 = shard_l1[s];
                    if (!l1_is_topk[l1])
                        continue;

                    uint32_t shard_l2_count = shard_nodes[s];
                    int shard_l2_start = level2_start + (int)shard_l2_offset[s];

                    for (uint32_t j = 0; j < shard_l2_count; j++)
                    {
                        int global_idx = shard_l2_start + (int)j;
                        if (global_idx >= total_serialized_nodes)
                            break;

                        if (rect_overlaps_mbr_host(qr, &serialized_tree[global_idx].mbr))
                        {
                            shard_l2_query_hits[s]++;
                            break;
                        }
                    }
                }
            }

            if (hybrid_sampled_queries > 0)
            {
                for (uint32_t s = 0; s < base_shard_count; s++)
                {
                    if (!l1_is_topk[shard_l1[s]])
                        continue;

                    shard_l2_query_hits[s] =
                        (shard_l2_query_hits[s] * (uint64_t)numQuery + (uint64_t)(hybrid_sampled_queries / 2u)) /
                        (uint64_t)hybrid_sampled_queries;
                }
            }

            clock_gettime(CLOCK_MONOTONIC, &drill1);
            hybrid_refine_time = sec_since(drill0, drill1);
        }

        for (uint32_t s = 0; s < base_shard_count; s++)
        {
            uint32_t l1 = shard_l1[s];
            if (drilling_enabled && l1_is_topk[l1] && shard_l2_query_hits[s] > 0)
            {
                shard_work[s] = shard_work_score(workload_score_policy,
                                                 (long double)shard_l2_query_hits[s],
                                                 (long double)shard_rect_sum[s]);
            }
            else
            {
                shard_work[s] = shard_work_score(workload_score_policy,
                                                 (long double)l1_query_hits[l1],
                                                 (long double)shard_rect_sum[s]);
            }
        }

        uint32_t assigned_dpus = hotspot_replication_enabled ? actual_dpus_needed : base_shard_count;
        uint32_t remaining_dpus = assigned_dpus - base_shard_count;
        if (workload_score_policy == WORKLOAD_SCORE_UNIFORM)
        {
            while (remaining_dpus > 0)
            {
                for (uint32_t s = 0; s < base_shard_count && remaining_dpus > 0; s++)
                {
                    shard_replicas[s]++;
                    remaining_dpus--;
                }
            }
        }
        else
        {
            while (remaining_dpus > 0)
            {
                uint32_t best_shard = 0;
                long double best_score = -1.0L;
                uint32_t best_replicas = UINT32_MAX;

                for (uint32_t s = 0; s < base_shard_count; s++)
                {
                    long double score = shard_work[s] / (long double)shard_replicas[s];
                    uint32_t replicas = shard_replicas[s];
                    if (score > best_score ||
                        (score == best_score && replicas < best_replicas) ||
                        (score == best_score && replicas == best_replicas && s < best_shard))
                    {
                        best_score = score;
                        best_replicas = replicas;
                        best_shard = s;
                    }
                }

                shard_replicas[best_shard]++;
                remaining_dpus--;
            }
        }

        for (uint32_t s = 0; s < base_shard_count; s++)
        {
            if ((int)shard_nodes[s] > max_node_count)
                max_node_count = (int)shard_nodes[s];
        }

        if (max_node_count > HOST_MAX_SUBTREE)
        {
            fprintf(stderr, "max_node_count=%d exceeds HOST_MAX_SUBTREE=%d\n",
                    max_node_count, HOST_MAX_SUBTREE);
            exit(EXIT_FAILURE);
        }

        if (hotspot_replication_enabled)
            printf("\n\t Load-balanced assignment enabled: hotspot shard replication, max_node_count=%d\n", max_node_count);
        else
            printf("\n\t Baseline assignment enabled: base shards only (no hotspot replication), max_node_count=%d\n", max_node_count);
        if (ablation_verbose)
            printf("\n\t [Verbose] Workload score policy: %s\n", workload_score_policy_name(workload_score_policy));

        SerializedLeafNode **dpu_subtrees = (SerializedLeafNode **)calloc((size_t)nr_total, sizeof(*dpu_subtrees));
        uint32_t *dpu_low_nodes_counts = (uint32_t *)calloc((size_t)nr_total, sizeof(*dpu_low_nodes_counts));
        uint32_t *dpu_l1_start = (uint32_t *)calloc((size_t)nr_total, sizeof(*dpu_l1_start));
        uint32_t *dpu_l1_count = (uint32_t *)calloc((size_t)nr_total, sizeof(*dpu_l1_count));
        uint32_t *dpu_replica_rank = (uint32_t *)calloc((size_t)nr_total, sizeof(*dpu_replica_rank));
        uint32_t *dpu_replica_count = (uint32_t *)calloc((size_t)nr_total, sizeof(*dpu_replica_count));
        if (dpu_subtrees == NULL || dpu_low_nodes_counts == NULL || dpu_l1_start == NULL ||
            dpu_l1_count == NULL || dpu_replica_rank == NULL || dpu_replica_count == NULL)
        {
            perror("alloc dpu buffers");
            exit(EXIT_FAILURE);
        }

        uint32_t *gi_order = (uint32_t *)malloc((size_t)assigned_dpus * sizeof(*gi_order));
        if (gi_order == NULL)
        {
            perror("alloc gi_order");
            exit(EXIT_FAILURE);
        }

        uint32_t gi_cursor = 0;
        uint32_t max_set = (n0 > n1) ? n0 : n1;
        for (uint32_t k = 0; k < max_set && gi_cursor < assigned_dpus; k++)
        {
            if (k < n0 && gi_cursor < assigned_dpus)
                gi_order[gi_cursor++] = k;
            if (k < n1 && gi_cursor < assigned_dpus)
                gi_order[gi_cursor++] = n0 + k;
        }
        if (gi_cursor != assigned_dpus)
        {
            fprintf(stderr, "gi_order fill mismatch: got=%u expected=%u\n",
                    gi_cursor, assigned_dpus);
            exit(EXIT_FAILURE);
        }

        uint32_t dpu_slot = 0;
        long double est_set0 = 0.0L;
        long double est_set1 = 0.0L;
        for (uint32_t s = 0; s < base_shard_count; s++)
        {
            uint32_t l1 = shard_l1[s];
            uint32_t this_node_count = shard_nodes[s];
            uint32_t replica_count = shard_replicas[s];
            l1_total_replicas[l1] += replica_count;

            for (uint32_t r = 0; r < replica_count; r++)
            {
                if (dpu_slot >= assigned_dpus)
                {
                    fprintf(stderr, "DPU slot overflow during replica assignment\n");
                    exit(EXIT_FAILURE);
                }

                uint32_t gi = gi_order[dpu_slot++];
                dpu_low_nodes_counts[gi] = this_node_count;
                dpu_l1_start[gi] = (this_node_count > 0) ? l1 : 0u;
                dpu_l1_count[gi] = (this_node_count > 0) ? 1u : 0u;
                dpu_replica_rank[gi] = r;
                dpu_replica_count[gi] = replica_count;

                dpu_subtrees[gi] = (SerializedLeafNode *)calloc((size_t)max_node_count, sizeof(SerializedLeafNode));
                if (dpu_subtrees[gi] == NULL)
                {
                    perror("calloc subtree");
                    exit(EXIT_FAILURE);
                }

                for (uint32_t j = 0; j < this_node_count; j++)
                {
                    int global_idx = level2_start + (int)shard_l2_offset[s] + (int)j;
                    const SerializedNode *src = &serialized_tree[global_idx];
                    SerializedLeafNode *dst = &dpu_subtrees[gi][j];

                    if (src->count > MAX_RECTS)
                    {
                        fprintf(stderr, "Leaf rectangle overflow: count=%d MAX_RECTS=%d\n",
                                src->count, MAX_RECTS);
                        exit(EXIT_FAILURE);
                    }

                    dst->isLeaf = src->isLeaf;
                    dst->count = src->count;
                    dst->mbr = src->mbr;
                    for (int rect_idx = 0; rect_idx < src->count; rect_idx++)
                    {
                        dst->rects[rect_idx] = src->rects[rect_idx];
                    }
                }

                long double work_share = shard_work[s] / (long double)replica_count;
                if (gi < n0)
                    est_set0 += work_share;
                else
                    est_set1 += work_share;
            }
        }

        if (dpu_slot != assigned_dpus)
        {
            fprintf(stderr, "DPU assignment mismatch: assigned=%u expected=%u\n",
                    dpu_slot, assigned_dpus);
            exit(EXIT_FAILURE);
        }

        LOGF(log_file, "\n[Optimization] Hotspot shard replication %s.\n",
             hotspot_replication_enabled ? "enabled" : "disabled");
        if (ablation_verbose)
        {
            LOGF(log_file, "[Verbose] Workload estimator ablation:\n");
            LOGF(log_file, "[Verbose]   policy=%s (uniform/query/data/combined)\n",
                 workload_score_policy_name(workload_score_policy));
        }
        if (hotspot_replication_enabled)
        {
            LOGF(log_file, "[Optimization] Method: base shards target cap=%d (HOST_MAX_SUBTREE=%d); remaining DPUs replicate hottest shards with %s.\n",
                 (int)replica_base_node_cap, HOST_MAX_SUBTREE, replica_query_policy_log);
        }
        else
        {
            LOGF(log_file, "[Optimization] Method: base shards target cap=%d (HOST_MAX_SUBTREE=%d); no extra replicas beyond the mandatory subtree partitioning.\n",
                 (int)replica_base_node_cap, HOST_MAX_SUBTREE);
        }
        LOGF(log_file, "[Optimization] Hybrid approach: selective leaf-level refinement for top-%u hottest L1 regions (balance accuracy vs. performance).\n",
             drill_k);
        LOGF(log_file, "[Optimization] Hybrid leaf-level refinement: %s | sample_rate=%u | sampled_queries=%u/%d | refined_shards=%u | host_time=%.6f s\n",
             drilling_enabled ? "enabled" : "disabled",
             hybrid_sample_rate,
             hybrid_sampled_queries,
             numQuery,
             hybrid_refined_shards,
             hybrid_refine_time);
        LOGF(log_file, "[Optimization] base_shards=%u replicas_added=%u total_active_dpus=%u\n",
             base_shard_count, assigned_dpus - base_shard_count, assigned_dpus);
        LOGF(log_file, "[Optimization] Hybrid leaf-level refinement details:\n");
        for (int l1 = 1; l1 <= l1_count; l1++)
        {
            if (l1_base_shards[l1] == 0)
                continue;

            long double est_per_replica = (l1_total_replicas[l1] > 0)
                                              ? ((((long double)l1_query_hits[l1] + 1.0L) *
                                                  ((long double)l1_rect_sum[l1] + 1.0L)) /
                                                 (long double)l1_total_replicas[l1])
                                              : 0.0L;
            const char *topk_marker = drilling_enabled
                                          ? (l1_is_topk[l1] ? " [TOP-K REFINED]" : "")
                                          : (l1_is_topk[l1] ? " [TOP-K]" : "");
            LOGF(log_file, "  L1[%02d]: nodes=%d hits=%" PRIu64 " rect_sum=%" PRIu64 " base_shards=%u total_replicas=%u est_per_replica=%.2Lf%s\n",
                 l1, l1_level2_count[l1], l1_query_hits[l1], l1_rect_sum[l1],
                 l1_base_shards[l1], l1_total_replicas[l1], est_per_replica, topk_marker);
        }
        LOGF(log_file, "[Optimization] Estimated set work split: set0=%.2Lf | set1=%.2Lf | ratio(set0/set1)=%.3Lf\n",
             est_set0, est_set1, (est_set1 > 0.0L) ? (est_set0 / est_set1) : 0.0L);

        free(l1_level2_start);
        free(l1_level2_count);
        free(l1_query_hits);
        free(l1_rect_sum);
        free(l1_base_shards);
        free(l1_is_topk);
        free(shard_l1);
        free(shard_l2_offset);
        free(shard_nodes);
        free(shard_rect_sum);
        free(shard_replicas);
        free(shard_l2_query_hits);
        free(shard_work);
        free(l1_total_replicas);
        free(gi_order);

        size_t max_transfer_size = (size_t)max_node_count * sizeof(SerializedLeafNode);
        printf("\n\t Max_transfer_size=%zu bytes\n", max_transfer_size);

        SerializedLeafNode *zero_subtree = (SerializedLeafNode *)calloc((size_t)max_node_count, sizeof(*zero_subtree));
        if (zero_subtree == NULL)
        {
            perror("calloc zero_subtree");
            exit(EXIT_FAILURE);
        }

        struct dpu_set_t dpu;
        int i = 0;
        DPU_FOREACH(set0, dpu, i)
        {
            uint32_t gi = (uint32_t)i;
            SerializedLeafNode *payload = (gi < nr_total && dpu_subtrees[gi] != NULL) ? dpu_subtrees[gi] : zero_subtree;
            DPU_ASSERT(dpu_prepare_xfer(dpu, payload));
        }
        DPU_ASSERT(dpu_push_xfer(set0, DPU_XFER_TO_DPU, "DPU_LOW_TREE", 0,
                                 max_transfer_size, DPU_XFER_DEFAULT));

        i = 0;
        DPU_FOREACH(set1, dpu, i)
        {
            uint32_t gi = n0 + (uint32_t)i;
            SerializedLeafNode *payload = (gi < nr_total && dpu_subtrees[gi] != NULL) ? dpu_subtrees[gi] : zero_subtree;
            DPU_ASSERT(dpu_prepare_xfer(dpu, payload));
        }
        DPU_ASSERT(dpu_push_xfer(set1, DPU_XFER_TO_DPU, "DPU_LOW_TREE", 0,
                                 max_transfer_size, DPU_XFER_DEFAULT));

        struct dpu_low_with_index *ctrl0 = (struct dpu_low_with_index *)calloc(n0, sizeof(*ctrl0));
        struct dpu_low_with_index *ctrl1 = (struct dpu_low_with_index *)calloc(n1, sizeof(*ctrl1));
        if (ctrl0 == NULL || ctrl1 == NULL)
        {
            perror("calloc ctrl");
            exit(EXIT_FAILURE);
        }

        for (uint32_t li = 0; li < n0; li++)
        {
            uint32_t gi = li;
            ctrl0[li].dpu_index = gi;
            ctrl0[li].low_tree_count = dpu_low_nodes_counts[gi];
            ctrl0[li].l1_index = dpu_l1_start[gi];
            ctrl0[li].l1_count = dpu_l1_count[gi];
            ctrl0[li].replica_rank = dpu_replica_rank[gi];
            ctrl0[li].replica_count = dpu_replica_count[gi];
        }

        for (uint32_t li = 0; li < n1; li++)
        {
            uint32_t gi = n0 + li;
            ctrl1[li].dpu_index = gi;
            ctrl1[li].low_tree_count = dpu_low_nodes_counts[gi];
            ctrl1[li].l1_index = dpu_l1_start[gi];
            ctrl1[li].l1_count = dpu_l1_count[gi];
            ctrl1[li].replica_rank = dpu_replica_rank[gi];
            ctrl1[li].replica_count = dpu_replica_count[gi];
        }

        i = 0;
        DPU_FOREACH(set0, dpu, i)
        {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &ctrl0[i]));
        }
        DPU_ASSERT(dpu_push_xfer(set0, DPU_XFER_TO_DPU, "DPU_LOW_WITH_INDEX", 0,
                                 sizeof(struct dpu_low_with_index), DPU_XFER_DEFAULT));

        i = 0;
        DPU_FOREACH(set1, dpu, i)
        {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &ctrl1[i]));
        }
        DPU_ASSERT(dpu_push_xfer(set1, DPU_XFER_TO_DPU, "DPU_LOW_WITH_INDEX", 0,
                                 sizeof(struct dpu_low_with_index), DPU_XFER_DEFAULT));

        const uint32_t launch_query_cap = choose_launch_query_cap(dataset_option);
        MramConfig mram_cfg = {
            .actual_query_count = launch_query_cap,
            .actual_top_tree_count = (uint32_t)top2_node_count,
            .actual_low_tree_count = (uint32_t)max_node_count,
            .actual_result_capacity = RESULT_PAIR_CAPACITY,
            .result_return_mode = result_return_mode,
            .replica_query_policy = replica_query_policy,
            .replica_query_block_queries = replica_query_block_queries,
            .reserved0 = 0u};

        i = 0;
        DPU_FOREACH(set0, dpu, i)
        {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &mram_cfg));
        }
        DPU_ASSERT(dpu_push_xfer(set0, DPU_XFER_TO_DPU, "MRAM_RUNTIME_CONFIG", 0,
                                 sizeof(MramConfig), DPU_XFER_DEFAULT));

        i = 0;
        DPU_FOREACH(set1, dpu, i)
        {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &mram_cfg));
        }
        DPU_ASSERT(dpu_push_xfer(set1, DPU_XFER_TO_DPU, "MRAM_RUNTIME_CONFIG", 0,
                                 sizeof(MramConfig), DPU_XFER_DEFAULT));

        printf("\nRuntime MRAM config broadcast complete.\n");
        printf("\nTree and DPU Index transfer complete. Now Transferring the Query---\n");

        for (uint32_t gi = 0; gi < nr_total; gi++)
        {
            free(dpu_subtrees[gi]);
        }
        free(dpu_subtrees);
        free(dpu_low_nodes_counts);
        free(dpu_l1_start);
        free(dpu_l1_count);
        free(dpu_replica_rank);
        free(dpu_replica_count);
        free(zero_subtree);
        free(ctrl0);
        free(ctrl1);
    }

    clock_gettime(CLOCK_MONOTONIC, &t5);
    free(serialized_tree);

    ctx.set0 = set0;
    ctx.set1 = set1;
    ctx.n0 = n0;
    ctx.n1 = n1;
    ctx.nr_total = nr_total;
    ctx.dpu_alloc_time = dpu_alloc_time;
    ctx.tree_transfer_time = sec_since(t4, t5);
    ctx.phase_start = t4;
    ctx.completed_with_cpu_fallback = false;
    return ctx;
}
