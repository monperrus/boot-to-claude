; Kernel entry point. Stage2 jumps here in 32-bit protected mode with
; segments already flat and a real-mode stack still set up. Establish our
; own stack and hand off to C.

BITS 32

section .text.entry
global _start
extern kmain
extern _bss_start
extern _bss_end

_start:
    mov esp, stack_top

    ; Zero .bss: the raw disk-loaded kernel image has no zero-fill
    ; guarantee (objcopy -O binary drops NOBITS sections entirely).
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kmain
.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:
