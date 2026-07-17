#include "memory.h"

#include <stdint.h>

/* Fixed heap region. Starts at 1MB to stay clear of the kernel image
 * (loaded at 0x10000), the boot stack (0x90000), and the legacy BIOS/VGA
 * memory holes below 1MB. QEMU's default RAM is well above this. */
#define HEAP_START 0x100000u
#define HEAP_SIZE  (1u * 1024u * 1024u)

typedef struct block_header {
    size_t size; /* usable size, excluding this header */
    int free;
    struct block_header *next;
} block_header_t;

static block_header_t *heap_head;

void memory_init(void) {
    heap_head = (block_header_t *)HEAP_START;
    heap_head->size = HEAP_SIZE - sizeof(block_header_t);
    heap_head->free = 1;
    heap_head->next = NULL;
}

void *kmalloc(size_t size) {
    block_header_t *cur = heap_head;

    while (cur) {
        if (cur->free && cur->size >= size) {
            size_t remaining = cur->size - size;
            if (remaining >= sizeof(block_header_t) + 16) {
                block_header_t *split =
                    (block_header_t *)((uint8_t *)cur + sizeof(block_header_t) + size);
                split->size = remaining - sizeof(block_header_t);
                split->free = 1;
                split->next = cur->next;
                cur->next = split;
                cur->size = size;
            }
            cur->free = 0;
            return (void *)((uint8_t *)cur + sizeof(block_header_t));
        }
        cur = cur->next;
    }
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }

    block_header_t *blk = (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));
    blk->free = 1;

    /* Coalesce adjacent free blocks. The list is address-ordered since
     * kmalloc only ever splits forward, so a single forward pass suffices. */
    block_header_t *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(block_header_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}
