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

/* Seeds a tinyfs file straight from a host file, for test fixtures under
 * tests/ so their source lives in one place instead of being duplicated
 * as a C string literal here. */
static void seed_file_from_host(const char *host_path, const char *fs_name) {
    FILE *f = fopen(host_path, "rb");
    if (!f) {
        fprintf(stderr, "mkfs_tinyfs: cannot open seed file %s\n", host_path);
        exit(1);
    }
    char buf[TINYFS_MAX_FILE_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    tinyfs_create(fs_name);
    tinyfs_write(fs_name, buf, (uint32_t)n);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <disk-image> [host-path:fs-name ...]\n", argv[0]);
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

    for (int i = 2; i < argc; i++) {
        char spec[256];
        size_t speclen = strlen(argv[i]);
        if (speclen >= sizeof(spec)) {
            fprintf(stderr, "mkfs_tinyfs: seed spec too long: %s\n", argv[i]);
            return 1;
        }
        memcpy(spec, argv[i], speclen + 1);

        char *sep = strchr(spec, ':');
        if (!sep) {
            fprintf(stderr, "mkfs_tinyfs: bad seed spec %s (want host-path:fs-name)\n", argv[i]);
            return 1;
        }
        *sep = '\0';
        seed_file_from_host(spec, sep + 1);
    }

    fclose(disk);
    printf("mkfs_tinyfs: formatted %s (%u data blocks available)\n",
           argv[1], (unsigned)TINYFS_DATA_BLOCKS);
    return 0;
}
