#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#include "rtreefunction.h"

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

typedef struct Queue {
    Node **buf;
    int head, tail, cap;
} Queue;

static void q_init(Queue *q, int cap) {
    q->buf = (Node**)malloc((size_t)cap * sizeof(Node*));
    q->head = q->tail = 0;
    q->cap = cap;
}

static void q_free(Queue *q) { free(q->buf); }
static void q_push(Queue *q, Node *x) { q->buf[q->tail++] = x; }
static Node* q_pop(Queue *q) { return q->buf[q->head++]; }
static int q_empty(Queue *q) { return q->head == q->tail; }


static void count_nodes_rects(Node *root, int *nodes_out, int *rects_out) {
    *nodes_out = 0;
    *rects_out = 0;
    if (!root) return;
    Queue q; q_init(&q, 1<<20); // temporary large buffer for counting only
    q_push(&q, root);
    while (!q_empty(&q)) {
        Node *u = q_pop(&q);
        (*nodes_out)++;
        if (u->isLeaf) {
            *rects_out += u->count;
        } else {
            for (int i = 0; i < u->count; ++i) q_push(&q, u->children[i]);
        }
    }
    q_free(&q);
}


static inline int rect_overlap_rect(int axmin,int aymin,int axmax,int aymax,
                                    int bxmin,int bymin,int bxmax,int bymax) {
    if (axmin > bxmax || axmax < bxmin) return 0;
    if (aymin > bymax || aymax < bymin) return 0;
    return 1;
}

static inline int mbr_overlap_rect(const FlatRTree *F, int i, const Rect q) {
    return rect_overlap_rect(F->xmin[i],F->ymin[i],F->xmax[i],F->ymax[i],
                             q.xmin,q.ymin,q.xmax,q.ymax);
}

/* pointer to index mapping for fast child lookup */
typedef struct { const Node *ptr; int idx; } PtrIdx;

static int cmp_ptridx(const void *a, const void *b) {
    const PtrIdx *pa = (const PtrIdx*)a;
    const PtrIdx *pb = (const PtrIdx*)b;
    if (pa->ptr < pb->ptr) return -1;
    if (pa->ptr > pb->ptr) return 1;
    return 0;
}

static int find_idx(PtrIdx *map, int n, const Node *p) {
    PtrIdx key; key.ptr = p; key.idx = -1;
    PtrIdx *res = (PtrIdx*)bsearch(&key, map, n, sizeof(PtrIdx), cmp_ptridx);
    return res ? res->idx : -1;
}

/* breadth-first collect with separate capacities */
static int bfs_collect_nodes(const Node *root, const Node ***out_nodes, int *out_n) {
    if (!root) { *out_nodes = NULL; *out_n = 0; return 0; }

    int qcap = 4096, ocap = 4096;
    const Node **queue = (const Node**)malloc(sizeof(Node*) * qcap);
    const Node **order = (const Node**)malloc(sizeof(Node*) * ocap);
    if (!queue || !order) { free(queue); free(order); return -1; }

    int qh = 0, qt = 0, n = 0;
    queue[qt++] = root;

    while (qh < qt) {
        const Node *u = queue[qh++];

        if (n == ocap) {
            ocap *= 2;
            const Node **tmp = (const Node**)realloc(order, sizeof(Node*) * ocap);
            if (!tmp) { free(queue); free(order); return -1; }
            order = tmp;
        }
        order[n++] = u;

        if (!u->isLeaf) {
            for (int i = 0; i < u->count; ++i) {
                const Node *c = u->children ? u->children[i] : NULL;
                if (!c) continue; /* guard against accidental NULLs */
                if (qt == qcap) {
                    qcap *= 2;
                    const Node **tmpq = (const Node**)realloc(queue, sizeof(Node*) * qcap);
                    if (!tmpq) { free(queue); free(order); return -1; }
                    queue = tmpq;
                }
                queue[qt++] = c;
            }
        }
    }
    free(queue);
    *out_nodes = order;
    *out_n = n;
    return 0;
}

