#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <common.h>
#include <float.h>
#include <limits.h>
#include <math.h>

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

// Node *createRTree(Rect *rectArr, int low, int high)
// {
//     int total = high - low + 1;
//     if (total <= 0)
//         return NULL;

//     // Step 1: Sort rectangles by X, then Y (space-filling order)
//     qsort(&rectArr[low], total, sizeof(Rect), compareByXCenter);

//     // Step 2: Create leaf nodes
//     int numLeaves = (total + BUNDLEFACTOR - 1) / BUNDLEFACTOR;
//     Node **current_level = (Node **)malloc(numLeaves * sizeof(Node *));

//     for (int i = 0; i < numLeaves; i++)
//     {
//         int start = low + i * BUNDLEFACTOR;
//         int end = (start + BUNDLEFACTOR - 1 < high) ? (start + BUNDLEFACTOR - 1) : high;
//         current_level[i] = createLeaf(rectArr, start, end);
//     }

//     // Step 3: Recursively group nodes into higher internal nodes
//     while (numLeaves > 1)
//     {
//         int numParents = (numLeaves + FANOUT - 1) / FANOUT;
//         Node **next_level = (Node **)malloc(numParents * sizeof(Node *));

//         for (int i = 0; i < numParents; i++)
//         {
//             int start = i * FANOUT;
//             int end = (start + FANOUT < numLeaves) ? (start + FANOUT) : numLeaves;

//             Node *parent = (Node *)malloc(sizeof(Node));
//             parent->isLeaf = 0;
//             parent->count = end - start;
//             parent->children = (Node **)malloc(parent->count * sizeof(Node *));
//             initMBR(&parent->mbr);

//             for (int j = start; j < end; j++)
//             {
//                 parent->children[j - start] = current_level[j];
//                 parent->mbr = unionJoin(&parent->mbr, &current_level[j]->mbr);
//             }

//             next_level[i] = parent;
//         }

//         free(current_level);
//         current_level = next_level;
//         numLeaves = numParents;
//     }

//     // Final root node
//     Node *root = current_level[0];
//     free(current_level);
//     return root;
// }
/*
Node *createRTree_STR(Rect *rectArr, int low, int high)
{
    int total = high - low + 1;
    if (total <= 0)
        return NULL;

    // 1. Sort globally by X
    qsort(&rectArr[low], total, sizeof(Rect), compareByXCenter);

    // 2. Compute slices
    int numLeaves = (total + BUNDLEFACTOR - 1) / BUNDLEFACTOR;
    int numSlices = (int)ceil(sqrt((double)numLeaves));
    int sliceSize = (total + numSlices - 1) / numSlices;

    Node **leaves = (Node **)malloc(numLeaves * sizeof(Node *));
    int leafCount = 0;

    // 3. Process each slice
    for (int s = 0; s < numSlices; s++) {
        int sliceLow = low + s * sliceSize;
        int sliceHigh = sliceLow + sliceSize - 1;
        if (sliceLow > high) break;
        if (sliceHigh > high) sliceHigh = high;

        int sliceCount = sliceHigh - sliceLow + 1;
        if (sliceCount <= 0) continue;

        // Sort slice by Y
        qsort(&rectArr[sliceLow], sliceCount, sizeof(Rect), compareByYCenter);

        // 4. Pack into leaves of size BUNDLEFACTOR
        for (int i = sliceLow; i <= sliceHigh; i += BUNDLEFACTOR) {
            int end = i + BUNDLEFACTOR - 1;
            if (end > sliceHigh) end = sliceHigh;
            leaves[leafCount++] = createLeaf(rectArr, i, end);
        }
    }

    // 5. Build upper levels recursively (same as before)
    int currCount = leafCount;
    Node **current_level = leaves;

    while (currCount > 1) {
        int numParents = (currCount + FANOUT - 1) / FANOUT;
        Node **next_level = (Node **)malloc(numParents * sizeof(Node *));

        for (int i = 0; i < numParents; i++) {
            int start = i * FANOUT;
            int end = (start + FANOUT < currCount) ? (start + FANOUT) : currCount;

            Node *parent = (Node *)malloc(sizeof(Node));
            parent->isLeaf = 0;
            parent->count = end - start;
            parent->children = (Node **)malloc(parent->count * sizeof(Node *));
            initMBR(&parent->mbr);

            for (int j = start; j < end; j++) {
                parent->children[j - start] = current_level[j];
                parent->mbr = unionJoin(&parent->mbr, &current_level[j]->mbr);
            }
            next_level[i] = parent;
        }

        free(current_level);
        current_level = next_level;
        currCount = numParents;
    }

    Node *root = current_level[0];
    free(current_level);
    return root;
}
    */

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

