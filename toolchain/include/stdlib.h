#pragma once
#include <stddef.h>
#include <stdint.h>

void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void free(void *p);
void exit(int code);

/* Only base 10/16 are ever passed (see toolchain/cc/lexer.c) and every
 * call site passes NULL for endptr - both hostlib/stdlib.c and the
 * type this returns reflect that (see stdint.h: int64_t is a 32-bit
 * alias here, so this never actually returns a true 64-bit value). */
int64_t strtoll(const char *s, char **endptr, int base);

/* base 0 (used by ld/main.c's `--flat-base 0xADDR`) auto-detects a
 * "0x"/"0X" prefix as hex, otherwise decimal - no octal auto-detect
 * (nothing in this codebase ever passes a leading-zero value meaning
 * octal). */
unsigned long strtoul(const char *s, char **endptr, int base);
