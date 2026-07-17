AS      := nasm
CC      := gcc
HOSTCC  := gcc
LD      := ld
OBJCOPY := objcopy
QEMU    := qemu-system-i386
BUILD   := build

CFLAGS  := -m32 -ffreestanding -fno-pic -fno-stack-protector -fno-builtin -nostdlib \
           -Wall -Wextra -O2 -std=c11 -Ikernel
LDFLAGS := -m elf_i386 -T kernel/link.ld -nostdlib

# Must match TINYFS_DISK_SECTORS / TINYFS_FS_START_LBA in kernel/fs/tinyfs.h
DISK_SECTORS  := 4096
KERNEL_START_SECTOR := 5

BOOT_BIN   := $(BUILD)/boot.bin
STAGE2_BIN := $(BUILD)/stage2.bin
DISK_IMG   := $(BUILD)/disk.img

KERNEL_C_SRCS := kernel/kmain.c kernel/console.c kernel/serial.c kernel/gdt.c \
                 kernel/idt.c kernel/string.c kernel/memory.c kernel/selftest.c \
                 kernel/ata.c kernel/fs/tinyfs.c kernel/fs/tinyfs_disk.c
KERNEL_ASM_SRCS := kernel/entry.S kernel/idt_stubs.S
KERNEL_OBJS   := $(patsubst kernel/%.S,$(BUILD)/%.o,$(KERNEL_ASM_SRCS)) \
                 $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_C_SRCS))
KERNEL_ELF    := $(BUILD)/kernel.elf
KERNEL_BIN    := $(BUILD)/kernel.bin

MKFS_BIN := $(BUILD)/mkfs_tinyfs
MKFS_OBJS := $(BUILD)/host_tinyfs.o $(BUILD)/host_mkfs_tinyfs.o

.PHONY: all run clean

all: $(DISK_IMG)

$(BUILD) $(BUILD)/fs:
	mkdir -p $@

$(BOOT_BIN): boot/boot.S | $(BUILD)
	$(AS) -f bin -o $@ $<

$(STAGE2_BIN): boot/stage2.S | $(BUILD)
	$(AS) -f bin -o $@ $<

$(BUILD)/%.o: kernel/%.S | $(BUILD) $(BUILD)/fs
	$(AS) -f elf32 -o $@ $<

$(BUILD)/%.o: kernel/%.c | $(BUILD) $(BUILD)/fs
	$(CC) $(CFLAGS) -c -o $@ $<

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/link.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# --- host-side mkfs tool (build-time infrastructure, not kernel code) ---

$(BUILD)/host_tinyfs.o: kernel/fs/tinyfs.c | $(BUILD)
	$(HOSTCC) -Wall -Wextra -O2 -c -o $@ $<

$(BUILD)/host_mkfs_tinyfs.o: tools/mkfs_tinyfs.c | $(BUILD)
	$(HOSTCC) -Wall -Wextra -O2 -c -o $@ $<

$(MKFS_BIN): $(MKFS_OBJS)
	$(HOSTCC) -o $@ $(MKFS_OBJS)

$(DISK_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(MKFS_BIN)
	dd if=/dev/zero of=$@ bs=512 count=$(DISK_SECTORS) status=none
	dd if=$(BOOT_BIN) of=$@ conv=notrunc status=none
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	$(MKFS_BIN) $@

run: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -display none -serial stdio -no-reboot

clean:
	rm -rf $(BUILD)
