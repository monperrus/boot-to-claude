#include "selftest.h"

#include <stdint.h>

#include "console.h"
#include "fs/tinyfs.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

static int ok = 1;

static void check(int cond, const char *what) {
    if (!cond) {
        ok = 0;
        console_write("  FAIL: ");
        console_write(what);
        console_write("\n");
        serial_write("  FAIL: ");
        serial_write(what);
        serial_write("\n");
    }
}

static void test_string(void) {
    char buf[16];

    check(strlen("hello") == 5, "strlen");
    check(strcmp("abc", "abc") == 0, "strcmp equal");
    check(strcmp("abc", "abd") != 0, "strcmp differs");

    itoa(12345, buf, 10);
    check(strcmp(buf, "12345") == 0, "itoa decimal positive");

    itoa(-42, buf, 10);
    check(strcmp(buf, "-42") == 0, "itoa decimal negative");

    itoa(255, buf, 16);
    check(strcmp(buf, "ff") == 0, "itoa hex");
}

static void test_memory(void) {
    memory_init();

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    check(a != NULL && b != NULL, "kmalloc basic");
    check(a != b, "kmalloc distinct pointers");

    memset(a, 0xAB, 64);
    uint8_t *pa = (uint8_t *)a;
    int pattern_ok = 1;
    for (int i = 0; i < 64; i++) {
        if (pa[i] != 0xAB) {
            pattern_ok = 0;
        }
    }
    check(pattern_ok, "kmalloc write/readback");

    kfree(a);
    kfree(b);

    /* After freeing everything, a large allocation should succeed again -
     * proves coalescing merged the freed blocks back together. The heap is
     * 1MB (see memory.c); this is comfortably larger than a and b combined
     * but well under the full heap. */
    void *c = kmalloc(512 * 1024);
    check(c != NULL, "kmalloc large after free (coalescing)");
    kfree(c);
}

static void test_fs(void) {
    tinyfs_dirent_t entries[TINYFS_NUM_INODES];
    int n = tinyfs_list(entries, TINYFS_NUM_INODES);
    check(n >= 1, "tinyfs_list finds seeded files");

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, "hello.txt") == 0) {
            found = 1;
        }
    }
    check(found, "tinyfs_list contains hello.txt");

    char buf[64];
    uint32_t len = 0;
    int rc = tinyfs_read("hello.txt", buf, sizeof(buf) - 1, &len);
    check(rc == 0, "tinyfs_read hello.txt succeeds");
    if (rc == 0) {
        buf[len] = '\0';
        check(strcmp(buf, "Hello, tinyfs!\n") == 0, "hello.txt content matches");
    }

    check(tinyfs_create("selftest.tmp") == 0, "tinyfs_create new file");
    check(tinyfs_write("selftest.tmp", "abc123", 6) == 0, "tinyfs_write new file");
    len = 0;
    rc = tinyfs_read("selftest.tmp", buf, sizeof(buf) - 1, &len);
    check(rc == 0 && len == 6, "tinyfs_read roundtrip length");
    if (rc == 0) {
        buf[len] = '\0';
        check(strcmp(buf, "abc123") == 0, "tinyfs_read roundtrip content");
    }
    check(tinyfs_delete("selftest.tmp") == 0, "tinyfs_delete");
}

int selftest_run(void) {
    ok = 1;
    console_write("selftest: running\n");
    serial_write("selftest: running\n");

    test_string();
    test_memory();
    test_fs();

    if (ok) {
        console_write("SELFTEST PASS\n");
        serial_write("SELFTEST PASS\n");
    } else {
        console_write("SELFTEST FAIL\n");
        serial_write("SELFTEST FAIL\n");
    }
    return ok;
}
