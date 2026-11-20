#pragma once
#include <cuda_runtime.h>

extern "C" {
#include "rtreefunction.h"   // must define Rect and FlatRTree
}

// Device-side view of your flattened tree + pool lengths for bounds checks
struct DevFlatRTree {
    int  num_nodes;
    int  num_rects;

    // node arrays (length = num_nodes)
    int *is_leaf;
    int *xmin; int *ymin; int *xmax; int *ymax;
    int *child_off;
    int *child_cnt;
    int *first_rect;
    int *rect_cnt;

    // pools
    int    *child_index;     // length = child_pool_len
    size_t  child_pool_len;

    int    *rxmin; int *rymin; int *rxmax; int *rymax;   // each length = rect_pool_len
    size_t  rect_pool_len;
};

// Build / free device copy
cudaError_t make_device_flat(const FlatRTree *F, DevFlatRTree *dF);
cudaError_t free_device_flat(DevFlatRTree *dF);

// Launch search over queries
void rtree_gpu_search(const DevFlatRTree *dF,
                      const Rect *d_queries,
                      int num_queries,
                      int *d_overlap_counts,
                      int threads_per_block);
