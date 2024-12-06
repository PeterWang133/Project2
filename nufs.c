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
#define DIR_NAME_LENGTH 48
#define BLOCK_SIZE 4096

typedef struct {
    char name[DIR_NAME_LENGTH];
    int inum;
} dirent_t;

typedef struct {
    char name[MAX_PATH_LEN];
    mode_t mode;
    size_t size;
    time_t atime;
    time_t mtime;
    time_t ctime;
    int is_dir;
    int parent_index;
    int block_indices[MAX_BLOCKS];
} inode_t;

inode_t inode_table[MAX_FILES];

// Helper Functions
void init_inode_table() {
    for (int i = 0; i < MAX_FILES; i++) {
        memset(&inode_table[i], 0, sizeof(inode_t));
        inode_table[i].parent_index = -1;
        for (int j = 0; j < MAX_BLOCKS; j++) {
            inode_table[i].block_indices[j] = -1;
        }
    }
    strncpy(inode_table[0].name, "/", MAX_PATH_LEN);
    inode_table[0].mode = S_IFDIR | 0755;
    inode_table[0].is_dir = 1;
    inode_table[0].ctime = inode_table[0].mtime = inode_table[0].atime = time(NULL);
}

int find_inode(const char *path, int parent_index) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(inode_table[i].name, path) == 0 && inode_table[i].parent_index == parent_index) {
            return i;
        }
    }
    return -1;
}

int create_inode(const char *name, mode_t mode, int is_dir, int parent_index) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].name[0] == '\0') {
            strncpy(inode_table[i].name, name, MAX_PATH_LEN);
            inode_table[i].mode = mode;
            inode_table[i].is_dir = is_dir;
            inode_table[i].size = 0;
            inode_table[i].ctime = inode_table[i].mtime = inode_table[i].atime = time(NULL);
            inode_table[i].parent_index = parent_index;
            for (int j = 0; j < MAX_BLOCKS; j++) {
                inode_table[i].block_indices[j] = -1;
            }
            if (!is_dir) {
                int block = alloc_block();
                if (block == -1) return -ENOSPC;
                inode_table[i].block_indices[0] = block;
            }
            return i;
        }
    }
    return -ENOSPC;
}

void split_path(const char *path, char *parent, char *name) {
    const char *last_slash = strrchr(path, '/');
    if (last_slash == path) {
        strcpy(parent, "/");
        strcpy(name, path + 1);
    } else {
        strncpy(parent, path, last_slash - path);
        parent[last_slash - path] = '\0';
        strcpy(name, last_slash + 1);
    }
}

// FUSE Callbacks
int nufs_access(const char *path, int mask) {
    int idx = find_inode(path, 0);
    int rv = (idx >= 0) ? 0 : -ENOENT;
    printf("access(%s, %04o) -> %d\n", path, mask, rv);
    return rv;
}

int nufs_getattr(const char *path, struct stat *st) {
    int idx = find_inode(path, 0);
    if (idx == -1) return -ENOENT;

    inode_t *inode = &inode_table[idx];
    st->st_mode = inode->mode;
    st->st_size = inode->size;
    st->st_atime = inode->atime;
    st->st_mtime = inode->mtime;
    st->st_ctime = inode->ctime;
    printf("getattr(%s) -> {mode: %04o, size: %ld}\n", path, st->st_mode, st->st_size);
    return 0;
}

int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    int idx = find_inode(path, 0);
    if (idx == -1 || !inode_table[idx].is_dir) return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].parent_index == idx && inode_table[i].name[0] != '\0') {
            filler(buf, inode_table[i].name, NULL, 0);
        }
    }
    return 0;
}

int nufs_mknod(const char *path, mode_t mode, dev_t dev) {
    char parent[MAX_PATH_LEN], name[DIR_NAME_LENGTH];
    split_path(path, parent, name);

    int parent_idx = find_inode(parent, 0);
    if (parent_idx == -1) return -ENOENT;

    inode_t *parent_inode = &inode_table[parent_idx];
    if (!parent_inode->is_dir) return -ENOTDIR;

    int new_idx = create_inode(name, mode, 0, parent_idx);
    return new_idx >= 0 ? 0 : new_idx;
}

int nufs_mkdir(const char *path, mode_t mode) {
    char parent[MAX_PATH_LEN], name[DIR_NAME_LENGTH];
    split_path(path, parent, name);

    int parent_idx = find_inode(parent, 0);
    if (parent_idx == -1) return -ENOENT;

    inode_t *parent_inode = &inode_table[parent_idx];
    int new_idx = create_inode(name, mode | S_IFDIR, 1, parent_idx);
    return new_idx >= 0 ? 0 : new_idx;
}

int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int idx = find_inode(path, 0);
    if (idx == -1) return -ENOENT;

    inode_t *inode = &inode_table[idx];
    size_t written = 0;
    while (size > 0) {
        int block_idx = (offset + written) / BLOCK_SIZE;
        if (block_idx >= MAX_BLOCKS) return -ENOSPC;

        if (inode->block_indices[block_idx] == -1) {
            inode->block_indices[block_idx] = alloc_block();
            if (inode->block_indices[block_idx] == -1) return -ENOSPC;
        }

        void *block = blocks_get_block(inode->block_indices[block_idx]);
        size_t block_offset = (offset + written) % BLOCK_SIZE;
        size_t to_write = BLOCK_SIZE - block_offset;
        if (to_write > size) to_write = size;

        memcpy(block + block_offset, buf + written, to_write);
        written += to_write;
        size -= to_write;
    }

    inode->size = offset + written > inode->size ? offset + written : inode->size;
    inode->mtime = time(NULL);
    return written;
}

int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int idx = find_inode(path, 0);
    if (idx == -1) return -ENOENT;

    inode_t *inode = &inode_table[idx];
    if (offset >= inode->size) return 0;

    size_t to_read = size;
    if (offset + size > inode->size) to_read = inode->size - offset;

    size_t read = 0;
    while (to_read > 0) {
        int block_idx = (offset + read) / BLOCK_SIZE;
        if (block_idx >= MAX_BLOCKS || inode->block_indices[block_idx] == -1) break;

        void *block = blocks_get_block(inode->block_indices[block_idx]);
        size_t block_offset = (offset + read) % BLOCK_SIZE;
        size_t to_copy = BLOCK_SIZE - block_offset;
        if (to_copy > to_read) to_copy = to_read;

        memcpy(buf + read, block + block_offset, to_copy);
        read += to_copy;
        to_read -= to_copy;
    }
    return read;
}

void nufs_init_ops(struct fuse_operations *ops) {
    memset(ops, 0, sizeof(struct fuse_operations));
    ops->access = nufs_access;
    ops->getattr = nufs_getattr;
    ops->readdir = nufs_readdir;
    ops->mknod = nufs_mknod;
    ops->mkdir = nufs_mkdir;
    ops->write = nufs_write;
    ops->read = nufs_read;
}

int main(int argc, char *argv[]) {
    blocks_init(argv[--argc]);
    init_inode_table();
    struct fuse_operations nufs_ops;
    nufs_init_ops(&nufs_ops);
    return fuse_main(argc, argv, &nufs_ops, NULL);
}
