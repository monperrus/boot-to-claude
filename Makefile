AS      := nasm
CC      := gcc
LD      := ld
OBJCOPY := objcopy
QEMU    := qemu-system-i386
BUILD   := build

CFLAGS  := -m32 -ffreestanding -fno-pic -fno-stack-protector -fno-builtin -nostdlib \
           -Wall -Wextra -O2 -std=c11 -Ikernel
LDFLAGS := -m elf_i386 -T kernel/link.ld -nostdlib

BOOT_BIN   := $(BUILD)/boot.bin
STAGE2_BIN := $(BUILD)/stage2.bin
DISK_IMG   := $(BUILD)/disk.img

KERNEL_C_SRCS := kernel/kmain.c kernel/console.c kernel/serial.c kernel/gdt.c \
                 kernel/idt.c kernel/string.c kernel/memory.c kernel/selftest.c
KERNEL_ASM_SRCS := kernel/entry.S kernel/idt_stubs.S
KERNEL_OBJS   := $(patsubst kernel/%.S,$(BUILD)/%.o,$(KERNEL_ASM_SRCS)) \
                 $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_C_SRCS))
KERNEL_ELF    := $(BUILD)/kernel.elf
KERNEL_BIN    := $(BUILD)/kernel.bin

.PHONY: all run clean

all: $(DISK_IMG)

$(BUILD):
	mkdir -p $(BUILD)

$(BOOT_BIN): boot/boot.S | $(BUILD)
	$(AS) -f bin -o $@ $<

$(STAGE2_BIN): boot/stage2.S | $(BUILD)
	$(AS) -f bin -o $@ $<

$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(AS) -f elf32 -o $@ $<

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/link.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(DISK_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BOOT_BIN) of=$@ conv=notrunc status=none
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=5 conv=notrunc status=none

run: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -display none -serial stdio -no-reboot

clean:
	rm -rf $(BUILD)
