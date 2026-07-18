#include "gdt.h"

#include <stdint.h>

#include "builtins.h"

/* The kernel owns its GDT rather than relying on the one stage2 built on
 * the real-mode stack - stage2's copy happens to still be valid at this
 * point, but the kernel should not depend on bootloader memory. */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr gdtp;

static void gdt_set_gate(int n, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[n].base_low = base & 0xFFFF;
    gdt[n].base_mid = (base >> 16) & 0xFF;
    gdt[n].base_high = (base >> 24) & 0xFF;
    gdt[n].limit_low = limit & 0xFFFF;
    gdt[n].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[n].access = access;
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);            /* null descriptor */
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xC0); /* 0x08: code, ring0, 4GB flat */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xC0); /* 0x10: data, ring0, 4GB flat */

    __builtin_gdt_flush(&gdtp);
}
