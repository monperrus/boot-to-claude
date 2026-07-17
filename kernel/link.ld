ENTRY(_start)

SECTIONS
{
    . = 0x10000;   /* matches KERNEL_LOAD_ADDR in boot/stage2.S */

    .text : {
        *(.text.entry)
        *(.text*)
    }
    .rodata : {
        *(.rodata*)
    }
    .data : {
        *(.data*)
    }
    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }
}
