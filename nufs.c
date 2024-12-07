// implements a custom filesystem using FUSE (Filesystem in Userspace), with functionalities for managing inodes, file operations, 
// and directory operations. It provides mechanisms to initialize storage, save and load inode metadata, create and manipulate files and directories, 
// and handle file operations like reading, writing, and renaming.

// Importing the necessary libraries
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <stdlib.h>
#include "blocks.h"
#include "bitmap.h"

#define FUSE_USE_VERSION 26
#include <fuse.h>

#define MAX_FILES 128 // Maximum number of inodes/files supported.
#define MAX_BLOCKS_PER_FILE 128 // Maximum number of data blocks per file.

// Define the metadata layout
#define INODE_META_BLOCK 1 // Stores the number of inodes
// Range for storing inode data.
#define FIRST_INODE_BLOCK 2
#define LAST_INODE_BLOCK 27

#define FIRST_DATA_BLOCK 28 // Starting block for file data.
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(inode_t))

//Represents metadata and block mapping for files and directories
// Inode (inode_t)
typedef struct {
    char path[256]; //The full path to the file or directory.
    int size; // File size in bytes.
    int blocks[MAX_BLOCKS_PER_FILE]; // Array mapping to data blocks.
    int block_count; // Number of blocks allocated to the file.
    mode_t mode; // Permissions and file type (e.g., directory or regular file).
    // Timestamps for last access, modification, and metadata change.
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

/**
 * Save the inodes to disk.
 * Initializes the filesystem with a backing disk image
 * Loads inodes from disk or creates a root directory (/) if missing.
 */
void save_inodes() {
    // Write inode_count to meta block
    void *meta_block = blocks_get_block(INODE_META_BLOCK);
    if (!meta_block) {
        fprintf(stderr, "save_inodes: Failed to access block %d\n", INODE_META_BLOCK);
        return;
    }
    memcpy(meta_block, &inode_count, sizeof(inode_count));
    // Write inodes to data blocks
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
        // Copy inodes to block
        memcpy(b, &inodes[written], count * sizeof(inode_t));
        written += count;
        block_num++;
    }

    // Ensure data is written to disk
    if (msync(blocks_base, NUFS_SIZE, MS_SYNC) == -1) {
        perror("msync");
    }

    printf("Saved %d inodes to disk.\n", inode_count);
}

// Reads inode metadata and array from disk into memory.
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

    // Trust the stored inode_count
    printf("Loaded %d inodes from disk.\n", inode_count);
}

/**
 * Initialize filesystem storage with a disk image.
 * 
 * @param path Path to the disk image file
 * @note Creates root directory if it doesn't exist
 */
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

/**
 * Look up an inode by its full path.
 * 
 * @param path Full path of the file or directory
 * @return Pointer to the inode, or NULL if not found
 */
inode_t *inode_lookup(const char *path) {
    char normalized_path[256];
    strncpy(normalized_path, path, sizeof(normalized_path) - 1);
    normalized_path[sizeof(normalized_path) - 1] = '\0';

    size_t len = strlen(normalized_path);
    while (len > 1 && normalized_path[len - 1] == '/') {
        normalized_path[--len] = '\0';
    }

    for (int i = 0; i < inode_count; i++) {
        if (strcmp(inodes[i].path, normalized_path) == 0) {
            return &inodes[i];
        }
    }
    return NULL;
}

/**
 * Create a new inode for a file or directory.
 * 
 * @param path Full path for the new inode
 * @param mode File mode (permissions and type)
 * @return Pointer to the newly created inode
 */
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

/**
 * Allocate a new block for a file.
 * 
 * @param node Pointer to the inode
 * @return Block number of the newly allocated block, or negative error code
 */
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

/**
 * Rename a file or directory.
 * 
 * @param from Original path
 * @param to New path
 * @return 0 on success, negative error code on failure
 */
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

    if (strlen(to) >= sizeof(inode->path)) {
        fprintf(stderr, "rename: destination path %s is too long\n", to);
        return -ENAMETOOLONG;
    }

    strncpy(inode->path, to, sizeof(inode->path) - 1);
    inode->path[sizeof(inode->path) - 1] = '\0';

    time_t now = time(NULL);
    inode->mtime = now;
    inode->ctime = now;

    save_inodes();
    printf("rename(%s -> %s) successful\n", from, to);
    return 0;
}

