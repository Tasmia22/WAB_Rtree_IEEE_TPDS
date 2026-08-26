#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <common.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <inttypes.h>
#include <stdarg.h>


#include "rtree.h"

// Function to initialize a bounding box
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

static inline int rect_center_x(Rect r)
{
    return (int)(((int64_t)r.xmin + (int64_t)r.xmax) / 2);
}

static inline int rect_center_y(Rect r)
{
    return (int)(((int64_t)r.ymin + (int64_t)r.ymax) / 2);
}

static inline Rect rect_to_point(Rect r)
{
    int cx = rect_center_x(r);
    int cy = rect_center_y(r);
    Rect p = {cx, cy, cx, cy};
    return p;
}

// Function to create a leaf node
Node *createLeaf(Rect *rectArr, int low, int high)
{
    Node *leaf = (Node *)malloc(sizeof(Node));
    leaf->isLeaf = 1;
    leaf->count = high - low + 1;
    leaf->rects = (Rect *)malloc(leaf->count * sizeof(Rect));

    // Initialize MBR and update with each rectangle
    initMBR(&leaf->mbr);
    for (int i = low; i <= high; i++)
    {
        leaf->rects[i - low] = rectArr[i];
        updateMBRWithRect(&leaf->mbr, rectArr[i]);
    }
    return leaf;
}

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

Node *createRTree(Rect *rectArr, int low, int high)
{
    return createRTree_STR(rectArr, low, high);
}

int countNodesBFS(Node *root)
{
    if (!root)
        return 0;

    int count = 0;
    size_t queue_cap = 1024;
    Node **queue = (Node **)malloc(queue_cap * sizeof(Node *));
    if (!queue)
    {
        perror("malloc queue");
        return 0;
    }

    size_t front = 0, rear = 0;
    queue[rear++] = root;

    while (front < rear)
    {
        Node *curr = queue[front++];
        count++;
        if (!curr->isLeaf)
        {
            for (int i = 0; i < curr->count; i++)
            {
                if (rear == queue_cap)
                {
                    size_t new_cap = queue_cap * 2;
                    Node **grown = (Node **)realloc(queue, new_cap * sizeof(Node *));
                    if (!grown)
                    {
                        perror("realloc queue");
                        free(queue);
                        return 0;
                    }
                    queue = grown;
                    queue_cap = new_cap;
                }
                queue[rear++] = curr->children[i];
            }
        }
    }

    free(queue);
    return count;
}

// Function to print the R-tree (for debugging)
void printRTree(Node *node, int level)
{
    for (int i = 0; i < level; i++)
        printf("  ");

    printf("Node (isLeaf=%d, count=%d, MBR=[%d, %d, %d, %d])\n",
           node->isLeaf, node->count, node->mbr.xmin, node->mbr.ymin, node->mbr.xmax, node->mbr.ymax);

    if (!node->isLeaf)
    {
        for (int i = 0; i < node->count; i++)
            printRTree(node->children[i], level + 1);
    }
}

bool isOverlap(MBR *mbr, Rect r)
{
    return !(r.xmax < mbr->xmin || r.xmin > mbr->xmax ||
             r.ymax < mbr->ymin || r.ymin > mbr->ymax);
}

static inline bool rectOverlapRect(const Rect *a, const Rect *b)
{
    return !(a->xmax < b->xmin || b->xmax < a->xmin ||
             a->ymax < b->ymin || b->ymax < a->ymin);
}

static inline bool rectOverlapMBR(const Rect *q, const MBR *m)
{
    return !(q->xmax < m->xmin || m->xmax < q->xmin ||
             q->ymax < m->ymin || m->ymax < q->ymin);
}

// int searchRTree(Node *node, Rect queryRect, int q)
// {
//     (void)q;

//     if (!isOverlap(&node->mbr, queryRect))
//         return 0;

//     if (node->isLeaf)
//     {
//         int overlaps = 0;
//         for (int i = 0; i < node->count; i++)
//         {
//             if (isOverlap((MBR *)&node->rects[i], queryRect))
//                 overlaps++;
//         }
//         return overlaps;
//     }

//     int overlaps = 0;
//     for (int i = 0; i < node->count; i++)
//         overlaps += searchRTree(node->children[i], queryRect, q);
//     return overlaps;
// }

