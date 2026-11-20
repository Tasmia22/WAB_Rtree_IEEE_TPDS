#include "rtree.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <errno.h>
#include <string.h>

static void *checked_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (!ptr)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    return ptr;
}

int countRectsInFile(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        fprintf(stderr, "Unable to open file for counting '%s': %s\n", filename, strerror(errno));
        return 0;
    }

    int count = 0;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), file))
    {
        count++;
    }

    fclose(file);
    return count;
}

// Function to initialize a bounding box
Rect *readRectsFromFile(const char *filename, int *num_rects)
{
    int line_count = countRectsInFile(filename); // Each line = one rectangle
    *num_rects = line_count;

    if (*num_rects <= 0)
    {
        return NULL;
    }

    Rect *rects = checked_malloc((size_t)*num_rects * sizeof(Rect));

    FILE *file = fopen(filename, "r");
    if (!file)
    {
        fprintf(stderr, "Unable to open file '%s': %s\n", filename, strerror(errno));
        free(rects);
        return NULL;
    }

    int x1, y1, x2, y2;
    for (int i = 0; i < *num_rects; i++)
    {
        if (fscanf(file, "%d,%d,%d,%d", &x1, &y1, &x2, &y2) != 4)
        {
            fprintf(stderr, "Error reading rectangle at line %d\n", i + 1);
            free(rects);
            fclose(file);
            return NULL;
        }

        // Normalize rectangle coordinates
        rects[i].xmin = (x1 < x2) ? x1 : x2;
        rects[i].ymin = (y1 < y2) ? y1 : y2;
        rects[i].xmax = (x1 > x2) ? x1 : x2;
        rects[i].ymax = (y1 > y2) ? y1 : y2;
    }

    fclose(file);
    return rects;
}

void initMBR(MBR *mbr)
{
    mbr->xmin = INT_MAX;
    mbr->ymin = INT_MAX;
    mbr->xmax = INT_MIN;
    mbr->ymax = INT_MIN;
}

// Function to update the MBR with a point
void updateMBRWithRect(MBR *mbr, Rect r)
{
    if (r.xmin < mbr->xmin)
        mbr->xmin = r.xmin;
    if (r.ymin < mbr->ymin)
        mbr->ymin = r.ymin;
    if (r.xmax > mbr->xmax)
        mbr->xmax = r.xmax;
    if (r.ymax > mbr->ymax)
        mbr->ymax = r.ymax;
}

// Function to compute the union of two MBRs
MBR unionJoin(MBR *mbr1, MBR *mbr2)
{
    MBR result;
    result.xmin = (mbr1->xmin < mbr2->xmin) ? mbr1->xmin : mbr2->xmin;
    result.ymin = (mbr1->ymin < mbr2->ymin) ? mbr1->ymin : mbr2->ymin;
    result.xmax = (mbr1->xmax > mbr2->xmax) ? mbr1->xmax : mbr2->xmax;
    result.ymax = (mbr1->ymax > mbr2->ymax) ? mbr1->ymax : mbr2->ymax;
    return result;
}
int compareByXCenter(const void *a, const void *b)
{
    const Rect *r1 = (const Rect *)a;
    const Rect *r2 = (const Rect *)b;
    int cx1 = (r1->xmin + r1->xmax) / 2;
    int cx2 = (r2->xmin + r2->xmax) / 2;
    return cx1 - cx2;
}

int compareByYCenter(const void *a, const void *b)
{
    const Rect *r1 = (const Rect *)a;
    const Rect *r2 = (const Rect *)b;
    int cy1 = (r1->ymin + r1->ymax) / 2;
    int cy2 = (r2->ymin + r2->ymax) / 2;
    return cy1 - cy2;
}

// static inline int rect_center_x(Rect r)
// {
//     return (int)(((int64_t)r.xmin + (int64_t)r.xmax) / 2);
// }

// static inline int rect_center_y(Rect r)
// {
//     return (int)(((int64_t)r.ymin + (int64_t)r.ymax) / 2);
// }

// int compareByXCenter(const void *a, const void *b)
// {
//     const Rect *r1 = (const Rect *)a;
//     const Rect *r2 = (const Rect *)b;
//     int cx1 = rect_center_x(*r1);
//     int cx2 = rect_center_x(*r2);
//     return cx1 - cx2;
// }

// int compareByYCenter(const void *a, const void *b)
// {
//     const Rect *r1 = (const Rect *)a;
//     const Rect *r2 = (const Rect *)b;
//     int cy1 = rect_center_y(*r1);
//     int cy2 = rect_center_y(*r2);
//     return cy1 - cy2;
// }

