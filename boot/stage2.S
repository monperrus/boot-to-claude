; Stage 2 bootloader. Loaded by stage1 at 0x0000:0x7e00, still 16-bit real mode.
; Job: enable A20, load the kernel from disk into memory below 1MB, build a
; flat GDT, switch to 32-bit protected mode, and jump into the kernel entry.

BITS 16
ORG 0x7e00

KERNEL_LOAD_ADDR equ 0x10000  ; 64KB - plenty of conventional memory headroom
KERNEL_START_LBA equ 5        ; right after stage1 (LBA 0) + stage2 (LBA 1-4)
KERNEL_SECTORS   equ 512      ; 256KB kernel image budget (must match Makefile/tinyfs.h)

stage2_start:
    mov si, msg_stage2
    call print_str16

    call enable_a20
    call load_kernel

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode_entry

; ---------------- 16-bit helpers ----------------

print_str16:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    mov bh, 0
    int 0x10
    jmp .loop
.done:
    popa
    ret

enable_a20:
    in al, 0x92
    or al, 2
    out 0x92, al
    ret

; Reads the kernel in CHUNK_SECTORS-sized pieces rather than one single
; INT13h AH=0x42 call - BIOSes (including SeaBIOS) commonly cap how many
; sectors a single extended read will reliably transfer well below 512,
; and silently misbehave past that rather than returning an error.
CHUNK_SECTORS equ 64

load_kernel:
    mov word [kernel_remaining], KERNEL_SECTORS
    mov dword [kernel_lba], KERNEL_START_LBA
    mov word [kernel_dest_seg], (KERNEL_LOAD_ADDR >> 4)

.loop:
    mov ax, [kernel_remaining]
    cmp ax, 0
    je .done
    cmp ax, CHUNK_SECTORS
    jbe .chunk_size_ok
    mov ax, CHUNK_SECTORS
.chunk_size_ok:
    ; ax = sectors to transfer this iteration

    mov [dap_sectors], ax
    mov si, [kernel_dest_seg]
    mov [dap_segment], si
    mov word [dap_offset], 0
    mov ebx, [kernel_lba]
    mov [dap_lba], ebx
    mov dword [dap_lba + 4], 0

    push ax
    mov si, dap
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc disk_error
    pop ax

    movzx eax, ax                  ; sectors transferred, zero-extended
    add dword [kernel_lba], eax
    sub word [kernel_remaining], ax
    mov cx, ax
    shl cx, 5                       ; sectors * (512/16) = paragraphs advanced
    add word [kernel_dest_seg], cx

    jmp .loop
.done:
    ret

disk_error:
    mov si, msg_disk_err
    call print_str16
.hang:
    hlt
    jmp .hang

kernel_remaining: dw 0
kernel_lba:       dd 0
kernel_dest_seg:  dw 0

align 4
dap:
dap_size:    db 0x10
             db 0
dap_sectors: dw 0
dap_offset:  dw 0
dap_segment: dw 0
dap_lba:     dq 0

msg_stage2:   db "stage2 ", 0
msg_disk_err: db "kernel disk err", 0

; ---------------- GDT: flat 4GB code/data ----------------

align 8
gdt_start:
    dq 0                            ; null descriptor
gdt_code:
    dw 0xFFFF, 0
    db 0, 10011010b, 11001111b, 0
gdt_data:
    dw 0xFFFF, 0
    db 0, 10010010b, 11001111b, 0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ---------------- 32-bit protected mode entry ----------------

BITS 32
protected_mode_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    jmp KERNEL_LOAD_ADDR

times 2048-($-$$) db 0   ; pad to STAGE2_SECTORS (4) * 512 bytes
