#pragma once
#include <stddef.h>

/* CC's own freestanding string.h - just what toolchain/{as,cc,ld} and
 * their hostlib actually call (see docs/toolchain.md, Phase T8). Not
 * shared with kernel/string.h: that one's a smaller, kernel-only subset
 * and the two are compiled for different targets. */

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int val, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
