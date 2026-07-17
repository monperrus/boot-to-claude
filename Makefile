AS      := nasm
QEMU    := qemu-system-i386
BUILD   := build

BOOT_SRC := boot/boot.S
BOOT_BIN := $(BUILD)/boot.bin
DISK_IMG := $(BUILD)/disk.img

.PHONY: all run clean

all: $(DISK_IMG)

$(BUILD):
	mkdir -p $(BUILD)

$(BOOT_BIN): $(BOOT_SRC) | $(BUILD)
	$(AS) -f bin -o $@ $<

$(DISK_IMG): $(BOOT_BIN)
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BOOT_BIN) of=$@ conv=notrunc status=none

run: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -display none -serial stdio -no-reboot

clean:
	rm -rf $(BUILD)
