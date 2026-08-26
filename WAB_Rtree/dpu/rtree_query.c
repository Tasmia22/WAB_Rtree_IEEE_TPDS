#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <barrier.h>
#include <stdint.h>
#include <alloc.h>
#include <stdio.h>
#include "common.h"
#include "../host/rtree.h"

#define MAX_SUBTREE_CAPACITY MAX_SUBTREE
/*
 * Keep the 512-DPU profile conservative because it uses the widest WRAM header
 * caches. The 2540-DPU profile also uses MAX_SUBTREE=256 for one-pass Roads
 * placement, but it benefits from a less cramped query staging size than the
 * 512-DPU fallback path.
 */
#if NR_DPUS == 512
#define Q_BATCH 32
#define RECT_CHUNK 32
#elif NR_DPUS == 2540
#define Q_BATCH 64
#define RECT_CHUNK 64
#else
#define Q_BATCH 128
#define RECT_CHUNK 64
#endif

typedef struct
{
    uint32_t isLeaf;
    uint32_t count;
    MBR mbr;
} NodeHdr;

// ------------------- MRAM -------------------

__mram_noinit PerfStats DPU_PERF;
__mram_noinit MramConfig MRAM_RUNTIME_CONFIG;
__mram_noinit Rect DPU_QUERY_RECT[MAX_QUERY];
__mram_noinit SerializedNodeHdr DPU_TOP_TREE[MAX_TOP2]; // Broadcast
__mram_noinit SerializedLeafNode DPU_LOW_TREE[MAX_SUBTREE_CAPACITY]; // Parallel transfer (leaf-only)
__mram_noinit ResultPair DPU_RESULT_PAIRS[RESULT_PAIR_CAPACITY];
__mram_noinit DenseResultSlot DPU_RESULT_DENSE[MAX_QUERY];
__mram_noinit uint64_t DPU_TASKLET_PAIR_COUNTS[NR_TASKLETS];
__mram_noinit uint64_t DPU_RESULT_PAIR_COUNT;
__mram_noinit struct dpu_low_with_index DPU_LOW_WITH_INDEX;
__mram_noinit uint64_t QUERY_NUM;
__mram_noinit uint64_t DPU_FANOUT;

// Header caches are populated once by tasklet 0 and then read-only.
__dma_aligned static NodeHdr low_hdr_cache[MAX_SUBTREE_CAPACITY];
__dma_aligned static SerializedNodeHdr top_hdr_cache[MAX_TOP2];
static uint32_t low_cnt_wram;
static uint32_t top_cnt_wram;
static uint32_t replica_rank_wram;
static uint32_t replica_count_wram;
static uint32_t replica_query_policy_wram;
static uint32_t replica_query_block_wram;
static uint32_t result_return_mode_wram;
static uint32_t top_start_i, top_end_i;

// ------------------- WRAM -------------------

BARRIER_INIT(my_barrier, NR_TASKLETS);

bool isRectOverlap(Rect r1, Rect r2)
{
    return !(r1.xmax < r2.xmin || r1.xmin > r2.xmax ||
             r1.ymax < r2.ymin || r1.ymin > r2.ymax);
}

__dma_aligned static Rect rbuf[NR_TASKLETS][RECT_CHUNK];

int search_leaf_subtree(Rect query)
{
    int count = 0;
    uint32_t tid = me();
    Rect *rb = rbuf[tid];

    for (uint32_t node = 0; node < low_cnt_wram; node++)
    {
        NodeHdr *h = &low_hdr_cache[node];

        Rect node_mbr = {h->mbr.xmin, h->mbr.ymin, h->mbr.xmax, h->mbr.ymax};
        if (!isRectOverlap(node_mbr, query))
            continue;
        if (!h->isLeaf)
            continue;

        uint32_t n = h->count;
        if (n > MAX_RECTS)
            n = MAX_RECTS;

        for (uint32_t j = 0; j < n; j += RECT_CHUNK)
        {
            uint32_t take = n - j;
            if (take > RECT_CHUNK)
                take = RECT_CHUNK;

            mram_read((__mram_ptr void const *)&DPU_LOW_TREE[node].rects[j],
                      rb, take * sizeof(Rect));

            for (uint32_t k = 0; k < take; k++)
                count += isRectOverlap(rb[k], query);
        }
    }
    return count;
}

