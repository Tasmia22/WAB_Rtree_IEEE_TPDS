#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <common.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <assert.h>   // for assert()

#include "rtree.h"

Node *copySubtree(Node *root);

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
/*
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
    */
// Function to create a leaf node (must be called only when (high - low + 1) <= BUNDLEFACTOR)

Node *createLeaf(Rect *rectArr, int low, int high)
{
    int cnt = high - low + 1;
    if (cnt <= 0)
        return NULL;

    // Enforce leaf capacity (catch any logic bugs in callers)
    assert(cnt <= BUNDLEFACTOR);

    Node *leaf = (Node *)malloc(sizeof(Node));
    if (!leaf)
        return NULL;

    leaf->isLeaf = 1;
    leaf->count = cnt;
    leaf->children = NULL; // union hygiene: leaves don't have children
    leaf->rects = (Rect *)malloc((size_t)cnt * sizeof(Rect));
    if (!leaf->rects)
    {
        free(leaf);
        return NULL;
    }

    // Initialize MBR and fill rects
    initMBR(&leaf->mbr);
    for (int i = 0; i < cnt; ++i)
    {
        Rect r = rectArr[low + i];
        leaf->rects[i] = r;
        updateMBRWithRect(&leaf->mbr, r); // or: leaf->mbr = unionJoin(&leaf->mbr, &r_mbr)
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


Node *createRTree(Rect *rectArr, int low, int high)
{
    int total = high - low + 1;
    if (total <= 0)
        return NULL;

    /* ---- Base case: make a proper leaf (≤ BUNDLEFACTOR rects) ---- */
    if (total <= BUNDLEFACTOR)
    {
        return createLeaf(rectArr, low, high); // must assert count ≤ BUNDLEFACTOR inside
    }

    /* ---- Internal node ---- */
    Node *root = (Node *)malloc(sizeof(Node));
    root->isLeaf = 0;
    root->count = 0;
    root->rects = NULL;                                        // union hygiene
    root->children = (Node **)malloc(FANOUT * sizeof(Node *)); // upper bound; we’ll fill up to FANOUT

    /* 1) Decide how many children we want at this node.
       Aim for children of size ~ BUNDLEFACTOR (if next level were leaves),
       but don’t exceed FANOUT. */
    int k = (total + BUNDLEFACTOR - 1) / BUNDLEFACTOR; // target #children
    if (k > FANOUT)
        k = FANOUT;
    if (k < 1)
        k = 1;

    /* 2) Choose a 2D grid Sx * Sy ≈ k for STR grouping */
    int Sx = (int)floor(sqrt((double)k));
    if (Sx < 1)
        Sx = 1;
  //  int Sy = (k + Sx - 1) / Sx; // ceil(k / Sx)

    /* 3) Sort by X and split into Sx X-slabs (balanced by count) */
    qsort(&rectArr[low], total, sizeof(Rect), compareByXCenter);

    int base_slab = total / Sx;
    int rem_slab = total % Sx;
    int x_start = low;

    for (int sx = 0; sx < Sx && root->count < FANOUT; ++sx)
    {
        int this_slab = base_slab + (sx < rem_slab ? 1 : 0);
        int slab_low = x_start;
        int slab_high = slab_low + this_slab - 1;
        x_start = slab_high + 1;

        if (slab_low > slab_high)
            continue;

        /* 4) Within each slab, sort by Y */
        qsort(&rectArr[slab_low], this_slab, sizeof(Rect), compareByYCenter);

        /* 5) Allocate Y-groups in this slab proportionally so that the
              total across slabs is about k (and never exceeds FANOUT). */
        // First: ideal groups for this slab
        double frac = (double)this_slab / (double)total;
        int groups_slab = (int)llround(frac * (double)k);
        if (groups_slab < 1)
            groups_slab = 1;

        // Clamp so we never exceed FANOUT overall
        if (root->count + groups_slab > FANOUT)
            groups_slab = FANOUT - root->count;

        // Split this slab into groups_slab contiguous Y-groups
        int base_grp = this_slab / groups_slab;
        int rem_grp = this_slab % groups_slab;

        int y_start = slab_low;
        for (int gy = 0; gy < groups_slab && root->count < FANOUT; ++gy)
        {
            int gsize = base_grp + (gy < rem_grp ? 1 : 0);
            int group_low = y_start;
            int group_high = group_low + gsize - 1;
            y_start = group_high + 1;

            if (group_low > group_high)
                continue;

            /* 6) Recurse: base case will ensure leaves are ≤ BUNDLEFACTOR.
                  These children will be internal if gsize > BUNDLEFACTOR. */
            Node *child = createRTree(rectArr, group_low, group_high);
            if (child)
                root->children[root->count++] = child;
        }
    }

    /* Safety: in pathological rounding cases, ensure we created at least 1 child. */
    if (root->count == 0)
    {
        // Fallback: split in half and recurse
        int mid = low + total / 2 - 1;
        if (mid < low)
            mid = low;
        Node *a = createRTree(rectArr, low, mid);
        Node *b = createRTree(rectArr, mid + 1, high);
        if (a)
            root->children[root->count++] = a;
        if (b && root->count < FANOUT)
            root->children[root->count++] = b;
    }

    /* 7) Build the MBR from children */
    initMBR(&root->mbr);
    for (int i = 0; i < root->count; ++i)
    {
        root->mbr = unionJoin(&root->mbr, &root->children[i]->mbr);
    }

    /* Optional invariants (enable in debug):
       assert(root->count >= 1 && root->count <= FANOUT); */
    return root;
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
        //        for (int i = 0; i < node->count; i++)
        //        {
        //            for (int j = 0; j <= level; j++)
        //                printf("  ");
        //           printf("Rect [(%d, %d), (%d, %d)]\n", node->rects[i].xmin, node->rects[i].ymin, node->rects[i].xmax, node->rects[i].ymax);
        //
        //        }
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
                // if (q == 6)
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

int serialize_rtree(Node *node, SerializedNode *serialized_tree, int *current_index)
{
    if (node == NULL)
        return -1; // Invalid index if node is NULL

    int index = (*current_index)++; // Current node's index in the serialized array

    // Populate the serialized node
    serialized_tree[index].isLeaf = node->isLeaf;
    serialized_tree[index].count = node->count;
    serialized_tree[index].mbr = node->mbr;

    if (node->isLeaf)
    {
        // Copy rectangles into the serialized node
        memcpy(serialized_tree[index].rects, node->rects, node->count * sizeof(Rect));
    }
    else
    {
        // Recursively serialize child nodes and store their indices
        for (int i = 0; i < node->count; i++)
        {
            serialized_tree[index].children[i] = serialize_rtree(node->children[i], serialized_tree, current_index);
        }
    }

    return index; // Return the index of this node
}

// Wrapper function to start serialization
int serialize_rtree_wrapper(Node *root, SerializedNode **output)
{
    // Step 1: Count how many nodes will be serialized
    int max_nodes = countNodesInSubtree(root);

    // Step 2: Allocate memory for the serialized tree
    *output = (SerializedNode *)malloc(max_nodes * sizeof(SerializedNode));
    if (*output == NULL)
    {
        perror("Failed to allocate memory for serialized tree");
        return -1;
    }

    // Step 3: Perform serialization
    int current_index = 0;
    serialize_rtree(root, *output, &current_index);

    // Step 4: Return number of serialized nodes
    return current_index;
}

// Prepare serialized subtrees for transfer to DPUs.
// - Caps *nr_of_dpus_io to root->count (so you don't overrun).
// - Fills: subtree[i], serialized_trees[i], num_nodes_eachsubtree[i], subtree_mbrs[i]
// - Returns max_serialized_size (in SerializedNode units), or -1 on error.
int prepare_serialized_subtrees(
    Node *root,
    uint32_t *nr_of_dpus_io,          // in/out: requested DPUs -> actual used
    Node **subtree,                   // [*nr_of_dpus_io]
    SerializedNode **serialized_trees,// [*nr_of_dpus_io]; each becomes malloc'ed buffer
    int *num_nodes_eachsubtree,       // [*nr_of_dpus_io]
    MBR *subtree_mbrs,                // [*nr_of_dpus_io]
    bool print_first_subtree
)
{
    if (!root || root->isLeaf) {
        fprintf(stderr, "prepare_serialized_subtrees: root must be an internal node.\n");
        return -1;
    }

    uint32_t have = (uint32_t)root->count;   // number of level-2 subtrees
    if (have == 0) {
        fprintf(stderr, "prepare_serialized_subtrees: root has no children.\n");
        return -1;
    }

    if (*nr_of_dpus_io == 0) {
        fprintf(stderr, "prepare_serialized_subtrees: requested 0 DPUs.\n");
        return -1;
    }

    if (*nr_of_dpus_io > have) {
        // Cap to available subtrees so callers don't overrun
        *nr_of_dpus_io = have;
    }

    int max_serialized_size = 0;

    for (uint32_t i = 0; i < *nr_of_dpus_io; ++i) {
        subtree[i] = root->children[i];

        if (print_first_subtree && i == 0) {
            printf("\n=== Printing subtree[0] ===\n");
            printRTree(subtree[0], 0);
        }

        int n_nodes = serialize_rtree_wrapper(subtree[i], &serialized_trees[i]);
        if (n_nodes <= 0 || serialized_trees[i] == NULL) {
            fprintf(stderr, "Serialization failed or memory allocation failed for subtree %u\n", i);
            return -1;
        }

        num_nodes_eachsubtree[i] = n_nodes;
        if (n_nodes > max_serialized_size) {
            max_serialized_size = n_nodes;
        }

        // First element of each serialized buffer is the subtree root
        subtree_mbrs[i] = serialized_trees[i][0].mbr;
    }

    return max_serialized_size; // caller can use this to pad buffers uniformly
}

void print_serialisedtree(int node_index, int depth, SerializedNode *serialized_tree)
{
    if (node_index < 0)
    {
        printf("Invalid node index: %d\n", node_index);
        return;
    }

    // Print indentation for the current depth
    for (int i = 0; i < depth; i++)
    {
        printf("  ");
    }

    // Print the current node's details
    printf("Node Index: %d | ", node_index);
    printf("MBR [xmin: %d, ymin: %d, xmax: %d, ymax: %d]",
           serialized_tree[node_index].mbr.xmin, serialized_tree[node_index].mbr.ymin,
           serialized_tree[node_index].mbr.xmax, serialized_tree[node_index].mbr.ymax);
    printf(" | isLeaf: %d | count: %d\n", serialized_tree[node_index].isLeaf, serialized_tree[node_index].count);

    if (serialized_tree[node_index].isLeaf)
    {
        // Print points for a leaf node
        for (int i = 0; i < serialized_tree[node_index].count; i++)
        {
            for (int j = 0; j < depth + 1; j++)
            {
                printf("  ");
            }
            // printf("Point (%.1f, %.1f)\n", serialized_tree[node_index].points[i].x,  serialized_tree[node_index].points[i].y);
        }
    }
    else
    {
        // Recursively print child nodes for an internal node
        for (int i = 0; i < serialized_tree[node_index].count; i++)
        {
            int child_index = serialized_tree[node_index].children[i];
            printf("\n");
            for (int j = 0; j < depth + 1; j++)
            {
                printf("  ");
            }
            // printf("Node index=%d and Child Node Index: %d\n", node_index,child_index);
            print_serialisedtree(child_index, depth + 1, serialized_tree);
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
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/Uniform_Box_6M_int.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_sports_999k.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_parks_300k.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/mbrs_cemetery_168k.csv",
        "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Data/lakes_mbr_int.csv"};

    if (option < 1 || option > 6)
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
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_6M_int_1%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_6M_int_5%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_6M_int_10%.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/Rtree_Broadcast_batch_SC/Query/Synthetic_Data/Uniform_Box_6M_int_25%.csv",
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
    { /* Synthetic */
        static const char *synthetic_paths[] = {
            "//home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_1pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_5pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_10pct.csv",
            "/home/tjv7w/RtreeCPU_MQ/Box/data_prev_uniform/Uniform_16M/Uniform_16/uniform_16M_25pct.csv"};
        printf("\nChoose the Query Dataset for Synthetic Dataset:\n"
               "\t1. Uniform_16_1%%\n"
               "\t2. Uniform_16_5%%\n"
               "\t3. Uniform_16_10%%\n"
               "\t4. Uniform_16_25%%\n"
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

    case 3:
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
