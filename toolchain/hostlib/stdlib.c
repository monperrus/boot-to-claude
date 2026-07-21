#include <stdlib.h>
#include <string.h>
#include <hostsys.h>

/* No real free (a bump allocator via brk(2) - see docs/toolchain.md,
 * Phase T8): as/cc/ld are short-lived, single-pass build tools that
 * never run long enough for that to matter, same spirit as the
 * non-optimizing single-pass codegen elsewhere in CC. */
static char *g_heap_cur;
static char *g_heap_end;

void *malloc(size_t size) {
    size_t sz = (size + 15) & ~(size_t)15;
    if (!g_heap_cur) {
        g_heap_cur = (char *)__sys_brk((void *)0);
        g_heap_end = g_heap_cur;
    }
    if (g_heap_cur + sz > g_heap_end) {
        size_t grow = sz > 65536 ? sz : 65536;
        g_heap_end = (char *)__sys_brk((void *)(g_heap_end + grow));
    }
    void *p = g_heap_cur;
    g_heap_cur += sz;
    return p;
}

void *calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = malloc(total);
    memset(p, 0, total);
    return p;
}

void free(void *p) {
    (void)p;
}

void exit(int code) {
    __sys_exit(code);
}

int64_t strtoll(const char *s, char **endptr, int base) {
    int64_t v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (*s) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) base = 16;
        else base = 10;
    }
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned long v = 0;
    while (*s) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return v;
}