// --- helpers: compare nodes by MBR center ---
static int cmpNodeX(const void *A, const void *B)
{
    const Node *a = *(Node *const *)A;
    const Node *b = *(Node *const *)B;
    long cxa = (long)a->mbr.xmin + a->mbr.xmax;
    long cxb = (long)b->mbr.xmin + b->mbr.xmax;
    return (cxa > cxb) - (cxa < cxb);
}
static int cmpNodeY(const void *A, const void *B)
{
    const Node *a = *(Node *const *)A;
    const Node *b = *(Node *const *)B;
    long cya = (long)a->mbr.ymin + a->mbr.ymax;
    long cyb = (long)b->mbr.ymin + b->mbr.ymax;
    return (cya > cyb) - (cya < cyb);
}

// Safer leaf (ensure children=NULL)
static Node *createLeaf_safe(Rect *rectArr, int low, int high)
{
    Node *leaf = checked_malloc(sizeof(Node));
    leaf->isLeaf = 1;
    leaf->children = NULL;
    leaf->count = high - low + 1;
    leaf->rects = checked_malloc((size_t)leaf->count * sizeof(Rect));
    initMBR(&leaf->mbr);
    for (int i = low; i <= high; ++i)
    {
        leaf->rects[i - low] = rectArr[i];
        updateMBRWithRect(&leaf->mbr, rectArr[i]);
    }
    return leaf;
}

// Group an array of Node* into parents using STR at THIS level (recursive).
// cap = max children per internal node (FANOUT).
static Node *group_nodes_STR(Node **nodes, int n, int cap)
{
    if (n <= 0)
        return NULL;
    if (n == 1)
        return nodes[0]; // nothing to group
    if (n <= cap)
    { // single parent root
        Node *p = checked_malloc(sizeof(Node));
        p->isLeaf = 0;
        p->rects = NULL;
        p->count = n;
        p->children = checked_malloc((size_t)n * sizeof(Node *));
        initMBR(&p->mbr);
        for (int i = 0; i < n; ++i)
        {
            p->children[i] = nodes[i];
            p->mbr = unionJoin(&p->mbr, &nodes[i]->mbr);
        }
        return p;
    }

    // STR tiling on nodes: sort by X, slice, within slice sort by Y, then pack groups of size 'cap'.
    qsort(nodes, (size_t)n, sizeof(Node *), cmpNodeX);

    int S = (int)ceil(sqrt((double)n / cap)); // number of X-slices at this level
    if (S < 1)
        S = 1;
    int sliceSize = (n + S - 1) / S;

    // Pass A: count parents
    int parentCount = 0;
    for (int s = 0; s < S; ++s)
    {
        int sLo = s * sliceSize;
        int sHi = sLo + sliceSize;
        if (sHi > n)
            sHi = n;
        int sc = sHi - sLo;
        if (sc <= 0)
            continue;
        parentCount += (sc + cap - 1) / cap; // ceil(sc / cap)
    }

    Node **parents = checked_malloc((size_t)parentCount * sizeof(Node *));
    int pc = 0;

    // Pass B: build parents
    for (int s = 0; s < S; ++s)
    {
        int sLo = s * sliceSize;
        int sHi = sLo + sliceSize;
        if (sHi > n)
            sHi = n;
        int sc = sHi - sLo;
        if (sc <= 0)
            continue;

        qsort(nodes + sLo, (size_t)sc, sizeof(Node *), cmpNodeY);

        for (int i = sLo; i < sHi; i += cap)
        {
            int jEnd = i + cap;
            if (jEnd > sHi)
                jEnd = sHi;
            int cnt = jEnd - i;

            Node *p = checked_malloc(sizeof(Node));
            p->isLeaf = 0;
            p->rects = NULL;
            p->count = cnt;
            p->children = checked_malloc((size_t)cnt * sizeof(Node *));
            initMBR(&p->mbr);

            for (int j = 0; j < cnt; ++j)
            {
                p->children[j] = nodes[i + j];
                p->mbr = unionJoin(&p->mbr, &nodes[i + j]->mbr);
            }
            parents[pc++] = p;
        }
    }

    // Recurse upward
    Node *root = group_nodes_STR(parents, pc, cap);
    free(parents);
    return root;
}

