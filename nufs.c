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
    time_t atime;
    time_t mtime;
    time_t ctime;
} inode_t;

static inode_t inodes[MAX_FILES];
static int inode_count = 0;

// Function declarations
void save_inodes();
void load_inodes();
void storage_init(const char *path);
inode_t *inode_lookup(const char *path);
inode_t *inode_create(const char *path, mode_t mode);
int inode_add_block(inode_t *node);
int nufs_access(const char *path, int mask);
int nufs_getattr(const char *path, struct stat *st);
int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
static int nufs_mknod(const char *path, mode_t mode, dev_t rdev);
int nufs_mkdir(const char *path, mode_t mode);
int nufs_rename(const char *from, const char *to);
int nufs_unlink(const char *path);
static int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
static int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
void nufs_init_ops(struct fuse_operations *ops);

void save_inodes() {
    // Write inode_count to block INODE_META_BLOCK
    void *meta_block = blocks_get_block(INODE_META_BLOCK);
    if (!meta_block) {
        fprintf(stderr, "save_inodes: Failed to access block %d\n", INODE_META_BLOCK);
        return;
    }
    memcpy(meta_block, &inode_count, sizeof(inode_count));

    // Now write inodes across blocks 2..27
    int total = inode_count;
    int written = 0;
    int block_num = FIRST_INODE_BLOCK;
    while (written < total && block_num <= LAST_INODE_BLOCK) {
        int count = total - written;
        if (count > INODES_PER_BLOCK) {
            count = INODES_PER_BLOCK;
        }

        void *b = blocks_get_block(block_num);
        if (!b) {
            fprintf(stderr, "save_inodes: Failed to access inode block %d\n", block_num);
            return;
        }

        memcpy(b, &inodes[written], count * sizeof(inode_t));
        written += count;
        block_num++;
    }

    printf("Saved %d inodes to disk.\n", inode_count);
}

void load_inodes() {
    void *meta_block = blocks_get_block(INODE_META_BLOCK);
    if (!meta_block) {
        fprintf(stderr, "load_inodes: Failed to access block %d\n", INODE_META_BLOCK);
        return;
    }

    memcpy(&inode_count, meta_block, sizeof(inode_count));
    memset(inodes, 0, sizeof(inodes));

    int read_count = 0;
    int block_num = FIRST_INODE_BLOCK;
    while (read_count < inode_count && block_num <= LAST_INODE_BLOCK) {
        int count = inode_count - read_count;
        if (count > INODES_PER_BLOCK) {
            count = INODES_PER_BLOCK;
        }

        void *b = blocks_get_block(block_num);
        if (!b) {
            fprintf(stderr, "load_inodes: Failed to access inode block %d\n", block_num);
            return;
        }

        memcpy(&inodes[read_count], b, count * sizeof(inode_t));
        read_count += count;
        block_num++;
    }

    // No recalculation of inode_count here; trust the stored value
    printf("Loaded %d inodes from disk.\n", inode_count);
}


// Initialize storage
void storage_init(const char *path) {
    printf("Initializing storage with disk image: %s\n", path);

    blocks_init(path);
    load_inodes();

    // Ensure root directory exists
    if (!inode_lookup("/")) {
        inode_create("/", S_IFDIR | 0755);
    }

    printf("Storage initialized successfully.\n");
}

