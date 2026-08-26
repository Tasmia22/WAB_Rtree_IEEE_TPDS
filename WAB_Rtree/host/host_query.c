#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_runtime.h"

typedef struct
{
    uint32_t thread_id;
    uint32_t start_dpu;
    uint32_t end_dpu;
    uint32_t tasklet_index;
    uint32_t base;
    uint32_t chunk_pairs;
    uint32_t bs;
    int offset;
    uint32_t nr_dpus;
    uint64_t *tasklet_pair_counts;
    ResultPair *pair_chunk_buf;
    uint64_t *aggregated_dpu_overlap;
    uint64_t *per_dpu_overlap;
    uint64_t local_sum;
} AggregationWorkerArgs;

static void *aggregate_results_thread(void *arg)
{
    AggregationWorkerArgs *a = (AggregationWorkerArgs *)arg;
    uint64_t local_sum = 0;

    for (uint32_t d = a->start_dpu; d < a->end_dpu; d++)
    {
        uint32_t tpairs = (uint32_t)a->tasklet_pair_counts[(size_t)d * NR_TASKLETS + a->tasklet_index];
        if (tpairs <= a->base)
            continue;

        uint32_t available = tpairs - a->base;
        if (available > a->chunk_pairs)
            available = a->chunk_pairs;

        ResultPair *pairs = a->pair_chunk_buf + (size_t)d * a->chunk_pairs;
        for (uint32_t j = 0; j < available; j++)
        {
            uint32_t qid = pairs[j].qid;
            uint32_t count = pairs[j].count;
            if (qid >= (uint32_t)a->bs)
                continue;

            __sync_fetch_and_add(&a->aggregated_dpu_overlap[a->offset + qid], count);
            if (a->per_dpu_overlap != NULL)
                a->per_dpu_overlap[d] += count;
            local_sum += count;
        }
    }

    a->local_sum = local_sum;
    return NULL;
}

