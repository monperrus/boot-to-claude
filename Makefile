BUILD  := build

# BOOTSTRAP_CC: the *host's* real C compiler, needed exactly once to
# build our own AS/CC/LD from source on a fresh checkout (see
# docs/toolchain.md's Bootstrap section). Everything below that point -
# every kernel/host-tool source file - is built exclusively by our own
# tools; BOOTSTRAP_CC is never invoked on OS code.
BOOTSTRAP_CC := gcc

AS     := $(BUILD)/toolchain/as
CC     := $(BUILD)/toolchain/cc
LD     := $(BUILD)/toolchain/ld
HOSTCC := $(BUILD)/toolchain/cc --target=host
QEMU   := qemu-system-i386

# -Itoolchain/include: CC has no built-in header search path at all (see
# docs/toolchain.md) - <stdint.h>/<stddef.h> only resolve because of this.
CFLAGS  := -Ikernel -Itoolchain/include
HOSTCFLAGS := -Ikernel -Itools -Itoolchain/include

# Must match TINYFS_DISK_SECTORS / TINYFS_FS_START_LBA in kernel/fs/tinyfs.h
# and KERNEL_START_LBA / KERNEL_SECTORS in boot/stage2.S
DISK_SECTORS  := 4096
KERNEL_START_SECTOR := 5
KERNEL_MAX_SECTORS  := 512

BOOT_BIN   := $(BUILD)/boot.bin
STAGE2_BIN := $(BUILD)/stage2.bin
DISK_IMG   := $(BUILD)/disk.img

# kernel/builtins_shim.c is gone: CC intercepts every __builtin_* call
# site directly in codegen (see docs/toolchain.md) and never needs a
# linkable definition - the shim only existed for the old gcc build.
KERNEL_C_SRCS := kernel/kmain.c kernel/console.c kernel/serial.c kernel/gdt.c \
                 kernel/idt.c kernel/string.c kernel/memory.c kernel/selftest.c \
                 kernel/ata.c kernel/fs/tinyfs.c kernel/fs/tinyfs_disk.c \
                 kernel/keyboard.c kernel/input.c kernel/vga.c kernel/font.c \
                 kernel/shell/shell.c \
                 kernel/game/game.c \
                 kernel/lang/lexer.c kernel/lang/script/ast.c \
                 kernel/lang/script/parser.c kernel/lang/script/eval.c \
                 kernel/lang/compiler/parser.c kernel/lang/compiler/codegen.c \
                 kernel/lang/vm.c
KERNEL_ASM_SRCS := kernel/entry.S kernel/idt_stubs.S
KERNEL_OBJS   := $(patsubst kernel/%.S,$(BUILD)/%.o,$(KERNEL_ASM_SRCS)) \
                 $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_C_SRCS))
KERNEL_BIN    := $(BUILD)/kernel.bin

MKFS_BIN := $(BUILD)/mkfs_tinyfs
# host_string.o/host_tinyfs.o: kernel/string.c and kernel/fs/tinyfs.c
# compiled a *second* time, natively - host tools are libc-free (see
# docs/toolchain.md), same as the kernel itself, and this build has its
# own build/fs/tinyfs.o etc for the freestanding kernel target, so these
# need the host_ prefix to not collide with those.
# tools/host_syscalls_shim.c is gone the same way builtins_shim.c is:
# CC intercepts __sys_* calls directly.
MKFS_OBJS := $(BUILD)/host_tinyfs.o $(BUILD)/host_string.o $(BUILD)/host_mkfs_tinyfs.o
# host-path:fs-name pairs seeded into every disk image; the host paths
# alone are listed separately so they can be used as make prerequisites
MKFS_SEED_FILES := tests/hello.tsh tests/hello.tc
MKFS_SEED_SPECS := tests/hello.tsh:hello.tsh tests/hello.tc:hello.tc

.PHONY: all run run-gui clean

all: $(DISK_IMG)

BUILD_SUBDIRS := $(BUILD) $(BUILD)/toolchain $(BUILD)/fs $(BUILD)/shell \
                  $(BUILD)/game $(BUILD)/lang $(BUILD)/lang/script $(BUILD)/lang/compiler

$(BUILD_SUBDIRS):
	mkdir -p $@

# --- bootstrap: the full self-hosting chain (Phase T8) ---
# tools/bootstrap_selfhost.sh does all three stages itself: Stage1 via
# the host's real compiler (BOOTSTRAP_CC, used nowhere else - this is
# its only invocation in the whole build), Stage2 by compiling
# toolchain/{as,cc,ld}'s own source with Stage1, Stage3 by compiling it
# again with Stage2 - and requires Stage2 and Stage3 to be byte-
# identical before installing that verified result here. Every target
# below this point (kernel, mkfs_tinyfs, the disk image) is therefore
# built by a toolchain that has proven it can reproduce its own binary,
# not just "compiled once by something else that tolerated its source".

TOOLCHAIN_AS_SRCS := $(wildcard toolchain/as/*.c)
TOOLCHAIN_CC_SRCS := $(wildcard toolchain/cc/*.c)
TOOLCHAIN_LD_SRCS := $(wildcard toolchain/ld/*.c)
TOOLCHAIN_HOSTLIB_SRCS := $(wildcard toolchain/hostlib/*.c)

$(BUILD)/toolchain/as $(BUILD)/toolchain/cc $(BUILD)/toolchain/ld &: \
		$(TOOLCHAIN_AS_SRCS) $(TOOLCHAIN_CC_SRCS) $(TOOLCHAIN_LD_SRCS) $(TOOLCHAIN_HOSTLIB_SRCS) | $(BUILD)/toolchain
	BOOTSTRAP_CC=$(BOOTSTRAP_CC) tools/bootstrap_selfhost.sh $(BUILD)

# --- boot loader (hand-written assembly, flat binary) ---

$(BOOT_BIN): boot/boot.S | $(BUILD) $(AS)
	$(AS) -f bin -o $@ $<

$(STAGE2_BIN): boot/stage2.S | $(BUILD) $(AS)
	$(AS) -f bin -o $@ $<

# --- kernel ---
# Hand-written assembly (entry.S/idt_stubs.S) assembles directly.
# C sources go through CC first (.c -> .S, AS-syntax text) and then AS
# (.S -> .o, ELF32 relocatable) - CC has no built-in assembler, it always
# emits assembly text (see docs/toolchain.md), so this is always two
# steps where gcc's `-c` used to be one.

$(BUILD)/%.o: kernel/%.S | $(BUILD_SUBDIRS) $(AS)
	$(AS) -f elf32 -o $@ $<

$(BUILD)/%.S: kernel/%.c | $(BUILD_SUBDIRS) $(CC)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/%.o: $(BUILD)/%.S | $(BUILD_SUBDIRS) $(AS)
	$(AS) -f elf32 -o $@ $<

# LD's link_flat mode writes the kernel image straight to a flat binary
# (--format bin) - no intermediate ELF, no OBJCOPY step (see
# docs/toolchain.md: LD already has the loadable bytes laid out in
# memory before an ELF wrapper would even be needed).
$(KERNEL_BIN): $(KERNEL_OBJS) kernel/link.ld | $(LD)
	$(LD) -T kernel/link.ld --format bin -o $@ $(KERNEL_OBJS)
	@size=$$(stat -c%s $@); budget=$$(( $(KERNEL_MAX_SECTORS) * 512 )); \
	if [ "$$size" -gt "$$budget" ]; then \
		echo "error: $@ is $$size bytes, exceeds the $$budget-byte ($(KERNEL_MAX_SECTORS)-sector) boot budget" >&2; \
		echo "       bump KERNEL_SECTORS in boot/stage2.S, KERNEL_MAX_SECTORS here, and TINYFS_FS_START_LBA in kernel/fs/tinyfs.h together" >&2; \
		exit 1; \
	fi

# --- host-side mkfs tool (build-time infrastructure, not kernel code) ---
# Same two-step (.c -> .S -> .o) pipeline as the kernel, via HOSTCC
# (CC --target=host, which adds a _start that calls main - see
# docs/toolchain.md) instead of CC.

$(BUILD)/host_tinyfs.S: kernel/fs/tinyfs.c | $(BUILD) $(CC)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<
$(BUILD)/host_tinyfs.o: $(BUILD)/host_tinyfs.S | $(BUILD) $(AS)
	$(AS) -f elf32 -o $@ $<

$(BUILD)/host_string.S: kernel/string.c | $(BUILD) $(CC)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<
$(BUILD)/host_string.o: $(BUILD)/host_string.S | $(BUILD) $(AS)
	$(AS) -f elf32 -o $@ $<

$(BUILD)/host_mkfs_tinyfs.S: tools/mkfs_tinyfs.c | $(BUILD) $(CC)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<
$(BUILD)/host_mkfs_tinyfs.o: $(BUILD)/host_mkfs_tinyfs.S | $(BUILD) $(AS)
	$(AS) -f elf32 -o $@ $<

# LD's default (link_static_exec) mode: a minimal static ELF32 executable,
# entry point taken from the linked _start symbol (see docs/toolchain.md).
$(MKFS_BIN): $(MKFS_OBJS) | $(LD)
	$(LD) -o $@ $(MKFS_OBJS)

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
