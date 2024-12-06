#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include "blocks.h"
#include "bitmap.h"

#define FUSE_USE_VERSION 26
#include <fuse.h>

#define MAX_FILES 128
#define MAX_BLOCKS_PER_FILE 128

typedef struct {
    char path[256];
    int size;
    int blocks[MAX_BLOCKS_PER_FILE];
    int block_count;
    mode_t mode;
} inode_t;

static inode_t inodes[MAX_FILES];
static int inode_count = 0;

void save_inodes() {
    void *block = blocks_get_block(1); // Block 1 reserved for metadata
    memcpy(block, inodes, sizeof(inodes));
    printf("Saved inodes to disk.\n");
}

void load_inodes() {
    void *block = blocks_get_block(1); // Block 1 reserved for metadata
    memcpy(inodes, block, sizeof(inodes));
    inode_count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (strlen(inodes[i].path) > 0) {
            inode_count++;
        }
    }
    printf("Loaded inodes from disk.\n");
}


// Initialize storage
void storage_init(const char *path) {
    blocks_init(path);
    load_inodes();
}

// Find an inode by path
inode_t *inode_lookup(const char *path) {
    for (int i = 0; i < inode_count; i++) {
        if (strcmp(inodes[i].path, path) == 0) {
            return &inodes[i];
        }
    }
    return NULL;
}

// Create a new inode
inode_t *inode_create(const char *path, mode_t mode) {
    if (inode_count >= MAX_FILES) {
        return NULL;
    }

    inode_t *node = &inodes[inode_count++];
    strncpy(node->path, path, sizeof(node->path) - 1);
    node->size = 0;
    node->block_count = 0;
    node->mode = mode;
    save_inodes();
    return node;
}

// Allocate a new block
int inode_add_block(inode_t *node) {
    if (node->block_count >= MAX_BLOCKS_PER_FILE) {
        return -1;
    }
    int block_index = alloc_block();
    if (block_index == -1) {
        return -1;
    }
    node->blocks[node->block_count++] = block_index;
    save_inodes();
    return block_index;
}

// File operations
int nufs_access(const char *path, int mask) {
    inode_t *node = inode_lookup(path);
    int rv = (node != NULL) ? 0 : -ENOENT;
    printf("access(%s, %04o) -> %d\n", path, mask, rv);
    return rv;
}

int nufs_getattr(const char *path, struct stat *st) {
    inode_t *node = inode_lookup(path);
    if (!node) return -ENOENT;

    memset(st, 0, sizeof(struct stat));
    st->st_mode = node->mode;
    st->st_size = node->size;
    st->st_uid = getuid();
    printf("getattr(%s) -> {mode: %04o, size: %lld}\n", path, st->st_mode, (long long)st->st_size);
    return 0;
}

int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for (int i = 0; i < inode_count; i++) {
        if (strcmp(path, "/") == 0 || strncmp(inodes[i].path, path, strlen(path)) == 0) {
            const char *name = inodes[i].path + strlen(path);
            if (name[0] == '/') name++; // Skip leading '/'
            if (strlen(name) > 0 && strchr(name, '/') == NULL) {
                filler(buf, name, NULL, 0);
            }
        }
    }

    printf("readdir(%s)\n", path);
    return 0;
}


static int nufs_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("mknod(%s, %o)\n", path, mode);
    if (inode_lookup(path)) return -EEXIST;

    inode_t *node = inode_create(path, mode);
    if (!node) return -ENOSPC;

    save_inodes();
    return 0;
}


int nufs_mkdir(const char *path, mode_t mode) {
    printf("mkdir(%s, %o)\n", path, mode);
    if (inode_lookup(path)) return -EEXIST;

    inode_t *node = inode_create(path, mode | S_IFDIR);
    if (!node) return -ENOMEM;

    save_inodes();
    return 0;
}



int nufs_unlink(const char *path) {
    inode_t *node = inode_lookup(path);
    if (!node) return -ENOENT;

    for (int i = 0; i < node->block_count; i++) {
        free_block(node->blocks[i]);
    }
    memset(node, 0, sizeof(inode_t)); // Clear inode data
    inode_count--;
    save_inodes();
    printf("unlink(%s) -> 0\n", path);
    return 0;
}



static int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode_t *inode = inode_lookup(path);
    if (!inode) return -ENOENT;

    if (offset >= inode->size) return 0;

    size_t remaining = inode->size - offset;
    if (size > remaining) size = remaining;

    size_t total_read = 0;
    while (size > 0) {
        int block_index = (offset + total_read) / BLOCK_SIZE;
        size_t block_offset = (offset + total_read) % BLOCK_SIZE;
        size_t to_read = BLOCK_SIZE - block_offset;
        if (to_read > size) to_read = size;

        int block_num = inode->blocks[block_index];
        void *block = blocks_get_block(block_num);
        memcpy(buf + total_read, block + block_offset, to_read);

        total_read += to_read;
        size -= to_read;
    }

    printf("read(%s, %zu bytes, offset %ld)\n", path, total_read, offset);
    return total_read;
}



static int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode_t *inode = inode_lookup(path);
    if (!inode) return -ENOENT;

    if (offset + size > inode->size) {
        inode->size = offset + size;
    }

    size_t total_written = 0;
    while (size > 0) {
        int block_index = (offset + total_written) / BLOCK_SIZE;
        size_t block_offset = (offset + total_written) % BLOCK_SIZE;
        size_t to_write = BLOCK_SIZE - block_offset;
        if (to_write > size) to_write = size;

        if (block_index >= inode->block_count) {
            int block_num = inode_add_block(inode);
            if (block_num < 0) return -ENOSPC;
        }

        int block_num = inode->blocks[block_index];
        void *block = blocks_get_block(block_num);
        memcpy(block + block_offset, buf + total_written, to_write);

        total_written += to_write;
        size -= to_write;
    }

    save_inodes();
    printf("write(%s, %zu bytes, offset %ld)\n", path, total_written, offset);
    return total_written;
}


// FUSE operations
void nufs_init_ops(struct fuse_operations *ops) {
    memset(ops, 0, sizeof(struct fuse_operations));
    ops->access = nufs_access;
    ops->getattr = nufs_getattr;
    ops->readdir = nufs_readdir;
    ops->mknod = nufs_mknod;
    ops->mkdir = nufs_mkdir;
    ops->unlink = nufs_unlink;
    ops->read = nufs_read;
    ops->write = nufs_write;
}

int main(int argc, char *argv[]) {
    assert(argc > 2 && argc < 6);
    printf("Mounting filesystem with disk image: %s\n", argv[argc - 1]);
    storage_init(argv[argc - 1]);

    struct fuse_operations nufs_ops;
    nufs_init_ops(&nufs_ops);
    int ret = fuse_main(argc - 1, argv, &nufs_ops, NULL);
    blocks_free();
    return ret;
}