int searchRTree(Node *node, Rect queryRect, int q)
{
    if (!node)
        return 0;
    if (!rectOverlapMBR(&queryRect, &node->mbr))
        return 0;

    int count = 0;
    if (node->isLeaf)
    {
        for (int i = 0; i < node->count; ++i)
            if (rectOverlapRect(&queryRect, &node->rects[i]))
                ++count;
    }
    else
    {
        for (int i = 0; i < node->count; ++i)
            count += searchRTree(node->children[i], queryRect, q);
    }
    return count;
}


int countNodesInSubtree(Node *root)
{
    if (root == NULL)
        return 0;

    int count = 1;
    if (!root->isLeaf)
    {
        for (int i = 0; i < root->count; i++)
            count += countNodesInSubtree(root->children[i]);
    }
    return count;
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
        count++;

    fclose(file);
    return count;
}

Rect *readRectsFromFile(const char *filename, int *num_rects)
{
    int line_count = countRectsInFile(filename);
    *num_rects = line_count;
    if (*num_rects <= 0)
        return NULL;

    Rect *rects = (Rect *)malloc((size_t)*num_rects * sizeof(Rect));
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

        rects[i].xmin = (x1 < x2) ? x1 : x2;
        rects[i].ymin = (y1 < y2) ? y1 : y2;
        rects[i].xmax = (x1 > x2) ? x1 : x2;
        rects[i].ymax = (y1 > y2) ? y1 : y2;
    }

    fclose(file);
    return rects;
}

void printRects(Rect rects[], int num_rects)
{
    for (int i = 0; i < num_rects; i++)
    {
        printf("Rect %d: [(%d, %d), (%d, %d)]\n",
               i,
               rects[i].xmin, rects[i].ymin,
               rects[i].xmax, rects[i].ymax);
    }
    printf("\n");
}

int serialize_rtree(Node *node, SerializedNode *s_tree, int *cur_idx)
{
    if (node == NULL)
        return -1;

    int idx = (*cur_idx)++;
    s_tree[idx].isLeaf = node->isLeaf;
    s_tree[idx].count = node->count;
    s_tree[idx].mbr = node->mbr;

    if (node->isLeaf)
    {
        memcpy(s_tree[idx].rects, node->rects, (size_t)node->count * sizeof(Rect));
    }
    else
    {
        for (int i = 0; i < node->count; i++)
        {
            int child_idx = serialize_rtree(node->children[i], s_tree, cur_idx);
            s_tree[idx].children[i] = child_idx;
        }
    }

    return idx;
}

int serialize_rtree_wrapper(Node *root, SerializedNode **out_tree)
{
    if (!root)
        return -1;

    int total_nodes = countNodesInSubtree(root);
    if (total_nodes <= 0)
    {
        fprintf(stderr, "Tree is empty or corrupted.\n");
        return -1;
    }

    *out_tree = (SerializedNode *)malloc((size_t)total_nodes * sizeof(SerializedNode));
    if (*out_tree == NULL)
    {
        perror("Failed to allocate memory for serialized R-tree");
        return -1;
    }

    int index = 0;
    int root_idx = serialize_rtree(root, *out_tree, &index);
    if (root_idx != 0)
        fprintf(stderr, "Warning: root serialized at index %d (expected 0)\n", root_idx);

    return index;
}

int serialize_rtree_bfs(Node *root, SerializedNode *out, int total_nodes)
{
    if (!root || !out || total_nodes <= 0)
        return -1;

    Node **queue = (Node **)malloc((size_t)total_nodes * sizeof(Node *));
    if (!queue)
    {
        perror("malloc failed");
        return -1;
    }

    int front = 0;
    int rear = 0;
    int index = 0;
    queue[rear++] = root;

    while (front < rear)
    {
        Node *node = queue[front++];
        int this_index = index++;
        SerializedNode *sn = &out[this_index];

        memset(sn, 0, sizeof(*sn));
        sn->isLeaf = node->isLeaf;
        sn->count = node->count;
        sn->mbr = node->mbr;

        if (node->isLeaf)
        {
            memcpy(sn->rects, node->rects, (size_t)node->count * sizeof(Rect));
        }
        else
        {
            for (int i = 0; i < node->count; i++)
            {
                if (rear >= total_nodes)
                {
                    fprintf(stderr, "serialize_rtree_bfs: queue overflow\n");
                    free(queue);
                    return -1;
                }
                sn->children[i] = rear;
                queue[rear++] = node->children[i];
            }
        }
    }

    free(queue);
    return index;
}