/**
 * Check file access permissions.
 * 
 * @param path File path
 * @param mask Access mode
 * @return 0 on success
 */
int nufs_access(const char *path, int mask) {
    inode_t *node = inode_lookup(path);
    if (!node) {
        printf("access: file or directory %s not found\n", path);
        return -ENOENT;
    }

    printf("access(%s, %04o) -> 0\n", path, mask);
    return 0;
}

/**
 * Get file attributes.
 * 
 * @param path File path
 * @param st Buffer to fill with file attributes
 * @return 0 on success
 */
int nufs_getattr(const char *path, struct stat *st) {
    inode_t *node = inode_lookup(path);
    if (!node) {
        fprintf(stderr, "getattr: inode not found for path %s\n", path);
        return -ENOENT;
    }

    // Fill stat struct with inode metadata
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


/**
 * Read directory contents.
 * 
 * @param path Directory path
 * @param buf Buffer to fill with directory entries
 * @param filler Function to add entries to the buffer
 * @param offset Unused
 * @param fi File information (unused)
 * @return 0 on success
 */
int nufs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("readdir(%s)\n", path);

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // List files and directories in the given directory
    for (int i = 0; i < inode_count; i++) {

        // Check if the inode path is a direct child of the given directory
        if (strcmp(path, "/") == 0) {
            const char *name = inodes[i].path + 1;
            if (strlen(name) > 0 && strchr(name, '/') == NULL) {
                filler(buf, name, NULL, 0);
            }
        // Check if the inode path is a child of the given directory
        } else {
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
/**
 * Create a new file.
 * 
 * @param path File path
 * @param mode File permissions
 * @param rdev Unused
 * @return 0 on success, or negative error code
 */
static int nufs_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("mknod(%s, %o)\n", path, mode);

    // Check if file already exists
    if (inode_count >= MAX_FILES) {
        fprintf(stderr, "mknod: max file count reached\n");
        return -ENOSPC;
    }

    // Check if path is too long
    if (strlen(path) >= sizeof(inodes[0].path)) {
        fprintf(stderr, "mknod: path too long\n");
        return -ENAMETOOLONG;
    }

    // Check if file already exists
    inode_t *node = inode_create(path, mode ? mode : (S_IFREG | 0644));
    if (!node) {
        fprintf(stderr, "mknod: failed to create inode for %s\n", path);
        return -ENOSPC;
    }

    save_inodes();
    printf("mknod: successfully created file %s\n", path);
    return 0;
}

/**
 * Create a new directory.
 * 
 * @param path Directory path
 * @param mode Directory permissions
 * @return 0 on success, or negative error code
 */
int nufs_mkdir(const char *path, mode_t mode) {
    printf("mkdir(%s, %o)\n", path, mode);

    if (inode_lookup(path)) {
        printf("mkdir: directory %s already exists\n", path);
        return -EEXIST;
    }

    inode_t *node = inode_create(path, mode | S_IFDIR);
    if (!node) {
        printf("mkdir: failed to create inode for directory %s\n", path);
        return -ENOMEM;
    }

    node->size = 0;
    save_inodes();
    printf("mkdir: successfully created directory %s\n", path);
    return 0;
}

/**
 * Remove a file or directory.
 * 
 * @param path File or directory path
 * @return 0 on success, or negative error code
 */
int nufs_unlink(const char *path) {
    inode_t *inode = inode_lookup(path);
    // Check if file exists
    if (!inode) {
        fprintf(stderr, "unlink: file %s not found\n", path);
        return -ENOENT;
    }
    // Check if file is a directory
    if (inode->mode & S_IFDIR) {
        fprintf(stderr, "unlink: cannot unlink directory %s\n", path);
        return -EISDIR;
    }

    // Free data blocks
    for (int i = 0; i < inode->block_count; i++) {
        if (inode->blocks[i] >= 0) {
            free_block(inode->blocks[i]);
        }
    }

    int index = inode - inodes;
    memmove(&inodes[index], &inodes[index + 1], (inode_count - index - 1) * sizeof(inode_t));
    memset(&inodes[inode_count - 1], 0, sizeof(inode_t));
    inode_count--;

    save_inodes();
    printf("unlink(%s) -> 0\n", path);
    return 0;
}

/**
 * Write data to a file.
 * 
 * @param path File path
 * @param buf Buffer containing data to write
 * @param size Number of bytes to write
 * @param offset Starting byte offset
 * @param fi File information (unused)
 * @return Number of bytes written, or negative error code
 */
static int nufs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode_t *inode = inode_lookup(path);
    // Check if file exists
    if (!inode) {
        fprintf(stderr, "write: inode not found for path %s\n", path);
        return -ENOENT;
    }

    // Check if file is a directory
    if (!(inode->mode & S_IFREG)) {
        fprintf(stderr, "write: cannot write to directory %s\n", path);
        return -EISDIR;
    }

    // Write data block by block
    size_t total_written = 0;
    while (total_written < size) {
        int block_index = (offset + total_written) / BLOCK_SIZE;
        size_t block_offset = (offset + total_written) % BLOCK_SIZE;
        size_t to_write = BLOCK_SIZE - block_offset;

        if (to_write > size - total_written) {
            to_write = size - total_written;
        }

        if (block_index >= inode->block_count) {
            int new_block = inode_add_block(inode);
            if (new_block < 0) {
                fprintf(stderr, "write: failed to allocate block\n");
                return total_written ? total_written : -ENOSPC;
            }
        }

        // Write data to block
        int block_num = inode->blocks[block_index];
        void *block = blocks_get_block(block_num);
        if (!block) {
            fprintf(stderr, "write: failed to get block %d\n", block_num);
            return -EIO;
        }

        memcpy((char *)block + block_offset, buf + total_written, to_write);
        total_written += to_write;
    }

    // Update file size if necessary
    if (offset + total_written > inode->size) {
        inode->size = offset + total_written;
    }

    time_t now = time(NULL);
    inode->mtime = now;
    inode->ctime = now;

    save_inodes();
    return total_written;
}

/**
 * Read data from a file.
 * 
 * @param path File path
 * @param buf Buffer to read data into
 * @param size Number of bytes to read
 * @param offset Starting byte offset
 * @param fi File information (unused)
 * @return Number of bytes read, or negative error code
 */
static int nufs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode_t *inode = inode_lookup(path);
    // Check if file exists
    if (!inode) {
        fprintf(stderr, "read: inode not found for path '%s'\n", path);
        return -ENOENT;
    }

    // Check if file is a directory
    if (!(inode->mode & S_IFREG)) {
        fprintf(stderr, "read: cannot read directory %s\n", path);
        return -EISDIR;
    }
    // Check if offset is beyond file size
    if (offset >= inode->size) {
        return 0;
    }
    // Check if size exceeds remaining file size
    size_t remaining = inode->size - offset;
    if (size > remaining) {
        size = remaining;
    }

    size_t total_read = 0;
    // Read data block by block
    while (total_read < size) {
        int block_index = (offset + total_read) / BLOCK_SIZE;
        size_t block_offset = (offset + total_read) % BLOCK_SIZE;
        size_t to_read = BLOCK_SIZE - block_offset;

        if (to_read > size - total_read) {
            to_read = size - total_read;
        }

        if (block_index >= inode->block_count) {
            break;
        }

        int block_num = inode->blocks[block_index];
        void *block = blocks_get_block(block_num);
        if (!block) {
            fprintf(stderr, "read: failed to get block %d for path '%s'\n", block_num, path);
            return -EIO;
        }

        memcpy(buf + total_read, (char *)block + block_offset, to_read);
        total_read += to_read;
    }

    inode->atime = time(NULL);
    save_inodes();

    return total_read;
}

/**
 * Initialize FUSE operations with custom filesystem functions.
 * 
 * @param ops Pointer to the FUSE operations struct
 */
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

// Main function to mount the filesystem with the given disk image.
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
