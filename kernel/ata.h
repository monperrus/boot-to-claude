#pragma once

#include <stdint.h>

/* Polled PIO access to the primary ATA master (LBA28). This is the same
 * disk QEMU exposes via -drive file=...,format=raw - the one the boot
 * sector was read from via BIOS INT13h. */
void ata_read_sector(uint32_t lba, void *buf);
void ata_write_sector(uint32_t lba, const void *buf);