int countNodesBFS(Node *root)
{
    if (!root)
        return 0;

    int count = 0;
    Node **queue = malloc(100000 * sizeof(Node *));
    int front = 0, rear = 0;
    queue[rear++] = root;

    while (front < rear)
    {
        Node *curr = queue[front++];
        count++;
        if (!curr->isLeaf)
        {
            for (int i = 0; i < curr->count; i++)
            {
                queue[rear++] = curr->children[i];
            }
        }
    }

    free(queue);
    return count;
}

// int serialize_rtree_bfs(Node *root, SerializedNode *out)
// {
//     if (!root)
//         return 0;

//     int index = 0;
//     Node **queue = malloc(100000 * sizeof(Node *));
//     int *index_map = malloc(100000 * sizeof(int)); // Maps Node* to its index

//     int front = 0, rear = 0;
//     queue[rear] = root;
//     index_map[rear++] = index++;

//     while (front < rear)
//     {
//         Node *node = queue[front];
//         int this_index = index_map[front++];

//         SerializedNode *sn = &out[this_index];
//         sn->isLeaf = node->isLeaf;
//         sn->count = node->count;
//         sn->mbr = node->mbr;

//         if (node->isLeaf)
//         {
//             memcpy(sn->rects, node->rects, sizeof(Rect) * node->count);
//         }
//         else
//         {
//             for (int i = 0; i < node->count; i++)
//             {
//                 queue[rear] = node->children[i];
//                 index_map[rear] = index;
//                 sn->children[i] = index; // Save index to child
//                 rear++;
//                 index++;
//             }
//         }
//     }

//     int total = index;
//     free(queue);
//     free(index_map);
//     return total;
// }

// int serialize_rtree_bfs_wrapper(Node *root, SerializedNode **out_tree)
// {
//     int total_nodes = countNodesBFS(root);
//     if (total_nodes <= 0)
//         return -1;

//     *out_tree = malloc(total_nodes * sizeof(SerializedNode));
//     if (!*out_tree)
//     {
//         perror("malloc failed");
//         return -1;
//     }

//     int serialized_count = serialize_rtree_bfs(root, *out_tree);
//     printf("serialized_count=%d",serialized_count);
//     return serialized_count;
// }


