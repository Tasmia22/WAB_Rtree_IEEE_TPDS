#ifndef RTREE_H
#define RTREE_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define BUNDLEFACTOR 128  // max rectangles per leaf
#define FANOUT 2540        // max children per internal node

typedef struct {
    int xmin, ymin, xmax, ymax;
} Rect, MBR;

typedef struct Node {
    int isLeaf;
    int count;
    union {
        struct Node **children; // internal node
        Rect *rects;            // leaf node
    };
    MBR mbr;
} Node;
typedef struct RTreeStats
{
    int totalNodes;
    int leafNodes;
    int internalNodes;
    int maxDepth;
} RTreeStats;
typedef struct {
    int z_value;
    int index;
} ZRect;

typedef struct {
    unsigned long long query_count;
    unsigned long long node_visits;
    unsigned long long leaf_visits;
    unsigned long long rect_tests;
    unsigned long long child_pushes;
} SearchCounters;

typedef struct {
    Node **nodes;
    size_t capacity;
} SearchWorkspace;

// Function declarations
Rect *readRectsFromFile(const char *filename, int *num_rects);
Rect *readScaledRectsFromFile(const char *filename, int *num_rects, double scale);
void initMBR(MBR *mbr);
void updateMBRWithRect(MBR *mbr, Rect r);
MBR unionJoin(MBR *mbr1, MBR *mbr2);
Node *createLeaf(Rect *rectArr, int low, int high);
int compareByXCenter(const void *a, const void *b);
int compareByYCenter(const void *a, const void *b);
Node *createRTree_STR(Rect *rectArr, int low, int high);
bool isOverlap(const MBR *mbr, Rect r);
int searchRTree(Node *node, Rect queryRect, int q);
void printRTreeStats(Node *root);
void resetSearchCounters(SearchCounters *counters);
void printSearchStats(const char *label, const SearchCounters *counters, double run_time_s);
double getEstimatedSearchBytes(const SearchCounters *counters);
double getEstimatedSearchOps(const SearchCounters *counters);
void Zsorting(Rect rects[], int num_rects);
void writeTimingLog(int numRects, int numQuery, int numThreads, double seq_time_ms, double par_time_ms, const SearchCounters *seqCounters, const SearchCounters *parCounters);
bool initSearchWorkspace(SearchWorkspace *workspace, size_t initial_capacity);
void freeSearchWorkspace(SearchWorkspace *workspace);
int searchRTree_iter(Node *root, Rect queryRect, int q, SearchCounters *counters,
                     SearchWorkspace *workspace);

Rect *selectDataDataset(int *numRects, int option);
Rect *selectQueryDataset(int *numQuery, int dataset_option);
#endif
