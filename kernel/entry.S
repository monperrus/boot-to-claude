; Kernel entry point. Stage2 jumps here in 32-bit protected mode with
; segments already flat and a real-mode stack still set up. Establish our
; own stack and hand off to C.

BITS 32

section .text.entry
global _start
extern kmain

_start:
    mov esp, stack_top
    call kmain
.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:
