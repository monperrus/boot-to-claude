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
                 kernel/keyboard.c kernel/shell/shell.c kernel/builtins_shim.c \
                 kernel/lang/lexer.c kernel/lang/script/ast.c \
                 kernel/lang/script/parser.c kernel/lang/script/eval.c \
                 kernel/lang/compiler/parser.c kernel/lang/compiler/codegen.c \
                 kernel/lang/vm.c
KERNEL_ASM_SRCS := kernel/entry.S kernel/idt_stubs.S
KERNEL_OBJS   := $(patsubst kernel/%.S,$(BUILD)/%.o,$(KERNEL_ASM_SRCS)) \
                 $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_C_SRCS))
KERNEL_ELF    := $(BUILD)/kernel.elf
KERNEL_BIN    := $(BUILD)/kernel.bin

HOSTCFLAGS := -m32 -Wall -Wextra -O2 -Ikernel

MKFS_BIN := $(BUILD)/mkfs_tinyfs
# host_string.o: kernel/string.c compiled natively - host tools are
# libc-free (see docs/toolchain.md), same as the kernel itself.
# host_syscalls_shim.o: real int 0x80 bodies for the interim GCC build
# only, dropped once Phase T7 switches to CC (see tools/host_syscalls.h).
MKFS_OBJS := $(BUILD)/host_tinyfs.o $(BUILD)/host_string.o \
             $(BUILD)/host_syscalls_shim.o $(BUILD)/host_mkfs_tinyfs.o
# host-path:fs-name pairs seeded into every disk image; the host paths
# alone are listed separately so they can be used as make prerequisites
MKFS_SEED_FILES := tests/hello.tsh tests/hello.tc
MKFS_SEED_SPECS := tests/hello.tsh:hello.tsh tests/hello.tc:hello.tc

.PHONY: all run run-gui clean

all: $(DISK_IMG)

BUILD_SUBDIRS := $(BUILD) $(BUILD)/fs $(BUILD)/shell $(BUILD)/lang $(BUILD)/lang/script $(BUILD)/lang/compiler

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
# -m32: matches CC's x86-32-only codegen, so tools/host_syscalls_shim.c's
# int 0x80 convention is correct and this build stays consistent with
# what Phase T7 will produce.

$(BUILD)/host_tinyfs.o: kernel/fs/tinyfs.c | $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -c -o $@ $<

$(BUILD)/host_string.o: kernel/string.c | $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -c -o $@ $<

$(BUILD)/host_syscalls_shim.o: tools/host_syscalls_shim.c | $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -c -o $@ $<

$(BUILD)/host_mkfs_tinyfs.o: tools/mkfs_tinyfs.c | $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -c -o $@ $<

$(MKFS_BIN): $(MKFS_OBJS)
	$(HOSTCC) -m32 -o $@ $(MKFS_OBJS)

$(DISK_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN) $(MKFS_BIN) $(MKFS_SEED_FILES)
	dd if=/dev/zero of=$@ bs=512 count=$(DISK_SECTORS) status=none
	dd if=$(BOOT_BIN) of=$@ conv=notrunc status=none
	dd if=$(STAGE2_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_START_SECTOR) conv=notrunc status=none
	$(MKFS_BIN) $@ $(MKFS_SEED_SPECS)

# Headless: no window at all, so there's no input surface for the emulated
# PS/2 keyboard - useful for a quick boot sanity check or scripting, but
# you cannot type into the shell this way. Use `make run-gui` to interact.
run: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -display none -serial stdio -no-reboot

# Opens a real window - click into it, then type. Serial output is also
# echoed to this terminal, but keystrokes only reach the guest via the
# window.
run-gui: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -serial stdio -no-reboot

clean:
	rm -rf $(BUILD)
