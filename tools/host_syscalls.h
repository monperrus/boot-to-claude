#pragma once
/* Declares the raw Linux x86-32 syscalls host tools use in place of
 * libc file I/O (see docs/toolchain.md and tools/mkfs_tinyfs.c). Under
 * CC (--target=host), calls to these exact names are recognized in
 * codegen and compiled directly to `int 0x80` - no function body is
 * ever compiled or linked for them, matching kernel/builtins.h's
 * __builtin_* mechanism. Under the current interim GCC-based build,
 * real definitions live in tools/host_syscalls_shim.c. */

int __sys_open(const char *path, int flags);
int __sys_read(int fd, void *buf, unsigned int count);
int __sys_write(int fd, const void *buf, unsigned int count);
int __sys_lseek(int fd, int offset, int whence);
int __sys_close(int fd);
void __sys_exit(int code);
