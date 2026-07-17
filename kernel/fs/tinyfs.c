#include "tinyfs.h"

/* Resolves to kernel/string.h under the freestanding kernel build
 * (-Ikernel puts it ahead of the system header) and to the host libc
 * <string.h> when this file is compiled natively for tools/mkfs_tinyfs -
 * same declarations (memcpy/memset/strlen/strcmp) either way. */
#include "string.h"

static tinyfs_block_read_fn  block_read;
static tinyfs_block_write_fn block_write;

static tinyfs_superblock_t sb;
static uint8_t bitmap[TINYFS_BITMAP_BLOCKS * TINYFS_BLOCK_SIZE];
static tinyfs_inode_t inode_table[TINYFS_NUM_INODES];

void tinyfs_init(tinyfs_block_read_fn read_fn, tinyfs_block_write_fn write_fn) {
    block_read = read_fn;
    block_write = write_fn;
}

static void load_bitmap(void) {
    for (uint32_t i = 0; i < TINYFS_BITMAP_BLOCKS; i++) {
        block_read(TINYFS_BITMAP_BLOCK + i, bitmap + (size_t)i * TINYFS_BLOCK_SIZE);
    }
}

static void save_bitmap(void) {
    for (uint32_t i = 0; i < TINYFS_BITMAP_BLOCKS; i++) {
        block_write(TINYFS_BITMAP_BLOCK + i, bitmap + (size_t)i * TINYFS_BLOCK_SIZE);
    }
}

static void load_inodes(void) {
    uint8_t *p = (uint8_t *)inode_table;
    for (uint32_t i = 0; i < TINYFS_INODE_BLOCKS; i++) {
        block_read(TINYFS_INODE_BLOCK + i, p + (size_t)i * TINYFS_BLOCK_SIZE);
    }
}

static void save_inodes(void) {
    const uint8_t *p = (const uint8_t *)inode_table;
    for (uint32_t i = 0; i < TINYFS_INODE_BLOCKS; i++) {
        block_write(TINYFS_INODE_BLOCK + i, p + (size_t)i * TINYFS_BLOCK_SIZE);
    }
}

static int bitmap_test(uint32_t idx) {
    return (bitmap[idx / 8] >> (idx % 8)) & 1;
}

static void bitmap_set(uint32_t idx, int val) {
    if (val) {
        bitmap[idx / 8] = (uint8_t)(bitmap[idx / 8] | (1u << (idx % 8)));
    } else {
        bitmap[idx / 8] = (uint8_t)(bitmap[idx / 8] & ~(1u << (idx % 8)));
    }
}

static int alloc_data_block(uint32_t *out_block) {
    for (uint32_t i = 0; i < sb.data_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i, 1);
            *out_block = TINYFS_DATA_BLOCK + i;
            return 0;
        }
    }
    return -1;
}

static void free_data_block(uint32_t block) {
    bitmap_set(block - TINYFS_DATA_BLOCK, 0);
}

static tinyfs_inode_t *find_inode(const char *name) {
    for (int i = 0; i < TINYFS_NUM_INODES; i++) {
        if (inode_table[i].used && strcmp(inode_table[i].name, name) == 0) {
            return &inode_table[i];
        }
    }
    return NULL;
}

static int find_free_inode(void) {
    for (int i = 0; i < TINYFS_NUM_INODES; i++) {
        if (!inode_table[i].used) {
            return i;
        }
    }
    return -1;
}

int tinyfs_format(void) {
    memset(bitmap, 0, sizeof(bitmap));
    memset(inode_table, 0, sizeof(inode_table));

    sb.magic = TINYFS_MAGIC;
    sb.total_blocks = TINYFS_TOTAL_FS_BLOCKS;
    sb.num_inodes = TINYFS_NUM_INODES;
    sb.data_blocks = TINYFS_DATA_BLOCKS;

    uint8_t sbbuf[TINYFS_BLOCK_SIZE];
    memset(sbbuf, 0, sizeof(sbbuf));
    memcpy(sbbuf, &sb, sizeof(sb));
    block_write(TINYFS_SB_BLOCK, sbbuf);

    save_bitmap();
    save_inodes();
    return 0;
}

