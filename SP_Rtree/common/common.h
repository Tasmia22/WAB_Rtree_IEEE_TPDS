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


//B= N/(F^H−1)
#define BUNDLEFACTOR 128 // Number of points to form a leaf node
#define FANOUT NR_DPUS   // Number of children per non-leaf node
#define MAX_NODES 1800  //62.5 MB = 62,500,000 bytes , Max nodes = 62,500,000 / 344() ≈ 181,686, SerializedNode size is 344

#define MAX_CHILDREN 2540
#define MAX_RECTS BUNDLEFACTOR

//#define ELEMENT_SIZE sizeof(uint32_t)

/* Structure used by both the host and the DPU to communicate results */
#include <stdint.h>
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_LIGHT_BLUE    "\x1b[94m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#endif /* __COMMON_H__ */