int flatten_rtree_bfs(const Node *root, FlatRTree *F) {
    memset(F, 0, sizeof(*F));
    const Node **nodes = NULL; int N = 0;
    if (bfs_collect_nodes(root, &nodes, &N) != 0) return -1;
    if (N == 0) { free(nodes); return 0; }

    /* pointer -> index map */
    PtrIdx *map = (PtrIdx*)malloc(sizeof(PtrIdx) * N);
    if (!map) { free(nodes); return -1; }
    for (int i = 0; i < N; ++i) { map[i].ptr = nodes[i]; map[i].idx = i; }
    qsort(map, N, sizeof(PtrIdx), cmp_ptridx);

    /* count totals */
    size_t total_children = 0, total_rects = 0;
    for (int i = 0; i < N; ++i) {
        const Node *u = nodes[i];
        if (u->isLeaf) total_rects   += (u->count > 0 ? (size_t)u->count : 0);
        else           total_children+= (u->count > 0 ? (size_t)u->count : 0);
    }

    /* allocate */
    F->num_nodes = N;
    F->num_rects = (int)total_rects;

    #define MALLOC_ARR(name, type, len) \
        F->name = (type*)malloc(sizeof(type) * ((len) > 0 ? (len) : 1)); \
        if (!F->name) { free(nodes); free(map); free_flat_rtree(F); return -1; }

    MALLOC_ARR(is_leaf,    int, N)
    MALLOC_ARR(xmin,       int, N)
    MALLOC_ARR(ymin,       int, N)
    MALLOC_ARR(xmax,       int, N)
    MALLOC_ARR(ymax,       int, N)
    MALLOC_ARR(child_off,  int, N)
    MALLOC_ARR(child_cnt,  int, N)
    MALLOC_ARR(first_rect, int, N)
    MALLOC_ARR(rect_cnt,   int, N)
    MALLOC_ARR(child_index,int, total_children)
    MALLOC_ARR(rxmin,      int, total_rects)
    MALLOC_ARR(rymin,      int, total_rects)
    MALLOC_ARR(rxmax,      int, total_rects)
    MALLOC_ARR(rymax,      int, total_rects)
    #undef MALLOC_ARR

    for (int i = 0; i < N; ++i) {
        const Node *u = nodes[i];
        F->is_leaf[i] = u->isLeaf ? 1 : 0;
        F->xmin[i] = u->mbr.xmin; F->ymin[i] = u->mbr.ymin;
        F->xmax[i] = u->mbr.xmax; F->ymax[i] = u->mbr.ymax;
        F->child_off[i] = -1; F->child_cnt[i] = 0;
        F->first_rect[i] = -1; F->rect_cnt[i] = 0;
    }

    size_t co = 0, ro = 0;
    for (int i = 0; i < N; ++i) {
        const Node *u = nodes[i];
        if (!u->isLeaf) {
            int cnt = u->count;
            if (cnt < 0) {
                fprintf(stderr, "flatten warn: negative child count at node %d, clamped to 0\n", i);
                cnt = 0;
            }
            F->child_off[i] = (int)co;
            F->child_cnt[i] = cnt;

            for (int k = 0; k < cnt; ++k) {
                const Node *cp = (u->children ? u->children[k] : NULL);
                if (!cp) continue;
                int cidx = find_idx(map, N, cp);
                if (cidx < 0) {
                    fprintf(stderr, "flatten warn: child not found (node %d, k=%d)\n", i, k);
                    continue;
                }
                if (co >= total_children) {
                    fprintf(stderr, "flatten overflow: co=%zu >= total_children=%zu at node %d\n", co, total_children, i);
                    free(nodes); free(map); free_flat_rtree(F); return -1;
                }
                F->child_index[co++] = cidx;
            }
        } else {
            int rcnt = u->count;
            if (rcnt < 0) {
                fprintf(stderr, "flatten warn: negative rect count at leaf %d, clamped to 0\n", i);
                rcnt = 0;
            }
            F->first_rect[i] = (int)ro;
            F->rect_cnt[i]   = rcnt;

            for (int r = 0; r < rcnt; ++r) {
                if (ro >= total_rects) {
                    fprintf(stderr, "flatten overflow: ro=%zu >= total_rects=%zu at leaf %d\n", ro, total_rects, i);
                    free(nodes); free(map); free_flat_rtree(F); return -1;
                }
                const Rect rr = u->rects[r];
                F->rxmin[ro] = rr.xmin; F->rymin[ro] = rr.ymin;
                F->rxmax[ro] = rr.xmax; F->rymax[ro] = rr.ymax;
                ++ro;
            }
        }
    }

    if (co != total_children) {
        fprintf(stderr, "flatten note: co=%zu total_children=%zu\n", co, total_children);
    }
    if (ro != total_rects) {
        fprintf(stderr, "flatten note: ro=%zu total_rects=%zu\n", ro, total_rects);
    }

    free(nodes);
    free(map);
    return 0;
}


