#ifndef RTREE_H
#define RTREE_H
#include <stdbool.h>   // <-- add this
#include "common.h"

#define MAX_QUERY (200000)

typedef struct
{
    int32_t xmin, ymin, xmax, ymax;
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

typedef struct RTreeStats
{
    int totalNodes;
    int leafNodes;
    int internalNodes;
    int maxDepth;
} RTreeStats;
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
Rect *selectQueryDataset(int *numQuery, int dataset_option);
Rect *selectDataDataset(int *numRects, int option);
int prepare_serialized_subtrees(
    Node *root,
    uint32_t *nr_of_dpus_io,          // in/out: requested DPUs -> actual used
    Node **subtree,                   // [*nr_of_dpus_io]
    SerializedNode **serialized_trees,// [*nr_of_dpus_io]; each becomes malloc'ed buffer
    int *num_nodes_eachsubtree,       // [*nr_of_dpus_io]
    MBR *subtree_mbrs,                // [*nr_of_dpus_io]
    bool print_first_subtree
);


#endif /* RTREE_H */