// Function to count the number of nodes in a subtree
int countNodesInSubtree(Node *root)
{
    if (root == NULL)
        return 0;

    // Start with 1 for the current node
    int count = 1;

    if (!root->isLeaf)
    {
        // Recursively count children for internal nodes
        for (int i = 0; i < root->count; i++)
        {
            count += countNodesInSubtree(root->children[i]);
        }
    }

    return count;
}
// safer BFS serializer: uses total_nodes for array sizes
int serialize_rtree_bfs(Node *root, SerializedNode *out, int total_nodes)
{
    if (!root || !out || total_nodes <= 0)
        return 0;

    Node **queue    = malloc((size_t)total_nodes * sizeof(Node *));
    int  *index_map = malloc((size_t)total_nodes * sizeof(int));
    if (!queue || !index_map) {
        perror("malloc in serialize_rtree_bfs");
        free(queue);
        free(index_map);
        return -1;
    }

    int front = 0, rear = 0;
    int index = 0;

    queue[rear]     = root;
    index_map[rear] = index;
    rear++;
    index++;

    while (front < rear)
    {
        Node *node    = queue[front];
        int this_index = index_map[front++];
        SerializedNode *sn = &out[this_index];

        sn->isLeaf = node->isLeaf;
        sn->count  = node->count;
        sn->mbr    = node->mbr;

        if (node->isLeaf)
        {
            // optional safety if you have MAX_RECTS
            // if (node->count > MAX_RECTS) { ... error ... }

            memcpy(sn->rects, node->rects, sizeof(Rect) * node->count);
        }
        else
        {
            // optional safety if you have MAX_CHILDREN
            // if (node->count > MAX_CHILDREN) { ... error ... }

            for (int i = 0; i < node->count; i++)
            {
                if (rear >= total_nodes || index >= total_nodes) {
                    fprintf(stderr,
                            "serialize_rtree_bfs overflow: rear=%d index=%d total_nodes=%d\n",
                            rear, index, total_nodes);
                    free(queue);
                    free(index_map);
                    return -1;
                }

                queue[rear]     = node->children[i];
                index_map[rear] = index;
                sn->children[i] = index;
                rear++;
                index++;
            }
        }
    }

    int total = index;
    free(queue);
    free(index_map);
    return total;
}
int serialize_rtree_bfs_wrapper(Node *root, SerializedNode **out_tree)
{
    if (!root) {
        *out_tree = NULL;
        return -1;
    }

    // you can use either countNodesBFS or countNodesInSubtree
    int total_nodes = countNodesInSubtree(root);  // or countNodesBFS(root)
    if (total_nodes <= 0) {
        fprintf(stderr, "serialize_rtree_bfs_wrapper: empty or invalid tree\n");
        *out_tree = NULL;
        return -1;
    }

    *out_tree = malloc((size_t)total_nodes * sizeof(SerializedNode));
    if (!*out_tree)
    {
        perror("malloc failed for serialized tree");
        *out_tree = NULL;
        return -1;
    }

    int serialized_count = serialize_rtree_bfs(root, *out_tree, total_nodes);
    printf("serialized_count=%d\n", serialized_count);

    if (serialized_count < 0) {
        // BFS reported an error, clean up
        free(*out_tree);
        *out_tree = NULL;
        return -1;
    }

    return serialized_count;  // total number of serialized nodes
}


// Function to print the R-tree (for debugging)
void printRTree(Node *node, int level)
{
    // printf("\n\nLevel=%d",level);
    for (int i = 0; i < level; i++)
        printf("  ");

    printf("Node (isLeaf=%d, count=%d, MBR=[%d, %d, %d, %d])\n",
           node->isLeaf, node->count, node->mbr.xmin, node->mbr.ymin, node->mbr.xmax, node->mbr.ymax);

    if (node->isLeaf)
    {
        for (int i = 0; i < node->count; i++)
        {
            for (int j = 0; j <= level; j++)
                printf("  ");
            printf("Rect [(%d, %d), (%d, %d)]\n", node->rects[i].xmin, node->rects[i].ymin, node->rects[i].xmax, node->rects[i].ymax);
        }
    }
    else
    {
        for (int i = 0; i < node->count; i++)
        {
            printRTree(node->children[i], level + 1);
        }
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
/*
int searchRTree(Node *node, Rect queryRect, int q)
{
    int count = 0;

    // If query does not overlap node's MBR, skip this subtree
    if (!isOverlap(&node->mbr, queryRect))
        return 0;

    if (node->isLeaf)
    {
        // Check each rectangle in the leaf node
        for (int i = 0; i < node->count; i++)
        {
            if (isOverlap((MBR *)&node->rects[i], queryRect))
            {
                //  if (q == 121509 ||q==186865|| q==196633||q==215315||q==220448)
                //     printf("\nQuery[%d]=[%d,%d-%d,%d] overlapped with Rect[%d,%d-%d,%d]", q, queryRect.xmin, queryRect.ymin, queryRect.xmax, queryRect.ymax, node->rects[i].xmin, node->rects[i].ymin, node->rects[i].xmax, node->rects[i].ymax);
                count++;
            }
        }
    }
    else
    {
        // Recurse into all overlapping children
        for (int i = 0; i < node->count; i++)
        {
            count += searchRTree(node->children[i], queryRect, q);
        }
    }

    return count;
}

*/
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
        {
            freeRTree(node->children[i]); // recursively free children
        }
        free(node->children); // free the child pointer array
    }

    free(node); // finally free the current node
}