void free_flat_rtree(FlatRTree *F) {
    if (!F) return;
    free(F->is_leaf);
    free(F->xmin); free(F->ymin); free(F->xmax); free(F->ymax);
    free(F->child_off); free(F->child_cnt); free(F->child_index);
    free(F->first_rect); free(F->rect_cnt);
    free(F->rxmin); free(F->rymin); free(F->rxmax); free(F->rymax);
    memset(F, 0, sizeof(*F));
}

/* debug print */
void print_flat_rtree(const FlatRTree *F, int max_nodes, int max_rects) {
    if (!F || F->num_nodes == 0) { printf("FlatRTree empty\n"); return; }
    int N = F->num_nodes;
    int R = F->num_rects;
    if (max_nodes <= 0 || max_nodes > N) max_nodes = N;
    if (max_rects <= 0 || max_rects > R) max_rects = R;

    printf("FlatRTree summary\n");
    printf("nodes %d  rects %d\n", F->num_nodes, F->num_rects);

    printf("\nfirst nodes up to %d\n", max_nodes);
    for (int i = 0; i < max_nodes; ++i) {
        printf("node %d  leaf %d  mbr [%d %d %d %d]  ",
               i, F->is_leaf[i], F->xmin[i],F->ymin[i],F->xmax[i],F->ymax[i]);
        if (F->is_leaf[i]) {
            printf("first_rect %d  rect_cnt %d\n", F->first_rect[i], F->rect_cnt[i]);
        } else {
            printf("child_off %d  child_cnt %d  children:", F->child_off[i], F->child_cnt[i]);
            int off = F->child_off[i], cnt = F->child_cnt[i];
            for (int k = 0; k < cnt && k < 8; ++k) printf(" %d", F->child_index[off + k]);
            if (cnt > 8) printf(" ...");
            printf("\n");
        }
    }

    printf("\nfirst rects up to %d\n", max_rects);
    for (int r = 0; r < max_rects; ++r) {
        printf("rect %d  [%d %d %d %d]\n", r, F->rxmin[r],F->rymin[r],F->rxmax[r],F->rymax[r]);
    }
}

