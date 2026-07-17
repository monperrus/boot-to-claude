#include "console.h"
#include "fs/tinyfs_disk.h"
#include "gdt.h"
#include "idt.h"
#include "memory.h"
#include "selftest.h"
#include "serial.h"
#include "shell/shell.h"

void kmain(void) {
    serial_init();
    console_clear();

    gdt_init();
    idt_init();
    memory_init();

    console_write("tiny-os: kernel entered\n");
    serial_write("tiny-os: kernel entered\n");

    if (tinyfs_disk_init() != 0) {
        console_write("tinyfs: mount FAILED\n");
        serial_write("tinyfs: mount FAILED\n");
    }

    selftest_run();

    shell_run();
}
