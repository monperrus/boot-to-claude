#pragma once

/* Mounts tinyfs on top of the kernel's ATA driver. Returns 0 on success. */
int tinyfs_disk_init(void);