/* simple CPU search counts how many rects overlap q */
int flat_search_count(const FlatRTree *F, Rect q) {
    if (!F || F->num_nodes == 0) return 0;

    /* explicit stack */
    int cap = 256;
    int *stack = (int*)malloc(sizeof(int)*cap);
    int sp = 0;
    stack[sp++] = 0; /* root at index zero */

    int hits = 0;
    while (sp) {
        int i = stack[--sp];
        if (!mbr_overlap_rect(F, i, q)) continue;

        if (F->is_leaf[i]) {
            int off = F->first_rect[i];
            int cnt = F->rect_cnt[i];
            for (int r = 0; r < cnt; ++r) {
                int j = off + r;
                if (rect_overlap_rect(F->rxmin[j],F->rymin[j],F->rxmax[j],F->rymax[j],
                                      q.xmin,q.ymin,q.xmax,q.ymax)) {
                    ++hits;
                }
            }
        } else {
            int off = F->child_off[i];
            int cnt = F->child_cnt[i];
            if (sp + cnt > cap) {
                cap = (sp + cnt) * 2;
                int *tmp = (int*)realloc(stack, sizeof(int)*cap);
                if (!tmp) { free(stack); return hits; }
                stack = tmp;
            }
            for (int k = 0; k < cnt; ++k) {
                stack[sp++] = F->child_index[off + k];
            }
        }
    }
    free(stack);
    return hits;
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
        "/home/tjv7w/projects/Rtree_GPU/data/Data/uniform_16M.csv",
        "/home/tjv7w/projects/Rtree_GPU/data/Data/Full_park_9.96M.csv",
        "/home/tjv7w/projects/Rtree_GPU/data/Data/mbrs_sports_999k.csv",
        "/home/tjv7w/projects/Rtree_GPU/data/Data/lakes_mbr_int.csv",
        "/home/tjv7w/projects/Rtree_GPU/data/Data/buildings_mbr_57M.csv"};

    if (option < 1 || option > 5)
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
    { /* 16M */
        static const char *synthetic_paths[] = {
            "/home/tjv7w/projects/Rtree_GPU/data/Query/16M/uniform_16M_1pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/16M/uniform_16M_5pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/16M/uniform_16M_10pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/16M/uniform_16M_25pct.csv"};

        printf("\nChoose the Query Dataset for 16M:\n"
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
    { /* Parks (9.9M) */
        static const char *parks_paths[] = {
            "/home/tjv7w/projects/Rtree_GPU/data/Query/FullParks/parks_mbr_int_4col_1pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/FullParks/parks_mbr_int_4col_5pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/FullParks/parks_mbr_int_4col_10pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/FullParks/parks_mbr_int_4col_25pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/FullParks/parks_mbr_int_4col_50pct.csv"};
        printf("\nChoose the Query Dataset for parks(9.9M):\n"
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
    case 3:
    { /* Sports (999k) */
        static const char *sports_paths[] = {
            "/home/tjv7w/projects/Rtree_GPU/data/Query/mbrs_sports_999k/mbrs_sports_999k_1%.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/mbrs_sports_999k/mbrs_sports_999k_5%.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/mbrs_sports_999k/mbrs_sports_999k_10%.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/mbrs_sports_999k/mbrs_sports_999k_25%.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/mbrs_sports_999k/mbrs_sports_999k_50%.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/mbrs_sports_999k/mbrs_sports_999k_75%.csv"};
        printf("\nChoose the Query Dataset for Sports(999k):\n"
               "\t1. 1%%\n"
               "\t2. 5%%\n"
               "\t3. 10%%\n"
               "\t4. 25%%\n"
               "\t5. 50%%\n"
               "\t6. 75%%\n"
               "Enter your option (1-6): ");
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
    { /* Lakes (8M) */
        static const char *lakes_paths[] = {
            "/home/tjv7w/projects/Rtree_GPU/data/Query/lakes_mbr_int/lakes_mbr_int_1_.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/lakes_mbr_int/lakes_mbr_int_5_.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/lakes_mbr_int/lakes_mbr_int_10_.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/lakes_mbr_int/lakes_mbr_int_25_.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/lakes_mbr_int/lakes_mbr_int_50_.csv"};
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
    case 5:
    { /* Buildings (57M) */
        static const char *buildings_paths[] = {
            "/home/tjv7w/projects/Rtree_GPU/data/Query/Building_14M/buildings_mbr_int_4col_12.5pct_1pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/Building_14M/buildings_mbr_int_4col_12.5pct_5pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/Building_14M/buildings_mbr_int_4col_12.5pct_10pct.csv",
            "/home/tjv7w/projects/Rtree_GPU/data/Query/Building_14M/buildings_mbr_int_4col_12.5pct_25pct.csv"};
        printf("\nChoose the Query Dataset for Buildings(57M):\n"
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
        paths = buildings_paths;
        npaths = sizeof(buildings_paths) / sizeof(buildings_paths[0]);
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

