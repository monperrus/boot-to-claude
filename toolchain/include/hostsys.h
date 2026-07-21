#pragma once
/* Raw Linux x86-32 syscalls (see tools/host_syscalls.h for the original
 * of this pattern) - CC's builtins.c recognizes calls to these exact
 * names and compiles them directly to `int 0x80`, no function body ever
 * linked. Only toolchain/hostlib/*.c calls these directly; every other
 * toolchain/{as,cc,ld} source file goes through the ordinary stdio/
 * stdlib API instead (see docs/toolchain.md, Phase T8). */

int __sys_open(const char *path, int flags);
int __sys_read(int fd, void *buf, unsigned int count);
int __sys_write(int fd, const void *buf, unsigned int count);
int __sys_lseek(int fd, int offset, int whence);
int __sys_close(int fd);
void __sys_exit(int code);
void *__sys_brk(void *addr);
int __sys_chmod(const char *path, int mode);
