#pragma once

/* tinyfs: a minimal flat-namespace filesystem. See docs/tinyfs.md for the
 * on-disk layout. This header is included by both the kernel (freestanding
 * build) and tools/mkfs_tinyfs.c (native host build) - it must only use
 * portable, standard C. */

#include <stdint.h>

#define TINYFS_BLOCK_SIZE 512
#define TINYFS_MAGIC       0x54465331u /* "TFS1" */

/* Disk layout constants. TINYFS_DISK_SECTORS must match the disk image
 * size the Makefile builds (DISK_SECTORS). */
#define TINYFS_DISK_SECTORS 4096
#define TINYFS_FS_START_LBA 640 /* leaves room for boot sector + a 256KB kernel budget */

#define TINYFS_NUM_INODES    64
#define TINYFS_NAME_MAX      27 /* 26 chars + NUL */
#define TINYFS_DIRECT_BLOCKS 8

#define TINYFS_SB_BLOCK       0
#define TINYFS_BITMAP_BLOCK   1
#define TINYFS_BITMAP_BLOCKS  1 /* 4096 bits = up to 4096 data blocks (2MB) */
#define TINYFS_INODE_BLOCK    (TINYFS_BITMAP_BLOCK + TINYFS_BITMAP_BLOCKS)
#define TINYFS_INODE_BLOCKS   8 /* 64 inodes * 64 bytes / 512-byte block */
#define TINYFS_DATA_BLOCK     (TINYFS_INODE_BLOCK + TINYFS_INODE_BLOCKS)

#define TINYFS_TOTAL_FS_BLOCKS (TINYFS_DISK_SECTORS - TINYFS_FS_START_LBA)
#define TINYFS_DATA_BLOCKS     (TINYFS_TOTAL_FS_BLOCKS - TINYFS_DATA_BLOCK)

#define TINYFS_MAX_FILE_SIZE (TINYFS_DIRECT_BLOCKS * TINYFS_BLOCK_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t num_inodes;
    uint32_t data_blocks;
} __attribute__((packed)) tinyfs_superblock_t;

typedef struct {
    uint8_t  used;
    char     name[TINYFS_NAME_MAX];
    uint32_t size; /* bytes */
    uint32_t blocks[TINYFS_DIRECT_BLOCKS]; /* fs-relative block numbers, 0 = none */
} __attribute__((packed)) tinyfs_inode_t;

typedef struct {
    char     name[TINYFS_NAME_MAX];
    uint32_t size;
} tinyfs_dirent_t;

typedef void (*tinyfs_block_read_fn)(uint32_t block, void *buf);
typedef void (*tinyfs_block_write_fn)(uint32_t block, const void *buf);

void tinyfs_init(tinyfs_block_read_fn read_fn, tinyfs_block_write_fn write_fn);

/* Formats a blank fs region: zeroes the bitmap/inode table and writes a
 * fresh superblock. Used only by tools/mkfs_tinyfs at image-build time. */
int tinyfs_format(void);

/* Loads the superblock/bitmap/inode table into memory. Must be called
 * before any other operation other than tinyfs_format. */
int tinyfs_mount(void);

int tinyfs_create(const char *name);
int tinyfs_delete(const char *name);
int tinyfs_write(const char *name, const void *data, uint32_t size);
int tinyfs_read(const char *name, void *out, uint32_t max_len, uint32_t *out_len);

/* Fills out[] with up to max_entries directory entries, returns the count
 * written. */
int tinyfs_list(tinyfs_dirent_t *out, int max_entries);