static uint64_t pull_sparse_results(struct dpu_set_t set, uint32_t nr_dpus, uint64_t *pair_counts,
                                    uint64_t *tasklet_pair_counts, ResultPair *pair_chunk_buf, PerfStats *perf,
                                    uint64_t *aggregated_dpu_overlap, uint64_t *per_dpu_overlap,
                                    int offset, int bs, uint64_t *batch_wall_cycles,
                                    double *d2h_time, double *agg_time)
{
    struct dpu_set_t each_dpu;
    uint64_t overlap_sum = 0;
    uint64_t local_batch_wall = 0;
    struct timespec d2h_start, d2h_end, agg_start, agg_end;
    double d2h_seconds = 0.0;
    double agg_seconds = 0.0;

    int i = 0;
    clock_gettime(CLOCK_MONOTONIC, &d2h_start);
    DPU_FOREACH(set, each_dpu, i)
    {
        DPU_ASSERT(dpu_prepare_xfer(each_dpu, &pair_counts[i]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "DPU_RESULT_PAIR_COUNT",
                             0, sizeof(uint64_t), DPU_XFER_DEFAULT));

    i = 0;
    DPU_FOREACH(set, each_dpu, i)
    {
        DPU_ASSERT(dpu_prepare_xfer(each_dpu, &perf[i]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "DPU_PERF",
                             0, sizeof(PerfStats), DPU_XFER_DEFAULT));

    i = 0;
    DPU_FOREACH(set, each_dpu, i)
    {
        DPU_ASSERT(dpu_prepare_xfer(each_dpu, tasklet_pair_counts + (size_t)i * NR_TASKLETS));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "DPU_TASKLET_PAIR_COUNTS",
                             0, (uint32_t)(NR_TASKLETS * sizeof(uint64_t)), DPU_XFER_DEFAULT));
    clock_gettime(CLOCK_MONOTONIC, &d2h_end);
    d2h_seconds += sec_since(d2h_start, d2h_end);

    for (uint32_t d = 0; d < nr_dpus; d++)
    {
        if (perf[d].total_cycles > local_batch_wall)
            local_batch_wall = perf[d].total_cycles;
    }

    for (uint32_t t = 0; t < NR_TASKLETS; t++)
    {
        uint32_t max_pairs_t = 0;
        for (uint32_t d = 0; d < nr_dpus; d++)
        {
            uint32_t c = (uint32_t)tasklet_pair_counts[(size_t)d * NR_TASKLETS + t];
            if (c > max_pairs_t)
                max_pairs_t = c;
        }

        if (max_pairs_t == 0)
            continue;

        uint32_t segment_base = t * TASKLET_QUERY_CAP;
        for (uint32_t base = 0; base < max_pairs_t; base += RESULT_PAIR_CHUNK)
        {
            uint32_t chunk_pairs = max_pairs_t - base;
            if (chunk_pairs > RESULT_PAIR_CHUNK)
                chunk_pairs = RESULT_PAIR_CHUNK;

            i = 0;
            clock_gettime(CLOCK_MONOTONIC, &d2h_start);
            DPU_FOREACH(set, each_dpu, i)
            {
                DPU_ASSERT(dpu_prepare_xfer(each_dpu, pair_chunk_buf + (size_t)i * chunk_pairs));
            }
            DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "DPU_RESULT_PAIRS",
                                     (uint32_t)((segment_base + base) * sizeof(ResultPair)),
                                     chunk_pairs * sizeof(ResultPair), DPU_XFER_DEFAULT));
            clock_gettime(CLOCK_MONOTONIC, &d2h_end);
            d2h_seconds += sec_since(d2h_start, d2h_end);

            int nthreads = (int)nr_dpus;
            if (nthreads > AGG_THREAD_COUNT)
                nthreads = AGG_THREAD_COUNT;
            if (nthreads < 1)
                nthreads = 1;

            uint32_t dpu_block = (nr_dpus + (uint32_t)nthreads - 1) / (uint32_t)nthreads;
            pthread_t *threads = (pthread_t *)malloc((size_t)nthreads * sizeof(*threads));
            AggregationWorkerArgs *args = (AggregationWorkerArgs *)malloc((size_t)nthreads * sizeof(*args));
            if (threads == NULL || args == NULL)
            {
                perror("alloc aggregation threads");
                exit(EXIT_FAILURE);
            }

            clock_gettime(CLOCK_MONOTONIC, &agg_start);
            for (int tid = 0; tid < nthreads; tid++)
            {
                uint32_t start_dpu = (uint32_t)tid * dpu_block;
                uint32_t end_dpu = start_dpu + dpu_block;
                if (end_dpu > nr_dpus)
                    end_dpu = nr_dpus;

                args[tid].thread_id = (uint32_t)tid;
                args[tid].start_dpu = start_dpu;
                args[tid].end_dpu = end_dpu;
                args[tid].tasklet_index = t;
                args[tid].base = base;
                args[tid].chunk_pairs = chunk_pairs;
                args[tid].bs = (uint32_t)bs;
                args[tid].offset = offset;
                args[tid].nr_dpus = nr_dpus;
                args[tid].tasklet_pair_counts = tasklet_pair_counts;
                args[tid].pair_chunk_buf = pair_chunk_buf;
                args[tid].aggregated_dpu_overlap = aggregated_dpu_overlap;
                args[tid].per_dpu_overlap = per_dpu_overlap;
                args[tid].local_sum = 0;

                if (pthread_create(&threads[tid], NULL, aggregate_results_thread, &args[tid]) != 0)
                {
                    perror("pthread_create");
                    exit(EXIT_FAILURE);
                }
            }

            for (int tid = 0; tid < nthreads; tid++)
            {
                pthread_join(threads[tid], NULL);
                overlap_sum += args[tid].local_sum;
            }
            clock_gettime(CLOCK_MONOTONIC, &agg_end);
            agg_seconds += sec_since(agg_start, agg_end);

            free(threads);
            free(args);
        }
    }

    *batch_wall_cycles = local_batch_wall;
    if (d2h_time != NULL)
        *d2h_time = d2h_seconds;
    if (agg_time != NULL)
        *agg_time = agg_seconds;
    return overlap_sum;
}

