/**
 * @file nufs.c
 * @author
 *
 * FUSE implementation of a simple file system.
 */

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

#include <fuse.h>

#include "bitmap.h"
#include "blocks.h"
#include "slist.h"

#define MAX_FILES 16
#define MAX_PATH_LEN 256

typedef struct {
    char name[MAX_PATH_LEN];   // File name
    mode_t mode;               // File permissions and type
    size_t size;               // File size
    time_t atime;              // Last access time
    time_t mtime;              // Last modification time
    time_t ctime;              // Creation time
    int block;                 // Block number of the file's data
} inode_t;

inode_t inode_table[MAX_FILES]; // Table to store inodes

/**
 * Initialize the inode table.
 * Clears all entries and prepares the table for use.
 */
void init_inode_table() {
    for (int i = 0; i < MAX_FILES; i++) {
        memset(&inode_table[i], 0, sizeof(inode_t));
    }
}

/**
 * Find an inode by its path.
 *
 * @param path The path of the file.
 * @return The index of the inode, or -1 if not found.
 */
int find_inode(const char *path) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(inode_table[i].name, path) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * Create a new inode with the specified path and mode.
 *
 * @param path The path of the file.
 * @param mode The mode/permissions of the file.
 * @return The index of the new inode, or -1 on failure.
 */
int create_inode(const char *path, mode_t mode) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].name[0] == '\0') { // Empty entry
            strncpy(inode_table[i].name, path, MAX_PATH_LEN);
            inode_table[i].mode = mode;
            inode_table[i].size = 0;
            inode_table[i].ctime = inode_table[i].mtime = inode_table[i].atime = time(NULL);
            inode_table[i].block = alloc_block(); // Allocate a block for the file
            return i;
        }
    }
    return -1; // No free inode
}

int nufs_access(const char *path, int mask) {
    return find_inode(path) >= 0 ? 0 : -ENOENT;
}

int nufs_getattr(const char *path, struct stat *st) {
    int ino = find_inode(path);
    if (ino == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[ino];
    st->st_mode = inode->mode;
    st->st_size = inode->size;
    st->st_atime = inode->atime;
    st->st_mtime = inode->mtime;
    st->st_ctime = inode->ctime;
    return 0;
}

int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi) {
    filler(buf, ".", NULL, 0); // Current directory
    filler(buf, "..", NULL, 0); // Parent directory

    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].name[0] != '\0') {
            filler(buf, inode_table[i].name + 1, NULL, 0);
        }
    }
    return 0;
}

int nufs_mknod(const char *path, mode_t mode, dev_t dev) {
    if (find_inode(path) >= 0) {
        return -EEXIST;
    }
    int ino = create_inode(path, mode);
    return ino >= 0 ? 0 : -ENOSPC;
}

int nufs_write(const char *path, const char *buf, size_t size, off_t offset,
               struct fuse_file_info *fi) {
    int ino = find_inode(path);
    if (ino == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[ino];
    void *block = blocks_get_block(inode->block);
    memcpy(block + offset, buf, size); // Copy data to block
    inode->size = offset + size;
    inode->mtime = time(NULL);
    return size;
}

int nufs_read(const char *path, char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi) {
    int ino = find_inode(path);
    if (ino == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[ino];
    void *block = blocks_get_block(inode->block);
    memcpy(buf, block + offset, size); // Copy data from block
    return size;
}

/**
 * Initialize the FUSE operations structure.
 */
void nufs_init_ops(struct fuse_operations *ops) {
    memset(ops, 0, sizeof(struct fuse_operations));
    ops->access = nufs_access;
    ops->getattr = nufs_getattr;
    ops->readdir = nufs_readdir;
    ops->mknod = nufs_mknod;
    ops->write = nufs_write;
    ops->read = nufs_read;
};

struct fuse_operations nufs_ops;

int main(int argc, char *argv[]) {
    blocks_init(argv[--argc]); // Initialize block storage
    init_inode_table(); // Initialize inode table
    nufs_init_ops(&nufs_ops); // Initialize FUSE operations
    return fuse_main(argc, argv, &nufs_ops, NULL);
}
