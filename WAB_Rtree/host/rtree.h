#ifndef RTREE_H
#define RTREE_H

#include "common.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef __DPU__
#include <stdio.h>
#include <time.h>
#endif

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

typedef struct SerializedLeafNode
{
    int isLeaf;
    int count;
    MBR mbr;
    Rect rects[MAX_RECTS];
} SerializedLeafNode;

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
    uint32_t replica_rank;   // query-shard rank within replicas of the same subtree shard
    uint32_t replica_count;  // number of replicas for this subtree shard
};

typedef struct {
    uint64_t total_cycles;                // DPU wall-clock cycles for the measured region
    uint32_t per_tasklet[NR_TASKLETS];    // cycles spent by each tasklet in the measured region
} PerfStats;

typedef struct {
    uint32_t qid;     // index within the current batch
    uint32_t count;   // partial overlap count for this DPU
} ResultPair;

typedef struct {
    uint32_t count;      // overlap count for this query slot
    uint32_t reserved;   // keeps MRAM transfers 8-byte aligned
} DenseResultSlot;

#define RESULT_RETURN_MODE_SPARSE 0u
#define RESULT_RETURN_MODE_DENSE 1u
#define REPLICA_QUERY_POLICY_RANGE 0u
#define REPLICA_QUERY_POLICY_BLOCK_CYCLIC 1u

typedef struct {
    uint32_t actual_query_count;     // actual queries used (may be < MAX_QUERY)
    uint32_t actual_top_tree_count;  // actual top tree nodes (may be < MAX_TOP2)
    uint32_t actual_low_tree_count;  // actual low tree nodes (may be < MAX_SUBTREE)
    uint32_t actual_result_capacity; // actual result capacity (may be < RESULT_PAIR_CAPACITY)
    uint32_t result_return_mode;     // sparse pairs by default, dense array for one-off ablation
    uint32_t replica_query_policy;   // range or block-cyclic query partition across replicas
    uint32_t replica_query_block_queries; // block size used by block-cyclic replica scheduling
    uint32_t reserved0;
} MramConfig;

#ifndef __DPU__
void print_summary_and_check(
    Node *root,
    FILE *log_file,
    uint32_t nr_of_dpus,
    int numRects,
    int numQuery,
    double rtree_construction_time,
    double search_time,
    double tree_transfer_time,
    double kernel_time_total,
    struct timespec t4,
    struct timespec t7,
    uint64_t cpu_total_overlaps,
    uint64_t dpu_total_overlaps
);
#endif
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
