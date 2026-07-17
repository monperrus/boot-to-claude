#include "ata.h"

#define ATA_IO_BASE   0x1F0
#define ATA_DATA      (ATA_IO_BASE + 0)
#define ATA_SECCOUNT  (ATA_IO_BASE + 2)
#define ATA_LBA_LO    (ATA_IO_BASE + 3)
#define ATA_LBA_MID   (ATA_IO_BASE + 4)
#define ATA_LBA_HI    (ATA_IO_BASE + 5)
#define ATA_DRIVE     (ATA_IO_BASE + 6)
#define ATA_STATUS    (ATA_IO_BASE + 7)
#define ATA_COMMAND   (ATA_IO_BASE + 7)

#define ATA_CMD_READ        0x20
#define ATA_CMD_WRITE       0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void ata_wait_bsy(void) {
    while (inb(ATA_STATUS) & ATA_SR_BSY) {}
}

static void ata_wait_drq(void) {
    while (!(inb(ATA_STATUS) & ATA_SR_DRQ)) {}
}

static void ata_select(uint32_t lba) {
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
}

void ata_read_sector(uint32_t lba, void *buf) {
    ata_wait_bsy();
    ata_select(lba);
    outb(ATA_COMMAND, ATA_CMD_READ);
    ata_wait_bsy();
    ata_wait_drq();

    uint16_t *p = buf;
    for (int i = 0; i < 256; i++) {
        p[i] = inw(ATA_DATA);
    }
}

void ata_write_sector(uint32_t lba, const void *buf) {
    ata_wait_bsy();
    ata_select(lba);
    outb(ATA_COMMAND, ATA_CMD_WRITE);
    ata_wait_bsy();
    ata_wait_drq();

    const uint16_t *p = buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, p[i]);
    }

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_bsy();
}
