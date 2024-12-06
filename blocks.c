// This file manages a disk image by providing functions for block allocation, deallocation, and access. 
// It implements a block-based storage system with a fixed number of blocks and a configurable block size.

// importing neccesary libraries
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "bitmap.h"
#include "blocks.h"

// Neccesary constraints/variables
const int BLOCK_COUNT = 256; // Number of blocks
const int BLOCK_SIZE = 4096; // Block size = 4KB
const int NUFS_SIZE = BLOCK_SIZE * BLOCK_COUNT; // Total size = 1MB
const int BLOCK_BITMAP_SIZE = BLOCK_COUNT / 8; // Size of the block bitmap in bytes

static int blocks_fd = -1;
void *blocks_base = NULL;

/** 
 * Compute the number of blocks needed to store the given number of bytes.
 *
 * @param bytes Size of data to store in bytes.
 *
 * @return Number of blocks needed to store the given number of bytes.
 */
int bytes_to_blocks(int bytes) {
    return (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

/**
 * Load and initialize the given disk image.
 *
 * @param image_path Path to the disk image file.
 */
void blocks_init(const char *image_path) {
    blocks_fd = open(image_path, O_CREAT | O_RDWR, 0644);
    assert(blocks_fd != -1);

    // Check current file size
    struct stat st;
    int rv = fstat(blocks_fd, &st);
    assert(rv == 0);

    if (st.st_size != NUFS_SIZE) {
        rv = ftruncate(blocks_fd, NUFS_SIZE);
        assert(rv == 0);
    }

    blocks_base = mmap(0, NUFS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, blocks_fd, 0);
    assert(blocks_base != MAP_FAILED);

    // Reserve block 0 for the block and inode bitmaps only if this is a fresh image
    // If file was newly created or truncated, we need to set bitmap for block 0
    // If it already existed and was correct size, assume metadata is intact
    if (st.st_size == 0) {
        void *bbm = get_blocks_bitmap();
        bitmap_put(bbm, 0, 1);
    }
}

// Free the disk image and unmap memory.
void blocks_free() {
    if (blocks_base) {
        int rv = munmap(blocks_base, NUFS_SIZE);
        assert(rv == 0);
        blocks_base = NULL;
    }
    if (blocks_fd != -1) {
        close(blocks_fd);
        blocks_fd = -1;
    }
}

/**
 * Get the block with the given index, returning a pointer to its start.
 *
 * @param bnum Block number (index).
 *
 * @return Pointer to the beginning of the block in memory.
 */
void *blocks_get_block(int bnum) {
    if (bnum < 0 || bnum >= BLOCK_COUNT) {
        fprintf(stderr, "blocks_get_block: invalid block number %d\n", bnum);
        return NULL;
    }
    return blocks_base + BLOCK_SIZE * bnum;
}

// @return A pointer to the beginning of the free blocks bitmap.
void *get_blocks_bitmap() {
    return blocks_get_block(0);
}

// @return A pointer to the beginning of the free inode bitmap.
void *get_inode_bitmap() {
    uint8_t *block = blocks_get_block(0);
    return block + BLOCK_BITMAP_SIZE;
}

/**
 * Allocate a new block and return its number.
 *
 * Grabs the first unused block and marks it as allocated.
 *
 * @return The index of the newly allocated block.
 */
int alloc_block() {
    void *bbm = get_blocks_bitmap();
    // Start from FIRST_DATA_BLOCK to avoid overwriting inode metadata blocks.
    for (int i = FIRST_DATA_BLOCK; i < BLOCK_COUNT; ++i) {
        if (!bitmap_get(bbm, i)) {
            bitmap_put(bbm, i, 1);
            void *block = blocks_get_block(i);
            if (block) {
                memset(block, 0, BLOCK_SIZE);
            }
            printf("+ alloc_block() -> %d\n", i);
            return i;
        }
    }
    fprintf(stderr, "alloc_block: no free blocks available\n");
    return -ENOSPC;
}

/**
 * Deallocate the block with the given number.
 *
 * @param bnun The block number to deallocate.
 */
void free_block(int bnum) {
    if (bnum <= 0 || bnum >= BLOCK_COUNT) {
        fprintf(stderr, "free_block: invalid block number %d\n", bnum);
        return;
    }

    void *bbm = get_blocks_bitmap();
    if (bitmap_get(bbm, bnum)) {
        bitmap_put(bbm, bnum, 0); // Mark block as free in the bitmap
        void *block = blocks_get_block(bnum);
        if (block) {
            memset(block, 0, BLOCK_SIZE); // Clear the block
        }
        printf("+ free_block(%d)\n", bnum);
    } else {
        fprintf(stderr, "free_block: block %d is already free\n", bnum);
    }
}