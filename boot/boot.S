; Stage 1 MBR boot sector.
; BIOS loads this at 0x7c00 and jumps to it in 16-bit real mode.
; Job: load stage2 (STAGE2_SECTORS sectors starting at LBA 1) to 0x0000:0x7e00
; and jump to it. Kept deliberately minimal - all real work happens in stage2.

BITS 16
ORG 0x7c00

STAGE2_SECTORS equ 4      ; must match the padding at the end of stage2.S

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    mov si, msg_boot
    call print_str16

    mov si, dap
    mov ah, 0x42        ; INT13h extended read (LBA)
    mov dl, 0x80         ; first hard disk (matches qemu -drive raw image)
    int 0x13
    jc disk_error

    jmp 0x0000:0x7e00

print_str16:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e         ; BIOS teletype output
    mov bh, 0
    int 0x10
    jmp .loop
.done:
    popa
    ret

disk_error:
    mov si, msg_err
    call print_str16
.hang:
    hlt
    jmp .hang

align 4
dap:                     ; disk address packet for INT13h AH=0x42
    db 0x10               ; packet size
    db 0                  ; reserved
    dw STAGE2_SECTORS      ; sectors to read
    dw 0x7e00              ; destination offset
    dw 0x0000               ; destination segment
    dq 1                    ; starting LBA (sector right after the MBR)

msg_boot: db "boot ", 0
msg_err:  db "disk err", 0

times 510-($-$$) db 0
dw 0xAA55
