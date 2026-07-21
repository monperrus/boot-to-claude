#pragma once
/* Only chmod (see toolchain/ld/link_static_exec.c, marking its output
 * executable) - no struct stat anywhere in this codebase. */
int chmod(const char *path, int mode);