// Find an inode by path
inode_t *inode_lookup(const char *path) {
    char normalized_path[256];
    // Normalize path: trim trailing slashes, except for root "/"
    strncpy(normalized_path, path, sizeof(normalized_path) - 1);
    normalized_path[sizeof(normalized_path) - 1] = '\0';

    size_t len = strlen(normalized_path);
    while (len > 1 && normalized_path[len - 1] == '/') {
        normalized_path[--len] = '\0'; // Remove trailing slashes
    }

    for (int i = 0; i < inode_count; i++) {
        if (strcmp(inodes[i].path, normalized_path) == 0) {
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
    node->path[sizeof(node->path) - 1] = '\0';
    node->size = 0;
    node->block_count = 0;
    node->mode = mode;
    time_t now = time(NULL);
    node->atime = node->mtime = node->ctime = now;
    save_inodes();
    return node;
}

// Allocate a new block for the given inode
int inode_add_block(inode_t *node) {
    if (node->block_count >= MAX_BLOCKS_PER_FILE) {
        fprintf(stderr, "inode_add_block: max blocks reached for inode\n");
        return -ENOSPC;
    }

    int block_index = alloc_block();
    if (block_index < 0) {
        fprintf(stderr, "inode_add_block: failed to allocate block\n");
        return -ENOSPC;
    }

    node->blocks[node->block_count++] = block_index;
    save_inodes();

    printf("inode_add_block: block %d allocated for inode, total blocks %d\n", block_index, node->block_count);
    return block_index;
}

int nufs_rename(const char *from, const char *to) {
    inode_t *inode = inode_lookup(from);
    if (!inode) {
        fprintf(stderr, "rename: source file %s not found\n", from);
        return -ENOENT;
    }

    if (inode_lookup(to)) {
        fprintf(stderr, "rename: destination %s already exists\n", to);
        return -EEXIST;
    }

    // Check path length
    if (strlen(to) >= sizeof(inode->path)) {
        fprintf(stderr, "rename: destination path %s is too long\n", to);
        return -ENAMETOOLONG;
    }

    // Update the path in the inode
    strncpy(inode->path, to, sizeof(inode->path) - 1);
    inode->path[sizeof(inode->path) - 1] = '\0';

    // Update modification and change times
    time_t now = time(NULL);
    inode->mtime = now;
    inode->ctime = now;

    save_inodes();
    printf("rename(%s -> %s) successful\n", from, to);
    return 0;
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
    inode_t *node = inode_lookup(path);
    if (!node) {
        fprintf(stderr, "getattr: inode not found for path %s\n", path);
        return -ENOENT;
    }

    memset(st, 0, sizeof(struct stat));
    st->st_mode = node->mode;
    st->st_size = node->size;
    st->st_nlink = (node->mode & S_IFDIR) ? 2 : 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = node->atime;
    st->st_mtime = node->mtime;
    st->st_ctime = node->ctime;
    st->st_blocks = (node->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    st->st_blksize = BLOCK_SIZE;

    printf("getattr(%s) -> mode: %o, size: %d, blocks: %ld\n", path, node->mode, node->size, st->st_blocks);
    return 0;
}

int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("readdir(%s)\n", path);

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for (int i = 0; i < inode_count; i++) {
        // If path is "/", show top-level files and directories
        if (strcmp(path, "/") == 0) {
            const char *name = inodes[i].path + 1; // Skip leading "/"
            if (strlen(name) > 0 && strchr(name, '/') == NULL) {
                filler(buf, name, NULL, 0);
            }
        } 
        // For nested paths, show contents of that specific directory
        else {
            size_t path_len = strlen(path);
            if (strncmp(inodes[i].path, path, path_len) == 0 && 
                strlen(inodes[i].path) > path_len && 
                inodes[i].path[path_len] == '/' &&
                strchr(inodes[i].path + path_len + 1, '/') == NULL) {
                
                const char *name = inodes[i].path + path_len + 1;
                filler(buf, name, NULL, 0);
            }
        }
    }

    return 0;
}

static int nufs_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("mknod(%s, %o)\n", path, mode);

    if (inode_count >= MAX_FILES) {
        fprintf(stderr, "mknod: max file count reached\n");
        return -ENOSPC;
    }

    if (strlen(path) >= sizeof(inodes[0].path)) {
        fprintf(stderr, "mknod: path too long\n");
        return -ENAMETOOLONG;
    }

    inode_t *node = inode_create(path, mode ? mode : (S_IFREG | 0644));
    if (!node) {
        fprintf(stderr, "mknod: failed to create inode for %s\n", path);
        return -ENOSPC;
    }

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
    inode_t *inode = inode_lookup(path);
    if (!inode) {
        fprintf(stderr, "unlink: file %s not found\n", path);
        return -ENOENT;
    }

    if (inode->mode & S_IFDIR) {
        fprintf(stderr, "unlink: cannot unlink directory %s\n", path);
        return -EISDIR;
    }

    // Free all blocks associated with the file
    for (int i = 0; i < inode->block_count; i++) {
        if (inode->blocks[i] >= 0) {
            free_block(inode->blocks[i]);
        }
    }

    // Find and remove the inode
    int index = inode - inodes; // Compute the inode index
    memmove(&inodes[index], &inodes[index + 1], (inode_count - index - 1) * sizeof(inode_t));
    memset(&inodes[inode_count - 1], 0, sizeof(inode_t));
    inode_count--;

    save_inodes();
    printf("unlink(%s) -> 0\n", path);
    return 0;
}

static int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode_t *inode = inode_lookup(path);
    if (!inode) {
        fprintf(stderr, "write: inode not found for path %s\n", path);
        return -ENOENT;
    }

    // Ensure the file is a regular file
    if (!(inode->mode & S_IFREG)) {
        fprintf(stderr, "write: cannot write to non-regular file %s\n", path);
        return -EISDIR;
    }

    size_t total_written = 0; 

    while (total_written < size) {
        int block_index = (offset + total_written) / BLOCK_SIZE;
        size_t block_offset = (offset + total_written) % BLOCK_SIZE;
        size_t to_write = BLOCK_SIZE - block_offset;

        if (to_write > size - total_written) {
            to_write = size - total_written;
        }

        // Allocate a new block if needed
        if (block_index >= inode->block_count) {
            int new_block = inode_add_block(inode);
            if (new_block < 0) {
                fprintf(stderr, "write: failed to allocate block\n");
                return total_written ? total_written : -ENOSPC;
            }
        }

        int block_num = inode->blocks[block_index];
        void *block = blocks_get_block(block_num);
        if (!block) {
            fprintf(stderr, "write: failed to retrieve block %d\n", block_num);
            return -EIO;
        }

        memcpy((char *)block + block_offset, buf + total_written, to_write);
        total_written += to_write;
    }

    // Update file size if we've written beyond current size
    if (offset + total_written > inode->size) {
        inode->size = offset + total_written;
    }

    // Update timestamps
    time_t now = time(NULL);
    inode->mtime = now;
    inode->ctime = now;

    save_inodes();
    return total_written;
}

