#include <sys/stat.h>
#include <hostsys.h>

int chmod(const char *path, int mode) {
    return __sys_chmod(path, mode);
}
