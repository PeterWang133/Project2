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

const int BLOCK_COUNT = 256; // Number of blocks
const int BLOCK_SIZE = 4096; // Block size = 4KB
const int NUFS_SIZE = BLOCK_SIZE * BLOCK_COUNT; // Total size = 1MB
const int BLOCK_BITMAP_SIZE = BLOCK_COUNT / 8;

static int blocks_fd = -1;
static void *blocks_base = NULL;

// Get the number of blocks needed for a given number of bytes.
int bytes_to_blocks(int bytes) {
    return (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

// Initialize the disk image.
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

// Get a pointer to the specified block.
void *blocks_get_block(int bnum) {
    if (bnum < 0 || bnum >= BLOCK_COUNT) {
        fprintf(stderr, "blocks_get_block: invalid block number %d\n", bnum);
        return NULL;
    }
    return blocks_base + BLOCK_SIZE * bnum;
}

// Get the block bitmap.
void *get_blocks_bitmap() {
    return blocks_get_block(0);
}

// Get the inode bitmap.
void *get_inode_bitmap() {
    uint8_t *block = blocks_get_block(0);
    return block + BLOCK_BITMAP_SIZE;
}

// Allocate a new block
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

// Free a block
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