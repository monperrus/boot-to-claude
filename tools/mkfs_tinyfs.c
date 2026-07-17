/* Host-side build tool: formats the tinyfs region of a disk image and
 * seeds it with initial files. Not part of the OS - infrastructure, like
 * the assembler/linker/QEMU, run at build time on the host. Reuses
 * kernel/fs/tinyfs.c verbatim (compiled natively here, freestanding in
 * the kernel) so there is exactly one implementation of the fs format. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/fs/tinyfs.h"

static FILE *disk;

static void disk_block_read(uint32_t block, void *buf) {
    long offset = (long)(TINYFS_FS_START_LBA + block) * TINYFS_BLOCK_SIZE;
    if (fseek(disk, offset, SEEK_SET) != 0 ||
        fread(buf, 1, TINYFS_BLOCK_SIZE, disk) != TINYFS_BLOCK_SIZE) {
        fprintf(stderr, "mkfs_tinyfs: read failed at block %u\n", block);
        exit(1);
    }
}

static void disk_block_write(uint32_t block, const void *buf) {
    long offset = (long)(TINYFS_FS_START_LBA + block) * TINYFS_BLOCK_SIZE;
    if (fseek(disk, offset, SEEK_SET) != 0 ||
        fwrite(buf, 1, TINYFS_BLOCK_SIZE, disk) != TINYFS_BLOCK_SIZE) {
        fprintf(stderr, "mkfs_tinyfs: write failed at block %u\n", block);
        exit(1);
    }
}

static void seed_file(const char *name, const char *contents) {
    tinyfs_create(name);
    tinyfs_write(name, contents, (uint32_t)strlen(contents));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <disk-image>\n", argv[0]);
        return 1;
    }

    disk = fopen(argv[1], "r+b");
    if (!disk) {
        perror("fopen");
        return 1;
    }

    tinyfs_init(disk_block_read, disk_block_write);
    tinyfs_format();

    seed_file("hello.txt", "Hello, tinyfs!\n");

    fclose(disk);
    printf("mkfs_tinyfs: formatted %s (%u data blocks available)\n",
           argv[1], (unsigned)TINYFS_DATA_BLOCKS);
    return 0;
}
