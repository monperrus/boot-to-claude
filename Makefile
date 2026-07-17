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
# and KERNEL_START_LBA / KERNEL_SECTORS in boot/stage2.S
DISK_SECTORS  := 4096
KERNEL_START_SECTOR := 5
KERNEL_MAX_SECTORS  := 512

BOOT_BIN   := $(BUILD)/boot.bin
STAGE2_BIN := $(BUILD)/stage2.bin
DISK_IMG   := $(BUILD)/disk.img

KERNEL_C_SRCS := kernel/kmain.c kernel/console.c kernel/serial.c kernel/gdt.c \
                 kernel/idt.c kernel/string.c kernel/memory.c kernel/selftest.c \
                 kernel/ata.c kernel/fs/tinyfs.c kernel/fs/tinyfs_disk.c \
                 kernel/keyboard.c kernel/shell/shell.c \
                 kernel/lang/lexer.c kernel/lang/script/ast.c \
                 kernel/lang/script/parser.c kernel/lang/script/eval.c
KERNEL_ASM_SRCS := kernel/entry.S kernel/idt_stubs.S
KERNEL_OBJS   := $(patsubst kernel/%.S,$(BUILD)/%.o,$(KERNEL_ASM_SRCS)) \
                 $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_C_SRCS))
KERNEL_ELF    := $(BUILD)/kernel.elf
KERNEL_BIN    := $(BUILD)/kernel.bin

MKFS_BIN := $(BUILD)/mkfs_tinyfs
MKFS_OBJS := $(BUILD)/host_tinyfs.o $(BUILD)/host_mkfs_tinyfs.o
# host-path:fs-name pairs seeded into every disk image; the host paths
# alone are listed separately so they can be used as make prerequisites
MKFS_SEED_FILES := tests/hello.tsh
MKFS_SEED_SPECS := tests/hello.tsh:hello.tsh

.PHONY: all run clean

all: $(DISK_IMG)

BUILD_SUBDIRS := $(BUILD) $(BUILD)/fs $(BUILD)/shell $(BUILD)/lang $(BUILD)/lang/script

$(BUILD_SUBDIRS):
	mkdir -p $@

$(BOOT_BIN): boot/boot.S | $(BUILD)
	$(AS) -f bin -o $@ $<

$(STAGE2_BIN): boot/stage2.S | $(BUILD)
	$(AS) -f bin -o $@ $<

$(BUILD)/%.o: kernel/%.S | $(BUILD_SUBDIRS)
	$(AS) -f elf32 -o $@ $<

$(BUILD)/%.o: kernel/%.c | $(BUILD_SUBDIRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/link.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -c%s $@); budget=$$(( $(KERNEL_MAX_SECTORS) * 512 )); \
	if [ "$$size" -gt "$$budget" ]; then \
		echo "error: $@ is $$size bytes, exceeds the $$budget-byte ($(KERNEL_MAX_SECTORS)-sector) boot budget" >&2; \
		echo "       bump KERNEL_SECTORS in boot/stage2.S, KERNEL_MAX_SECTORS here, and TINYFS_FS_START_LBA in kernel/fs/tinyfs.h together" >&2; \
		exit 1; \
	fi

# --- host-side mkfs tool (build-time infrastructure, not kernel code) ---

$(BUILD)/host_tinyfs.o: kernel/fs/tinyfs.c | $(BUILD)
	$(HOSTCC) -Wall -Wextra -O2 -c -o $@ $<

$(BUILD)/host_mkfs_tinyfs.o: tools/mkfs_tinyfs.c | $(BUILD)
	$(HOSTCC) -Wall -Wextra -O2 -c -o $@ $<

$(MKFS_BIN): $(MKFS_OBJS)
	$(HOSTCC) -o $@ $(MKFS_OBJS)

$(DISK_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(MKFS_BIN) $(MKFS_SEED_FILES)
	dd if=/dev/zero of=$@ bs=512 count=$(DISK_SECTORS) status=none
	dd if=$(BOOT_BIN) of=$@ conv=notrunc status=none
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	$(MKFS_BIN) $@ $(MKFS_SEED_SPECS)

run: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -display none -serial stdio -no-reboot

clean:
	rm -rf $(BUILD)
