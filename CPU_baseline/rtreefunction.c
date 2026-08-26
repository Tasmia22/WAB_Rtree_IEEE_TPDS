#include "rtree.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <stdint.h>

void resetSearchCounters(SearchCounters *counters)
{
    counters->query_count = 0ull;
    counters->node_visits = 0ull;
    counters->leaf_visits = 0ull;
    counters->rect_tests = 0ull;
    counters->child_pushes = 0ull;
}

double getEstimatedSearchBytes(const SearchCounters *counters)
{
    double bytes = 0.0;
    bytes += (double)counters->node_visits * (double)sizeof(Node);
    bytes += (double)counters->child_pushes * (double)sizeof(Node *);
    bytes += (double)counters->rect_tests * (double)sizeof(Rect);
    return bytes;
}

double getEstimatedSearchOps(const SearchCounters *counters)
{
    // Use R-tree predicate work directly: node MBR overlap tests + rectangle overlap tests.
    double overlap_tests = (double)counters->node_visits + (double)counters->rect_tests;
    return overlap_tests;
}

void printSearchStats(const char *label, const SearchCounters *counters, double run_time_s)
{
    double bytes = getEstimatedSearchBytes(counters);
    double ops = getEstimatedSearchOps(counters);
    double intensity = (bytes > 0.0) ? (ops / bytes) : 0.0;
    double throughput = (run_time_s > 0.0) ? (ops / run_time_s) : 0.0;
    double bandwidth = (run_time_s > 0.0) ? (bytes / run_time_s) : 0.0;

    printf("\n--- %s Search Roofline Stats ---\n", label);
    printf("Queries executed     : %llu\n", (unsigned long long)counters->query_count);
    printf("Node visits          : %llu\n", (unsigned long long)counters->node_visits);
    printf("Leaf visits          : %llu\n", (unsigned long long)counters->leaf_visits);
    printf("Rectangle tests      : %llu\n", (unsigned long long)counters->rect_tests);
    printf("Child pointer pushes : %llu\n", (unsigned long long)counters->child_pushes);
    printf("Estimated memory load: %.2f MB\n", bytes / (1024.0 * 1024.0));
    printf("Overlap tests        : %.2f Gtests\n", ops / 1e9);
    printf("Arithmetic intensity : %.4f tests/byte\n", intensity);
    printf("Search throughput    : %.3f Gtests/s\n", throughput / 1e9);
    printf("Effective bandwidth  : %.2f GB/s\n", bandwidth / 1e9);
    printf("--------------------------------\n");
}