int tinyfs_mount(void) {
    uint8_t sbbuf[TINYFS_BLOCK_SIZE];
    block_read(TINYFS_SB_BLOCK, sbbuf);
    memcpy(&sb, sbbuf, sizeof(sb));
    if (sb.magic != TINYFS_MAGIC) {
        return -1;
    }
    load_bitmap();
    load_inodes();
    return 0;
}

int tinyfs_create(const char *name) {
    if (find_inode(name)) {
        return -1;
    }
    int idx = find_free_inode();
    if (idx < 0) {
        return -1;
    }

    tinyfs_inode_t *ino = &inode_table[idx];
    memset(ino, 0, sizeof(*ino));
    ino->used = 1;

    int i = 0;
    while (name[i] && i < TINYFS_NAME_MAX - 1) {
        ino->name[i] = name[i];
        i++;
    }
    ino->name[i] = '\0';

    save_inodes();
    return 0;
}

int tinyfs_delete(const char *name) {
    tinyfs_inode_t *ino = find_inode(name);
    if (!ino) {
        return -1;
    }
    for (int b = 0; b < TINYFS_DIRECT_BLOCKS; b++) {
        if (ino->blocks[b]) {
            free_data_block(ino->blocks[b]);
        }
    }
    ino->used = 0;
    save_bitmap();
    save_inodes();
    return 0;
}

int tinyfs_write(const char *name, const void *data, uint32_t size) {
    tinyfs_inode_t *ino = find_inode(name);
    if (!ino || size > TINYFS_MAX_FILE_SIZE) {
        return -1;
    }

    for (int b = 0; b < TINYFS_DIRECT_BLOCKS; b++) {
        if (ino->blocks[b]) {
            free_data_block(ino->blocks[b]);
            ino->blocks[b] = 0;
        }
    }

    const uint8_t *p = data;
    uint32_t remaining = size;
    int b = 0;
    while (remaining > 0) {
        uint32_t block;
        if (alloc_data_block(&block) != 0) {
            save_bitmap();
            save_inodes();
            return -1;
        }

        uint8_t buf[TINYFS_BLOCK_SIZE];
        uint32_t chunk = remaining < TINYFS_BLOCK_SIZE ? remaining : TINYFS_BLOCK_SIZE;
        memset(buf, 0, sizeof(buf));
        memcpy(buf, p, chunk);
        block_write(block, buf);

        ino->blocks[b++] = block;
        p += chunk;
        remaining -= chunk;
    }

    ino->size = size;
    save_bitmap();
    save_inodes();
    return 0;
}

int tinyfs_read(const char *name, void *out, uint32_t max_len, uint32_t *out_len) {
    tinyfs_inode_t *ino = find_inode(name);
    if (!ino) {
        return -1;
    }

    uint32_t n = ino->size < max_len ? ino->size : max_len;
    uint8_t *p = out;
    uint32_t copied = 0;
    int b = 0;
    while (copied < n) {
        uint8_t buf[TINYFS_BLOCK_SIZE];
        block_read(ino->blocks[b], buf);
        uint32_t chunk = (n - copied) < TINYFS_BLOCK_SIZE ? (n - copied) : TINYFS_BLOCK_SIZE;
        memcpy(p + copied, buf, chunk);
        copied += chunk;
        b++;
    }

    if (out_len) {
        *out_len = ino->size;
    }
    return 0;
}

int tinyfs_list(tinyfs_dirent_t *out, int max_entries) {
    int n = 0;
    for (int i = 0; i < TINYFS_NUM_INODES && n < max_entries; i++) {
        if (inode_table[i].used) {
            memcpy(out[n].name, inode_table[i].name, TINYFS_NAME_MAX);
            out[n].size = inode_table[i].size;
            n++;
        }
    }
    return n;
}
