#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t used;
    char name[27];
    uint32_t size;
    uint32_t blocks[8];
} __attribute__((packed)) inode_t;

typedef union {
    int32_t i;
    char *s;
} value_t;

typedef void (*read_fn_t)(uint32_t block, void *buf);

static read_fn_t g_reader;
static inode_t g_inodes[64];

int find_free(int max) {
    for (int i = 0; i < max; i++) {
        if (!g_inodes[i].used) {
            return i;
        }
    }
    return -1;
}

void set_reader(read_fn_t fn) {
    g_reader = fn;
}

int sum_blocks(inode_t *ino) {
    int total = 0;
    int i = 0;
    while (i < 8) {
        total = total + (int)ino->blocks[i];
        i++;
    }
    return total;
}

int main(void) {
    value_t v;
    v.i = 42;
    int idx = find_free(64);
    return idx + v.i;
}