static int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode_t *inode = inode_lookup(path);
    if (!inode) {
        fprintf(stderr, "read: inode not found for path '%s'\n", path);
        return -ENOENT;
    }

    // Ensure the file is a regular file
    if (!(inode->mode & S_IFREG)) {
        fprintf(stderr, "read: cannot read from non-regular file %s\n", path);
        return -EISDIR;
    }

    // Check if we're past the end of the file
    if (offset >= inode->size) {
        return 0;
    }

    // Adjust read size if it would go beyond file size
    size_t remaining = inode->size - offset;
    if (size > remaining) {
        size = remaining;
    }

    size_t total_read = 0;

    while (total_read < size) {
        int block_index = (offset + total_read) / BLOCK_SIZE;
        size_t block_offset = (offset + total_read) % BLOCK_SIZE;
        size_t to_read = BLOCK_SIZE - block_offset;

        if (to_read > size - total_read) {
            to_read = size - total_read;
        }

        if (block_index >= inode->block_count) {
            break; // No more blocks to read
        }

        int block_num = inode->blocks[block_index];
        void *block = blocks_get_block(block_num);
        if (!block) {
            fprintf(stderr, "read: failed to retrieve block %d for path '%s'\n", block_num, path);
            return -EIO;
        }

        memcpy(buf + total_read, (char *)block + block_offset, to_read);
        total_read += to_read;
    }

    // Update access time
    inode->atime = time(NULL);
    save_inodes();

    return total_read;
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
    ops->rename = nufs_rename;
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