static uint64_t pull_dense_results(struct dpu_set_t set, uint32_t nr_dpus, DenseResultSlot *dense_chunk_buf, PerfStats *perf,
                                   uint64_t *aggregated_dpu_overlap, uint64_t *per_dpu_overlap,
                                   int offset, int bs, uint64_t *batch_wall_cycles,
                                   double *d2h_time, double *agg_time)
{
    struct dpu_set_t each_dpu;
    uint64_t overlap_sum = 0;
    uint64_t local_batch_wall = 0;
    struct timespec d2h_start, d2h_end, agg_start, agg_end;
    double d2h_seconds = 0.0;
    double agg_seconds = 0.0;

    int i = 0;
    clock_gettime(CLOCK_MONOTONIC, &d2h_start);
    DPU_FOREACH(set, each_dpu, i)
    {
        DPU_ASSERT(dpu_prepare_xfer(each_dpu, &perf[i]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "DPU_PERF",
                             0, sizeof(PerfStats), DPU_XFER_DEFAULT));
    clock_gettime(CLOCK_MONOTONIC, &d2h_end);
    d2h_seconds += sec_since(d2h_start, d2h_end);

    for (uint32_t d = 0; d < nr_dpus; d++)
    {
        if (perf[d].total_cycles > local_batch_wall)
            local_batch_wall = perf[d].total_cycles;
    }

    for (uint32_t base = 0; base < (uint32_t)bs; base += DENSE_RESULT_CHUNK)
    {
        uint32_t chunk_slots = (uint32_t)bs - base;
        if (chunk_slots > DENSE_RESULT_CHUNK)
            chunk_slots = DENSE_RESULT_CHUNK;

        i = 0;
        clock_gettime(CLOCK_MONOTONIC, &d2h_start);
        DPU_FOREACH(set, each_dpu, i)
        {
            DPU_ASSERT(dpu_prepare_xfer(each_dpu, dense_chunk_buf + (size_t)i * chunk_slots));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "DPU_RESULT_DENSE",
                                 (uint32_t)(base * sizeof(DenseResultSlot)),
                                 chunk_slots * sizeof(DenseResultSlot), DPU_XFER_DEFAULT));
        clock_gettime(CLOCK_MONOTONIC, &d2h_end);
        d2h_seconds += sec_since(d2h_start, d2h_end);

        clock_gettime(CLOCK_MONOTONIC, &agg_start);
        for (uint32_t d = 0; d < nr_dpus; d++)
        {
            DenseResultSlot *slots = dense_chunk_buf + (size_t)d * chunk_slots;
            uint64_t dpu_sum = 0;
            for (uint32_t j = 0; j < chunk_slots; j++)
            {
                uint32_t count = slots[j].count;
                __sync_fetch_and_add(&aggregated_dpu_overlap[offset + (int)base + (int)j], count);
                dpu_sum += count;
            }
            if (per_dpu_overlap != NULL)
                per_dpu_overlap[d] += dpu_sum;
            overlap_sum += dpu_sum;
        }
        clock_gettime(CLOCK_MONOTONIC, &agg_end);
        agg_seconds += sec_since(agg_start, agg_end);
    }

    *batch_wall_cycles = local_batch_wall;
    if (d2h_time != NULL)
        *d2h_time = d2h_seconds;
    if (agg_time != NULL)
        *agg_time = agg_seconds;
    return overlap_sum;
}

