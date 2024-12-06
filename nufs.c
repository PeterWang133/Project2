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
    if (!block) {
        fprintf(stderr, "save_inodes: Failed to access Block 1 for saving inodes.\n");
        return;
    }
    memcpy(block, inodes, sizeof(inodes));
    printf("Saved inodes to disk.\n");
}


void load_inodes() {
    void *block = blocks_get_block(1); // Block 1 reserved for metadata
    if (!block) {
        fprintf(stderr, "load_inodes: Failed to access Block 1 for loading inodes.\n");
        return;
    }
    memcpy(inodes, block, sizeof(inodes));
    inode_count = 0;

    for (int i = 0; i < MAX_FILES; i++) {
        // Ensure path is a valid string
        if (strlen(inodes[i].path) > 0) {
            inode_count++;
        }
    }
    printf("Loaded inodes from disk.\n");
}

// Initialize storage
void storage_init(const char *path) {
    printf("Initializing storage with disk image: %s\n", path);
    // Initialize the block storage system
    blocks_init(path);
    // Load the inodes from disk
    load_inodes();
    printf("Storage initialized successfully.\n");
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

// Allocate a new block for the given inode
int inode_add_block(inode_t *node) {
    // Check if the file has reached the maximum number of blocks
    if (node->block_count >= MAX_BLOCKS_PER_FILE) {
        printf("inode_add_block: reached max blocks for file\n");
        return -ENOSPC; // Return "No space left on device"
    }

    // Allocate a new block
    int block_index = alloc_block();
    if (block_index == -1) {
        printf("inode_add_block: failed to allocate block\n");
        return -ENOSPC; // Return "No space left on device"
    }

    // Add the block to the inode's block list and update metadata
    node->blocks[node->block_count++] = block_index;

    // Save inode metadata to persist the change
    save_inodes();
    printf("inode_add_block: allocated block %d for inode, total blocks %d\n",
           block_index, node->block_count);
    return block_index;
}

// Check if the file or directory exists and has the required permissions
int nufs_access(const char *path, int mask) {
    // Lookup the inode for the given path
    inode_t *node = inode_lookup(path);
    if (!node) {
        printf("access: file or directory %s not found\n", path);
        return -ENOENT; // Return "No such file or directory"
    }

    // Access check is currently basic, as permissions aren't implemented fully
    int rv = 0; // For simplicity, assume all files are accessible

    // Log the access operation
    printf("access(%s, %04o) -> %d\n", path, mask, rv);
    return rv;
}


int nufs_getattr(const char *path, struct stat *st) {
    // Lookup the inode for the specified path
    inode_t *node = inode_lookup(path);
    if (!node) {
        printf("getattr: inode not found for path %s\n", path);
        return -ENOENT; // Return "No such file or directory" if not found
    }

    // Clear the stat structure
    memset(st, 0, sizeof(struct stat));

    // Fill the stat structure with inode metadata
    st->st_mode = node->mode;          // File type and permissions
    st->st_size = node->size;          // File size in bytes
    st->st_nlink = 1;                  // Number of hard links (default to 1)
    st->st_uid = getuid();             // Owner ID (current user)
    st->st_gid = getgid();             // Group ID (current group)
    st->st_blocks = (node->size + BLOCK_SIZE - 1) / BLOCK_SIZE; // Number of 512B blocks allocated
    st->st_blksize = BLOCK_SIZE;       // Preferred block size for I/O operations

    // Log the operation
    printf("getattr(%s) -> {mode: %04o, size: %lld, blocks: %lld}\n",
           path, st->st_mode, (long long)st->st_size, (long long)st->st_blocks);
    return 0;
}


int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("readdir(%s)\n", path);

    // Add current directory and parent directory entries
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    inode_t *dir_inode = inode_lookup(path);
    if (!dir_inode) {
        printf("readdir: directory %s not found\n", path);
        return -ENOENT;
    }

    // Ensure the path corresponds to a directory
    if (!(dir_inode->mode & S_IFDIR)) {
        printf("readdir: %s is not a directory\n", path);
        return -ENOTDIR;
    }

    // Iterate over all inodes to find entries belonging to this directory
    for (int i = 0; i < inode_count; i++) {
        // Check if the inode is a direct child of the directory
        const char *child_path = inodes[i].path;
        if (strncmp(child_path, path, strlen(path)) == 0 && strlen(child_path) > strlen(path)) {
            const char *name = child_path + strlen(path);

            // Skip the leading '/' if present
            if (name[0] == '/') name++;

            // Ensure we only list direct children (no nested paths)
            if (strchr(name, '/') == NULL && strlen(name) > 0) {
                filler(buf, name, NULL, 0);
            }
        }
    }

    return 0;
}



static int nufs_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("mknod(%s, %o)\n", path, mode);

    // Check if the file already exists
    if (inode_lookup(path)) {
        printf("mknod: file %s already exists\n", path);
        return -EEXIST;
    }

    // Create a new inode for the file
    inode_t *node = inode_create(path, mode);
    if (!node) {
        printf("mknod: failed to create inode for %s\n", path);
        return -ENOSPC; // No space left
    }

    // Save changes to disk
    save_inodes();
    printf("mknod: successfully created file %s\n", path);
    return 0;
}