int serialize_rtree_bfs_wrapper(Node *root, SerializedNode **out_tree)
{
    int total_nodes = countNodesBFS(root);
    if (total_nodes <= 0)
        return -1;

    *out_tree = (SerializedNode *)malloc((size_t)total_nodes * sizeof(SerializedNode));
    if (!*out_tree)
    {
        perror("malloc failed");
        return -1;
    }

    int serialized_count = serialize_rtree_bfs(root, *out_tree, total_nodes);
    if (serialized_count < 0)
    {
        free(*out_tree);
        *out_tree = NULL;
        return -1;
    }

    return serialized_count;
}

void print_serialized_tree(const SerializedNode *tree, int total_nodes)
{
    for (int i = 0; i < total_nodes; i++)
    {
        printf("Node[%d]: isLeaf = %d, count = %d, MBR = (%d, %d, %d, %d)\n",
               i, tree[i].isLeaf,
               tree[i].count,
               tree[i].mbr.xmin,
               tree[i].mbr.ymin,
               tree[i].mbr.xmax,
               tree[i].mbr.ymax);

        if (tree[i].isLeaf)
        {
            for (int j = 0; j < tree[i].count; j++)
            {
                Rect r = tree[i].rects[j];
                printf("    Rect[%d] = (%d, %d, %d, %d)\n", j, r.xmin, r.ymin, r.xmax, r.ymax);
            }
        }
        else
        {
            for (int j = 0; j < tree[i].count; j++)
            {
                int child = tree[i].children[j];
                printf("    Child[%d] = Node[%d]\n", j, child);
            }
        }
    }
}

void collectRTreeStats(Node *node, int depth, RTreeStats *stats)
{
    if (!node)
        return;

    stats->totalNodes++;
    if (node->isLeaf)
        stats->leafNodes++;
    else
        stats->internalNodes++;

    if (depth > stats->maxDepth)
        stats->maxDepth = depth;

    if (!node->isLeaf)
    {
        for (int i = 0; i < node->count; i++)
            collectRTreeStats(node->children[i], depth + 1, stats);
    }
}

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

size_t get_serialized_size(Node *node)
{
    return node->count * sizeof(SerializedNode);
}

void freeRTree(Node *node)
{
    if (node == NULL)
        return;

    if (!node->isLeaf)
    {
        for (int i = 0; i < node->count; i++)
            freeRTree(node->children[i]);
        free(node->children);
    }

    free(node);
}

Rect *selectDataDataset(int *numRects, int option)
{
    const char *path = NULL;

    switch (option)
    {
    case 1:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M.csv";
        break;
    case 2:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_sports_999k.csv";
        break;
    case 3:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/sports_mbr_1.7M.csv";
        break;
    case 4:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_parks_300k.csv";
        break;
    case 5:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_cemetery_168k.csv";
        break;
    case 6:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/lakes_mbr_int.csv";
        break;
    case 7:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/buildings_mbr_int_4col_12.5pct.csv";
        break;
    case 8:
        path ="/home/tjv7w/RtreeCPU_MQ/Box/roads_mbr_int_4col.csv";
        break;
    case 9:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/ParksFulldataset/parks_dataset_correct/parks_mbr_int_4col.csv";
        break;
    case 10:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/FullBuilding/buildings_mbr_int_4col.csv";
        break;
    case 11:
        path = "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/HalfBuiling/halfBuiling/data/buildings_mbr_int_4col_firsthalf.csv";
        break;
    default:
        printf("Invalid dataset option. Exiting.\n");
        exit(1);
    }

    return readRectsFromFile(path, numRects);
}

