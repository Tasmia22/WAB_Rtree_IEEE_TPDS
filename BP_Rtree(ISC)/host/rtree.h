#ifndef RTREE_H
#define RTREE_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct Rect
{
    int xmin, ymin, xmax, ymax;
} Rect;

typedef struct MBR
{
    int xmin, ymin;
    int xmax, ymax;
} MBR;

typedef struct Node
{
    int isLeaf;
    int count;
    MBR mbr;
    union
    {
        struct Node **children;
        Rect *rects;
    };
} Node;

typedef struct SerializedNode
{
    int isLeaf;
    int count;
    MBR mbr;
    int children[MAX_CHILDREN];
    Rect rects[MAX_RECTS];
} SerializedNode;

typedef struct {
    int isLeaf;   // same types/order as SerializedNode
    int count;
    MBR mbr;
} SerializedNodeHdr;

typedef struct {
    int isLeaf;      // 0
    int count;
    MBR mbr;
    int children[MAX_CHILDREN];  // typically fewer than MAX_CHILDREN
} InternalNodeBroadcast;  //2072 bytes


typedef struct RTreeStats
{
    int totalNodes;
    int leafNodes;
    int internalNodes;
    int maxDepth;
} RTreeStats;

struct dpu_low_with_index {
    uint32_t dpu_index;
    uint32_t low_tree_count;
    uint32_t l1_index;   // new
    uint32_t l1_count;   // new
};

typedef struct {
    uint64_t total_cycles;                // DPU wall-clock cycles for the measured region
    uint32_t per_tasklet[NR_TASKLETS];    // cycles spent by each tasklet in the measured region
} PerfStats;

// typedef struct {
//     uint64_t per_tasklet[NR_TASKLETS];
//     uint64_t total_cycles;

//     // --- New: per-tasklet instrumentation ---
//     uint64_t bytes_read[NR_TASKLETS];     // approximate bytes read from MRAM
//     uint64_t bytes_written[NR_TASKLETS];  // bytes written to MRAM
//     uint64_t nodes_visited[NR_TASKLETS];  // nodes whose MBR we tested
//     uint64_t rects_tested[NR_TASKLETS];   // number of rectangles tested for overlap
// } PerfStats;

Rect *readRectsFromFile(const char *filename, int *num_rects);
void printRects(Rect rects[], int num_rects);
Node *createRTree(Rect *rectArr, int low, int high);
Node *createRTree_STR(Rect *rectArr, int low, int high);
void printRTree(Node *node, int level);
int searchRTree(Node *node, Rect queryRect, int q);
void Zsorting(Rect rects[], int num_rects);
int serialize_rtree_wrapper(Node *root, SerializedNode **output);
void print_serialisedtree(int node_index, int depth, SerializedNode *serialized_tree);
//Node *getSubtree(Node *root, int targetIndex);
void printRTreeStats(Node *root);
int get_rtree_memory_size(Node *node);
size_t get_serialized_size(Node *node);
//int serialize_rtree_bfs(Node *root, SerializedNode *out);
int serialize_rtree_bfs(Node *root, SerializedNode *out, int total_nodes);
int serialize_rtree_bfs_wrapper(Node *root, SerializedNode **out_tree);
void print_serialized_tree(const SerializedNode *tree, int total_nodes);
Rect *selectDataDataset(int *numRects, int option);
Rect *selectQueryDataset(int *numQuery, int dataset_option);







#endif /* RTREE_H */