// Fully recursive STR bulk loader (leaves + all upper levels use STR tiling).
Node *createRTree_STR(Rect *rectArr, int low, int high)
{
    int total = high - low + 1;
    if (total <= 0)
        return NULL;

    // Leaf-level STR: sort by X, slice, within slice sort by Y, pack leaves of size BUNDLEFACTOR.
    qsort(&rectArr[low], (size_t)total, sizeof(Rect), compareByXCenter);

    int S = (int)ceil(sqrt((double)total / BUNDLEFACTOR)); // recommended STR formula
    if (S < 1)
        S = 1;
    int sliceSize = (total + S - 1) / S;

    // Count leaves (two-pass to avoid overflow)
    int leafCount = 0;
    for (int s = 0; s < S; ++s)
    {
        int sliceLow = low + s * sliceSize;
        int sliceHigh = sliceLow + sliceSize - 1;
        if (sliceLow > high)
            break;
        if (sliceHigh > high)
            sliceHigh = high;
        int sc = sliceHigh - sliceLow + 1;
        if (sc <= 0)
            continue;
        leafCount += (sc + BUNDLEFACTOR - 1) / BUNDLEFACTOR;
    }

    Node **leaves = checked_malloc((size_t)leafCount * sizeof(Node *));
    int L = 0;

    for (int s = 0; s < S; ++s)
    {
        int sliceLow = low + s * sliceSize;
        int sliceHigh = sliceLow + sliceSize - 1;
        if (sliceLow > high)
            break;
        if (sliceHigh > high)
            sliceHigh = high;
        int sc = sliceHigh - sliceLow + 1;
        if (sc <= 0)
            continue;

        qsort(&rectArr[sliceLow], (size_t)sc, sizeof(Rect), compareByYCenter);

        for (int i = sliceLow; i <= sliceHigh; i += BUNDLEFACTOR)
        {
            int end = i + BUNDLEFACTOR - 1;
            if (end > sliceHigh)
                end = sliceHigh;
            leaves[L++] = createLeaf_safe(rectArr, i, end);
        }
    }

    // Upper levels: recursively group leaves with STR using FANOUT as capacity.
    Node *root = group_nodes_STR(leaves, L, FANOUT);
    free(leaves);
    return root;
}

void destroyRTree(Node *node)
{
    if (!node)
        return;

    if (node->isLeaf)
    {
        free(node->rects);
    }
    else
    {
        for (int i = 0; i < node->count; i++)
        {
            destroyRTree(node->children[i]);
        }
        free(node->children);
    }

    free(node);
}

bool isOverlap(const MBR *mbr, Rect r)
{
    return !(r.xmax < mbr->xmin || r.xmin > mbr->xmax ||
             r.ymax < mbr->ymin || r.ymin > mbr->ymax);
}

static inline void roofline_record_overlap(SearchRooflineMetrics *metrics, size_t bytes)
{
    if (!metrics)
        return;

    metrics->overlap_tests++;
    metrics->bytes_accessed += (unsigned long long)bytes;
}

static inline void roofline_record_bytes(SearchRooflineMetrics *metrics, size_t bytes)
{
    if (!metrics)
        return;

    metrics->bytes_accessed += (unsigned long long)bytes;
}

static inline double roofline_operational_intensity(const SearchRooflineMetrics *metrics)
{
    if (!metrics || metrics->bytes_accessed == 0)
        return 0.0;

    return (double)metrics->overlap_tests / (double)metrics->bytes_accessed;
}

int searchRTree(Node *node, Rect queryRect, int q)
{
    return searchRTree_with_metrics(node, queryRect, q, NULL);
}

int searchRTree_with_metrics(Node *node, Rect queryRect, int q, SearchRooflineMetrics *metrics)
{
    if (node == NULL)
        return 0;

    int count = 0;
    (void)q;

    roofline_record_overlap(metrics, sizeof(node->mbr) + sizeof(queryRect));
    if (!isOverlap(&node->mbr, queryRect))
        return 0;

    if (node->isLeaf)
    {
        for (int i = 0; i < node->count; i++)
        {
            const Rect *r = &node->rects[i];
            roofline_record_overlap(metrics, sizeof(*r) + sizeof(queryRect));
            if (isOverlap((const MBR *)r, queryRect))
                count++;
        }
    }
    else
    {
        roofline_record_bytes(metrics, (size_t)node->count * sizeof(*node->children));
        for (int i = 0; i < node->count; i++)
        {
            count += searchRTree_with_metrics(node->children[i], queryRect, q, metrics);
        }
    }

    return count;
}
// Iterative search: avoids recursion overhead and improves I-cache usage
int searchRTree_iter(Node *root, Rect queryRect, int q)
{
    return searchRTree_iter_with_metrics(root, queryRect, q, NULL);
}

