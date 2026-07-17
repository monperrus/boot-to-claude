#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "memory.h"
#include "selftest.h"
#include "serial.h"

void kmain(void) {
    serial_init();
    console_clear();

    gdt_init();
    idt_init();
    memory_init();

    console_write("tiny-os: kernel entered\n");
    serial_write("tiny-os: kernel entered\n");

    selftest_run();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
