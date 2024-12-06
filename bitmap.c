// High level overview: provides a low-level implementation of a bitmap data structure, which is used for efficiently tracking 
//    the allocation status of blocks or resources in a memory-efficient manner.

// Uses byte-level operations to manipulate individual bits
// Supports efficient bit tracking with minimal memory overhead
// Provides a simple interface for bit manipulation

// neccesaru libraries
#include <stdint.h>
#include <stdio.h>
#include "bitmap.h"

// Helper macros for bit manipulation
#define nth_bit_mask(n) (1 << (n))
#define byte_index(n) ((n) / 8)
#define bit_index(n) ((n) % 8)


/**
 * Get the given bit from the bitmap.
 *
 * @param bm Pointer to the start of the bitmap.
 * @param i The bit index.
 *
 * @return The state of the given bit (0 or 1).
 */
int bitmap_get(void *bm, int i) {
    if (i < 0) {
        fprintf(stderr, "bitmap_get: invalid index %d\n", i);
        return 0;
    }
    uint8_t *base = (uint8_t *)bm;
    return (base[byte_index(i)] >> bit_index(i)) & 1;
}


/**
 * Set the given bit in the bitmap to the given value.
 *
 * @param bm Pointer to the start of the bitmap.
 * @param i Bit index.
 * @param v Value the bit should be set to (0 or 1).
 */
void bitmap_put(void *bm, int i, int v) {
    if (i < 0) {
        fprintf(stderr, "bitmap_put: invalid index %d\n", i);
        return;
    }
    uint8_t *base = (uint8_t *)bm;
    uint8_t bit_mask = nth_bit_mask(bit_index(i));
    if (v) {
        base[byte_index(i)] |= bit_mask; // Set the bit
    } else {
        base[byte_index(i)] &= ~bit_mask; // Clear the bit
    }
}

/**
 * Pretty-print a bitmap. 
 *
 * @param bm Pointer to the bitmap.
 * @param size The number of bits to print.
 */
void bitmap_print(void *bm, int size) {
    for (int i = 0; i < size; i++) {
        putchar(bitmap_get(bm, i) ? '1' : '0');
        if ((i + 1) % 64 == 0) {
            putchar('\n');
        } else if ((i + 1) % 8 == 0) {
            putchar(' ');
        }
    }
}
