#include "tinyfs_disk.h"

#include "../ata.h"
#include "tinyfs.h"

static void disk_block_read(uint32_t block, void *buf) {
    ata_read_sector(TINYFS_FS_START_LBA + block, buf);
}

static void disk_block_write(uint32_t block, const void *buf) {
    ata_write_sector(TINYFS_FS_START_LBA + block, buf);
}

int tinyfs_disk_init(void) {
    tinyfs_init(disk_block_read, disk_block_write);
    return tinyfs_mount();
}
