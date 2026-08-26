#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int xmin, ymin;
    int xmax, ymax;
} Rect;

// Structure to store Z-value and index of each rectangle
typedef struct {
    int z_value;
    int index;
} ZRect;

// Function to compute the center point of a rectangle
int computeCenterX(Rect r) {
    return (r.xmin + r.xmax) / 2;
}

int computeCenterY(Rect r) {
    return (r.ymin + r.ymax) / 2;
}

// Function to compute Z-value (Morton code) from (x, y)
int Zval(int x, int y) {
    x = (x ^ (x << 16)) & 0x0000ffff;
    x = (x ^ (x << 8))  & 0x00ff00ff;
    x = (x ^ (x << 4))  & 0x0f0f0f0f;
    x = (x ^ (x << 2))  & 0x33333333;
    x = (x ^ (x << 1))  & 0x55555555;

    y = (y ^ (y << 16)) & 0x0000ffff;
    y = (y ^ (y << 8))  & 0x00ff00ff;
    y = (y ^ (y << 4))  & 0x0f0f0f0f;
    y = (y ^ (y << 2))  & 0x33333333;
    y = (y ^ (y << 1))  & 0x55555555;

    return (y << 1) | x;
}

// Comparison function for sorting ZRects by Z-value
int compareZRects(const void *a, const void *b) {
    return ((ZRect*)a)->z_value - ((ZRect*)b)->z_value;
}

// Function to sort rectangles based on Z-values of their centers
void Zsorting(Rect rects[], int num_rects) {
    ZRect *zrects = (ZRect *)malloc(num_rects * sizeof(ZRect));
    Rect *sorted_rects = (Rect *)malloc(num_rects * sizeof(Rect));

    if (zrects == NULL || sorted_rects == NULL) {
        fprintf(stderr, "Memory allocation failed!\n");
        exit(1);
    }

    // Compute Z-values based on center of each rectangle
    for (int i = 0; i < num_rects; i++) {
        int centerX = computeCenterX(rects[i]);
        int centerY = computeCenterY(rects[i]);
        zrects[i].z_value = Zval(centerX, centerY);
        zrects[i].index = i;
    }

    // Sort rectangles based on Z-values
    qsort(zrects, num_rects, sizeof(ZRect), compareZRects);

    // Arrange rectangles into sorted order
    for (int i = 0; i < num_rects; i++) {
        sorted_rects[i] = rects[zrects[i].index];
    }

    // Copy back to original array
    memcpy(rects, sorted_rects, num_rects * sizeof(Rect));

    // Free allocated memory
    free(zrects);
    free(sorted_rects);
}