int nufs_mkdir(const char *path, mode_t mode) {
    printf("mkdir(%s, %o)\n", path, mode);

    // Check if the directory already exists
    if (inode_lookup(path)) {
        printf("mkdir: directory %s already exists\n", path);
        return -EEXIST;
    }

    // Create a new inode for the directory
    inode_t *node = inode_create(path, mode | S_IFDIR); // Set the directory flag
    if (!node) {
        printf("mkdir: failed to create inode for directory %s\n", path);
        return -ENOMEM; // No memory left
    }

    // Initialize directory-specific metadata (if any)
    node->size = 0; // Initially, the directory is empty

    // Save changes to disk
    save_inodes();
    printf("mkdir: successfully created directory %s\n", path);
    return 0;
}


int nufs_unlink(const char *path) {
    // Lookup the inode for the given path
    inode_t *node = inode_lookup(path);
    if (!node) {
        printf("unlink: file %s not found\n", path);
        return -ENOENT; // File not found
    }

    // Free all blocks associated with the file
    for (int i = 0; i < node->block_count; i++) {
        if (node->blocks[i] >= 0) { // Ensure valid block numbers
            free_block(node->blocks[i]);
            node->blocks[i] = -1; // Mark the block as unused
        }
    }

    // Clear inode data and decrement inode count
    memset(node, 0, sizeof(inode_t));
    inode_count--;

    // Save updated inode data to disk
    save_inodes();

    printf("unlink(%s) -> 0\n", path);
    return 0;
}




static int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Lookup the inode for the given path
    inode_t *inode = inode_lookup(path);
    if (!inode) {
        printf("read: inode not found for path %s\n", path);
        return -ENOENT;
    }

    // If offset is beyond the file size, return 0 (nothing to read)
    if (offset >= inode->size) {
        printf("read: offset %ld beyond file size %d\n", offset, inode->size);
        return 0;
    }

    // Adjust size to not exceed the file's remaining size
    size_t remaining = inode->size - offset;
    if (size > remaining) {
        size = remaining;
    }

    size_t total_read = 0;

    // Read the data block by block
    while (size > 0) {
        // Calculate the block index and the offset within the block
        int block_index = (offset + total_read) / BLOCK_SIZE;
        size_t block_offset = (offset + total_read) % BLOCK_SIZE;
        size_t to_read = BLOCK_SIZE - block_offset;
        if (to_read > size) {
            to_read = size;
        }

        // Get the block number for the current block index
        if (block_index >= inode->block_count) {
            printf("read: block_index %d out of range for inode %s\n", block_index, path);
            return -EIO; // Unexpected condition
        }

        int block_num = inode->blocks[block_index];
        if (block_num < 0) {
            printf("read: invalid block number %d for block_index %d\n", block_num, block_index);
            return -EIO; // Invalid block mapping
        }

        // Get the actual block data and copy to the buffer
        void *block = blocks_get_block(block_num);
        if (!block) {
            printf("read: failed to get block number %d\n", block_num);
            return -EIO; // Failed to retrieve block
        }

        memcpy(buf + total_read, (char *)block + block_offset, to_read);

        // Update counters and reduce remaining size
        total_read += to_read;
        size -= to_read;
    }

    printf("read(%s, %zu bytes, offset %ld) -> %zu bytes read\n", path, size, offset, total_read);
    return total_read;
}


static int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Lookup the inode for the given path
    inode_t *inode = inode_lookup(path);
    if (!inode) {
        printf("write: inode not found for path %s\n", path);
        return -ENOENT;
    }

    // Expand file size if necessary
    if (offset + size > inode->size) {
        inode->size = offset + size;
    }

    size_t total_written = 0;

    // Write data block by block
    while (size > 0) {
        // Calculate the block index and the offset within the block
        int block_index = (offset + total_written) / BLOCK_SIZE;
        size_t block_offset = (offset + total_written) % BLOCK_SIZE;
        size_t to_write = BLOCK_SIZE - block_offset;
        if (to_write > size) {
            to_write = size;
        }

        // Allocate a block if necessary
        if (block_index >= inode->block_count) {
            int block_num = inode_add_block(inode);
            if (block_num < 0) {
                printf("write: failed to allocate block for inode %s\n", path);
                return -ENOSPC;
            }
        }

        // Get the block number and ensure it's valid
        int block_num = inode->blocks[block_index];
        if (block_num < 0) {
            printf("write: invalid block number %d at block_index %d\n", block_num, block_index);
            return -EIO; // Invalid block mapping
        }

        // Get the actual block and write the data
        void *block = blocks_get_block(block_num);
        if (!block) {
            printf("write: failed to get block number %d\n", block_num);
            return -EIO; // Failed to retrieve block
        }

        memcpy((char *)block + block_offset, buf + total_written, to_write);

        // Update counters
        total_written += to_write;
        size -= to_write;
    }

    // Save inode metadata after writing
    save_inodes();
    printf("write(%s, %zu bytes, offset %ld) -> %zu bytes written\n",
       path, (size_t)total_written, offset, (size_t)total_written);

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

