#ifndef __COMMON_H__
#define __COMMON_H__

#define XSTR(x) STR(x)
#define STR(x) #x

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif


#define BLOCK_SIZE (256)
//#define NR_DPUS 3
static inline uint32_t ceil8(uint32_t a) {
    return (a + 0x07) & ~(0x07UL);
}
static inline uint32_t pceil8(uint32_t *a) {
    return (*a + 0x07) & ~(0x07UL);
}


/*
 * Supported experiment profiles while keeping FANOUT == NR_DPUS.
 *
 * 2540 DPUs:
 *   Original reproducibility path. These constants are written out directly
 *   so the 2540-DPU configuration stays visibly identical in intent:
 *   BUNDLEFACTOR=128, FANOUT=2540, MAX_TOP2=72, MAX_SUBTREE=72,
 *   base shard cap=72.
 *
 * 2048 DPUs:
 *   BUNDLEFACTOR=128, FANOUT=2048, MAX_TOP2=72,  MAX_SUBTREE=72,
 *   base shard cap=72.
 *
 * 1024 DPUs:
 *   BUNDLEFACTOR=128, FANOUT=1024, MAX_TOP2=128, MAX_SUBTREE=128,
 *   base shard cap=128.
 *
 * 512 DPUs:
 *   BUNDLEFACTOR=128, FANOUT=512,  MAX_TOP2=256, MAX_SUBTREE=256,
 *   base shard cap=256.
 */
#if NR_DPUS == 2540
#define BUNDLEFACTOR 128
#define FANOUT NR_DPUS
#define MAX_NODES 1800  //62.5 MB = 62,500,000 bytes , Max nodes = 62,500,000 / 344() ≈ 181,686, SerializedNode size is 344
#define MAX_CHILDREN 2540
#define MAX_RECTS BUNDLEFACTOR
#define MAX_SUBTREE 256
#define MAX_TOP2 256
#define REPLICA_BASE_NODE_CAP_DEFAULT 256
#elif NR_DPUS == 2048
#define BUNDLEFACTOR 128
#define FANOUT NR_DPUS
#define MAX_NODES 1800  //62.5 MB = 62,500,000 bytes , Max nodes = 62,500,000 / 344() ≈ 181,686, SerializedNode size is 344
#define MAX_CHILDREN 2540
#define MAX_RECTS BUNDLEFACTOR
#define MAX_SUBTREE 72
#define MAX_TOP2 72
#define REPLICA_BASE_NODE_CAP_DEFAULT 72
#elif NR_DPUS == 1024
#define BUNDLEFACTOR 128
#define FANOUT NR_DPUS
#define MAX_NODES 1800  //62.5 MB = 62,500,000 bytes , Max nodes = 62,500,000 / 344() ≈ 181,686, SerializedNode size is 344
#define MAX_CHILDREN 2540
#define MAX_RECTS BUNDLEFACTOR
#define MAX_SUBTREE 128
#define MAX_TOP2 128
#define REPLICA_BASE_NODE_CAP_DEFAULT 128
#elif NR_DPUS == 512
#define BUNDLEFACTOR 128
#define FANOUT NR_DPUS
#define MAX_NODES 1800  //62.5 MB = 62,500,000 bytes , Max nodes = 62,500,000 / 344() ≈ 181,686, SerializedNode size is 344
#define MAX_CHILDREN 2540
#define MAX_RECTS BUNDLEFACTOR
#define MAX_SUBTREE 256
#define MAX_TOP2 256
#define REPLICA_BASE_NODE_CAP_DEFAULT 256
#else
#error "Supported NR_DPUS values are 512, 1024, 2048, and 2540."
#endif

#define MAX_QUERY 1440000
#define DEFAULT_LAUNCH_QUERY_CAP 720000
#define ROADS_LAUNCH_QUERY_CAP 1440000
#define TASKLET_QUERY_CAP ((MAX_QUERY + NR_TASKLETS - 1) / NR_TASKLETS)
#define RESULT_PAIR_CAPACITY (TASKLET_QUERY_CAP * NR_TASKLETS)
//#define ELEMENT_SIZE sizeof(uint32_t)

/* Structure used by both the host and the DPU to communicate results */
#include <stdint.h>
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_LIGHT_BLUE    "\x1b[94m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#endif /* __COMMON_H__ */