HostQueryStats run_dpu_query_batches(const HostDpuContext *ctx,
                                     Rect *query_rects,
                                     int numQuery,
                                     int dataset_option,
                                     uint32_t result_return_mode,
                                     FILE *log_file)
{
    HostQueryStats stats = {0};
    struct dpu_set_t set0 = ctx->set0;
    struct dpu_set_t set1 = ctx->set1;
    uint32_t n0 = ctx->n0;
    uint32_t n1 = ctx->n1;

    uint64_t *aggregated_dpu_overlap = (uint64_t *)calloc((size_t)numQuery, sizeof(*aggregated_dpu_overlap));
    if (numQuery > 0 && aggregated_dpu_overlap == NULL)
    {
        perror("aggregated_dpu_overlap");
        exit(EXIT_FAILURE);
    }

    uint64_t overall_dpu_overlap = 0;
    uint64_t kernel_cycles_total = 0;
    uint64_t kernel_cycles_peak = 0;

    uint64_t *pair_counts0 = NULL;
    uint64_t *pair_counts1 = NULL;
    uint64_t *tasklet_counts0 = NULL;
    uint64_t *tasklet_counts1 = NULL;
    ResultPair *pair_chunk0 = NULL;
    ResultPair *pair_chunk1 = NULL;
    DenseResultSlot *dense_chunk0 = NULL;
    DenseResultSlot *dense_chunk1 = NULL;
    PerfStats *perf0 = NULL;
    PerfStats *perf1 = NULL;
    uint64_t *per_dpu_overlap0 = NULL;
    uint64_t *per_dpu_overlap1 = NULL;

    if (posix_memalign((void **)&perf0, 64, sizeof(*perf0) * (size_t)n0) != 0)
    {
        perror("perf0");
        exit(EXIT_FAILURE);
    }
    if (posix_memalign((void **)&perf1, 64, sizeof(*perf1) * (size_t)n1) != 0)
    {
        perror("perf1");
        exit(EXIT_FAILURE);
    }

    per_dpu_overlap0 = (uint64_t *)calloc((size_t)n0, sizeof(*per_dpu_overlap0));
    per_dpu_overlap1 = (uint64_t *)calloc((size_t)n1, sizeof(*per_dpu_overlap1));
    if ((n0 > 0 && per_dpu_overlap0 == NULL) || (n1 > 0 && per_dpu_overlap1 == NULL))
    {
        perror("per_dpu_overlap");
        exit(EXIT_FAILURE);
    }

    if (result_return_mode == RESULT_RETURN_MODE_DENSE)
    {
        if (posix_memalign((void **)&dense_chunk0, 64, sizeof(*dense_chunk0) * (size_t)n0 * DENSE_RESULT_CHUNK) != 0)
        {
            perror("dense_chunk0");
            exit(EXIT_FAILURE);
        }
        if (posix_memalign((void **)&dense_chunk1, 64, sizeof(*dense_chunk1) * (size_t)n1 * DENSE_RESULT_CHUNK) != 0)
        {
            perror("dense_chunk1");
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        if (posix_memalign((void **)&pair_counts0, 64, sizeof(*pair_counts0) * (size_t)n0) != 0)
        {
            perror("pair_counts0");
            exit(EXIT_FAILURE);
        }
        if (posix_memalign((void **)&pair_counts1, 64, sizeof(*pair_counts1) * (size_t)n1) != 0)
        {
            perror("pair_counts1");
            exit(EXIT_FAILURE);
        }
        if (posix_memalign((void **)&tasklet_counts0, 64, sizeof(*tasklet_counts0) * (size_t)n0 * NR_TASKLETS) != 0)
        {
            perror("tasklet_counts0");
            exit(EXIT_FAILURE);
        }
        if (posix_memalign((void **)&tasklet_counts1, 64, sizeof(*tasklet_counts1) * (size_t)n1 * NR_TASKLETS) != 0)
        {
            perror("tasklet_counts1");
            exit(EXIT_FAILURE);
        }
        if (posix_memalign((void **)&pair_chunk0, 64, sizeof(*pair_chunk0) * (size_t)n0 * RESULT_PAIR_CHUNK) != 0)
        {
            perror("pair_chunk0");
            exit(EXIT_FAILURE);
        }
        if (posix_memalign((void **)&pair_chunk1, 64, sizeof(*pair_chunk1) * (size_t)n1 * RESULT_PAIR_CHUNK) != 0)
        {
            perror("pair_chunk1");
            exit(EXIT_FAILURE);
        }
    }

    const int launch_query_cap = (int)choose_launch_query_cap(dataset_option);
    int num_batches = (numQuery + launch_query_cap - 1) / launch_query_cap;
    const int logical_batch_query = LEGACY_LOGICAL_BATCH_QUERY;
    int logical_total_batches = (numQuery + logical_batch_query - 1) / logical_batch_query;

    double *h2d0_batch = (double *)calloc((size_t)num_batches, sizeof(*h2d0_batch));
    double *h2d1_batch = (double *)calloc((size_t)num_batches, sizeof(*h2d1_batch));
    if (num_batches > 0 && (h2d0_batch == NULL || h2d1_batch == NULL))
    {
        perror("h2d batch timing buffers");
        exit(EXIT_FAILURE);
    }

    if (num_batches > 0)
    {
        double launch_reduction = (double)logical_total_batches / (double)num_batches;
        LOGF(log_file, "\n[Persistent-SuperBatch] launch_query_cap=%d | logical_batch_query=%d | launches=%d (logical=%d, reduction=%.2fx)\n",
             launch_query_cap, logical_batch_query, num_batches, logical_total_batches, launch_reduction);
    }

    double host_kernel_wall_total = 0.0;
    double h2d0_total = 0.0;
    double h2d1_total = 0.0;
    double d2h0_total = 0.0;
    double d2h1_total = 0.0;
    double agg0_total = 0.0;
    double agg1_total = 0.0;
    double host_kernel0_total = 0.0;
    double host_kernel1_total = 0.0;

    int running_batch0 = -1;
    int running_batch1 = -1;
    int next_launch0 = 0;
    int next_launch1 = 0;
    struct timespec launch_start0 = {0};
    struct timespec launch_start1 = {0};

    if (num_batches > 0)
    {
        int offset0 = 0;
        int bs0 = (numQuery < launch_query_cap) ? numQuery : launch_query_cap;
        uint64_t qnum0 = (uint64_t)bs0;
        size_t qbytes0 = (size_t)bs0 * sizeof(Rect);
        struct timespec io0, io1;

        clock_gettime(CLOCK_MONOTONIC, &io0);
        DPU_ASSERT(dpu_broadcast_to(set0, "QUERY_NUM", 0, &qnum0, sizeof(qnum0), DPU_XFER_DEFAULT));
        DPU_ASSERT(dpu_broadcast_to(set0, "DPU_QUERY_RECT", 0, &query_rects[offset0], qbytes0, DPU_XFER_DEFAULT));
        clock_gettime(CLOCK_MONOTONIC, &io1);
        h2d0_batch[0] = sec_since(io0, io1);
        h2d0_total += h2d0_batch[0];

        clock_gettime(CLOCK_MONOTONIC, &launch_start0);
        DPU_ASSERT(dpu_launch(set0, DPU_ASYNCHRONOUS));
        running_batch0 = 0;
        next_launch0 = 1;

        clock_gettime(CLOCK_MONOTONIC, &io0);
        DPU_ASSERT(dpu_broadcast_to(set1, "QUERY_NUM", 0, &qnum0, sizeof(qnum0), DPU_XFER_DEFAULT));
        DPU_ASSERT(dpu_broadcast_to(set1, "DPU_QUERY_RECT", 0, &query_rects[offset0], qbytes0, DPU_XFER_DEFAULT));
        clock_gettime(CLOCK_MONOTONIC, &io1);
        h2d1_batch[0] = sec_since(io0, io1);
        h2d1_total += h2d1_batch[0];

        clock_gettime(CLOCK_MONOTONIC, &launch_start1);
        DPU_ASSERT(dpu_launch(set1, DPU_ASYNCHRONOUS));
        running_batch1 = 0;
        next_launch1 = 1;
    }

    for (int batch_no = 0; batch_no < num_batches; batch_no++)
    {
        int offset = batch_no * launch_query_cap;
        int bs = numQuery - offset;
        if (bs > launch_query_cap)
            bs = launch_query_cap;

        int logical_start = offset / logical_batch_query;
        int logical_end = (offset + bs - 1) / logical_batch_query;

        double host_kernel0_this = 0.0;
        double host_kernel1_this = 0.0;
        double d2h0_this = 0.0;
        double d2h1_this = 0.0;
        double agg0_this = 0.0;
        double agg1_this = 0.0;
        uint64_t batch_wall0 = 0;
        uint64_t batch_wall1 = 0;
        struct timespec t0, t1;

        if (running_batch0 != batch_no)
        {
            fprintf(stderr, "Unexpected set0 pipeline state: running=%d expected=%d\n",
                    running_batch0, batch_no);
            exit(EXIT_FAILURE);
        }

        DPU_ASSERT(dpu_sync(set0));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        host_kernel0_this = sec_since(launch_start0, t1);
        host_kernel0_total += host_kernel0_this;
        host_kernel_wall_total += host_kernel0_this;

        if (result_return_mode == RESULT_RETURN_MODE_DENSE)
        {
            overall_dpu_overlap += pull_dense_results(set0, n0, dense_chunk0, perf0,
                                                      aggregated_dpu_overlap, per_dpu_overlap0,
                                                      offset, bs, &batch_wall0, &d2h0_this, &agg0_this);
        }
        else
        {
            overall_dpu_overlap += pull_sparse_results(set0, n0, pair_counts0, tasklet_counts0, pair_chunk0, perf0,
                                                       aggregated_dpu_overlap, per_dpu_overlap0,
                                                       offset, bs, &batch_wall0, &d2h0_this, &agg0_this);
        }
        d2h0_total += d2h0_this;
        agg0_total += agg0_this;

        if (next_launch0 < num_batches)
        {
            int next_offset0 = next_launch0 * launch_query_cap;
            int next_bs0 = numQuery - next_offset0;
            if (next_bs0 > launch_query_cap)
                next_bs0 = launch_query_cap;
            uint64_t next_qnum0 = (uint64_t)next_bs0;
            size_t next_qbytes0 = (size_t)next_bs0 * sizeof(Rect);

            clock_gettime(CLOCK_MONOTONIC, &t0);
            DPU_ASSERT(dpu_broadcast_to(set0, "QUERY_NUM", 0, &next_qnum0, sizeof(next_qnum0), DPU_XFER_DEFAULT));
            DPU_ASSERT(dpu_broadcast_to(set0, "DPU_QUERY_RECT", 0, &query_rects[next_offset0], next_qbytes0, DPU_XFER_DEFAULT));
            clock_gettime(CLOCK_MONOTONIC, &t1);

            h2d0_batch[next_launch0] = sec_since(t0, t1);
            h2d0_total += h2d0_batch[next_launch0];

            clock_gettime(CLOCK_MONOTONIC, &launch_start0);
            DPU_ASSERT(dpu_launch(set0, DPU_ASYNCHRONOUS));
            running_batch0 = next_launch0;
            next_launch0++;
        }
        else
        {
            running_batch0 = -1;
        }

        if (running_batch1 != batch_no)
        {
            fprintf(stderr, "Unexpected set1 pipeline state: running=%d expected=%d\n",
                    running_batch1, batch_no);
            exit(EXIT_FAILURE);
        }

        DPU_ASSERT(dpu_sync(set1));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        host_kernel1_this = sec_since(launch_start1, t1);
        host_kernel1_total += host_kernel1_this;
        host_kernel_wall_total += host_kernel1_this;

        if (result_return_mode == RESULT_RETURN_MODE_DENSE)
        {
            overall_dpu_overlap += pull_dense_results(set1, n1, dense_chunk1, perf1,
                                                      aggregated_dpu_overlap, per_dpu_overlap1,
                                                      offset, bs, &batch_wall1, &d2h1_this, &agg1_this);
        }
        else
        {
            overall_dpu_overlap += pull_sparse_results(set1, n1, pair_counts1, tasklet_counts1, pair_chunk1, perf1,
                                                       aggregated_dpu_overlap, per_dpu_overlap1,
                                                       offset, bs, &batch_wall1, &d2h1_this, &agg1_this);
        }
        d2h1_total += d2h1_this;
        agg1_total += agg1_this;

        if (next_launch1 < num_batches)
        {
            int next_offset1 = next_launch1 * launch_query_cap;
            int next_bs1 = numQuery - next_offset1;
            if (next_bs1 > launch_query_cap)
                next_bs1 = launch_query_cap;
            uint64_t next_qnum1 = (uint64_t)next_bs1;
            size_t next_qbytes1 = (size_t)next_bs1 * sizeof(Rect);

            clock_gettime(CLOCK_MONOTONIC, &t0);
            DPU_ASSERT(dpu_broadcast_to(set1, "QUERY_NUM", 0, &next_qnum1, sizeof(next_qnum1), DPU_XFER_DEFAULT));
            DPU_ASSERT(dpu_broadcast_to(set1, "DPU_QUERY_RECT", 0, &query_rects[next_offset1], next_qbytes1, DPU_XFER_DEFAULT));
            clock_gettime(CLOCK_MONOTONIC, &t1);

            h2d1_batch[next_launch1] = sec_since(t0, t1);
            h2d1_total += h2d1_batch[next_launch1];

            clock_gettime(CLOCK_MONOTONIC, &launch_start1);
            DPU_ASSERT(dpu_launch(set1, DPU_ASYNCHRONOUS));
            running_batch1 = next_launch1;
            next_launch1++;
        }
        else
        {
            running_batch1 = -1;
        }

        uint64_t batch_wall = (batch_wall0 > batch_wall1) ? batch_wall0 : batch_wall1;
        kernel_cycles_total += batch_wall;
        if (batch_wall > kernel_cycles_peak)
            kernel_cycles_peak = batch_wall;

        if (DPU_FREQ_HZ > 0.0)
        {
            double batch_kernel0_s = (double)batch_wall0 / DPU_FREQ_HZ;
            double batch_kernel1_s = (double)batch_wall1 / DPU_FREQ_HZ;
            double batch_kernel_s = (double)batch_wall / DPU_FREQ_HZ;
            LOGF(log_file, "\nLaunch batch %d | logical=%d..%d | offset=%d size=%d"
                 "\n  set0: H2D=%.6f s | launch+sync=%.6f s | D2H=%.6f s | agg=%.6f s | DPU cycles wall=%.6f s"
                 "\n  set1: H2D=%.6f s | launch+sync=%.6f s | D2H=%.6f s | agg=%.6f s | DPU cycles wall=%.6f s"
                 "\n  batch max over sets: DPU cycles wall=%.6f s\n",
                 batch_no, logical_start, logical_end, offset, bs,
                 h2d0_batch[batch_no], host_kernel0_this, d2h0_this, agg0_this, batch_kernel0_s,
                 h2d1_batch[batch_no], host_kernel1_this, d2h1_this, agg1_this, batch_kernel1_s,
                 batch_kernel_s);
        }
        else
        {
            LOGF(log_file, "\nLaunch batch %d | logical=%d..%d | offset=%d size=%d"
                 "\n  set0: H2D=%.6f s | launch+sync=%.6f s | D2H=%.6f s | agg=%.6f s | DPU cycles wall=%" PRIu64
                 "\n  set1: H2D=%.6f s | launch+sync=%.6f s | D2H=%.6f s | agg=%.6f s | DPU cycles wall=%" PRIu64
                 "\n  batch max over sets: DPU cycles wall=%" PRIu64 "\n",
                 batch_no, logical_start, logical_end, offset, bs,
                 h2d0_batch[batch_no], host_kernel0_this, d2h0_this, agg0_this, batch_wall0,
                 h2d1_batch[batch_no], host_kernel1_this, d2h1_this, agg1_this, batch_wall1,
                 batch_wall);
        }
    }

    free(h2d0_batch);
    free(h2d1_batch);

    double setup_time = ctx->dpu_alloc_time + ctx->tree_transfer_time;
    double h2d_total = h2d0_total + h2d1_total;
    double d2h_total = d2h0_total + d2h1_total;
    double agg_total = agg0_total + agg1_total;
    double kernel_cycles_total_s = (double)kernel_cycles_total / DPU_FREQ_HZ;
    double summed_dpu_steps = setup_time + h2d_total + kernel_cycles_total_s + d2h_total + agg_total;

    LOGF(log_file, "\nSetup timing:"
         "\n  DPU allocation/load: %.6f s"
         "\n  Tree distribution: %.6f s"
         "\n  Setup total: %.6f s\n",
         ctx->dpu_alloc_time, ctx->tree_transfer_time, setup_time);

    LOGF(log_file, "\nDetailed timing totals:"
         "\n  H2D totals: set0=%.6f s | set1=%.6f s | combined=%.6f s"
         "\n  D2H totals: set0=%.6f s | set1=%.6f s | combined=%.6f s"
         "\n  Aggregate totals: set0=%.6f s | set1=%.6f s | combined=%.6f s"
         "\n  launch+sync totals: set0=%.6f s | set1=%.6f s | combined=%.6f s\n",
         h2d0_total, h2d1_total, h2d_total,
         d2h0_total, d2h1_total, d2h_total,
         agg0_total, agg1_total, agg_total,
         host_kernel0_total, host_kernel1_total, host_kernel_wall_total);
    LOGF(log_file, "\nCombined DPU step wall times:"
         "\n  H2D (query transfer): %.6f s"
         "\n  Kernel (cycle-derived actual search): %.6f s"
         "\n  D2H (result transfer): %.6f s"
         "\n  Aggregation: %.6f s"
         "\n  Setup: %.6f s"
         "\n  Summed step total: %.6f s\n",
         h2d_total, kernel_cycles_total_s, d2h_total, agg_total, setup_time, summed_dpu_steps);

    if (DPU_FREQ_HZ > 0.0)
    {
        double kernel_cycles_peak_s = (double)kernel_cycles_peak / DPU_FREQ_HZ;
        LOGF(log_file, "\nDPU cycle-derived kernel timing:"
             "\n  total kernel wall time (sum of batch walls)=%.6f s"
             "\n  peak single-batch kernel wall time=%.6f s"
             "\n  host launch+sync minus DPU cycle total=%.6f s\n",
             kernel_cycles_total_s, kernel_cycles_peak_s,
             host_kernel_wall_total - kernel_cycles_total_s);
    }

    LOGF(log_file, "\nTotal host measured (launch+sync) sum: %.6f s\n", host_kernel_wall_total);
    LOGF(log_file, "\nSummed DPU step total: %.6f s\n", summed_dpu_steps);

    clock_gettime(CLOCK_MONOTONIC, &stats.phase_end);

    double dpu_search_time_total = sec_since(ctx->phase_start, stats.phase_end);
    printf("\n[DPU] Actual elapsed DPU phase time (from transfer start to end): %.6f s\n", dpu_search_time_total);
    fprintf(log_file, "\n[DPU] Actual elapsed DPU phase time (from transfer start to end): %.6f s\n", dpu_search_time_total);

    if (result_return_mode == RESULT_RETURN_MODE_DENSE)
        LOGF(log_file, "\nPer-DPU overlap totals (computed on host from returned dense array):\n");
    else
        LOGF(log_file, "\nPer-DPU overlap totals (computed on host from returned sparse pairs):\n");

    for (uint32_t d = 0; d < n0; d++)
    {
        LOGF(log_file, "  DPU[%u] overlaps=%" PRIu64 "\n", d, per_dpu_overlap0[d]);
    }
    for (uint32_t d = 0; d < n1; d++)
    {
        LOGF(log_file, "  DPU[%u] overlaps=%" PRIu64 "\n", n0 + d, per_dpu_overlap1[d]);
    }

    free(per_dpu_overlap0);
    free(per_dpu_overlap1);
    free(perf0);
    free(perf1);
    free(pair_counts0);
    free(pair_counts1);
    free(tasklet_counts0);
    free(tasklet_counts1);
    free(pair_chunk0);
    free(pair_chunk1);
    free(dense_chunk0);
    free(dense_chunk1);
    free(aggregated_dpu_overlap);

    stats.overall_dpu_overlap = overall_dpu_overlap;
    stats.kernel_cycles_total = kernel_cycles_total;
    return stats;
}
