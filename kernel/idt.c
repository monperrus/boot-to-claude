#include "idt.h"

#include <stdint.h>

#include "console.h"
#include "serial.h"
#include "string.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

extern uint32_t isr_stub_table[32];

static struct idt_entry idt[256];
static struct idt_ptr idtp;

static void idt_set_gate(int n, uint32_t handler) {
    idt[n].offset_low = handler & 0xFFFF;
    idt[n].selector = 0x08; /* kernel code segment, see gdt.c */
    idt[n].zero = 0;
    idt[n].type_attr = 0x8E; /* present, ring0, 32-bit interrupt gate */
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;

    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stub_table[i]);
    }

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

/* Called from idt_stubs.S. Never returns - the stub halts right after. */
void isr_handler(uint32_t vector, uint32_t error_code) {
    char numbuf[16];

    console_write("\nEXCEPTION vector=");
    console_write(itoa((int)vector, numbuf, 10));
    console_write(" error_code=");
    console_write(itoa((int)error_code, numbuf, 10));
    console_write(" - halted\n");

    serial_write("\nEXCEPTION vector=");
    serial_write(itoa((int)vector, numbuf, 10));
    serial_write(" error_code=");
    serial_write(itoa((int)error_code, numbuf, 10));
    serial_write(" - halted\n");
}
