#pragma once

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int val, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strchr(const char *s, int c);

/* Writes the decimal (base 10) or hex (base 16) representation of value
 * into buf (caller-provided, must be large enough) and returns buf. */
char *itoa(int value, char *buf, int base);
