; Stage 1 MBR boot sector.
; BIOS loads this at 0x7c00 and jumps to it in 16-bit real mode.
; For now: prove the boot path works by printing a character and halting.
; Stage 2 loading is added in Phase 1.

BITS 16
ORG 0x7c00

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    mov al, 'K'
    mov ah, 0x0e        ; BIOS teletype output
    mov bh, 0
    mov bl, 0x07
    int 0x10

    ; Also write to COM1 (0x3f8) so headless QEMU (-display none -serial stdio)
    ; can observe this too. Raw port I/O here only; serial.c driver comes in Phase 2.
    mov dx, 0x3f8 + 5   ; line status register
.wait_tx:
    in al, dx
    test al, 0x20       ; transmit holding register empty?
    jz .wait_tx
    mov dx, 0x3f8
    mov al, 'K'
    out dx, al

hang:
    hlt
    jmp hang

times 510-($-$$) db 0
dw 0xAA55