static inline int search_top2_then_local(Rect query)
{
    for (uint32_t idx = top_start_i; idx < top_end_i; idx++)
    {
        SerializedNodeHdr *hdr = &top_hdr_cache[idx];
        Rect m = {hdr->mbr.xmin, hdr->mbr.ymin, hdr->mbr.xmax, hdr->mbr.ymax};
        if (isRectOverlap(m, query))
            return search_leaf_subtree(query);
    }
    return 0;
}

int main()
{
    uint32_t tasklet_id = me();

    if (tasklet_id == 0)
    {
        perfcounter_config(COUNT_CYCLES, true);
        mem_reset();

        MramConfig runtime_cfg;
        mram_read((__mram_ptr void const *)&MRAM_RUNTIME_CONFIG, &runtime_cfg, sizeof(MramConfig));

        uint32_t actual_query_count = runtime_cfg.actual_query_count;
        if (actual_query_count == 0 || actual_query_count > MAX_QUERY)
            actual_query_count = MAX_QUERY;

        uint32_t actual_top_tree_count = runtime_cfg.actual_top_tree_count;
        if (actual_top_tree_count == 0 || actual_top_tree_count > MAX_TOP2)
            actual_top_tree_count = MAX_TOP2;

        uint32_t actual_low_tree_count = runtime_cfg.actual_low_tree_count;
        if (actual_low_tree_count == 0 || actual_low_tree_count > MAX_SUBTREE_CAPACITY)
            actual_low_tree_count = MAX_SUBTREE_CAPACITY;

        result_return_mode_wram = runtime_cfg.result_return_mode;
        if (result_return_mode_wram != RESULT_RETURN_MODE_DENSE)
            result_return_mode_wram = RESULT_RETURN_MODE_SPARSE;

        replica_query_policy_wram = runtime_cfg.replica_query_policy;
        if (replica_query_policy_wram != REPLICA_QUERY_POLICY_RANGE)
            replica_query_policy_wram = REPLICA_QUERY_POLICY_BLOCK_CYCLIC;

        replica_query_block_wram = runtime_cfg.replica_query_block_queries;
        if (replica_query_block_wram == 0u)
            replica_query_block_wram = 2048u;
        if (replica_query_block_wram > actual_query_count)
            replica_query_block_wram = actual_query_count;
        if (replica_query_block_wram == 0u)
            replica_query_block_wram = 1u;

        top_cnt_wram = actual_top_tree_count;
        for (uint32_t i = 0; i < top_cnt_wram; i++)
        {
            mram_read((__mram_ptr void const *)(DPU_TOP_TREE + i),
                      &top_hdr_cache[i], sizeof(SerializedNodeHdr));
        }

        low_cnt_wram = (uint32_t)DPU_LOW_WITH_INDEX.low_tree_count;
        if (low_cnt_wram > actual_low_tree_count)
            low_cnt_wram = actual_low_tree_count;
        for (uint32_t i = 0; i < low_cnt_wram; i++)
        {
            mram_read((__mram_ptr void const *)(DPU_LOW_TREE + i),
                      &low_hdr_cache[i], sizeof(NodeHdr));
        }

        uint32_t replica_count = (uint32_t)DPU_LOW_WITH_INDEX.replica_count;
        if (replica_count == 0u)
            replica_count = 1u;
        replica_count_wram = replica_count;

        uint32_t replica_rank = (uint32_t)DPU_LOW_WITH_INDEX.replica_rank;
        if (replica_rank >= replica_count_wram)
            replica_rank = 0u;
        replica_rank_wram = replica_rank;

        if (low_cnt_wram == 0)
        {
            top_start_i = 1u;
            top_end_i = 1u;
        }
        else
        {
            uint32_t s = (uint32_t)DPU_LOW_WITH_INDEX.l1_index;
            uint32_t e = s + (uint32_t)DPU_LOW_WITH_INDEX.l1_count;

            if (s < 1u)
                s = 1u;
            if (e > top_cnt_wram)
                e = top_cnt_wram;

            if (s >= e)
            {
                s = 1u;
                e = top_cnt_wram;
            }

            top_start_i = s;
            top_end_i = e;
        }
    }

    barrier_wait(&my_barrier);

    uint32_t start = perfcounter_get();
    uint32_t query_start = 0;
    uint32_t query_end = (uint32_t)QUERY_NUM;
    bool use_range_partition = false;

    /*
     * Replicated shards need a query partition. A single contiguous range keeps
     * MRAM reads sequential, but it can underutilize replicas when the query
     * file is spatially clustered. Block-cyclic scheduling keeps reads mostly
     * contiguous while spreading those clusters across replicas.
     */
    if (result_return_mode_wram == RESULT_RETURN_MODE_SPARSE &&
        replica_count_wram > 1u &&
        replica_query_policy_wram == REPLICA_QUERY_POLICY_RANGE)
    {
        uint64_t qn = QUERY_NUM;
        query_start = (uint32_t)((qn * replica_rank_wram) / replica_count_wram);
        query_end = (uint32_t)((qn * (replica_rank_wram + 1u)) / replica_count_wram);
        use_range_partition = true;
    }

    uint32_t assigned_queries = query_end - query_start;
    uint32_t queries_per_tasklet = assigned_queries / NR_TASKLETS;
    uint32_t remaining = assigned_queries % NR_TASKLETS;

    uint32_t start_q = query_start + tasklet_id * queries_per_tasklet + (tasklet_id < remaining ? tasklet_id : remaining);
    uint32_t count = queries_per_tasklet + (tasklet_id < remaining ? 1 : 0);
    uint32_t end_q = start_q + count;

    if (count > TASKLET_QUERY_CAP)
        return -1;

    uint32_t segment_base = tasklet_id * TASKLET_QUERY_CAP;
    uint32_t local_pairs = 0;
    uint32_t buffered_pairs = 0;

    Rect *qbuf = (Rect *)mem_alloc(Q_BATCH * sizeof(Rect));
    ResultPair *pairbuf = NULL;
    DenseResultSlot *densebuf = NULL;
    if (result_return_mode_wram == RESULT_RETURN_MODE_DENSE)
        densebuf = (DenseResultSlot *)mem_alloc(Q_BATCH * sizeof(DenseResultSlot));
    else
        pairbuf = (ResultPair *)mem_alloc(Q_BATCH * sizeof(ResultPair));
    if (qbuf == NULL || (result_return_mode_wram == RESULT_RETURN_MODE_DENSE ? densebuf == NULL : pairbuf == NULL))
        return -1;

    if (result_return_mode_wram == RESULT_RETURN_MODE_SPARSE &&
        replica_count_wram > 1u &&
        replica_query_policy_wram == REPLICA_QUERY_POLICY_BLOCK_CYCLIC)
    {
        uint32_t total_queries = (uint32_t)QUERY_NUM;
        uint32_t block_queries = replica_query_block_wram;
        if (block_queries < Q_BATCH)
            block_queries = Q_BATCH;

        uint32_t total_blocks = (total_queries + block_queries - 1u) / block_queries;
        uint64_t first_block = (uint64_t)tasklet_id * (uint64_t)replica_count_wram + (uint64_t)replica_rank_wram;
        uint64_t block_stride = (uint64_t)NR_TASKLETS * (uint64_t)replica_count_wram;

        for (uint64_t block = first_block; block < (uint64_t)total_blocks; block += block_stride)
        {
            uint32_t block_start = (uint32_t)(block * (uint64_t)block_queries);
            uint32_t block_end = block_start + block_queries;
            if (block_end > total_queries)
                block_end = total_queries;

            for (uint32_t q = block_start; q < block_end; q += Q_BATCH)
            {
                uint32_t fetch = block_end - q;
                if (fetch > Q_BATCH)
                    fetch = Q_BATCH;

                mram_read((__mram_ptr void const *)(DPU_QUERY_RECT + q), qbuf, fetch * sizeof(Rect));

                for (uint32_t i = 0; i < fetch; i++)
                {
                    uint32_t qid = q + i;
                    uint32_t hits32 = (uint32_t)search_top2_then_local(qbuf[i]);
                    if (hits32 == 0)
                        continue;

                    pairbuf[buffered_pairs].qid = qid;
                    pairbuf[buffered_pairs].count = hits32;
                    buffered_pairs++;

                    if (buffered_pairs == Q_BATCH)
                    {
                        if (local_pairs + buffered_pairs > TASKLET_QUERY_CAP)
                            return -1;

                        mram_write(pairbuf,
                                   (__mram_ptr void *)(DPU_RESULT_PAIRS + segment_base + local_pairs),
                                   buffered_pairs * sizeof(ResultPair));
                        local_pairs += buffered_pairs;
                        buffered_pairs = 0;
                    }
                }
            }
        }
    }
    else
    {
        (void)use_range_partition;

        for (uint32_t q = start_q; q < end_q; q += Q_BATCH)
        {
            uint32_t fetch = end_q - q;
            if (fetch > Q_BATCH)
                fetch = Q_BATCH;

            mram_read((__mram_ptr void const *)(DPU_QUERY_RECT + q), qbuf, fetch * sizeof(Rect));

            if (result_return_mode_wram == RESULT_RETURN_MODE_DENSE)
            {
                for (uint32_t i = 0; i < fetch; i++)
                {
                    uint32_t qid = q + i;
                    uint32_t hits32 = 0;
                    if (!(replica_count_wram > 1u && (qid % replica_count_wram) != replica_rank_wram))
                        hits32 = (uint32_t)search_top2_then_local(qbuf[i]);

                    densebuf[i].count = hits32;
                    densebuf[i].reserved = 0;
                }

                mram_write(densebuf,
                           (__mram_ptr void *)(DPU_RESULT_DENSE + q),
                           fetch * sizeof(DenseResultSlot));
                continue;
            }

            for (uint32_t i = 0; i < fetch; i++)
            {
                uint32_t qid = q + i;
                uint32_t hits32 = (uint32_t)search_top2_then_local(qbuf[i]);
                if (hits32 == 0)
                    continue;

                pairbuf[buffered_pairs].qid = qid;
                pairbuf[buffered_pairs].count = hits32;
                buffered_pairs++;

                if (buffered_pairs == Q_BATCH)
                {
                    if (local_pairs + buffered_pairs > TASKLET_QUERY_CAP)
                        return -1;

                    mram_write(pairbuf,
                               (__mram_ptr void *)(DPU_RESULT_PAIRS + segment_base + local_pairs),
                               buffered_pairs * sizeof(ResultPair));
                    local_pairs += buffered_pairs;
                    buffered_pairs = 0;
                }
            }
        }
    }

    if (result_return_mode_wram == RESULT_RETURN_MODE_SPARSE && buffered_pairs > 0)
    {
        if (local_pairs + buffered_pairs > TASKLET_QUERY_CAP)
            return -1;

        mram_write(pairbuf,
                   (__mram_ptr void *)(DPU_RESULT_PAIRS + segment_base + local_pairs),
                   buffered_pairs * sizeof(ResultPair));
        local_pairs += buffered_pairs;
    }

    DPU_TASKLET_PAIR_COUNTS[tasklet_id] = (result_return_mode_wram == RESULT_RETURN_MODE_DENSE) ? count : local_pairs;

    uint32_t stop = perfcounter_get();
    DPU_PERF.per_tasklet[tasklet_id] = stop - start;

    barrier_wait(&my_barrier);

    if (tasklet_id == 0)
    {
        uint32_t out_count = 0;
        for (uint32_t t = 0; t < NR_TASKLETS; t++)
            out_count += DPU_TASKLET_PAIR_COUNTS[t];
        DPU_RESULT_PAIR_COUNT = (result_return_mode_wram == RESULT_RETURN_MODE_DENSE) ? QUERY_NUM : out_count;

        uint32_t max_cycles = 0;
        for (int t = 0; t < NR_TASKLETS; t++)
            if (DPU_PERF.per_tasklet[t] > max_cycles)
                max_cycles = DPU_PERF.per_tasklet[t];
        DPU_PERF.total_cycles = (uint64_t)max_cycles;
    }

    return 0;
}
