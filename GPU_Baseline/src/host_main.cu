// src/host_main.cu
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <ctime>
#include <filesystem>

#include <cuda_runtime.h>
#include "rtree_gpu.cuh"

#define THREADS_PER_BLOCK 512

extern "C"
{
#include "rtreefunction.h"
}

static inline double sec_since(std::chrono::high_resolution_clock::time_point a,
                               std::chrono::high_resolution_clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

static inline std::string current_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};

#if defined(_WIN32)
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(stamp);
}

// safe index kernel
__global__ void add(int *a, int *b, int *c, int n)
{
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < n)
        c[index] = a[index] + b[index];
}

// simple CUDA error macro
#define CUDA_OK(cmd)                                                                                \
    do                                                                                              \
    {                                                                                               \
        cudaError_t e = (cmd);                                                                      \
        if (e != cudaSuccess)                                                                       \
        {                                                                                           \
            fprintf(stderr, "CUDA error %s at %s:%d\n", cudaGetErrorString(e), __FILE__, __LINE__); \
            std::exit(EXIT_FAILURE);                                                                \
        }                                                                                           \
    } while (0)

int main(int argc, char **argv)
{
    // ---- interactive dataset selection ----
    int numRects = 0, numQuery = 0, dataset_option = 0;

    std::printf("\nHow many data you want to work with? Choose option: \n"
                "\t1. 16M\n\t2. parks(9.9M )\n"
                "\t3. Sports(999k)\n\t4. Lakes(8M)\n\t5. Buildings(57M)\n");
    std::printf("\nEnter your option: ");

    if (std::scanf(" %d", &dataset_option) != 1)
    {
        std::fprintf(stderr, "Invalid input. Exiting.\n");
        return EXIT_FAILURE;
    }

    // ---- create dataset-specific log folder and file ----
    const char *log_dirs[] = {"log/16M", "log/parks_9.9M", "log/Sports_999k", "log/Lakes_8M", "log/Buildings_57M"};
    const char *dataset_names[] = {"16M", "parks_9.9M", "Sports_999k", "Lakes_8M", "Buildings_57M"};
    
    if (dataset_option < 1 || dataset_option > 5)
    {
        std::fprintf(stderr, "Invalid option. Exiting.\n");
        return EXIT_FAILURE;
    }

    char log_filename[256] = {0};
    std::filesystem::path project_root = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::filesystem::path log_dir = project_root / log_dirs[dataset_option - 1];
    std::filesystem::path log_path = log_dir / (std::string("rtree_run_") + dataset_names[dataset_option - 1] + ".log");

    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec)
    {
        std::fprintf(stderr, "Failed to create log directory: %s\n", ec.message().c_str());
        return EXIT_FAILURE;
    }

    FILE *log_file = std::fopen(log_path.string().c_str(), "a");
    if (!log_file)
    {
        perror("Failed to open log file");
        return EXIT_FAILURE;
    }

    std::strncpy(log_filename, log_path.string().c_str(), sizeof(log_filename) - 1);
    const std::string run_stamp = current_timestamp();

    std::fprintf(log_file, "\n===== Run started at %s =====\n", run_stamp.c_str());
    log_filename[sizeof(log_filename) - 1] = '\0';

    auto t_load0 = std::chrono::high_resolution_clock::now();
    Rect *rects = selectDataDataset(&numRects, dataset_option);
    auto t_load1 = std::chrono::high_resolution_clock::now();

    if (!rects)
    {
        std::fprintf(stderr, "Failed to read rectangles.\n");
        std::fclose(log_file);
        return EXIT_FAILURE;
    }
    std::printf("Read %d rects successfully.\n", numRects);
    std::printf("Total dataset size: %.2f MB\n", (numRects * sizeof(Rect)) / (1024.0 * 1024.0));
    std::printf("Data load time: %.3f s\n", sec_since(t_load0, t_load1));
    
    std::fprintf(log_file, "Dataset: %s\n", dataset_names[dataset_option - 1]);
    std::fprintf(log_file, "Run timestamp: %s\n", run_stamp.c_str());
    std::fprintf(log_file, "Read %d rects successfully.\n", numRects);
    std::fprintf(log_file, "Total dataset size: %.2f MB\n", (numRects * sizeof(Rect)) / (1024.0 * 1024.0));
    std::fprintf(log_file, "Data load time: %.3f s\n", sec_since(t_load0, t_load1));

    // ---- build R-tree on CPU (STR) ----
    auto t_build0 = std::chrono::high_resolution_clock::now();
    Node *root = createRTree_STR(rects, 0, numRects - 1);
    auto t_build1 = std::chrono::high_resolution_clock::now();

    if (!root)
    {
        std::fprintf(stderr, "createRTree_STR failed.\n");
        std::free(rects);
        std::fclose(log_file);
        return EXIT_FAILURE;
    }

    std::printf("CPU R-tree build: %.3f s\n", sec_since(t_build0, t_build1));
    std::fprintf(log_file, "CPU R-tree build: %.3f s\n", sec_since(t_build0, t_build1));
    printRTreeStats(root);
    std::fprintf(log_file, "\n=== R-Tree Stats ===\n");

    FlatRTree F = {0};
    if (flatten_rtree_bfs(root, &F) != 0)
    {
        fprintf(stderr, "ERROR: flatten failed (see diagnostics above)\n");
        return 1;
    }
   // print_flat_rtree(&F, 8, 8);

    std::printf("Flatten done\n");

    // ---- load queries for the same dataset family ----
    auto t_qload0 = std::chrono::high_resolution_clock::now();
    Rect *qrects = selectQueryDataset(&numQuery, dataset_option);
    auto t_qload1 = std::chrono::high_resolution_clock::now();

    if (!qrects || numQuery <= 0)
    {
        std::fprintf(stderr, "Failed to read queries.\n");
        return EXIT_FAILURE;
    }
    std::printf("Loaded %d query rects. Query load time: %.3f s\n",
                numQuery, sec_since(t_qload0, t_qload1));
    std::fprintf(log_file, "Loaded %d query rects. Query load time: %.3f s\n",
                numQuery, sec_since(t_qload0, t_qload1));

    // ---- CPU total overlap count over all queries ----
    auto tc0 = std::chrono::high_resolution_clock::now();

    unsigned long long cpu_total = 0ULL;
    for (int i = 0; i < numQuery; ++i)
    {
        cpu_total += (unsigned long long)flat_search_count(&F, qrects[i]);
    }

    auto tc1 = std::chrono::high_resolution_clock::now();
    std::printf("\nCPU total overlaps: %llu  (time: %.3f s)\n",
                cpu_total, sec_since(tc0, tc1));
    std::fprintf(log_file, "\nCPU total overlaps: %llu  (time: %.3f s)\n",
                cpu_total, sec_since(tc0, tc1));

    // guard for visible device
    int devs = 0;
    if (cudaGetDeviceCount(&devs) != cudaSuccess || devs == 0)
    {
        std::fprintf(stderr, "No CUDA device visible. Skipping GPU search.\n");
    }
    else
    {
        CUDA_OK(cudaSetDevice(0));
        cudaDeviceProp prop{};
        CUDA_OK(cudaGetDeviceProperties(&prop, 0));
        std::printf("CUDA Device Property:\n  Using GPU 0: %s, global mem %.1f GB, SMs %d\n",
                    prop.name, prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0),
                    prop.multiProcessorCount);
        printf("   maxThreadsPerBlock %d\n", prop.maxThreadsPerBlock);
        printf("   maxThreadsPerMultiProcessor %d\n", prop.maxThreadsPerMultiProcessor);
        printf("   maxBlocksPerMultiProcessor %d\n", prop.maxBlocksPerMultiProcessor);
        printf("   maxGridSize %d %d %d\n", prop.maxGridSize[0], prop.maxGridSize[1], prop.maxGridSize[2]);
        printf("   maxThreadsDim %d %d %d\n", prop.maxThreadsDim[0], prop.maxThreadsDim[1], prop.maxThreadsDim[2]);

        // make device copy of flat tree
        DevFlatRTree dF{};
        CUDA_OK(make_device_flat(&F, &dF));

        // debug
        //  ---- device-side sanity for one leaf under node 1 ----
        int h_off1 = 0, h_cnt1 = 0;
        CUDA_OK(cudaMemcpy(&h_off1, dF.child_off + 1, sizeof(int), cudaMemcpyDeviceToHost));
        CUDA_OK(cudaMemcpy(&h_cnt1, dF.child_cnt + 1, sizeof(int), cudaMemcpyDeviceToHost));
        printf("  DBG node1: child_off=%d child_cnt=%d\n", h_off1, h_cnt1);

        // fetch first child id of node 1 (should be ~73)
        int leaf_id = -1;
        if (h_off1 >= 0 && h_cnt1 > 0)
        {
            CUDA_OK(cudaMemcpy(&leaf_id, dF.child_index + h_off1, sizeof(int), cudaMemcpyDeviceToHost));
        }
        printf("  DBG node1 first child id = %d\n", leaf_id);

        // read leaf flags and rect slice
        int is_leaf = -1, first_rect = -2, rect_cnt = -3;
        if (leaf_id >= 0)
        {
            CUDA_OK(cudaMemcpy(&is_leaf, dF.is_leaf + leaf_id, sizeof(int), cudaMemcpyDeviceToHost));
            CUDA_OK(cudaMemcpy(&first_rect, dF.first_rect + leaf_id, sizeof(int), cudaMemcpyDeviceToHost));
            CUDA_OK(cudaMemcpy(&rect_cnt, dF.rect_cnt + leaf_id, sizeof(int), cudaMemcpyDeviceToHost));
        }
        printf("  DBG leaf node %d: is_leaf=%d first_rect=%d rect_cnt=%d (rect_pool_len=%zu)\n",
               leaf_id, is_leaf, first_rect, rect_cnt, dF.rect_pool_len);

        // peek first one or two rects from device and from host to compare
        if (is_leaf == 1 && first_rect >= 0 && rect_cnt > 0 &&
            (size_t)first_rect + 1 <= dF.rect_pool_len)
        {
            int d_rxmin0 = 0, d_rymin0 = 0, d_rxmax0 = 0, d_rymax0 = 0;
            CUDA_OK(cudaMemcpy(&d_rxmin0, dF.rxmin + first_rect, sizeof(int), cudaMemcpyDeviceToHost));
            CUDA_OK(cudaMemcpy(&d_rymin0, dF.rymin + first_rect, sizeof(int), cudaMemcpyDeviceToHost));
            CUDA_OK(cudaMemcpy(&d_rxmax0, dF.rxmax + first_rect, sizeof(int), cudaMemcpyDeviceToHost));
            CUDA_OK(cudaMemcpy(&d_rymax0, dF.rymax + first_rect, sizeof(int), cudaMemcpyDeviceToHost));

            printf("  DBG DEV rect[first]: [%d %d %d %d]\n", d_rxmin0, d_rymin0, d_rxmax0, d_rymax0);
            printf("  DBG CPU rect[first]: [%d %d %d %d]\n",
                   F.rxmin[first_rect], F.rymin[first_rect], F.rxmax[first_rect], F.rymax[first_rect]);
        }
        //

        // copy queries and allocate result
        Rect *d_queries = nullptr;
        int *d_counts = nullptr;
        CUDA_OK(cudaMalloc((void **)&d_queries, numQuery * sizeof(Rect)));
        CUDA_OK(cudaMemcpy(d_queries, qrects, numQuery * sizeof(Rect), cudaMemcpyHostToDevice));
        CUDA_OK(cudaMalloc((void **)&d_counts, numQuery * sizeof(int)));
        CUDA_OK(cudaMemset(d_counts, 0, numQuery * sizeof(int)));

        // launch
        int tpb = THREADS_PER_BLOCK;
        auto tg0 = std::chrono::high_resolution_clock::now();
        rtree_gpu_search(&dF, d_queries, numQuery, d_counts, tpb);

        CUDA_OK(cudaDeviceSynchronize());
        auto tg1 = std::chrono::high_resolution_clock::now();

        // get results
        std::vector<int> counts(numQuery);
        CUDA_OK(cudaMemcpy(counts.data(), d_counts, numQuery * sizeof(int), cudaMemcpyDeviceToHost));

        // ---- GPU total ----
        unsigned long long gpu_total = 0ULL;
        for (int i = 0; i < numQuery; ++i)
            gpu_total += (unsigned long long)counts[i];

        // ---- prints ----
        std::printf("GPU search time: %.3f s\n", sec_since(tg0, tg1));
        std::fprintf(log_file, "GPU search time: %.3f s\n", sec_since(tg0, tg1));

        // // optional: show first few per-query results
        // for (int i = 0; i < 5 && i < numQuery; ++i)
        // {
        //     std::printf("GPU query %d -> %d overlaps\n", i, counts[i]);
        // }

        // optional: consistency check against CPU total (only if you computed cpu_total above)
        if (gpu_total != cpu_total)
        {
            std::printf("WARNING: totals differ! CPU=%llu GPU=%llu\n", cpu_total, gpu_total);
            std::fprintf(log_file, "WARNING: totals differ! CPU=%llu GPU=%llu\n", cpu_total, gpu_total);
        }
        else
        {
            std::printf("\n\nMATCHED!!!CPU=%llu GPU=%llu\n\n", cpu_total, gpu_total);
            std::fprintf(log_file, "\n\nMATCHED!!!CPU=%llu GPU=%llu\n\n", cpu_total, gpu_total);
        }
        if (sec_since(tg0, tg1) < sec_since(tc0, tc1))
        {
            std::printf("GPU is %.3f times faster\n\n", (sec_since(tc0, tc1)) / (sec_since(tg0, tg1)));
            std::fprintf(log_file, "GPU is %.3f times faster\n\n", (sec_since(tc0, tc1)) / (sec_since(tg0, tg1)));
        }

        // clean up
        cudaFree(d_queries);
        cudaFree(d_counts);
        free_device_flat(&dF);
    }
    
    std::fclose(log_file);
    std::printf("\nLog saved to: %s\n", log_filename);
}
