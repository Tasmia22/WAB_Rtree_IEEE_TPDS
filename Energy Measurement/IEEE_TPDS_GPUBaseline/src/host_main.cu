// src/host_main.cu
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <ctime>
#include <filesystem>
#include <cerrno>
#include <climits>

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

#ifndef RTREE_ENERGY_MEASUREMENT
static int get_repeat_count()
{
    const char *env = std::getenv("REPEAT_COUNT");
    if (env == nullptr || env[0] == '\0')
        return 5;

    char *endp = nullptr;
    errno = 0;
    long parsed = std::strtol(env, &endp, 10);
    if (errno == ERANGE || endp == env || *endp != '\0' || parsed <= 0 || parsed > INT_MAX)
    {
        std::fprintf(stderr, "Invalid REPEAT_COUNT=%s, falling back to 5.\n", env);
        return 5;
    }
    return static_cast<int>(parsed);
}
#endif

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
#ifdef RTREE_ENERGY_MEASUREMENT
    long measured_runs = 1000;
    long warmup_runs = 5;

    if (argc > 1)
        measured_runs = std::strtol(argv[1], nullptr, 10);
    if (argc > 2)
        warmup_runs = std::strtol(argv[2], nullptr, 10);
    if (measured_runs <= 0 || warmup_runs < 0)
    {
        std::fprintf(stderr, "Usage: %s [positive-measured-runs] [nonnegative-warmup-runs]\n", argv[0]);
        return EXIT_FAILURE;
    }

    std::printf("\nPN150 wall-energy mode: %ld measured runs after %ld warm-up runs.\n",
                measured_runs, warmup_runs);
#endif

    // ---- interactive dataset selection ----
    int numRects = 0, numQuery = 0, dataset_option = 0;


    std::printf("\nWhich dataset do you want to work with? Choose option:\n"
                "\t1. Sports\n"
                "\t2. Lakes\n"
                "\t3. Parks\n"
                "\t4. Synthetic\n"
                "\t5. Buildings\n");

    std::printf("\nEnter your option: ");

    if (std::scanf(" %d", &dataset_option) != 1)
    {
        std::fprintf(stderr, "Invalid input. Exiting.\n");
        return EXIT_FAILURE;
    }


    // ---- create dataset-specific log folder and file ----
    const char *log_dirs[] = {"GPU_Baseline_log/Sports_999k", "GPU_Baseline_log/Lakes_8M", "GPU_Baseline_log/parks_9.9M", "GPU_Baseline_log/16M", "GPU_Baseline_log/Buildings_57M"};
    const char *dataset_names[] = {"Sports_999k", "Lakes_8M", "parks_9.9M", "16M", "Buildings_57M"};
    
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
        auto t_tree_h2d0 = std::chrono::high_resolution_clock::now();
        CUDA_OK(make_device_flat(&F, &dF));
        auto t_tree_h2d1 = std::chrono::high_resolution_clock::now();
        const double tree_h2d_seconds = sec_since(t_tree_h2d0, t_tree_h2d1);

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
        auto t_query_h2d0 = std::chrono::high_resolution_clock::now();
        CUDA_OK(cudaMalloc((void **)&d_queries, numQuery * sizeof(Rect)));
        CUDA_OK(cudaMemcpy(d_queries, qrects, numQuery * sizeof(Rect), cudaMemcpyHostToDevice));
        auto t_query_h2d1 = std::chrono::high_resolution_clock::now();
        const double query_h2d_seconds = sec_since(t_query_h2d0, t_query_h2d1);

        auto t_init0 = std::chrono::high_resolution_clock::now();
        CUDA_OK(cudaMalloc((void **)&d_counts, numQuery * sizeof(int)));
        CUDA_OK(cudaMemset(d_counts, 0, numQuery * sizeof(int)));
        // cudaMemset is asynchronous; synchronize so T_init includes its execution.
        CUDA_OK(cudaDeviceSynchronize());
        auto t_init1 = std::chrono::high_resolution_clock::now();
        const double init_seconds = sec_since(t_init0, t_init1);

        // launch
        int tpb = THREADS_PER_BLOCK;
