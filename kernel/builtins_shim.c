/* Real GCC inline-asm bodies for the builtins declared in builtins.h -
 * exists only so the kernel keeps building under the current interim
 * GCC-based toolchain. Dropped in Phase T7 once the default build
 * switches to CC, which intercepts every call site in codegen and never
 * needs (or could compile - it has no inline-asm support at all) a
 * linkable definition for these. Do not add any behavior here that
 * isn't already implemented by CC's builtins.c. */

#include "builtins.h"

void __builtin_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t __builtin_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void __builtin_outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

uint16_t __builtin_inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void __builtin_hlt(void) {
    __asm__ volatile ("hlt");
}

void __builtin_lidt(void *idt_descriptor) {
    __asm__ volatile ("lidt (%0)" : : "r"(idt_descriptor));
}

void __builtin_gdt_flush(void *gdt_descriptor) {
    __asm__ volatile (
        "lgdt (%0)\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "ljmp $0x08, $1f\n"
        "1:\n"
        :
        : "r"(gdt_descriptor)
        : "eax"
    );
}
