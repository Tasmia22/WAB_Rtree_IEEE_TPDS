#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <barrier.h>
#include <stdint.h>
#include <alloc.h>
#include <stdio.h>
#include "common.h"
#include "../host/rtree.h"

#define MAX_SUBTREE 72
#define DEBUG_PRINT_MAX 16
#define Q_BATCH 128 // tune if WRAM tight
#define HDR_BATCH 64

// ------------------- MRAM -------------------

__mram_noinit PerfStats DPU_PERF;
__mram_noinit Rect DPU_QUERY_RECT[MAX_QUERY];
__mram_noinit SerializedNodeHdr DPU_TOP_TREE[MAX_TOP2]; // Broadcst
__mram_noinit SerializedNode DPU_LOW_TREE[MAX_SUBTREE]; // Parallel Transfer
__mram_noinit uint64_t DPU_OVERLAP_COUNT[MAX_QUERY];
__mram_noinit struct dpu_low_with_index DPU_LOW_WITH_INDEX;
__mram_noinit uint64_t QUERY_NUM;
__mram_noinit uint64_t DPU_FANOUT;

// ------------------- WRAM -------------------

BARRIER_INIT(my_barrier, NR_TASKLETS);

bool isRectOverlap(Rect r1, Rect r2)
{
    return !(r1.xmax < r2.xmin || r1.xmin > r2.xmax ||
             r1.ymax < r2.ymin || r1.ymin > r2.ymax);
}

__dma_aligned static SerializedNodeHdr top_tree[MAX_TOP2];

int search_leaf_subtree(Rect query)
{

    int count = 0;
    int dpu_low_nodes = DPU_LOW_WITH_INDEX.low_tree_count;
    for (int i = 0; i < (int)dpu_low_nodes; i++)
    {
        Rect node_mbr = {
            DPU_LOW_TREE[i].mbr.xmin,
            DPU_LOW_TREE[i].mbr.ymin,
            DPU_LOW_TREE[i].mbr.xmax,
            DPU_LOW_TREE[i].mbr.ymax};
        if (!isRectOverlap(node_mbr, query))
            continue;
        if (DPU_LOW_TREE[i].isLeaf)
        {
            for (int j = 0; j < DPU_LOW_TREE[i].count; j++)
            {
                Rect r = DPU_LOW_TREE[i].rects[j];

                if (isRectOverlap(r, query))
                {
                    count++;
                }
            }
        }
    }
    return count;
}

int search_top2_then_local(Rect query)
{
    int group_size = DPU_FANOUT / top_tree[0].count; // 128
    int level1_idx = DPU_LOW_WITH_INDEX.dpu_index / group_size;
    if (level1_idx == 0)
        level1_idx++;
    for (int i = level1_idx - 1; i < level1_idx + 3; i++)
    {
        // Get the MBR of the current node in top_tree
        Rect level1_mbr = {
            top_tree[i].mbr.xmin,
            top_tree[i].mbr.ymin,
            top_tree[i].mbr.xmax,
            top_tree[i].mbr.ymax};
        // Check if the query overlaps with the MBR of the top-level node
        if (isRectOverlap(level1_mbr, query))
        {
            // If overlap, proceed to search the low-level tree (DPU_LOW_TREE)
            return search_leaf_subtree(query); // Now search through the low-level tree
        }
    }
    return 0;
}

// int search_top2_then_local(Rect query)
// {
//     return search_leaf_subtree(query);
// }

int main()
{
    uint32_t tasklet_id = me();

    if (tasklet_id == 0)
    {
        // configure perf counter once
        perfcounter_config(COUNT_CYCLES, true);

        // init shared WRAM allocator (only if you use mem_alloc later)
        mem_reset();
        int top2_count = DPU_TOP_TREE[0].count+1;

        // copy root + L1 headers from MRAM to WRAM once
        for (int i = 0; i < top2_count; i++)
        {
            mram_read((__mram_ptr void const *)(DPU_TOP_TREE + i),
                      &top_tree[i], sizeof(SerializedNodeHdr));
        }
    }

    // everyone waits until allocator + TOP_WRAM are ready
    barrier_wait(&my_barrier);

    uint32_t start = perfcounter_get(); // snapshot at start
    int queries_per_tasklet = QUERY_NUM / NR_TASKLETS;
    uint32_t remaining = QUERY_NUM % NR_TASKLETS;

    uint32_t start_q = tasklet_id * queries_per_tasklet + (tasklet_id < remaining ? tasklet_id : remaining);
    uint32_t count = queries_per_tasklet + (tasklet_id < remaining ? 1 : 0);
    uint32_t end_q = start_q + count;

    Rect *qbuf = (Rect *)mem_alloc(Q_BATCH * sizeof(Rect));
    uint64_t *hbuf = (uint64_t *)mem_alloc(Q_BATCH * sizeof(uint64_t));
    if (qbuf == NULL || hbuf == NULL)
    {
        // Not enough WRAM; optionally handle error or reduce Q_BATCH
        return -1;
    }

    // 3) process in batches, careful with the tail (partial fetch)
    for (uint32_t q = start_q; q < end_q; q += Q_BATCH)
    {
        uint32_t fetch = end_q - q;
        if (fetch > Q_BATCH)
            fetch = Q_BATCH;

        // (a) read 'fetch' queries into WRAM
        mram_read((__mram_ptr void const *)(DPU_QUERY_RECT + q), qbuf, fetch * sizeof(Rect));

        // (b) compute hits for this batch
        for (uint32_t i = 0; i < fetch; i++)
        {
            uint32_t hits32 = (uint32_t)search_top2_then_local(qbuf[i]);
            hbuf[i] = (uint64_t)hits32;
        }

        mram_write(hbuf, (__mram_ptr void *)(DPU_OVERLAP_COUNT + q), fetch * sizeof(uint64_t));
    }

    uint32_t stop = perfcounter_get();               // snapshot at end
    DPU_PERF.per_tasklet[tasklet_id] = stop - start; // store per-tasklet cycles

    barrier_wait(&my_barrier);

    // Compute DPU wall time as max over tasklets’ cycle deltas
    if (tasklet_id == 0)
    {
        uint32_t max_cycles = 0;
        for (int t = 0; t < NR_TASKLETS; t++)
            if (DPU_PERF.per_tasklet[t] > max_cycles)
                max_cycles = DPU_PERF.per_tasklet[t];
        DPU_PERF.total_cycles = (uint64_t)max_cycles;
    }
}