Rect *selectDataDataset(int *numRects, int option)
{
    const char *paths[] = {
        "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_sports_999k.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/sports_mbr_1.7M.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_parks_300k.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_cemetery_168k.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/lakes_mbr_int.csv",
        // "/home/tjv7w/RtreeCPU_MQ/Box/Lake_data/lake_dataset_correct/lakes_mbr_int_4col.csv",
        // "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/buildings_mbr_int_4col_12.5pct.csv"};
        "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/buildings_mbr_int_first_quarter.csv"};

    if (option < 1 || option > 7)
    {
        printf("Invalid option. Exiting.\n");
        exit(1);
    }

    return readRectsFromFile(paths[option - 1], numRects);
}

Rect *selectQueryDataset(int *numQuery, int dataset_option)
{
    int option = 0;

    /* Will point at the correct table for the chosen dataset */
    const char **paths = NULL;
    size_t npaths = 0;

    switch (dataset_option)
    {
    case 1:
    { /* Synthetic */
        static const char *synthetic_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_1408.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_1pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_5pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_10pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_25pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_90k.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_180k.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_360k.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_720k.csv"};
        printf("\nChoose the Query Dataset for Synthetic Dataset:\n"
               "\t1. Uniform_Box_1408\n"
               "\t2. Uniform_Box_1%%\n"
               "\t3. Uniform_Box_5%%\n"
               "\t4. Uniform_Box_10%%\n"
               "\t5. Uniform_Box_25%%\n"
               "\t6. Uniform_Box_90k\n"
               "\t7. Uniform_Box_180k\n"
               "\t8. Uniform_Box_360k\n"
               "\t9. Uniform_Box_720k\n"
               "Enter your option (1-9): ");
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
    { /* Sports (999k) */
        static const char *sports_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_25%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/mbrs_sports_999k/mbrs_sports_999k_50%.csv",
        };
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
        paths = sports_paths;
        npaths = sizeof(sports_paths) / sizeof(sports_paths[0]);
        break;
    }
    case 3:
    { /* Sports (1.7M) */
        static const char *sports_17_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Sports_1.7M/sports_mbr_int_25%.csv",
        };
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
    { /* Parks (300k) */
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
    { /* Cemetery (168k) */
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
    { /* Lakes (8M) */
        static const char *lakes_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_25%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/lakes_mbr_int/lakes_mbr_int_50%.csv"};

            //     static const char *lakes_paths[] = {
            // "/home/tjv7w/RtreeCPU_MQ/Box/Lake_data/lake_dataset_correct/lakes_mbr_int_4col_1pct.csv",
            // "/home/tjv7w/RtreeCPU_MQ/Box/Lake_data/lake_dataset_correct/lakes_mbr_int_4col_5pct.csv",
            // "/home/tjv7w/RtreeCPU_MQ/Box/Lake_data/lake_dataset_correct/lakes_mbr_int_4col_10pct.csv",
            // "/home/tjv7w/RtreeCPU_MQ/Box/Lake_data/lake_dataset_correct/lakes_mbr_int_4col_25pct.csv",
            // "/home/tjv7w/RtreeCPU_MQ/Box/Lake_data/lake_dataset_correct/lakes_mbr_int_4col_50pct.csv"};

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
    { /* Buildings (28M) */
        static const char *building_paths[] = {
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_first_12.5%_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_first_12.5%_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_first_12.5%_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_first_12.5%_25%.csv"};
        // static const char *building_paths[] = {
        //     "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_1pct.csv",
        //     "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_5pct.csv",
        //     "/home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_10pct.csv",
        //     "//home/tjv7w/RtreeCPU_MQ/Box/Building_data/Building_Query/buildings_mbr_int_4col_12.5pct_25pct.csv"};


        printf("\nChoose the Query Dataset for Lakes(8M):\n"
               "\t1. 1%% (143496)\n"
               "\t2. 5%% (717478)\n"
               "\t3. 10%% (1434957) \n"
               "\t4. 25%% (3587392) \n"
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

    /* Validate selection */
    if (option < 1 || (size_t)option > npaths)
    {
        printf("Invalid option. Exiting.\n");
        exit(1);
    }

    /* Load and return the chosen query file */
    return readRectsFromFile(paths[option - 1], numQuery);
}
