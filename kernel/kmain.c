#include "console.h"
#include "serial.h"

void kmain(void) {
    serial_init();

    console_clear();
    console_write("tiny-os: kernel entered\n");
    serial_write("tiny-os: kernel entered\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
