#ifndef RTREE_H
#define RTREE_H

#include <limits.h>
#include <stdbool.h>

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
typedef struct __attribute__((aligned(64))) SearchRooflineMetrics
{
    unsigned long long overlap_tests;
    unsigned long long bytes_accessed;
    char cacheline_pad[48];
} SearchRooflineMetrics;
typedef struct {
    int z_value;
    int index;
} ZRect;


// Function declarations
Rect *readRectsFromFile(const char *filename, int *num_rects);
void initMBR(MBR *mbr);
void updateMBRWithRect(MBR *mbr, Rect r);
MBR unionJoin(MBR *mbr1, MBR *mbr2);
Node *createLeaf(Rect *rectArr, int low, int high);
int compareByXCenter(const void *a, const void *b);
int compareByYCenter(const void *a, const void *b);
Node *createRTree_STR(Rect *rectArr, int low, int high);
void destroyRTree(Node *node);
bool isOverlap(const MBR *mbr, Rect r);
int searchRTree(Node *node, Rect queryRect, int q);
int searchRTree_with_metrics(Node *node, Rect queryRect, int q, SearchRooflineMetrics *metrics);
void printRTreeStats(Node *root);
void Zsorting(Rect rects[], int num_rects);
void writeTimingLog(int numRects, int numQuery, int numThreads, double seq_time_ms, double par_time_ms,
                    const SearchRooflineMetrics *seq_metrics, const SearchRooflineMetrics *par_metrics);
int searchRTree_iter(Node *root, Rect queryRect, int q);
int searchRTree_iter_with_metrics(Node *root, Rect queryRect, int q, SearchRooflineMetrics *metrics);

Rect *selectDataDataset(int *numRects, int option);
Rect *selectQueryDataset(int *numQuery, int dataset_option);
#endif
