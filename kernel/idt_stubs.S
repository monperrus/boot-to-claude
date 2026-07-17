; Exception-only ISR stubs (vectors 0-31). This kernel is single-tasking and
; polling-only - interrupts are never enabled (no `sti`, no PIC remap, no
; IRQ handlers) - so the only things that can land here are synchronous CPU
; exceptions. There is no recovery path: each stub reports the vector/error
; code to the C handler and then halts forever, so no iret/register
; restore is needed.

BITS 32
section .text

extern isr_handler

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push dword %1
    jmp isr_common
%endmacro

isr_common:
    ; stack: [vector] [error_code] ...  (both macros leave this layout)
    mov eax, [esp]
    mov ebx, [esp+4]
    push ebx
    push eax
    call isr_handler
.hang:
    hlt
    jmp .hang

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dd isr_stub_%+i
%assign i i+1
%endrep
