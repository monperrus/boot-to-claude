#include <string.h>
#include <stdint.h>

/* toolchain/{as,cc,ld}'s own string.h implementation, for --target=host
 * builds (see docs/toolchain.md, Phase T8). Deliberately not shared with
 * kernel/string.c: same simple algorithms, different target/header. */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int val, size_t n) {
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    size_t i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return c == '\0' ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}