int searchRTree_iter_with_metrics(Node *root, Rect queryRect, int q, SearchRooflineMetrics *metrics)
{
    if (!root)
        return 0;

    (void)q;

    Node *stack[256];
    int top = 0;
    stack[top++] = root;

    int count = 0;

    while (top)
    {
        Node *node = stack[--top];

        roofline_record_overlap(metrics, sizeof(node->mbr) + sizeof(queryRect));
        if (!isOverlap(&node->mbr, queryRect))
            continue;

        if (node->isLeaf)
        {
            for (int i = 0; i < node->count; i++)
            {
                const Rect *r = &node->rects[i];
                roofline_record_overlap(metrics, sizeof(*r) + sizeof(queryRect));
                if (isOverlap((const MBR *)r, queryRect))
                    count++;
            }
        }
        else
        {
            roofline_record_bytes(metrics, (size_t)node->count * sizeof(*node->children));
            for (int i = 0; i < node->count; i++)
            {
                stack[top++] = node->children[i];
                if (top >= (int)(sizeof(stack) / sizeof(stack[0])))
                {
                    count += searchRTree_with_metrics(node->children[i], queryRect, q, metrics);
                    top--;
                }
            }
        }
    }

    return count;
}

// Helper function to collect statistics recursively
void collectRTreeStats(Node *node, int depth, RTreeStats *stats)
{
    if (!node)
        return;

    stats->totalNodes++;

    if (node->isLeaf)
    {
        stats->leafNodes++;
    }
    else
    {
        stats->internalNodes++;
    }

    if (depth > stats->maxDepth)
        stats->maxDepth = depth;

    if (!node->isLeaf)
    {
        for (int i = 0; i < node->count; i++)
        {
            collectRTreeStats(node->children[i], depth + 1, stats);
        }
    }
}
// Function to compute and print R-tree statistics
void printRTreeStats(Node *root)
{
    RTreeStats stats = {0, 0, 0, 0};
    collectRTreeStats(root, 1, &stats);

    printf("\n=== R-Tree Stats ===\n");
    printf("Total Nodes       : %d\n", stats.totalNodes);
    printf("Subtree Nodes     : %d\n", stats.internalNodes);
    printf("Leaf Nodes        : %d\n", stats.leafNodes);
    printf("Number of Levels  : %d\n", stats.maxDepth);
    printf("====================\n");
}

static void writeRooflineLogSection(FILE *log, const char *label, double time_s,
                                    const SearchRooflineMetrics *metrics)
{
    double intensity = roofline_operational_intensity(metrics);
    double throughput = (time_s > 0.0) ? (double)metrics->overlap_tests / time_s : 0.0;
    double mib = (double)metrics->bytes_accessed / (1024.0 * 1024.0);

    fprintf(log, "%s Roofline:\n", label);
    fprintf(log, "  Work (overlap tests): %llu\n", metrics->overlap_tests);
    fprintf(log, "  Bytes accessed est.:  %llu (%.2f MiB)\n", metrics->bytes_accessed, mib);
    fprintf(log, "  Operational intensity: %.6f tests/byte\n", intensity);
    fprintf(log, "  Achieved throughput:   %.2f Mtests/s\n", throughput / 1e6);
}

void writeTimingLog(int numRects, int numQuery, int numThreads, double seq_time_s, double par_time_s,
                    const SearchRooflineMetrics *seq_metrics, const SearchRooflineMetrics *par_metrics)
{
    double speedup = seq_time_s / par_time_s;

    if (mkdir("Log", 0777) != 0 && errno != EEXIST)
    {
        perror("mkdir Log");
        return;
    }

    time_t rawtime;
    struct tm timeinfo_storage;
    struct tm *timeinfo;
    char filename[100];

    time(&rawtime);
    timeinfo = localtime_r(&rawtime, &timeinfo_storage);
    if (!timeinfo)
    {
        perror("localtime_r");
        return;
    }
    strftime(filename, sizeof(filename), "Log/%Y-%m-%d.txt", timeinfo);

    FILE *log = fopen(filename, "a");
    if (log)
    {
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
        fprintf(log, "\n=== Run @ %s ===\n", timestamp);

        fprintf(log, "Dataset: %d rects, %d queries\n", numRects, numQuery);
        fprintf(log, "Threads used: %d\n", numThreads);
        fprintf(log, "Sequential Time: %.2f s\n", seq_time_s);
        fprintf(log, "Parallel Time:   %.2f s\n", par_time_s);
        fprintf(log, "Speedup: %.2fx\n", speedup);

        if (seq_metrics)
            writeRooflineLogSection(log, "Sequential", seq_time_s, seq_metrics);
        if (par_metrics)
            writeRooflineLogSection(log, "Parallel", par_time_s, par_metrics);

        fprintf(log, "-------------------------\n");

        fclose(log);
        printf("📁 Timing log saved to: %s\n", filename);
    }
    else
    {
        perror("❌ Failed to open log file");
    }
}



