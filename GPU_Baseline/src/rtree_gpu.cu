#include "rtree_gpu.cuh"
#include <cstdio>

#define CUDA_OK(cmd)                                                                                \
    do                                                                                              \
    {                                                                                               \
        cudaError_t e = (cmd);                                                                      \
        if (e != cudaSuccess)                                                                       \
        {                                                                                           \
            fprintf(stderr, "CUDA error %s at %s:%d\n", cudaGetErrorString(e), __FILE__, __LINE__); \
            return e;                                                                               \
        }                                                                                           \
    } while (0)

__device__ __forceinline__ bool rect_overlap_dev(int axmin, int aymin, int axmax, int aymax,
                                                 int bxmin, int bymin, int bxmax, int bymax)
{
    // inclusive edges
    return !(axmax < bxmin || bxmax < axmin || aymax < bymin || bymax < aymin);
}
__global__ void rtree_search_kernel(const DevFlatRTree dF,
                                    const Rect *queries,
                                    int num_queries,
                                    int *overlap_counts)
{
    int qid = blockIdx.x * blockDim.x + threadIdx.x;
    if (qid >= num_queries)
        return;

    Rect q = queries[qid];
    const int qxmin = q.xmin, qymin = q.ymin, qxmax = q.xmax, qymax = q.ymax;

    // increase stack to reduce accidental drops
    int stack[1024];
    int sp = 0;
    stack[sp++] = 0; // start at root

    int hits = 0;

    while (sp)
    {
        int nid = stack[--sp];

        // node AABB quick reject
        const int nxmin = dF.xmin[nid];
        const int nymin = dF.ymin[nid];
        const int nxmax = dF.xmax[nid];
        const int nymax = dF.ymax[nid];
        if (nxmax < qxmin || qxmax < nxmin || nymax < qymin || qymax < nymin)
            continue;

        if (dF.is_leaf[nid])
        {
            const int off = dF.first_rect[nid];
            const int cnt = dF.rect_cnt[nid];
            if (off >= 0 && cnt > 0 && (size_t)off + (size_t)cnt <= dF.rect_pool_len)
            {
                for (int i = 0; i < cnt; ++i)
                {
                    const int rid = off + i;
                    const int rxmin = dF.rxmin[rid], rymin = dF.rymin[rid];
                    const int rxmax = dF.rxmax[rid], rymax = dF.rymax[rid];
                    if (!(rxmax < qxmin || qxmax < rxmin || rymax < qymin || qymax < rymin))
                    {
                        ++hits;
                    }
                }
            }
        }
        else
        {
            const int off = dF.child_off[nid];
            const int cnt = dF.child_cnt[nid];
            if (off >= 0 && cnt > 0 && (size_t)off + (size_t)cnt <= dF.child_pool_len)
            {

                // *** FILTER BEFORE PUSHING ***
                for (int i = 0; i < cnt; ++i)
                {
                    const int cid = dF.child_index[off + i];
                    if (cid < 0 || cid >= dF.num_nodes)
                        continue;

                    // child bbox
                    const int cxmin = dF.xmin[cid];
                    const int cymin = dF.ymin[cid];
                    const int cxmax = dF.xmax[cid];
                    const int cymax = dF.ymax[cid];

                    // only push overlapping children
                    if (!(cxmax < qxmin || qxmax < cxmin || cymax < qymin || qymax < cymin))
                    {
                        if (sp < (int)(sizeof(stack) / sizeof(stack[0])))
                        {
                            stack[sp++] = cid;
                        }
                        // else: drop very deep/broad paths (rare after filtering)
                    }
                }
            }
        }
    }

    overlap_counts[qid] = hits;
}

// ------- helpers and API -------

static cudaError_t malloc_and_copy(int **dst, const int *src, size_t n_elems)
{
    if (n_elems == 0)
    {
        *dst = nullptr;
        return cudaSuccess;
    }
    CUDA_OK(cudaMalloc((void **)dst, n_elems * sizeof(int)));
    CUDA_OK(cudaMemcpy(*dst, src, n_elems * sizeof(int), cudaMemcpyHostToDevice));
    return cudaSuccess;
}

cudaError_t make_device_flat(const FlatRTree *F, DevFlatRTree *dF)
{
    dF->num_nodes = F->num_nodes;
    dF->num_rects = F->num_rects;

    // node arrays
    CUDA_OK(malloc_and_copy(&dF->is_leaf, F->is_leaf, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->xmin, F->xmin, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->ymin, F->ymin, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->xmax, F->xmax, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->ymax, F->ymax, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->child_off, F->child_off, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->child_cnt, F->child_cnt, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->first_rect, F->first_rect, F->num_nodes));
    CUDA_OK(malloc_and_copy(&dF->rect_cnt, F->rect_cnt, F->num_nodes));

    // compute child_index pool length robustly
    size_t child_pool_len = 0;
    for (int i = 0; i < F->num_nodes; ++i)
    {
        int off = F->child_off[i];
        int cnt = F->child_cnt[i];
        if (off >= 0 && cnt > 0)
        {
            size_t end = (size_t)off + (size_t)cnt;
            if (end > child_pool_len)
                child_pool_len = end;
        }
    }
    CUDA_OK(malloc_and_copy(&dF->child_index, F->child_index, child_pool_len));
    dF->child_pool_len = child_pool_len;

    // rectangle pools
    CUDA_OK(malloc_and_copy(&dF->rxmin, F->rxmin, F->num_rects));
    CUDA_OK(malloc_and_copy(&dF->rymin, F->rymin, F->num_rects));
    CUDA_OK(malloc_and_copy(&dF->rxmax, F->rxmax, F->num_rects));
    CUDA_OK(malloc_and_copy(&dF->rymax, F->rymax, F->num_rects));
    dF->rect_pool_len = (size_t)F->num_rects;

    return cudaSuccess;
}

cudaError_t free_device_flat(DevFlatRTree *dF)
{
    // free in any order
    cudaFree(dF->is_leaf);
    cudaFree(dF->xmin);
    cudaFree(dF->ymin);
    cudaFree(dF->xmax);
    cudaFree(dF->ymax);
    cudaFree(dF->child_off);
    cudaFree(dF->child_cnt);
    cudaFree(dF->first_rect);
    cudaFree(dF->rect_cnt);
    cudaFree(dF->child_index);
    cudaFree(dF->rxmin);
    cudaFree(dF->rymin);
    cudaFree(dF->rxmax);
    cudaFree(dF->rymax);

    *dF = DevFlatRTree{}; // correct type
    return cudaSuccess;
}

void rtree_gpu_search(const DevFlatRTree *dF,
                      const Rect *d_queries,
                      int num_queries,
                      int *d_overlap_counts,
                      int threads_per_block)
{
    int blocks = (num_queries + threads_per_block - 1) / threads_per_block;
    rtree_search_kernel<<<blocks, threads_per_block>>>(*dF, d_queries, num_queries, d_overlap_counts);
    cudaError_t e = cudaGetLastError();
    if (e != cudaSuccess)
    {
        fprintf(stderr, "CUDA launch error %s\n", cudaGetErrorString(e));
    }
}
