#ifndef HOST_RUNTIME_H
#define HOST_RUNTIME_H

#include <dpu.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "rtree.h"

#ifndef DPU_BINARY
#define DPU_BINARY "build/dpu"
#endif

#define LOGF(log_file, fmt, ...)                 \
    do                                           \
    {                                            \
        printf(fmt, ##__VA_ARGS__);              \
        fprintf((log_file), fmt, ##__VA_ARGS__); \
    } while (0)

#define REPLICA_QUERY_BLOCK_SMALL 256u
#define REPLICA_QUERY_BLOCK_LARGE 2048u
#define REPLICA_QUERY_LARGE_THRESHOLD 2000000
#define DPU_FREQ_HZ 400e6
#define RESULT_PAIR_CHUNK 512
#define DENSE_RESULT_CHUNK 4096
#define AGG_THREAD_COUNT 4
#define HOST_MAX_SUBTREE MAX_SUBTREE
#define REPLICA_BASE_NODE_CAP REPLICA_BASE_NODE_CAP_DEFAULT
#define LEGACY_LOGICAL_BATCH_QUERY 50000

typedef struct
{
    struct dpu_set_t set0;
    struct dpu_set_t set1;
    uint32_t n0;
    uint32_t n1;
    uint32_t nr_total;
    double dpu_alloc_time;
    double tree_transfer_time;
    struct timespec phase_start;
    bool completed_with_cpu_fallback;
} HostDpuContext;

typedef struct
{
    uint64_t overall_dpu_overlap;
    uint64_t kernel_cycles_total;
    struct timespec phase_end;
} HostQueryStats;

static inline double sec_since(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static inline bool rect_overlaps_mbr_host(const Rect *r, const MBR *m)
{
    return !(r->xmax < m->xmin || r->xmin > m->xmax ||
             r->ymax < m->ymin || r->ymin > m->ymax);
}

static inline uint32_t choose_replica_query_block_queries(int num_query)
{
    return (num_query >= REPLICA_QUERY_LARGE_THRESHOLD)
               ? REPLICA_QUERY_BLOCK_LARGE
               : REPLICA_QUERY_BLOCK_SMALL;
}

static inline uint32_t choose_launch_query_cap(int dataset_option)
{
    return (dataset_option == 8 || dataset_option == 9)
               ? ROADS_LAUNCH_QUERY_CAP
               : DEFAULT_LAUNCH_QUERY_CAP;
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
                                 FILE *log_file);

HostQueryStats run_dpu_query_batches(const HostDpuContext *ctx,
                                     Rect *query_rects,
                                     int numQuery,
                                     int dataset_option,
                                     uint32_t result_return_mode,
                                     FILE *log_file);

#endif
