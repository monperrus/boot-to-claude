#pragma once
#include <stddef.h>
#include <stdarg.h>

/* Minimal buffered-in-name-only FILE - every read/write is a direct
 * __sys_read/__sys_write, see toolchain/hostlib/stdio.c. Just enough of
 * real stdio for toolchain/{as,cc,ld}'s own source (file I/O + a small
 * printf-format subset: %s %d %u %c, the '+' flag, and 'l'/'ll' length
 * modifiers that are parsed and ignored - see hostlib/stdio.c). */
typedef struct FILE {
    int fd;
    int eof;
    int err;
} FILE;

extern FILE g_stdout_file;
extern FILE g_stderr_file;
#define stdout (&g_stdout_file)
#define stderr (&g_stderr_file)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)

FILE *fopen(const char *path, const char *mode);
size_t fread(void *ptr, size_t size, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int fclose(FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int fputc(int c, FILE *f);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
void perror(const char *msg);