int countRectsInFile(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        perror("Unable to open file for counting");
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

    Rect *rects = (Rect *)malloc(*num_rects * sizeof(Rect));
    if (!rects)
    {
        perror("Unable to allocate memory for rectangles");
        return NULL;
    }

    FILE *file = fopen(filename, "r");
    if (!file)
    {
        perror("Unable to open file");
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

Rect *readScaledRectsFromFile(const char *filename, int *num_rects, double scale)
{
    int line_count = countRectsInFile(filename);
    *num_rects = line_count;

    if (*num_rects <= 0)
        return NULL;

    Rect *rects = malloc((size_t)*num_rects * sizeof(Rect));
    if (!rects)
    {
        perror("Unable to allocate memory for rectangles");
        return NULL;
    }

    FILE *file = fopen(filename, "r");
    if (!file)
    {
        perror("Unable to open file");
        free(rects);
        return NULL;
    }

    double x1, y1, x2, y2;
    for (int i = 0; i < *num_rects; i++)
    {
        if (fscanf(file, "%lf,%lf,%lf,%lf", &x1, &y1, &x2, &y2) != 4)
        {
            fprintf(stderr, "Error reading rectangle at line %d\n", i + 1);
            free(rects);
            fclose(file);
            return NULL;
        }

        int sx1 = (int)llround(x1 * scale);
        int sy1 = (int)llround(y1 * scale);
        int sx2 = (int)llround(x2 * scale);
        int sy2 = (int)llround(y2 * scale);
        rects[i].xmin = (sx1 < sx2) ? sx1 : sx2;
        rects[i].ymin = (sy1 < sy2) ? sy1 : sy2;
        rects[i].xmax = (sx1 > sx2) ? sx1 : sx2;
        rects[i].ymax = (sy1 > sy2) ? sy1 : sy2;
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
    Node *leaf = (Node *)malloc(sizeof(Node));
    leaf->isLeaf = 1;
    leaf->children = NULL;
    leaf->count = high - low + 1;
    leaf->rects = (Rect *)malloc((size_t)leaf->count * sizeof(Rect));
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
        Node *p = (Node *)malloc(sizeof(Node));
        p->isLeaf = 0;
        p->rects = NULL;
        p->count = n;
        p->children = (Node **)malloc((size_t)n * sizeof(Node *));
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

    Node **parents = (Node **)malloc((size_t)parentCount * sizeof(Node *));
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

            Node *p = (Node *)malloc(sizeof(Node));
            p->isLeaf = 0;
            p->rects = NULL;
            p->count = cnt;
            p->children = (Node **)malloc((size_t)cnt * sizeof(Node *));
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

    Node **leaves = (Node **)malloc((size_t)leafCount * sizeof(Node *));
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



bool isOverlap(const MBR *mbr, Rect r)
{
   return !(r.xmax < mbr->xmin || r.xmin > mbr->xmax ||
            r.ymax < mbr->ymin || r.ymin > mbr->ymax);
}



int searchRTree(Node *node, Rect queryRect, int q)
{
    if (node == NULL) return 0;

    int count = 0;

    // Prune if query doesn't overlap node MBR
    if (!isOverlap(&node->mbr, queryRect))
        return 0;

    if (node->isLeaf) {
        for (int i = 0; i < node->count; i++) {
            const Rect *r = &node->rects[i];
            if (isOverlap((const MBR *)r, queryRect)) {
                // Optional debug logging for a specific query ID
                // if (q == 6)
                //     printf("\nQuery[%d]=[%d,%d-%d,%d] overlapped with Rect[%d,%d-%d,%d]", q,
                //         queryRect.xmin, queryRect.ymin, queryRect.xmax, queryRect.ymax,
                //         r->xmin, r->ymin, r->xmax, r->ymax);
                count++;
            }
        }
    } else {
        for (int i = 0; i < node->count; i++) {
            count += searchRTree(node->children[i], queryRect, q);
        }
    }

    return count;
}
bool initSearchWorkspace(SearchWorkspace *workspace, size_t initial_capacity)
{
    if (!workspace)
        return false;
    if (initial_capacity == 0)
        initial_capacity = 256;
    workspace->nodes = malloc(initial_capacity * sizeof(*workspace->nodes));
    workspace->capacity = workspace->nodes ? initial_capacity : 0;
    return workspace->nodes != NULL;
}

void freeSearchWorkspace(SearchWorkspace *workspace)
{
    if (!workspace)
        return;
    free(workspace->nodes);
    workspace->nodes = NULL;
    workspace->capacity = 0;
}

static bool growSearchWorkspace(SearchWorkspace *workspace, size_t required)
{
    size_t new_capacity = workspace->capacity ? workspace->capacity : 256;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2)
            return false;
        new_capacity *= 2;
    }
    Node **new_nodes = realloc(workspace->nodes,
                               new_capacity * sizeof(*new_nodes));
    if (!new_nodes)
        return false;
    workspace->nodes = new_nodes;
    workspace->capacity = new_capacity;
    return true;
}

// Iterative search with a reusable, growable stack. All traversed work is counted.
int searchRTree_iter(Node *root, Rect queryRect, int q, SearchCounters *counters,
                     SearchWorkspace *workspace)
{
    (void)q;
    if (!root || !workspace || !workspace->nodes)
        return 0;

    counters->query_count++;

    size_t top = 0;
    workspace->nodes[top++] = root;

    int count = 0;

    while (top) {
        Node *node = workspace->nodes[--top];

        counters->node_visits++;

        // Prune
        if (!isOverlap(&node->mbr, queryRect))
            continue;

        if (node->isLeaf) {
            counters->leaf_visits++;
            // Scan leaf
            for (int i = 0; i < node->count; i++) {
                counters->rect_tests++;
                const Rect *r = &node->rects[i];
                if (isOverlap((const MBR *)r, queryRect))
                    count++;
            }
        } else {
            // Push (lightly prefetched) children
            // Optional prefetch of the first child MBR to hide latency:
            // if (node->count > 0) __builtin_prefetch(&node->children[0]->mbr, 0, 1);
            for (int i = 0; i < node->count; i++) {
                if (top == workspace->capacity &&
                    !growSearchWorkspace(workspace, top + 1)) {
                    fprintf(stderr, "Unable to grow R-tree search workspace\n");
                    exit(EXIT_FAILURE);
                }
                counters->child_pushes++;
                workspace->nodes[top++] = node->children[i];
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



void writeTimingLog(int numRects, int numQuery, int numThreads, double seq_time_s, double par_time_s, const SearchCounters *seqCounters, const SearchCounters *parCounters)
{
    // Compute speedup
    double speedup = seq_time_s / par_time_s;
    double seq_bytes = getEstimatedSearchBytes(seqCounters);
    double par_bytes = getEstimatedSearchBytes(parCounters);
    double seq_tests = getEstimatedSearchOps(seqCounters);
    double par_tests = getEstimatedSearchOps(parCounters);
    double seq_intensity = (seq_bytes > 0.0) ? (seq_tests / seq_bytes) : 0.0;
    double par_intensity = (par_bytes > 0.0) ? (par_tests / par_bytes) : 0.0;

    // Ensure Log directory exists
    mkdir("Log", 0777);

    // Format log filename (same file for each day), unless an experiment overrides it.
    time_t rawtime;
    struct tm *timeinfo;
    char filename[100];

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    const char *log_override = getenv("RTREE_LOG_FILE");
    if (log_override && log_override[0] != '\0')
    {
        snprintf(filename, sizeof(filename), "%s", log_override);
    }
    else
    {
        strftime(filename, sizeof(filename), "Log/%Y-%m-%d.txt", timeinfo);
    }

    // Open log file in append mode
    FILE *log = fopen(filename, "a");
    if (log)
    {
        // Add timestamped section header
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
        fprintf(log, "\n=== Run @ %s ===\n", timestamp);

        fprintf(log, "Dataset: %d rects, %d queries\n", numRects, numQuery);
        fprintf(log, "Threads used: %d\n", numThreads);
        fprintf(log, "Sequential Time: %.6f s\n", seq_time_s);
        fprintf(log, "Parallel Time:   %.6f s\n", par_time_s);
        fprintf(log, "Speedup: %.2fx\n", speedup);
        fprintf(log, "--- Sequential Roofline Stats ---\n");
        fprintf(log, "Seq queries          : %llu\n", (unsigned long long)seqCounters->query_count);
        fprintf(log, "Seq node visits      : %llu\n", (unsigned long long)seqCounters->node_visits);
        fprintf(log, "Seq rectangle tests  : %llu\n", (unsigned long long)seqCounters->rect_tests);
        fprintf(log, "Seq overlap tests    : %.6f Gtests\n", seq_tests / 1e9);
        fprintf(log, "Seq memory load      : %.2f MB\n", seq_bytes / (1024.0 * 1024.0));
        fprintf(log, "Seq intensity        : %.4f tests/byte\n", seq_intensity);
        fprintf(log, "Seq throughput      : %.3f Gtests/s\n", (seq_tests / seq_time_s) / 1e9);
        fprintf(log, "Seq bandwidth       : %.2f GB/s\n", (seq_bytes / seq_time_s) / 1e9);
        fprintf(log, "--- Parallel Roofline Stats ---\n");
        fprintf(log, "Par queries          : %llu\n", (unsigned long long)parCounters->query_count);
        fprintf(log, "Par node visits      : %llu\n", (unsigned long long)parCounters->node_visits);
        fprintf(log, "Par rectangle tests  : %llu\n", (unsigned long long)parCounters->rect_tests);
        fprintf(log, "Par overlap tests    : %.6f Gtests\n", par_tests / 1e9);
        fprintf(log, "Par memory load      : %.2f MB\n", par_bytes / (1024.0 * 1024.0));
        fprintf(log, "Par intensity        : %.4f tests/byte\n", par_intensity);
        fprintf(log, "Par throughput      : %.3f Gtests/s\n", (par_tests / par_time_s) / 1e9);
        fprintf(log, "Par bandwidth       : %.2f GB/s\n", (par_bytes / par_time_s) / 1e9);
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
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/Uniform_Box_6M_int.csv",
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/mbrs_sports_999k.csv",
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/sports_mbr_1.7M.csv",
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/parks_mbr_int_4col.csv",
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/mbrs_cemetery_168k.csv",
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/lakes_mbr_int.csv",
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/uniform_16.csv",
        "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Data/buildings_mbr_int_4col_firsthalf.csv"};

    if (option < 1 || option > 8)
    {
        printf("Invalid option. Exiting.\n");
        exit(1);
    }

    if (option == 7)
        return readScaledRectsFromFile(paths[option - 1], numRects, 1.0e8);

    return readRectsFromFile(paths[option - 1], numRects);
}

Rect *selectQueryDataset(int *numQuery, int dataset_option)
{
    int option = 0;

    /* Will point at the correct table for the chosen dataset */
    const char **paths = NULL;
    size_t npaths = 0;

    switch (dataset_option) {
    case 1: { /* Synthetic */
        static const char *synthetic_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_1408.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_6M_int_1_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_6M_int_5_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_6M_int_10_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_6M_int_25_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_90k.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_180k.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_360k.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Synthetic_Data/Uniform_Box_720k.csv"
        };
        printf("\nChoose the Query Dataset for Synthetic Dataset:\n"
               "\t1. Uniform_Box_1408\n"
               "\t2. Uniform_Box_1%%\n"
               "\t3. Uniform_Box_5%%\n"
               "\t4. Uniform_Box_10%%\n"
               "\t5. Uniform_Box_45k\n"
               "\t6. Uniform_Box_90k\n"
               "\t7. Uniform_Box_180k\n"
               "\t8. Uniform_Box_360k\n"
               "\t9. Uniform_Box_720k\n"
               "Enter your option (1-9): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths  = synthetic_paths;
        npaths = sizeof(synthetic_paths) / sizeof(synthetic_paths[0]);
        break;
    }
    case 2: { /* Sports (999k) */
        static const char *sports_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_sports_999k/mbrs_sports_999k_1_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_sports_999k/mbrs_sports_999k_5_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_sports_999k/mbrs_sports_999k_10_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_sports_999k/mbrs_sports_999k_25_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_sports_999k/mbrs_sports_999k_50_.csv",
        };
        printf("\nChoose the Query Dataset for Sports(999k):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "Enter your option (1-5): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths  = sports_paths;
        npaths = sizeof(sports_paths) / sizeof(sports_paths[0]);
        break;
    }
    case 3: { /* Sports (1.7M) */
        static const char *sports_17_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Sports_1.7M/sports_mbr_int_1_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Sports_1.7M/sports_mbr_int_5_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Sports_1.7M/sports_mbr_int_10_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Sports_1.7M/sports_mbr_int_25_.csv",
        };
        printf("\nChoose the Query Dataset for Sports(1.7M):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths  = sports_17_paths;
        npaths = sizeof(sports_17_paths) / sizeof(sports_17_paths[0]);
        break;
    }
    case 4: { /* Parks (9.9M) */
        static const char *parks_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/FullParks/parks_mbr_int_4col_1pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/FullParks/parks_mbr_int_4col_5pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/FullParks/parks_mbr_int_4col_10pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/FullParks/parks_mbr_int_4col_25pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/FullParks/parks_mbr_int_4col_50pct.csv"
        };
        printf("\nChoose the Query Dataset for Parks(9.9M):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "Enter your option (1-5): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths  = parks_paths;
        npaths = sizeof(parks_paths) / sizeof(parks_paths[0]);
        break;
    }
    case 5: { /* Cemetery (168k) */
        static const char *cemetery_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_1_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_5_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_10_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_25_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_50_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_75_.csv"
        };
        printf("\nChoose the Query Dataset for Cemetery(168k):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "\t6. 75%%\n"
               "Enter your option (1-6): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths  = cemetery_paths;
        npaths = sizeof(cemetery_paths) / sizeof(cemetery_paths[0]);
        break;
    }
    case 6: { /* Lakes (8M) */
        static const char *lakes_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/lakes_mbr_int/lakes_mbr_int_1_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/lakes_mbr_int/lakes_mbr_int_5_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/lakes_mbr_int/lakes_mbr_int_10_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/lakes_mbr_int/lakes_mbr_int_25_.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/lakes_mbr_int/lakes_mbr_int_50_.csv"
        };
        printf("\nChoose the Query Dataset for Lakes(8M):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "Enter your option (1-5): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths  = lakes_paths;
        npaths = sizeof(lakes_paths) / sizeof(lakes_paths[0]);
        break;
    }
    case 7: { /* Uniform (16M) */
        static const char *uniform_16m_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/16M/uniform_16M_1pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/16M/uniform_16M_5pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/16M/uniform_16M_10pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/16M/uniform_16M_25pct.csv"
        };
        printf("\nChoose the Query Dataset for Uniform(16M):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths = uniform_16m_paths;
        npaths = sizeof(uniform_16m_paths) / sizeof(uniform_16m_paths[0]);
        break;
    }
    case 8: { /* Buildings */
        static const char *buildings_paths[] = {
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Building_14M/buildings_mbr_int_4col_12.5pct_1pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Building_14M/buildings_mbr_int_4col_12.5pct_5pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Building_14M/buildings_mbr_int_4col_12.5pct_10pct.csv",
            "/Users/tjv7w/Documents/ISC_Paper/IEEE_TPDS/TPDS_CPU_Baseline/Query/Building_14M/buildings_mbr_int_4col_12.5pct_25pct.csv"
        };
        printf("\nChoose the Query Dataset for Buildings:\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "Enter your option (1-4): ");
        if (scanf(" %d", &option) != 1) { printf("Invalid input.\n"); exit(1); }
        paths = buildings_paths;
        npaths = sizeof(buildings_paths) / sizeof(buildings_paths[0]);
        break;
    }
    default:
        printf("Invalid dataset option. Exiting.\n");
        exit(1);
    }

    /* Validate selection */
    if (option < 1 || (size_t)option > npaths) {
        printf("Invalid option. Exiting.\n");
        exit(1);
    }

    /* Load and return the chosen query file */
    return readRectsFromFile(paths[option - 1], numQuery);
}