#ifdef RTREE_ENERGY_MEASUREMENT
        std::printf("Warming up the GPU with %ld search runs...\n", warmup_runs);
        for (long run = 0; run < warmup_runs; ++run)
            rtree_gpu_search(&dF, d_queries, numQuery, d_counts, tpb);
        CUDA_OK(cudaDeviceSynchronize());

        // Consume the newline left by scanf, then wait until the meter is ready.
        int input_char = 0;
        while ((input_char = std::getchar()) != '\n' && input_char != EOF)
        {
        }
        std::printf("\nGPU warm-up complete. Reset the PN150 accumulated energy (EC),\n"
                    "record its starting value, then press Enter to launch %ld searches...\n",
                    measured_runs);
        std::fflush(stdout);
        (void)std::getchar();

        for (int seconds = 5; seconds >= 1; --seconds)
        {
            std::printf("%d%s", seconds, seconds == 1 ? "\n" : " ");
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#endif

#ifndef RTREE_ENERGY_MEASUREMENT
        const int repeat_count = get_repeat_count();
        double repeat_search_total_s = 0.0;
        for (int rep = 0; rep < repeat_count; ++rep)
        {
            std::printf("\nGPU search timings (rep %d/%d):\n", rep + 1, repeat_count);
            std::fprintf(log_file, "\nGPU search timings (rep %d/%d):\n", rep + 1, repeat_count);
#endif
        auto tg0 = std::chrono::high_resolution_clock::now();
#ifdef RTREE_ENERGY_MEASUREMENT
        for (long run = 0; run < measured_runs; ++run)
            rtree_gpu_search(&dF, d_queries, numQuery, d_counts, tpb);
#else
        rtree_gpu_search(&dF, d_queries, numQuery, d_counts, tpb);
#endif

        CUDA_OK(cudaDeviceSynchronize());
        auto tg1 = std::chrono::high_resolution_clock::now();
#ifdef RTREE_ENERGY_MEASUREMENT
        std::printf("END\n");
        std::fflush(stdout);
#endif
        const double gpu_measurement_seconds = sec_since(tg0, tg1);
#ifdef RTREE_ENERGY_MEASUREMENT
        const double gpu_search_seconds = gpu_measurement_seconds / (double)measured_runs;
#else
        const double gpu_search_seconds = gpu_measurement_seconds;
        repeat_search_total_s += gpu_search_seconds;
#endif

        // Copy the per-query overlap counts back to the host.
        std::vector<int> counts(numQuery);
        auto t_result_d2h0 = std::chrono::high_resolution_clock::now();
        CUDA_OK(cudaMemcpy(counts.data(), d_counts, numQuery * sizeof(int), cudaMemcpyDeviceToHost));
        auto t_result_d2h1 = std::chrono::high_resolution_clock::now();
        const double result_d2h_seconds = sec_since(t_result_d2h0, t_result_d2h1);

        // Aggregate per-query results into the same total used by the CPU path.
        auto t_host_result0 = std::chrono::high_resolution_clock::now();
        unsigned long long gpu_total = 0ULL;
        for (int i = 0; i < numQuery; ++i)
            gpu_total += (unsigned long long)counts[i];
        auto t_host_result1 = std::chrono::high_resolution_clock::now();
        const double host_result_seconds = sec_since(t_host_result0, t_host_result1);

        // ---- prints ----
#ifdef RTREE_ENERGY_MEASUREMENT
        std::printf("\nENERGY MEASUREMENT COMPLETE -- record the PN150 EC value now.\n");
        std::printf("Measured runs: %ld\n", measured_runs);
        std::printf("Queries per run: %d\n", numQuery);
        std::printf("Total measured GPU time: %.6f s\n", gpu_measurement_seconds);
        std::printf("Average GPU search time: %.9f s/run\n", gpu_search_seconds);
        std::printf("Total query executions: %llu\n",
                    (unsigned long long)measured_runs * (unsigned long long)numQuery);
        std::fprintf(log_file,
                     "GPU energy mode: runs=%ld warmups=%ld total_time=%.6f s "
                     "average_time=%.9f s queries_per_run=%d\n",
                     measured_runs, warmup_runs, gpu_measurement_seconds,
                     gpu_search_seconds, numQuery);
#else
        const double gpu_total_seconds = tree_h2d_seconds + query_h2d_seconds +
                                         init_seconds + gpu_search_seconds +
                                         result_d2h_seconds + host_result_seconds;
        std::printf("T_tree,H2D (flattened index distribution): %.6f s\n", tree_h2d_seconds);
        std::printf("T_query,H2D: %.6f s\n", query_h2d_seconds);
        std::printf("T_init (result-buffer allocation + clear): %.6f s\n", init_seconds);
        std::printf("T_GPU,kernel (launch + GPU search + synchronization): %.3f s\n", gpu_search_seconds);
        std::printf("T_result,D2H: %.6f s\n", result_d2h_seconds);
        std::printf("T_host,result (aggregate per-query counts): %.6f s\n", host_result_seconds);
        std::printf("T_GPU,total: %.6f s\n", gpu_total_seconds);
        std::fprintf(log_file, "T_tree,H2D (flattened index distribution): %.6f s\n", tree_h2d_seconds);
        std::fprintf(log_file, "T_query,H2D: %.6f s\n", query_h2d_seconds);
        std::fprintf(log_file, "T_init (result-buffer allocation + clear): %.6f s\n", init_seconds);
        std::fprintf(log_file, "T_GPU,kernel (launch + GPU search + synchronization): %.3f s\n", gpu_search_seconds);
        std::fprintf(log_file, "T_result,D2H: %.6f s\n", result_d2h_seconds);
        std::fprintf(log_file, "T_host,result (aggregate per-query counts): %.6f s\n", host_result_seconds);
        std::fprintf(log_file, "T_GPU,total: %.6f s\n", gpu_total_seconds);
#endif

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
        if (gpu_search_seconds < sec_since(tc0, tc1))
        {
            std::printf("GPU is %.3f times faster\n\n", sec_since(tc0, tc1) / gpu_search_seconds);
            std::fprintf(log_file, "GPU is %.3f times faster\n\n", sec_since(tc0, tc1) / gpu_search_seconds);
        }

#ifndef RTREE_ENERGY_MEASUREMENT
        }
        std::printf("[Repeat Summary] GPU search time over %d repetitions: %.6f s (avg %.9f s)\n",
                    repeat_count, repeat_search_total_s, repeat_search_total_s / repeat_count);
        std::fprintf(log_file, "[Repeat Summary] GPU search time over %d repetitions: %.6f s (avg %.9f s)\n",
                     repeat_count, repeat_search_total_s, repeat_search_total_s / repeat_count);
#endif

        // clean up
        cudaFree(d_queries);
        cudaFree(d_counts);
        free_device_flat(&dF);
    }
    
    std::fclose(log_file);
    std::printf("\nLog saved to: %s\n", log_filename);
}
