#ifndef RTREE_H
#define RTREE_H

#include <stdbool.h>
#include <stddef.h>

#ifndef FANOUT
#define FANOUT 2540
#endif

#ifndef BUNDLEFACTOR
#define BUNDLEFACTOR 128
#endif

typedef struct { int xmin, ymin, xmax, ymax; } MBR;
typedef struct { int xmin, ymin, xmax, ymax; } Rect;


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

typedef struct RTreeStats
{
    int totalNodes;
    int leafNodes;
    int internalNodes;
    int maxDepth;
} RTreeStats;

/* rtreefunction.h additions */

typedef struct {
    int num_nodes;
    int num_rects;

    /* per node arrays length = num_nodes */
    int *is_leaf;      /* 1 or 0 */
    int *xmin, *ymin, *xmax, *ymax;
    int *child_off;    /* CSR style children start offset in child_index, -1 for leaf */
    int *child_cnt;    /* number of children, 0 for leaf */
    int *first_rect;   /* start offset into rect pools, -1 for internal */
    int *rect_cnt;     /* number of rects in this leaf, 0 for internal */

    /* children index pool length = sum(child_cnt) */
    int *child_index;

    /* leaf rectangle pools length = num_rects */
    int *rxmin, *rymin, *rxmax, *rymax;
} FlatRTree; 

/* build and destroy */
int  flatten_rtree_bfs(const Node *root, FlatRTree *F);
void free_flat_rtree(FlatRTree *F);

/* debug print */
void print_flat_rtree(const FlatRTree *F, int max_nodes, int max_rects);

/* simple CPU search on the flattened structure */
int flat_search_count(const FlatRTree *F, Rect q);



Rect *readRectsFromFile(const char *filename, int *num_rects);
//void printRects(Rect rects[], int num_rects);
Node *createRTree_STR(Rect *rectArr, int low, int high);
void printRTree(Node *node, int level);
//int searchRTree(Node *node, Rect queryRect, int q);
void Zsorting(Rect rects[], int num_rects);
//void print_serialisedtree(int node_index, int depth, SerializedNode *serialized_tree);
//Node *getSubtree(Node *root, int targetIndex);
void printRTreeStats(Node *root);
//int get_rtree_memory_size(Node *node);
//size_t get_serialized_size(Node *node);
//int serialize_rtree_bfs(Node *root, SerializedNode *out);
//int serialize_rtree_bfs_wrapper(Node *root, SerializedNode **out_tree);
//void print_serialized_tree(const SerializedNode *tree, int total_nodes);
Rect *selectDataDataset(int *numRects, int option);
Rect *selectQueryDataset(int *numQuery, int dataset_option);

#endif /* RTREE_H */
