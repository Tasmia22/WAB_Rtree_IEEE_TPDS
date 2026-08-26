#define _POSIX_C_SOURCE 200809L
#include <dpu.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "common.h"
#include "rtree.h"
#include <inttypes.h>

#ifndef DPU_BINARY
#define DPU_BINARY "build/dpu"
#endif

int get_rtree_memory_size(Node *node)
{
    if (node == NULL)
        return 0;
    int size = sizeof(Node);
    if (node->isLeaf)
    {
        size += sizeof(Rect) * node->count;
    }
    else
    {
        size += sizeof(Node *) * node->count;
        for (int i = 0; i < node->count; i++)
        {
            size += get_rtree_memory_size(node->children[i]);
        }
    }
    return size;
}

#define LOGF(fmt, ...)                         \
    do                                         \
    {                                          \
        printf(fmt, ##__VA_ARGS__);            \
        fprintf(log_file, fmt, ##__VA_ARGS__); \
    } while (0)

// --- timing helpers ---
static inline double sec_since(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static const double DPU_FREQ_HZ = 400e6; // e.g., 450e6

int main()
{
    struct dpu_set_t dpu_set, dpu;
    uint32_t nr_of_dpus;
    bool status = true;

    struct timespec t0, t1, t2, t3, t4, t5, t6, t7,upperlevel_time;
    double rtree_construction_time;
    time_t raw_time = time(NULL);
    struct tm *tm_info = localtime(&raw_time);
    char log_filename[64];
    strftime(log_filename, sizeof(log_filename), "logs/dpu_%Y%m%d_%H:%M.log", tm_info);
    FILE *log_file = fopen(log_filename, "w");

    if (!log_file)
    {
        perror("Failed to open log file");
        return EXIT_FAILURE;
    }

    int numRects, numQuery, dataset_option = 0;
    printf("\nHow many data you want to work with? Choose option: \n\t1. 16M\n\t2. Sports(999k)\n\t3. Sports(1.7M) \n\t4. parks(300k)\n\t5. cemetery(168k)\n\t6. Lakes(8M)\n\t7. Buildings(14.3M)\n");
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

    clock_gettime(CLOCK_MONOTONIC, &t0);
    // Node *root = createRTree(rects, 0, numRects - 1);
    Node *root = createRTree_STR(rects, 0, numRects - 1);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    rtree_construction_time = sec_since(t0, t1);
    printRTreeStats(root);
    // printRTree(root, 0);
    int rtree_mem_size = get_rtree_memory_size(root);
    printf("\nActual R-tree memory size: %.2f MB\n", rtree_mem_size / (1024.0 * 1024.0));
    Rect *query_rects = selectQueryDataset(&numQuery, dataset_option);

    //Zsorting(query_rects, numQuery);
    if (!query_rects)
    {
        printf("Failed to read Query Rectangles.\n");
        return -1;
    }
    printf("Read %d rects successfully. with Querydata size: %.2f MB\n", numQuery, (numQuery * sizeof(Rect)) / (1024.0 * 1024.0));

    // Allocate memory to store per-query CPU results
    uint64_t *cpu_overlap_count = calloc(numQuery, sizeof(uint64_t));
    int found_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    int result_host;
    for (int i = 0; i < numQuery; i++)
    {
        result_host = searchRTree(root, query_rects[i], i);
        cpu_overlap_count[i] = result_host;
        found_count += result_host;
    }

    clock_gettime(CLOCK_MONOTONIC, &t3);
    double search_time = sec_since(t2, t3);
    printf("[CPU-ONLY] Overlaps=%llu and Searching Time=%.2f s \n", (unsigned long long)found_count, search_time);

    clock_gettime(CLOCK_MONOTONIC, &t4); // Start

    DPU_ASSERT(dpu_alloc(NR_DPUS, NULL, &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY, NULL));
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
    printf("\n\nAllocated %d DPU(s)\n", nr_of_dpus);

    // uint64_t dpu_ids[nr_of_dpus];

    printf("\nPassing Tree and Query to DPUs...\n");

    printf("\n\t Step 1: Serializing the entire Rtree...");

    SerializedNode *serialized_tree = NULL;
    int total_serialized_nodes = serialize_rtree_bfs_wrapper(root, &serialized_tree);

    if (total_serialized_nodes <= 0 || serialized_tree == NULL)
    {
        fprintf(stderr, "Serialization failed.\n");
        return EXIT_FAILURE;
    }

    printf("\n\t Serialized %d nodes in BFS order.", total_serialized_nodes);
    printf("\n\t Step 1: Serialization done.");

    printf("\n\t Printing Serialized Tree:\n");

    printf("\n\t Step 2: Get the top 2 levels and broadcast.");
    int top2_node_count = root->count + 1;
    printf("\n\t   Top2 Node count=%d ", top2_node_count);

    SerializedNodeHdr top2_hdrs[MAX_TOP2] = {0};
    for (int i = 0; i < top2_node_count; ++i)
    {
        top2_hdrs[i].isLeaf = serialized_tree[i].isLeaf;
        top2_hdrs[i].count = serialized_tree[i].count;
        top2_hdrs[i].mbr = serialized_tree[i].mbr;
    }

    DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_TOP_TREE", 0,
                                top2_hdrs, MAX_TOP2 * sizeof(SerializedNodeHdr),
                                DPU_XFER_DEFAULT));

    uint64_t fanout = FANOUT; // FANOUT = NR_DPUS
    DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_FANOUT", 0, &fanout, sizeof(uint64_t), DPU_XFER_DEFAULT));

    printf("\n\t Step 2: Broadcasting the upper level is done.\n");
    clock_gettime(CLOCK_MONOTONIC, &upperlevel_time); // upperlevel_end
    double tree_transfer_time=sec_since(t4,upperlevel_time);

    uint64_t *zeros = calloc(MAX_QUERY, sizeof(uint64_t));
    DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_OVERLAP_COUNT", 0, zeros, MAX_QUERY * sizeof(uint64_t), DPU_XFER_DEFAULT));
    free(zeros);

    printf("\n\t Step 3: Parallel Transfer lower level tree.\n");

    int level2_start = 1 + serialized_tree[0].count;
    printf("\n\t Level2_Starts at=%d", level2_start);

    int total_level2 = 0;
    for (int i = 1; i <= serialized_tree[0].count; i++)
    {
        total_level2 += serialized_tree[i].count;
        // printf("\n       Level 1 node %d has %d children\n", i, serialized_tree[i].count);
    }
    printf("\n\t Total Nodes in level 2= %d\n", total_level2);
    int actual_dpus_needed = (total_level2 < (int)nr_of_dpus) ? total_level2 : (int)nr_of_dpus;
    printf("\n\t actual_dpus_needed= %d", actual_dpus_needed);

    SerializedNode **dpu_subtrees = malloc(actual_dpus_needed * sizeof(SerializedNode *));
    uint64_t *dpu_low_nodes_counts = malloc(actual_dpus_needed * sizeof(uint64_t));
    if (!dpu_subtrees || !dpu_low_nodes_counts)
    {
        perror("alloc");
        exit(1);
    }

    int nodes_per_dpu = total_level2 / actual_dpus_needed;
    int remainder = total_level2 % actual_dpus_needed;            // e.g., 137
    int max_node_count = nodes_per_dpu + (remainder > 0 ? 1 : 0); // 8 in your case

    // Print stats
    printf("\nnodes_per_dpu: %d (base), remainder (extra nodes) = %d\n", nodes_per_dpu, remainder);

    int subtree_offset = 0;
    for (int d = 0; d < actual_dpus_needed; d++)
    {
        int this_node_count = nodes_per_dpu + (d < remainder ? 1 : 0);
        dpu_subtrees[d] = calloc(max_node_count, sizeof(SerializedNode)); // calloc zeroes unused slots
        if (!dpu_subtrees[d])
        {
            perror("malloc");
            exit(1);
        }
        dpu_low_nodes_counts[d] = (uint64_t)this_node_count;
        for (int j = 0; j < this_node_count; j++)
        {
            int global_idx = level2_start + subtree_offset++;
            dpu_subtrees[d][j] = serialized_tree[global_idx];
        }
        // remaining entries (this_node_count .. max_node_count-1) are zeroed by calloc
    }

    // Bulk transfer: Prepare and push max-sized transfers
    size_t max_transfer_size = max_node_count * sizeof(SerializedNode);
    printf("\n\n Max_transfer_size=%ld", max_transfer_size);
    int i;

    DPU_FOREACH(dpu_set, dpu, i)
    {
        DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_subtrees[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_LOW_TREE", 0, max_transfer_size, DPU_XFER_DEFAULT));

    struct dpu_low_with_index *ctrl = malloc((size_t)nr_of_dpus * sizeof(*ctrl));
    if (!ctrl)
    {
        perror("malloc ctrl");
        exit(1);
    }

    // Fill per-DPU values
    for (uint32_t i = 0; i < nr_of_dpus; i++)
    {
        ctrl[i].dpu_index = i;
        ctrl[i].low_tree_count = (i < (uint32_t)actual_dpus_needed)
                                     ? (uint32_t)dpu_low_nodes_counts[i]
                                     : 0u;
    }

    // Transfer to all DPUs in one pass
    DPU_FOREACH(dpu_set, dpu, i)
    {
        DPU_ASSERT(dpu_prepare_xfer(dpu, &ctrl[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU,
                             "DPU_LOW_WITH_INDEX", 0,
                             sizeof(struct dpu_low_with_index),
                             DPU_XFER_DEFAULT));

    printf("\nTree and DPU Index transfer complete. Now Transferring the Query---\n");

    uint64_t *aggregated_dpu_overlap = calloc(numQuery, sizeof(uint64_t));
    uint64_t *per_dpu_overlap_total = calloc((size_t)nr_of_dpus, sizeof(uint64_t));
    int Overall_DPU_overlap_c = 0;
    if (aggregated_dpu_overlap == NULL || per_dpu_overlap_total == NULL)
    {
        perror("calloc overlap buffers");
        exit(EXIT_FAILURE);
    }
    uint64_t query_num = MAX_QUERY;
    // DPU_ASSERT(dpu_broadcast_to(dpu_set, "QUERY_NUM", 0, &query_num, sizeof(uint64_t), DPU_XFER_DEFAULT));
    int batch_no = 0;
    double batch_time;
    // double batch_query_transfer_time, result_retrieval_time, kernel_time;
    clock_gettime(CLOCK_MONOTONIC, &t5);
    double kernel_time_total = 0.0;
    uint64_t kernel_cycles_total = 0; // sum of batch wall cycles (max over DPUs per batch)
    uint64_t kernel_cycles_peak = 0;
    struct timespec t_io0, t_io1;
    double transfer_cumu = 0.0;
    for (int offset = 0; offset < numQuery; offset += MAX_QUERY)
    {

        int current_batch_size = (offset + MAX_QUERY > numQuery) ? (numQuery - offset) : MAX_QUERY;

        if (offset + current_batch_size > numQuery)
        {
            printf("Error: Attempting to access queries beyond bounds: offset = %d, current_batch_size = %d\n", offset, current_batch_size);
            exit(EXIT_FAILURE);
        }

        // Update QUERY_NUM if needed
        if (current_batch_size != MAX_QUERY)
        {
            query_num = (uint64_t)current_batch_size;
            printf("\n Query size is modified to size=%lu ", query_num);
        }
        if (current_batch_size > MAX_QUERY)
        {
            fprintf(stderr, "ERROR: current_batch_size (%d) > MAX_QUERY (%d). Reduce batch or recompile DPU.\n",
                    current_batch_size, MAX_QUERY);
            exit(EXIT_FAILURE);
        }
        clock_gettime(CLOCK_MONOTONIC, &t_io0);
        // printf("\nBatch %d and query_num=%lu Current BatchSize=%d", batch_no, query_num, current_batch_size);
        DPU_ASSERT(dpu_broadcast_to(dpu_set, "QUERY_NUM", 0, &query_num, sizeof(uint64_t), DPU_XFER_DEFAULT));
        // Broadcast queries

        DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_QUERY_RECT", 0, &query_rects[offset], current_batch_size * sizeof(Rect), DPU_XFER_DEFAULT));
        clock_gettime(CLOCK_MONOTONIC, &t_io1);
        double transfer_this_batch = sec_since(t_io0, t_io1);
        transfer_cumu += transfer_this_batch;

        // --- KERNEL ONLY (measure just this) ---
        struct timespec k0, k1;
        clock_gettime(CLOCK_MONOTONIC, &k0);
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
        clock_gettime(CLOCK_MONOTONIC, &k1);
        kernel_time_total += sec_since(k0, k1);

        // DPU_FOREACH(dpu_set, dpu)
        // {
        //     DPU_ASSERT(dpu_log_read(dpu, stdout));
        // }

        uint64_t batch_wall_cycles = 0; // “kernel wall time” in cycles for this batch (max over DPUs)
        // 1) Allocate one big buffer for all DPUs’ counts (reuse across batches if size stable)
        size_t counts_per_dpu = (size_t)current_batch_size;
        size_t counts_bytes = counts_per_dpu * sizeof(uint64_t);

        // You can cache these allocations outside the loop; shown inline for clarity
        uint64_t *counts_all = NULL;
        if (posix_memalign((void **)&counts_all, 64, counts_bytes * nr_of_dpus) != 0)
        {
            perror("posix_memalign counts_all");
            exit(EXIT_FAILURE);
        }
        struct timespec t_res0, t_res1;
        double result_retrieval_this_batch = 0.0;
        clock_gettime(CLOCK_MONOTONIC, &t_res0);
        // 2) Prepare one host slice per DPU
        size_t idx = 0;
        struct dpu_set_t dpu;
        DPU_FOREACH(dpu_set, dpu)
        {
            // each DPU writes into its own slice [idx * counts_per_dpu, ... )
            dpu_prepare_xfer(dpu, counts_all + idx * counts_per_dpu);
            idx++;
        }

        // 3) Single vectorized pull for all DPUs (DPU -> host)
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "DPU_OVERLAP_COUNT",
                                 0,            // MRAM offset in the symbol
                                 counts_bytes, // bytes per DPU
                                 DPU_XFER_DEFAULT));
        clock_gettime(CLOCK_MONOTONIC, &t_res1);
        result_retrieval_this_batch = sec_since(t_res0, t_res1);
        // 2) DPU_PERF: pull perf stats (cycles)
        PerfStats *perf_all = NULL;
        if (posix_memalign((void **)&perf_all, 64, sizeof(PerfStats) * nr_of_dpus) != 0)
        {
            perror("posix_memalign perf_all");
            exit(EXIT_FAILURE);
        }

        idx = 0;
        DPU_FOREACH(dpu_set, dpu)
        {
            dpu_prepare_xfer(dpu, &perf_all[idx]);
            idx++;
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "DPU_PERF",
                                 0, sizeof(PerfStats), DPU_XFER_DEFAULT));

        // Aggregate overlap counts and compute batch wall cycles from perf
        for (uint32_t d = 0; d < nr_of_dpus; ++d)
        {
            // counts
            uint64_t *temp_result = counts_all + d * counts_per_dpu;
            for (int i = 0; i < current_batch_size; ++i)
            {
                aggregated_dpu_overlap[offset + i] += temp_result[i];
                Overall_DPU_overlap_c += temp_result[i];
                per_dpu_overlap_total[d] += temp_result[i];
            }
            // perf
            if (perf_all[d].total_cycles > batch_wall_cycles)
                batch_wall_cycles = perf_all[d].total_cycles;
        }

        free(counts_all);
        free(perf_all);

        // Update cycle totals
        kernel_cycles_total += batch_wall_cycles;
        if (batch_wall_cycles > kernel_cycles_peak)
            kernel_cycles_peak = batch_wall_cycles;

        clock_gettime(CLOCK_MONOTONIC, &t6); // end
        batch_time = sec_since(t5, t6);

        if (DPU_FREQ_HZ > 0.0)
        {
            double batch_kernel_s = (double)batch_wall_cycles / DPU_FREQ_HZ;
            printf("\nBatch %d completes. Transfer wall time: %.2f s |Transfer: %.3f s (cumu %.3f s)| Kernel(DPU cycles) wall time: %.6f s | Host kernel wall time: %.6f s |result_retrieval=%.6f s\n",
                   batch_no, batch_time, transfer_this_batch, transfer_cumu, batch_kernel_s, sec_since(k0, k1), result_retrieval_this_batch);
        }
        else
        {
            printf("\nBatch %d completes. Transfer wall time: %.2f s | Kernel(DPU cycles) wall cycles: %" PRIu64 " | Host kernel wall time: %.6f s\n",
                   batch_no, batch_time, batch_wall_cycles, sec_since(k0, k1));
        }

        batch_no++;
    }
    // After the loop, optional totals:
    if (DPU_FREQ_HZ > 0.0)
    {
        double total_kernel_s = (double)kernel_cycles_total / DPU_FREQ_HZ;
        double peak_kernel_s = (double)kernel_cycles_peak / DPU_FREQ_HZ;
        printf("\nTotal kernel wall time from DPU cycles (sum of batch walls): %.6f s", total_kernel_s);
        printf("\nPeak single-batch kernel wall time from DPU cycles: %.6f s", peak_kernel_s);
    }
    printf("\nTotal host-measured kernel wall time (sum over batches): %.6f s\n", kernel_time_total);

    clock_gettime(CLOCK_MONOTONIC, &t7); // end of batch

    for (int d = 0; d < actual_dpus_needed; d++)
    {
        free(dpu_subtrees[d]);
    }
    free(dpu_subtrees);
    free(dpu_low_nodes_counts);

    int mismatch_count = 0;
    int total_difference = 0;

    for (int i = 0; i < numQuery; i++)
    {
        if (cpu_overlap_count[i] != aggregated_dpu_overlap[i])
        {
            // printf("❌ Query %d: CPU = %lu, DPU = %lu\n",
            //        i, cpu_overlap_count[i], aggregated_dpu_overlap[i]);
            mismatch_count++;                                                                              // Count how many queries differ
            total_difference += llabs((int64_t)cpu_overlap_count[i] - (int64_t)aggregated_dpu_overlap[i]); // Optional: sum of errors
        }
    }
    if (mismatch_count == 0)
        LOGF("✅ All queries matched between CPU and DPU.\n");
    else
    {
        printf("Total mismatched queries: %d\n", mismatch_count);
        printf("Sum of overlap differences: %d\n", total_difference);
    }
    printf("\nOverall DPU Overlap count= %d", Overall_DPU_overlap_c);
    LOGF("\nPer-DPU overlap totals (computed on host from returned sparse pairs):\n");
    for (uint32_t d = 0; d < (uint32_t)actual_dpus_needed; d++)
    {
        LOGF("  DPU[%u] overlaps=%" PRIu64 "\n", d, per_dpu_overlap_total[d]);
    }

    printRTreeStats(root);
    LOGF("\n\n\t\tSUMMARY\n");
    LOGF("DPU(s): %d\tBUNDLEFACTOR: %d\tTasklet(s): %d\n", nr_of_dpus, BUNDLEFACTOR, NR_TASKLETS);
    LOGF("Dataset size: %d\tQuery size: %d\n", numRects, numQuery);
    LOGF("CPU R-tree construction time: %.2f s\t", rtree_construction_time);
    LOGF("Search Time in HOST: %.2f s\n", search_time);
    LOGF("\nTotal Time In CPU (construction+search): %.2f s\n", (rtree_construction_time + search_time));
    LOGF("\nRtree transfer time (Broadcast upperlayer): %.2f s", tree_transfer_time);
    LOGF("\nKernel-only time (sum over batches): %.2f s\n", kernel_time_total);
    if (search_time > kernel_time_total)
        LOGF("\nKernel Speedup (Host/Kernel)=%.2f", search_time / kernel_time_total);

    double total_dpu_time = sec_since(t4, t7);
    LOGF("\nTotal DPU Time (Communication+search): %.2f s\n\n", total_dpu_time);
    if (total_dpu_time > 0)
        LOGF("\nend-to-end Speedup (Host/DPU ratio): %.2f\n", search_time / total_dpu_time);
    else
        LOGF("Host/DPU ratio: undefined (DPU time is 0)\n");

    if (total_dpu_time < search_time)
        LOGF("DPU is %.2f times Faster\n\n", search_time / total_dpu_time);
    else
        LOGF("DPU is Slower\n\n");

    if (found_count == Overall_DPU_overlap_c)
        LOGF("\n                       Matched!!!!    \n Overlaps found by both DPU and CPU : %d\n\n", found_count);
    else
    {
        LOGF("\n                       ERROR!!!       \n Overlaps found by CPU %d", found_count);
        LOGF("\nOverlaps found by DPU %d\n", Overall_DPU_overlap_c);
    }
    free(per_dpu_overlap_total);
    free(aggregated_dpu_overlap);
    free(cpu_overlap_count);
    DPU_ASSERT(dpu_free(dpu_set));

    fclose(log_file);
    return status ? 0 : -1;
}


//readelf -S build/dpu | grep -i mram