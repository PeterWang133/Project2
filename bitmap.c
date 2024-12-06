#include <stdint.h>
#include <stdio.h>
#include "bitmap.h"

#define nth_bit_mask(n) (1 << (n))
#define byte_index(n) ((n) / 8)
#define bit_index(n) ((n) % 8)

// Get a bit from the bitmap
int bitmap_get(void *bm, int i) {
    if (i < 0) {
        fprintf(stderr, "bitmap_get: invalid index %d\n", i);
        return 0;
    }
    uint8_t *base = (uint8_t *)bm;
    return (base[byte_index(i)] >> bit_index(i)) & 1;
}

// Set a bit in the bitmap
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

// Pretty-print the bitmap for debugging
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
