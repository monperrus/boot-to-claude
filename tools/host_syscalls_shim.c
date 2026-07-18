/* Real x86-32 int 0x80 syscall bodies for the names declared in
 * host_syscalls.h - exists only so host tools (tools/mkfs_tinyfs.c) keep
 * building under the current interim GCC-based toolchain. Dropped in
 * Phase T7 once the default build switches to CC, which intercepts
 * every call site in codegen and never needs (or could compile - it has
 * no inline-asm support at all) a linkable definition for these. Must
 * be compiled -m32: these are x86-32 Linux syscall numbers/calling
 * convention, matching what CC's builtins.c emits, since CC only ever
 * targets x86-32.
 *
 * Argument conventions mirror builtins.c exactly - e.g. __sys_open
 * takes only path+flags, with mode hardcoded to 420 (0644), since
 * that's what the actual builtin does. */

#include "host_syscalls.h"

int __sys_open(const char *path, int flags) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(5), "b"(path), "c"(flags), "d"(420) : "memory");
    return ret;
}

int __sys_read(int fd, void *buf, unsigned int count) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(3), "b"(fd), "c"(buf), "d"(count) : "memory");
    return ret;
}

int __sys_write(int fd, const void *buf, unsigned int count) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(4), "b"(fd), "c"(buf), "d"(count) : "memory");
    return ret;
}

int __sys_lseek(int fd, int offset, int whence) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(19), "b"(fd), "c"(offset), "d"(whence) : "memory");
    return ret;
}

int __sys_close(int fd) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(6), "b"(fd) : "memory");
    return ret;
}

void __sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(1), "b"(code));
    for (;;) {}
}
