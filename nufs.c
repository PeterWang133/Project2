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
#define MAX_PATH_LEN 256
#define MAX_BLOCKS 8 // Support up to 8 blocks per file

#define DEBUG 1 // Set to 0 to disable debug output
#define log_debug(fmt, ...) \
    do { if (DEBUG) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)

typedef struct {
    char name[MAX_PATH_LEN];   // Name of the file or directory
    mode_t mode;               // File permissions and type
    size_t size;               // File size in bytes
    time_t atime;              // Last access time
    time_t mtime;              // Last modification time
    time_t ctime;              // Creation time
    int is_dir;                // 1 for directories, 0 for files
    int parent_index;          // Parent directory inode index (-1 for root)
    int block_indices[MAX_BLOCKS]; // Array of block indices
} inode_t;

inode_t inode_table[MAX_FILES];

// Initialize the inode table
void init_inode_table() {
    for (int i = 0; i < MAX_FILES; i++) {
        memset(&inode_table[i], 0, sizeof(inode_t));
        inode_table[i].parent_index = -1; // Default to no parent
        for (int j = 0; j < MAX_BLOCKS; j++) {
            inode_table[i].block_indices[j] = -1; // Mark all blocks as unallocated
        }
    }
    // Initialize root directory
    strncpy(inode_table[0].name, "/", MAX_PATH_LEN);
    inode_table[0].mode = S_IFDIR | 0755;
    inode_table[0].is_dir = 1;
    inode_table[0].ctime = inode_table[0].mtime = inode_table[0].atime = time(NULL);
}

// Find an inode by name and parent index
int find_inode(const char *path, int parent_index) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(inode_table[i].name, path) == 0 && inode_table[i].parent_index == parent_index) {
            return i;
        }
    }
    return -1; // Not found
}

// Create a new inode
int create_inode(const char *name, mode_t mode, int is_dir, int parent_index) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].name[0] == '\0') { // Empty slot
            strncpy(inode_table[i].name, name, MAX_PATH_LEN);
            inode_table[i].mode = mode;
            inode_table[i].is_dir = is_dir;
            inode_table[i].size = 0;
            inode_table[i].ctime = inode_table[i].mtime = inode_table[i].atime = time(NULL);
            inode_table[i].parent_index = parent_index;
            for (int j = 0; j < MAX_BLOCKS; j++) {
                inode_table[i].block_indices[j] = -1; // Initialize blocks
            }
            if (!is_dir) {
                int block = alloc_block();
                if (block == -1) {
                    return -ENOSPC;
                }
                inode_table[i].block_indices[0] = block;
            }
            return i;
        }
    }
    return -ENOSPC; // No space left
}

// Access a file or directory
int nufs_access(const char *path, int mask) {
    return find_inode(path, 0) >= 0 ? 0 : -ENOENT;
}

// Get file attributes
int nufs_getattr(const char *path, struct stat *st) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    st->st_mode = inode->mode;
    st->st_size = inode->size;
    st->st_atime = inode->atime;
    st->st_mtime = inode->mtime;
    st->st_ctime = inode->ctime;
    return 0;
}

// Read directory contents
int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    int parent_index = find_inode(path, 0);
    if (parent_index == -1 || !inode_table[parent_index].is_dir) {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0);
    if (parent_index != 0) {
        filler(buf, "..", NULL, 0);
    }
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].parent_index == parent_index && inode_table[i].name[0] != '\0') {
            filler(buf, inode_table[i].name, NULL, 0);
        }
    }
    return 0;
}

// Create a file
int nufs_mknod(const char *path, mode_t mode, dev_t dev) {
    if (find_inode(path, 0) >= 0) {
        return -EEXIST;
    }
    return create_inode(path, mode, 0, 0);
}

// Create a directory
int nufs_mkdir(const char *path, mode_t mode) {
    if (find_inode(path, 0) >= 0) {
        return -EEXIST;
    }
    return create_inode(path, mode | S_IFDIR, 1, 0);
}

// Remove a file
int nufs_unlink(const char *path) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (inode->block_indices[i] != -1) {
            free_block(inode->block_indices[i]);
        }
    }
    memset(inode, 0, sizeof(inode_t));
    return 0;
}

// Remove a directory
int nufs_rmdir(const char *path) {
    int idx = find_inode(path, 0);
    if (idx == -1 || !inode_table[idx].is_dir) {
        return -ENOENT;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].parent_index == idx) {
            return -ENOTEMPTY;
        }
    }
    memset(&inode_table[idx], 0, sizeof(inode_t));
    return 0;
}

// Rename a file or directory
int nufs_rename(const char *from, const char *to) {
    int idx = find_inode(from, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    strncpy(inode_table[idx].name, to, MAX_PATH_LEN);
    return 0;
}

// Write data to a file
int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    size_t remaining = size, written = 0;
    while (remaining > 0) {
        int block_idx = (offset + written) / BLOCK_SIZE;
        if (block_idx >= MAX_BLOCKS) {
            return -ENOSPC;
        }
        if (inode->block_indices[block_idx] == -1) {
            inode->block_indices[block_idx] = alloc_block();
            if (inode->block_indices[block_idx] == -1) {
                return -ENOSPC;
            }
        }
        void *block = blocks_get_block(inode->block_indices[block_idx]);
        size_t block_offset = (offset + written) % BLOCK_SIZE;
        size_t to_write = BLOCK_SIZE - block_offset;
        if (to_write > remaining) to_write = remaining;
        memcpy(block + block_offset, buf + written, to_write);
        written += to_write;
        remaining -= to_write;
    }
    inode->size = offset + written;
    inode->mtime = time(NULL);
    return written;
}

// Read data from a file
int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    if (offset >= inode->size) {
        return 0;
    }
    size_t to_read = size;
    if (offset + size > inode->size) {
        to_read = inode->size - offset;
    }
    size_t remaining = to_read, read = 0;
    while (remaining > 0) {
        int block_idx = (offset + read) / BLOCK_SIZE;
        if (block_idx >= MAX_BLOCKS || inode->block_indices[block_idx] == -1) {
            break;
        }
        void *block = blocks_get_block(inode->block_indices[block_idx]);
        size_t block_offset = (offset + read) % BLOCK_SIZE;
        size_t to_copy = BLOCK_SIZE - block_offset;
        if (to_copy > remaining) to_copy = remaining;
        memcpy(buf + read, block + block_offset, to_copy);
        read += to_copy;
        remaining -= to_copy;
    }
    return read;
}

// Initialize FUSE operations
void nufs_init_ops(struct fuse_operations *ops) {
    memset(ops, 0, sizeof(struct fuse_operations));
    ops->access = nufs_access;
    ops->getattr = nufs_getattr;
    ops->readdir = nufs_readdir;
    ops->mknod = nufs_mknod;
    ops->mkdir = nufs_mkdir;
    ops->unlink = nufs_unlink;
    ops->rmdir = nufs_rmdir;
    ops->rename = nufs_rename;
    ops->write = nufs_write;
    ops->read = nufs_read;
}

// Main entry point
int main(int argc, char *argv[]) {
    blocks_init(argv[--argc]);
    init_inode_table();
    struct fuse_operations nufs_ops;
    nufs_init_ops(&nufs_ops);
    return fuse_main(argc, argv, &nufs_ops, NULL);
}
