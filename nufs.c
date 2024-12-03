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

// BLOCK SIZE deleted because it is included in blocks.c file

typedef struct {
    char name[MAX_PATH_LEN];   // Name of the file or directory
    mode_t mode;              // File permissions and type (e.g., regular file, directory)
    size_t size;              // Size of the file in bytes
    time_t atime;             // Last access time
    time_t mtime;             // Last modification time
    time_t ctime;             // Creation time
    int block_index;          // Index of the block storing file data (-1 for directories)
    int is_dir;               // Flag: 1 if it's a directory, 0 if it's a file
    int parent_index;         // Index of the parent directory in the inode table (-1 for root)
} inode_t;


// Table to store metadata for files and directories
inode_t inode_table[MAX_FILES];

// Initialize the inode table and create the root directory
void init_inode_table() {
    for (int i = 0; i < MAX_FILES; i++) {
        memset(&inode_table[i], 0, sizeof(inode_t));
        inode_table[i].parent_index = -1; // Default to no parent
    }
    // Initialize root directory
    strncpy(inode_table[0].name, "/", MAX_PATH_LEN);  // Root directory name
    inode_table[0].mode = S_IFDIR | 0755;            // Directory mode with permissions
    inode_table[0].is_dir = 1;                       // Mark as a directory
    inode_table[0].ctime = inode_table[0].mtime = inode_table[0].atime = time(NULL);
}

// Find the index of an inode by its name and parent directory index
int find_inode(const char *path, int parent_index) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (strcmp(inode_table[i].name, path) == 0 && inode_table[i].parent_index == parent_index) {
            return i; // Found inode
        }
    }
    return -1; // Not found
}

// Create a new inode for a file or directory
int create_inode(const char *name, mode_t mode, int is_dir, int parent_index) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].name[0] == '\0') { // Empty slot
            strncpy(inode_table[i].name, name, MAX_PATH_LEN);
            inode_table[i].mode = mode;
            inode_table[i].is_dir = is_dir;
            inode_table[i].size = 0;
            inode_table[i].ctime = inode_table[i].mtime = inode_table[i].atime = time(NULL);
            inode_table[i].parent_index = parent_index;
            inode_table[i].block_index = is_dir ? -1 : alloc_block(); // Allocate block for files
            return i; // Success
        }
    }
    return -ENOSPC; // No space for a new inode
}

// Check if a file or directory exists
int nufs_access(const char *path, int mask) {
    return find_inode(path, 0) >= 0 ? 0 : -ENOENT;
}

// Retrieve file or directory attributes
int nufs_getattr(const char *path, struct stat *st) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    st->st_mode = inode->mode;  // File mode
    st->st_size = inode->size;  // File size
    st->st_atime = inode->atime; // Access time
    st->st_mtime = inode->mtime; // Modification time
    st->st_ctime = inode->ctime; // Creation time
    return 0;
}

// List contents of a directory
int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    filler(buf, ".", NULL, 0);  // Current directory
    filler(buf, "..", NULL, 0); // Parent directory
    int parent_index = find_inode(path, 0); // Find the directory inode
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].parent_index == parent_index) {
            filler(buf, inode_table[i].name, NULL, 0); // Add child entry
        }
    }
    return 0;
}

// Create a file
int nufs_mknod(const char *path, mode_t mode, dev_t dev) {
    if (find_inode(path, 0) >= 0) {
        return -EEXIST;
    }
    return create_inode(path, mode, 0, 0); // Create a new file
}

// Create a directory
int nufs_mkdir(const char *path, mode_t mode) {
    if (find_inode(path, 0) >= 0) {
        return -EEXIST;
    }
    return create_inode(path, mode | S_IFDIR, 1, 0); // Create a new directory
}

// Delete a directory
int nufs_rmdir(const char *path) {
    int idx = find_inode(path, 0);
    if (idx == -1 || !inode_table[idx].is_dir) {
        return -ENOENT;
    }
    // Ensure directory is empty
    for (int i = 0; i < MAX_FILES; i++) {
        if (inode_table[i].parent_index == idx) {
            return -ENOTEMPTY;
        }
    }
    memset(&inode_table[idx], 0, sizeof(inode_t)); // Clear directory entry
    return 0;
}

// Delete a file
int nufs_unlink(const char *path) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    if (inode->block_index != -1) {
        free_block(inode->block_index); // Free block storage
    }
    memset(inode, 0, sizeof(inode_t)); // Clear file entry
    return 0;
}

// Rename a file or directory
int nufs_rename(const char *from, const char *to) {
    int idx = find_inode(from, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    strncpy(inode_table[idx].name, to, MAX_PATH_LEN); // Update name
    return 0;
}

// Write data to a file
int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    void *block = blocks_get_block(inode->block_index);
    if (offset + size > BLOCK_SIZE) {
        return -ENOSPC;
    }
    memcpy(block + offset, buf, size); // Write to block
    inode->size = offset + size;      // Update size
    inode->mtime = time(NULL);        // Update modification time
    return size;
}

// Read data from a file
int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    int idx = find_inode(path, 0);
    if (idx == -1) {
        return -ENOENT;
    }
    inode_t *inode = &inode_table[idx];
    void *block = blocks_get_block(inode->block_index);
    if (offset >= inode->size) {
        return 0;
    }
    size_t to_read = size;
    if (offset + size > inode->size) {
        to_read = inode->size - offset; // Adjust read size if beyond file size
    }
    memcpy(buf, block + offset, to_read); // Read from block
    return to_read;
}

// Initialize FUSE operations
void nufs_init_ops(struct fuse_operations *ops) {
    memset(ops, 0, sizeof(struct fuse_operations));
    ops->access = nufs_access;
    ops->getattr = nufs_getattr;
    ops->readdir = nufs_readdir;
    ops->mknod = nufs_mknod;
    ops->write = nufs_write;
    ops->read = nufs_read;
}


// Main entry point
int main(int argc, char *argv[]) {
    blocks_init(argv[--argc]); // Initialize block storage
    init_inode_table();        // Initialize inode table
    struct fuse_operations nufs_ops;
    nufs_init_ops(&nufs_ops);  // Set up FUSE operations
    return fuse_main(argc, argv, &nufs_ops, NULL); // Start FUSE
}
