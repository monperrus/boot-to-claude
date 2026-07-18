/* Host-side build tool: formats the tinyfs region of a disk image and
 * seeds it with initial files. Not part of the OS - infrastructure, like
 * the assembler/linker/QEMU, run at build time on the host. Reuses
 * kernel/fs/tinyfs.c verbatim (compiled natively here, freestanding in
 * the kernel) so there is exactly one implementation of the fs format.
 *
 * Libc-free: uses only the __sys_* raw-syscall names CC's builtins.c
 * intercepts directly at the call site (see docs/toolchain.md), and
 * kernel/string.h instead of the system <string.h>, so this file
 * compiles unchanged once Phase T7 switches the build to CC. Under the
 * current interim GCC-based build, __sys_open/read/write/lseek/close/
 * exit need real definitions to link against - see
 * tools/host_syscalls_shim.c (dropped once CC takes over, same as
 * kernel/builtins_shim.c - CC intercepts every call site directly and
 * couldn't even parse that shim's inline asm). */

#include "../kernel/fs/tinyfs.h"
#include "../kernel/string.h"
#include "host_syscalls.h"

#define O_RDONLY 0
#define O_RDWR   2
#define SEEK_SET 0

static void die(const char *msg) {
    __sys_write(2, msg, strlen(msg));
    __sys_exit(1);
}

static int g_disk_fd;

static void disk_block_read(uint32_t block, void *buf) {
    long offset = (long)(TINYFS_FS_START_LBA + block) * TINYFS_BLOCK_SIZE;
    if (__sys_lseek(g_disk_fd, (int)offset, SEEK_SET) < 0 ||
        __sys_read(g_disk_fd, buf, TINYFS_BLOCK_SIZE) != TINYFS_BLOCK_SIZE) {
        die("mkfs_tinyfs: disk read failed\n");
    }
}

static void disk_block_write(uint32_t block, const void *buf) {
    long offset = (long)(TINYFS_FS_START_LBA + block) * TINYFS_BLOCK_SIZE;
    if (__sys_lseek(g_disk_fd, (int)offset, SEEK_SET) < 0 ||
        __sys_write(g_disk_fd, buf, TINYFS_BLOCK_SIZE) != TINYFS_BLOCK_SIZE) {
        die("mkfs_tinyfs: disk write failed\n");
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
    int fd = __sys_open(host_path, O_RDONLY);
    if (fd < 0) {
        die("mkfs_tinyfs: cannot open seed file\n");
    }
    static char buf[TINYFS_MAX_FILE_SIZE];
    int n = __sys_read(fd, buf, sizeof(buf));
    __sys_close(fd);
    if (n < 0) {
        die("mkfs_tinyfs: cannot read seed file\n");
    }

    tinyfs_create(fs_name);
    tinyfs_write(fs_name, buf, (uint32_t)n);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        die("usage: mkfs_tinyfs <disk-image> [host-path:fs-name ...]\n");
    }

    g_disk_fd = __sys_open(argv[1], O_RDWR);
    if (g_disk_fd < 0) {
        die("mkfs_tinyfs: cannot open disk image\n");
    }

    tinyfs_init(disk_block_read, disk_block_write);
    tinyfs_format();

    seed_file("hello.txt", "Hello, tinyfs!\n");

    for (int i = 2; i < argc; i++) {
        static char spec[256];
        size_t speclen = strlen(argv[i]);
        if (speclen >= sizeof(spec)) {
            die("mkfs_tinyfs: seed spec too long\n");
        }
        memcpy(spec, argv[i], speclen + 1);

        char *sep = strchr(spec, ':');
        if (!sep) {
            die("mkfs_tinyfs: bad seed spec (want host-path:fs-name)\n");
        }
        *sep = '\0';
        seed_file_from_host(spec, sep + 1);
    }

    __sys_close(g_disk_fd);
    __sys_write(1, "mkfs_tinyfs: done\n", 19);
    __sys_exit(0);
    return 0;
}