Rect *selectQueryDataset(int *numQuery, int dataset_option)
{
    const char *const *paths = NULL;
    size_t npaths = 0;
    int option = 0;

    switch (dataset_option)
    {
    case 1:
    {
        static const char *synthetic_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_1pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_5pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_10pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_25pct.csv"};
        printf("\nChoose the Query Dataset for Uniform_Box_16M:\n"
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
    case 2:
    {
        static const char *sports_999_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_25%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_50%.csv"};
        printf("\nChoose the Query Dataset for Sports(999k):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "Enter your option (1-5): ");
        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }
        paths = sports_999_paths;
        npaths = sizeof(sports_999_paths) / sizeof(sports_999_paths[0]);
        break;
    }
    case 3:
    {
        static const char *sports_17_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_25%.csv"};
        printf("\nChoose the Query Dataset for Sports(1.7M):\n"
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
        paths = sports_17_paths;
        npaths = sizeof(sports_17_paths) / sizeof(sports_17_paths[0]);
        break;
    }
    case 4:
    {
        static const char *parks_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_parks_300k/mbrs_parks_300k_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_parks_300k/mbrs_parks_300k_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_parks_300k/mbrs_parks_300k_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_parks_300k/mbrs_parks_300k_25%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_parks_300k/mbrs_parks_300k_50%.csv"};
        printf("\nChoose the Query Dataset for Parks(300k):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "Enter your option (1-5): ");
        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }
        paths = parks_paths;
        npaths = sizeof(parks_paths) / sizeof(parks_paths[0]);
        break;
    }
    case 5:
    {
        static const char *cemetery_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_25%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_50%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_cemetery_168k/mbrs_cemetery_168k_75%.csv"};
        printf("\nChoose the Query Dataset for Cemetery(168k):\n"
               "\t1. 10%%\n"
               "\t2. 25%%\n"
               "\t3. 50%%\n"
               "\t4. 75%%\n"
               "Enter your option (1-4): ");
        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }
        paths = cemetery_paths;
        npaths = sizeof(cemetery_paths) / sizeof(cemetery_paths[0]);
        break;
    }
    case 6:
    {
        static const char *lakes_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_25%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_50%.csv"};
        printf("\nChoose the Query Dataset for Lakes(8M):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "Enter your option (1-5): ");
        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }
        paths = lakes_paths;
        npaths = sizeof(lakes_paths) / sizeof(lakes_paths[0]);
        break;
    }
    case 7:
    {
        static const char *building_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_1pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_5pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_10pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_25pct.csv"};
        printf("\nChoose the Query Dataset for Buildings(14.3M):\n"
               "\t1. 1%% (143496)\n"
               "\t2. 5%% (717478)\n"
               "\t3. 10%% (1434957)\n"
               "\t4. 25%% (3587392)\n"
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
        case 8:
    {
        static const char *roads_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Roads/mbrs_roads_71M_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Roads/mbrs_roads_71M_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Roads/mbrs_roads_71M_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Roads/mbrs_roads_71M_25%.csv"};
        printf("\nChoose the Query Dataset for Roads(71M):\n"
               "\t1. 1%% (143,496)\n"
               "\t2. 5%% (717,478)\n"
               "\t3. 10%% (1,434,957)\n"
               "\t4. 25%% (3,587,392)\n"
               "Enter your option (1-4): ");
        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }
        paths = roads_paths;
        npaths = sizeof(roads_paths) / sizeof(roads_paths[0]);
        break;
    }
    case 9:
    {
        static const char *full_parks_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/ParksFulldataset/parks_dataset_correct/parks_mbr_int_4col_1pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/ParksFulldataset/parks_dataset_correct/parks_mbr_int_4col_5pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/ParksFulldataset/parks_dataset_correct/parks_mbr_int_4col_10pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/ParksFulldataset/parks_dataset_correct/parks_mbr_int_4col_25pct.csv"};
        printf("\nChoose the Query Dataset for Full Parks:\n"
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
        paths = full_parks_paths;
        npaths = sizeof(full_parks_paths) / sizeof(full_parks_paths[0]);
        break;
    }
    case 10:
    {
        static const char *full_building_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/FullBuilding/Querymbrs_buildings_114M/mbrs_buildings_114M_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/FullBuilding/Querymbrs_buildings_114M/mbrs_buildings_114M_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/FullBuilding/Querymbrs_buildings_114M/mbrs_buildings_114M_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/FullBuilding/Querymbrs_buildings_114M/mbrs_buildings_114M_25%.csv"};
        printf("\nChoose the Query Dataset for FullBuilding:\n"
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
        paths = full_building_paths;
        npaths = sizeof(full_building_paths) / sizeof(full_building_paths[0]);
        break;
    }
    case 11:
    {
        static const char *half_building_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_1pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_5pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_10pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_25pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/HalfBuiling/halfBuiling/Query/buildings_mbr_int_4col_firsthalf_query_1pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/HalfBuiling/halfBuiling/Query/buildings_mbr_int_4col_firsthalf_query_5pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/HalfBuiling/halfBuiling/Query/buildings_mbr_int_4col_firsthalf_query_10pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/HalfBuiling/halfBuiling/Query/buildings_mbr_int_4col_firsthalf_query_25pct.csv"};
        printf("\nChoose the Query Dataset for HalfBuilding:\n"
               "\t1. 143,481\n"
               "\t2. 717,405\n"
               "\t3. 1,434,809\n"
               "\t4. 3,587,023\n"
                "\t5. 1%%\n"
               "\t6. 5%%\n"
               "\t7. 10%%\n"
               "\t8. 25%%\n"
               "Enter your option (1-8): ");
        if (scanf(" %d", &option) != 1)
        {
            printf("Invalid input.\n");
            exit(1);
        }
        paths = half_building_paths;
        npaths = sizeof(half_building_paths) / sizeof(half_building_paths[0]);
        break;
    }
    default:
        printf("Invalid dataset option. Exiting.\n");
        exit(1);
    }

    if (option < 1 || (size_t)option > npaths)
    {
        printf("Invalid option. Exiting.\n");
        exit(1);
    }

    return readRectsFromFile(paths[option - 1], numQuery);
}

static inline double sec_since(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}


static void log_both(FILE *log_file, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (log_file)
    {
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        va_end(ap);
    }
}


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
    uint64_t dpu_total_overlaps)
{
    printRTreeStats(root);

    log_both(log_file, "\n\n\t\tSUMMARY\n");
    log_both(log_file, "DPU(s): %u\tBUNDLEFACTOR: %d\tTasklet(s): %d\n",
             nr_of_dpus, BUNDLEFACTOR, NR_TASKLETS);
    log_both(log_file, "Dataset size: %d\tQuery size: %d\n", numRects, numQuery);

    bool host_search_skipped = (search_time < 0.0);
    log_both(log_file, "CPU R-tree construction time: %.2f s\t", rtree_construction_time);
    if (!host_search_skipped)
    {
        log_both(log_file, "Search Time in HOST: %.2f s\n", search_time);
        log_both(log_file, "\nTotal Time In CPU (construction+search): %.2f s\n",
                 rtree_construction_time + search_time);
    }
    else
    {
        log_both(log_file, "Search Time in HOST: SKIPPED (SKIP_CPU_VERIFY=1)\n");
        log_both(log_file, "\nTotal Time In CPU (construction only): %.2f s\n",
                 rtree_construction_time);
    }

    log_both(log_file, "\nR-tree distribution/setup time to DPUs (post-serialization): %.2f s", tree_transfer_time);
    log_both(log_file, "\nKernel-only time (sum over batches): %.2f s\n", kernel_time_total);

    if (!host_search_skipped && kernel_time_total > 0.0)
        log_both(log_file, "\nKernel Speedup (Host/Kernel)=%.2f", search_time / kernel_time_total);
    else if (host_search_skipped)
        log_both(log_file, "\nKernel Speedup (Host/Kernel)=SKIPPED (host search skipped)");

    double total_dpu_time = sec_since(t4, t7);
    log_both(log_file, "\nTotal DPU Time (Communication+search): %.2f s\n\n", total_dpu_time);

    if (!host_search_skipped && total_dpu_time > 0.0)
        log_both(log_file, "\nend-to-end Speedup (Host/DPU ratio): %.2f\n", search_time / total_dpu_time);
    else if (total_dpu_time <= 0.0)
        log_both(log_file, "Host/DPU ratio: undefined (DPU time is 0)\n");
    else
        log_both(log_file, "\nend-to-end Speedup (Host/DPU ratio): SKIPPED (host search skipped)\n");

    if (!host_search_skipped && total_dpu_time > 0.0 && total_dpu_time < search_time)
        log_both(log_file, "DPU is %.2f times Faster\n\n", search_time / total_dpu_time);
    else if (!host_search_skipped)
        log_both(log_file, "DPU is Slower\n\n");
    else
        log_both(log_file, "DPU vs Host search verdict: SKIPPED (host search skipped)\n\n");

    if (cpu_total_overlaps == UINT64_MAX)
    {
        log_both(log_file,
                 "\nCPU overlap verification: SKIPPED (SKIP_CPU_VERIFY=1)\n"
                 "DPU overlap total: %" PRIu64 "\n\n",
                 dpu_total_overlaps);
    }
    else if (cpu_total_overlaps == dpu_total_overlaps)
    {
        log_both(log_file,
                 "\n                       Matched!!!!    \n Overlaps found by both DPU and CPU : %" PRIu64 "\n\n",
                 cpu_total_overlaps);
    }
    else
    {
        log_both(log_file,
                 "\n                       ERROR!!!       \n Overlaps found by CPU %" PRIu64 "\n",
                 cpu_total_overlaps);
        log_both(log_file,
                 "Overlaps found by DPU %" PRIu64 "\n",
                 dpu_total_overlaps);
    }
}