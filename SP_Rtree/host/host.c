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
int main()
{
    struct dpu_set_t dpu_set, dpu;
    uint32_t nr_of_dpus;
    bool status = true;
    struct timespec t0, t1, t2, t3, t4, t5, t6, t7, t8;
    time_t raw_time = time(NULL);
    struct tm *tm_info = localtime(&raw_time);
    char log_filename[64];
    strftime(log_filename, sizeof(log_filename), "logs/dpu_%Y%m%d_%H:%M.log", tm_info);
    FILE *log_file = fopen(log_filename, "w");
    double kernel_time_total = 0.0;

    if (!log_file)
    {
        perror("Failed to open log file");
        return EXIT_FAILURE;
    }

    int numRects, numQuery, dataset_option;
    printf("\nHow many data you want to work with? Choose option: \n\t1. 6M\n\t2. Synthetic Data (16M)\n\t3. Sports(999k)\n\t4. parks(300k)\n\t5. cemetery(168k)\n\t6. Lakes(8.4M)");
    printf("\nEnter your option: ");

    if (scanf(" %d", &dataset_option) != 1)
    {
        fprintf(stderr, "Invalid input\n");
        exit(EXIT_FAILURE);
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
    Node *root = createRTree(rects, 0, numRects - 1);
   // Node *root = createRTree_STR(rects, 0, numRects - 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double rtree_construction_time = sec_since(t0, t1);
    printRTreeStats(root);
    // printRTree(root, 0);
    int rtree_mem_size = get_rtree_memory_size(root);
    printf("\nActual R-tree memory size: %.2f MB\n", rtree_mem_size / (1024.0 * 1024.0));
    Rect *query_rects = selectQueryDataset(&numQuery, dataset_option);

    Zsorting(query_rects, numQuery);
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
    printf("[CPU-ONLY] Overlaps=%llu and Searching Time=%.2f s\n", (unsigned long long)found_count, search_time);

    clock_gettime(CLOCK_MONOTONIC, &t4); // Start

    DPU_ASSERT(dpu_alloc(NR_DPUS, NULL, &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY, NULL));
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_of_dpus));
    printf("\n\nAllocated %d DPU(s)\n", nr_of_dpus);

    printf("\nPreparing Subtree..\n");
    //  int start_index = 1;
    MBR subtree_mbrs[nr_of_dpus];
    Node *subtree[nr_of_dpus];
    SerializedNode *serialized_trees[nr_of_dpus];
    uint64_t dpu_ids[nr_of_dpus];
    int num_nodes_eachsubtree[nr_of_dpus];

    int max_serialized_size = prepare_serialized_subtrees(root, &nr_of_dpus, subtree, serialized_trees, num_nodes_eachsubtree, subtree_mbrs,
                                                          /*print_first_subtree=*/true);
    if (max_serialized_size < 0)
    {
        // handle error
        return EXIT_FAILURE;
    }
    /*
        for (int i = 0; i < (int)nr_of_dpus; i++)
        {
            subtree[i] = root->children[i];
            if (i == 0)
            {
                printf("\n=== Printing subtree[0] ===\n");
                printRTree(subtree[0], 0); // Print subtree[0]
            }
            num_nodes_eachsubtree[i] = serialize_rtree_wrapper(subtree[i], &serialized_trees[i]);
            // if(i>950)
            // printf("Subtree %d has %d nodes\n", i, num_nodes_eachsubtree[i]);

            // start_index += num_nodes_eachsubtree[i];

            if (!serialized_trees[i])
            {
                fprintf(stderr, "Memory allocation failed for serialized tree %d\n", i);
                exit(EXIT_FAILURE);
            }

            if (num_nodes_eachsubtree[i] > max_serialized_size)
            {
                max_serialized_size = num_nodes_eachsubtree[i];
            }

            // Extract first MBR of each serialized tree
            MBR root_mbr = serialized_trees[i][0].mbr;
            subtree_mbrs[i] = root_mbr;
        }

        */
    // Pad all buffers to the same size
    size_t uniform_size = max_serialized_size * sizeof(SerializedNode);
    for (int i = 0; i < (int)nr_of_dpus; i++)
    {
        serialized_trees[i] = realloc(serialized_trees[i], uniform_size); // safe as long as realloc is successful
        memset((char *)serialized_trees[i] + (num_nodes_eachsubtree[i] * sizeof(SerializedNode)), 0,
               uniform_size - num_nodes_eachsubtree[i] * sizeof(SerializedNode));
        dpu_ids[i] = i;
    }

    // Print the size of each padded serialized tree
    printf("Serialized tree size for DPU %d: %zu kilo bytes (%d nodes padded to %d nodes)\n",
           0, uniform_size / 1024, num_nodes_eachsubtree[0], max_serialized_size);

    int i;
    printf("\nPreparing Subtree for Transfer...");
    DPU_FOREACH(dpu_set, dpu, i)
    {
        dpu_prepare_xfer(dpu, serialized_trees[i]);
    }
    printf("\n\nTransfering Subtrees with unique size %.2f  ...", uniform_size / (1024.0 * 1024.0));
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_TREE", 0, uniform_size, DPU_XFER_DEFAULT));
    printf("\nTree transfer complete");

    // Transfer dpu_idsdpu_ids in parallel
    DPU_FOREACH(dpu_set, dpu, i)
    {
        dpu_prepare_xfer(dpu, &dpu_ids[i]);
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INDEX", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));

    clock_gettime(CLOCK_MONOTONIC, &t5); // Tree Transfer complete

    double tree_transfer_time = sec_since(t4, t5);

    // ------------------- QUERY -------------------

    uint64_t *aggregated_dpu_overlap = calloc(numQuery, sizeof(uint64_t));
    int Overall_DPU_overlap_c = 0;
    uint64_t query_num = MAX_QUERY;
    int batch_no = 0;
    clock_gettime(CLOCK_MONOTONIC, &t6);
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
        // printf("\nBatch %d and query_num=%lu Current BatchSize=%d", batch_no, query_num, current_batch_size);
        DPU_ASSERT(dpu_broadcast_to(dpu_set, "QUERY_NUM", 0, &query_num, sizeof(uint64_t), DPU_XFER_DEFAULT));
        // Broadcast queries
        DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_QUERY_RECT", 0, &query_rects[offset], current_batch_size * sizeof(Rect), DPU_XFER_DEFAULT));
        printf("\nBatch %d Query has been broadcasted", batch_no);
        // Launch
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

        // Retrieve and aggregate this batch
        DPU_FOREACH(dpu_set, dpu)
        {
            uint64_t *temp_result = malloc(current_batch_size * sizeof(uint64_t));
            DPU_ASSERT(dpu_copy_from(dpu, "DPU_OVERLAP_COUNT", 0, temp_result, current_batch_size * sizeof(uint64_t)));

            for (int i = 0; i < current_batch_size; i++)
            {
                aggregated_dpu_overlap[offset + i] += temp_result[i];
                Overall_DPU_overlap_c += temp_result[i];
            }

            free(temp_result);
        }
        printf("\nBatch %d Results have been Retrieved", batch_no);
        clock_gettime(CLOCK_MONOTONIC, &t7);
        double batch_time = sec_since(t6, t7);
        printf("\nBatch %d tranfer time %.2f\n", batch_no, batch_time);
        batch_no++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t8);

    int mismatch_count = 0;
    int total_difference = 0;

    for (int i = 0; i < numQuery; i++)
    {
        if (cpu_overlap_count[i] != aggregated_dpu_overlap[i])
        {
            printf("❌ Query %d: CPU = %lu, DPU = %lu\n",
                   i, cpu_overlap_count[i], aggregated_dpu_overlap[i]);
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

    printRTreeStats(root);
    LOGF("\n\n\t\t\tSUMMARY\n");
    LOGF("DPU(s): %d\tBUNDLEFACTOR: %d\tTasklet(s): %d\n", nr_of_dpus, BUNDLEFACTOR, NR_TASKLETS);
    LOGF("Dataset size: %d\tQuery size: %d\n", numRects, numQuery);
    LOGF("CPU R-tree construction time: %.2f s\t", rtree_construction_time);
    LOGF("Search Time in HOST: %.2f s\n", search_time);
    LOGF("Total Time In CPU (construction+search): %.2f s\n\n", (rtree_construction_time + search_time));

    LOGF("\nRtree transfer time (Subtree): %.2f s\n", tree_transfer_time);
    LOGF("\nKernel-only time (sum over batches): %.2f s\n", kernel_time_total);
    double total_dpu_time = sec_since(t4, t8);
    LOGF("\nTotal DPU Time: %.2f s\n\n", total_dpu_time);

    if (total_dpu_time > 0)
        LOGF("Host/DPU ratio: %.2f\n", search_time / total_dpu_time);
    else
        LOGF("Host/DPU ratio: undefined (DPU time is 0)\n");

    if (total_dpu_time < search_time)
        LOGF("DPU is %.2f times Faster\n\n", search_time / total_dpu_time);
    else
        LOGF("DPU is Slower\n\n");
    if (found_count == Overall_DPU_overlap_c)
        LOGF("\n✅ All queries matched between CPU and DPU. Number of Overlaps found by CPU and DPU %d\n", found_count);
    // LOGF("\nOverlaps found by DPU %d\n", Overall_DPU_overlap_c);
    DPU_ASSERT(dpu_free(dpu_set));

    fclose(log_file);
    return status ? 0 : -1;
}
