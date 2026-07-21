#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <hostsys.h>

/* toolchain/{as,cc,ld}'s own stdio.h implementation, for --target=host
 * builds (see docs/toolchain.md, Phase T8). Every FILE op is a direct
 * raw syscall - no buffering, since none of these build tools are
 * performance-sensitive enough to need it.
 *
 * fmt_core implements exactly the printf-format subset toolchain/*.c
 * actually uses (surveyed before writing this): %s %d %u %c, the '+'
 * flag, and 'l'/'ll' length modifiers - which are parsed and silently
 * ignored, always reading a plain 4-byte value regardless. That matches
 * stdint.h's int64_t/long being 4-byte aliases here (see its comment):
 * a caller casting a value to `long long` for a `%lld` conversion is
 * still only ever pushing 4 real bytes, so reading 4 bytes back here is
 * the only self-consistent choice, not a rounding-down of something
 * larger. */

FILE g_stdout_file = { 1, 0, 0 };
FILE g_stderr_file = { 2, 0, 0 };

static int fmt_core(char *buf, size_t bufsize, const char *fmt, va_list ap) {
    size_t o = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (o + 1 < bufsize) buf[o] = *p;
            o++;
            continue;
        }
        p++;
        int plus = 0;
        if (*p == '+') { plus = 1; p++; }
        while (*p == 'l') p++;
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s) {
                if (o + 1 < bufsize) buf[o] = *s;
                o++;
                s++;
            }
        } else if (*p == 'c') {
            int c = va_arg(ap, int);
            if (o + 1 < bufsize) buf[o] = (char)c;
            o++;
        } else if (*p == 'd') {
            int v = va_arg(ap, int);
            int neg = v < 0;
            unsigned int uv = neg ? (unsigned int)(-v) : (unsigned int)v;
            char tmp[16];
            int ti = 0;
            if (uv == 0) tmp[ti++] = '0';
            while (uv) { tmp[ti++] = (char)('0' + uv % 10); uv /= 10; }
            if (neg) { if (o + 1 < bufsize) buf[o] = '-'; o++; }
            else if (plus) { if (o + 1 < bufsize) buf[o] = '+'; o++; }
            while (ti > 0) { ti--; if (o + 1 < bufsize) buf[o] = tmp[ti]; o++; }
        } else if (*p == 'u') {
            unsigned int v = va_arg(ap, unsigned int);
            char tmp[16];
            int ti = 0;
            if (v == 0) tmp[ti++] = '0';
            while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
            while (ti > 0) { ti--; if (o + 1 < bufsize) buf[o] = tmp[ti]; o++; }
        } else if (*p == '%') {
            if (o + 1 < bufsize) buf[o] = '%';
            o++;
        } else {
            if (o + 1 < bufsize) buf[o] = '%';
            o++;
            if (*p) { if (o + 1 < bufsize) buf[o] = *p; o++; }
        }
    }
    if (bufsize > 0) buf[o < bufsize ? o : bufsize - 1] = '\0';
    return (int)o;
}

FILE *fopen(const char *path, const char *mode) {
    /* Linux x86 open(2) flag values - O_WRONLY=1, O_CREAT=64, O_TRUNC=512
     * (decimal, not octal: CC's lexer has no octal-literal support, see
     * lexer.c's numeric-literal reader). Only "r"/"rb" and "w"/"wb" are
     * ever passed (surveyed before writing this). */
    int flags = (mode[0] == 'r') ? 0 : (1 | 64 | 512);
    int fd = __sys_open(path, flags);
    if (fd < 0) return NULL;
    FILE *f = malloc(sizeof(FILE));
    f->fd = fd;
    f->eof = 0;
    f->err = 0;
    return f;
}

size_t fread(void *ptr, size_t size, size_t n, FILE *f) {
    if (size == 0 || n == 0) return 0;
    int want = (int)(size * n);
    int got = __sys_read(f->fd, ptr, (unsigned int)want);
    if (got < 0) { f->err = 1; return 0; }
    if (got < want) f->eof = 1;
    return (size_t)(got / (int)size);
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f) {
    if (size == 0 || n == 0) return 0;
    int want = (int)(size * n);
    int got = __sys_write(f->fd, ptr, (unsigned int)want);
    if (got < 0) { f->err = 1; return 0; }
    return (size_t)(got / (int)size);
}

int fclose(FILE *f) {
    __sys_close(f->fd);
    free(f);
    return 0;
}

int fseek(FILE *f, long offset, int whence) {
    __sys_lseek(f->fd, (int)offset, whence);
    return 0;
}

long ftell(FILE *f) {
    return (long)__sys_lseek(f->fd, 0, SEEK_CUR);
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    __sys_write(f->fd, &ch, 1);
    return c;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[4096];
    int n = fmt_core(buf, sizeof(buf), fmt, ap);
    int wn = n < (int)sizeof(buf) - 1 ? n : (int)sizeof(buf) - 1;
    __sys_write(f->fd, buf, (unsigned int)wn);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = fmt_core(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

void perror(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}