Rect *selectDataDataset(int *numRects, int option)
{
    const char *paths[] = {
        "Data/1. mbrs_sports_999k.csv",
        "Data/2. lakes_mbr_int.csv",
        "Data/3. parks_mbr_int_4col.csv",
        "Data/4. uniform_16.csv",
        "Data/5. buildings_mbr_57M.csv"
    };

    if (option < 1 || option > 5)
    {
        printf("Invalid option. Please choose between 1 and 5.\n");
        exit(1);
    }

    return readRectsFromFile(paths[option - 1], numRects);
}

Rect *selectQueryDataset(int *numQuery, int dataset_option)
{
    int option = 0;

    const char **paths = NULL;
    size_t npaths = 0;

    switch (dataset_option)
    {
    case 1:
    { /* Sports */
        static const char *sports_paths[] = {
            "Query/1. mbrs_sports_999k/mbrs_sports_999k_1_.csv",
            "Query/1. mbrs_sports_999k/mbrs_sports_999k_5_.csv",
            "Query/1. mbrs_sports_999k/mbrs_sports_999k_10_.csv",
            "Query/1. mbrs_sports_999k/mbrs_sports_999k_25_.csv"
        };

        printf("\nChoose the Query Dataset for Sports:\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");

        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }

        paths = sports_paths;
        npaths = sizeof(sports_paths) / sizeof(sports_paths[0]);
        break;
    }

    case 2:
    { /* Lakes */
        static const char *lakes_paths[] = {
            "Query/2. lakes_mbr_int/lakes_mbr_int_1_.csv",
            "Query/2. lakes_mbr_int/lakes_mbr_int_5_.csv",
            "Query/2. lakes_mbr_int/lakes_mbr_int_10_.csv",
            "Query/2. lakes_mbr_int/lakes_mbr_int_25_.csv"
        };

        printf("\nChoose the Query Dataset for Lakes:\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");

        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }

        paths = lakes_paths;
        npaths = sizeof(lakes_paths) / sizeof(lakes_paths[0]);
        break;
    }

    case 3:
    { /* Parks */
        static const char *parks_paths[] = {
            "Query/3. FullParks/parks_mbr_int_4col_1pct.csv",
            "Query/3. FullParks/parks_mbr_int_4col_5pct.csv",
            "Query/3. FullParks/parks_mbr_int_4col_10pct.csv",
            "Query/3. FullParks/parks_mbr_int_4col_25pct.csv"
        };

        printf("\nChoose the Query Dataset for Parks:\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");

        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }

        paths = parks_paths;
        npaths = sizeof(parks_paths) / sizeof(parks_paths[0]);
        break;
    }

    case 4:
    { /* Synthetic */
        static const char *synthetic_paths[] = {
            "Query/4. 16M/uniform_16M_1pct.csv",
            "Query/4. 16M/uniform_16M_5pct.csv",
            "Query/4. 16M/uniform_16M_10pct.csv",
            "Query/4. 16M/uniform_16M_25pct.csv"
        };

        printf("\nChoose the Query Dataset for Synthetic:\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");

        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }

        paths = synthetic_paths;
        npaths = sizeof(synthetic_paths) / sizeof(synthetic_paths[0]);
        break;
    }

    case 5:
    { /* Buildings */
        static const char *building_paths[] = {
            "Query/5. Building_14M/buildings_mbr_int_4col_12.5pct_1pct.csv",
            "Query/5. Building_14M/buildings_mbr_int_4col_12.5pct_5pct.csv",
            "Query/5. Building_14M/buildings_mbr_int_4col_12.5pct_10pct.csv",
            "Query/5. Building_14M/buildings_mbr_int_4col_12.5pct_25pct.csv"
        };

        printf("\nChoose the Query Dataset for Buildings:\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");

        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }

        paths = building_paths;
        npaths = sizeof(building_paths) / sizeof(building_paths[0]);
        break;
    }

    default:
        printf("Invalid dataset option. Exiting.\n");
        exit(1);
    }

    /* Validate query selection */
    if (option < 1 || (size_t)option > npaths)
    {
        printf("Invalid option. Exiting.\n");
        exit(1);
    }

    return readRectsFromFile(paths[option - 1], numQuery);
}
