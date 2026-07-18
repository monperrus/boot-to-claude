#pragma once
/* Declares the fixed set of low-level operations the kernel needs that
 * C can't express portably - port I/O, halting, loading the IDT/GDT.
 * Under CC (the self-hosted compiler, see docs/toolchain.md), calls to
 * these exact names are recognized in codegen and compiled directly to
 * a handful of instructions - no function body is ever compiled or
 * linked for them. Under the current interim GCC-based build, real
 * definitions live in kernel/builtins_shim.c (GCC inline asm) purely so
 * the same kernel source keeps building until Phase T7 switches the
 * default build over to CC, at which point builtins_shim.c is dropped
 * (CC never needs it - every call site is intercepted before linking). */

#include <stdint.h>

void __builtin_outb(uint16_t port, uint8_t val);
uint8_t __builtin_inb(uint16_t port);
void __builtin_outw(uint16_t port, uint16_t val);
uint16_t __builtin_inw(uint16_t port);
void __builtin_hlt(void);
void __builtin_lidt(void *idt_descriptor);
void __builtin_gdt_flush(void *gdt_descriptor);
