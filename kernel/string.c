#include "string.h"

#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int val, size_t n) {
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)val;
    }
    return dst;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return c == '\0' ? (char *)s : NULL;
}

char *itoa(int value, char *buf, int base) {
    char tmp[34];
    int i = 0;
    int negative = (base == 10 && value < 0);
    unsigned int uval = negative ? (unsigned int)(-value) : (unsigned int)value;

    if (uval == 0) {
        tmp[i++] = '0';
    }
    while (uval) {
        int digit = (int)(uval % (unsigned int)base);
        tmp[i++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        uval /= (unsigned int)base;
    }
    if (negative) {
        tmp[i++] = '-';
    }

    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return buf;
}
