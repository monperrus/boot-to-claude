# tinyfs

A minimal flat-namespace filesystem for tiny-os. Block size is 512 bytes,
matching the disk sector size, so no separate sector-buffering layer is
needed.

Constants live in `kernel/fs/tinyfs.h`, shared verbatim by the kernel
(freestanding build) and `tools/mkfs_tinyfs` (native host build, run at
image-build time to format and seed the disk).

## Disk layout

The fs region starts at LBA `TINYFS_FS_START_LBA` (128, i.e. 64KB into the
disk image), leaving room before it for the boot sector, stage2, and the
kernel image. All block numbers below are relative to that offset ("fs
block 0" = disk LBA 128).

| fs block(s) | contents |
|---|---|
| 0 | superblock |
| 1 | free-block bitmap (1 block = 4096 bits = up to 4096 data blocks / 2MB) |
| 2-9 | inode table (64 inodes x 64 bytes = 8 blocks) |
| 10.. | data blocks |

### Superblock (`tinyfs_superblock_t`)

`magic` (must equal `TINYFS_MAGIC`), `total_blocks`, `num_inodes`,
`data_blocks`.

### Inode (`tinyfs_inode_t`, 64 bytes, 8 per block)

`used` flag, `name` (26 chars + NUL, flat namespace - no directories),
`size` in bytes, and 8 direct data block numbers (`0` = unallocated). No
indirect blocks, so max file size is `8 * 512 = 4096` bytes
(`TINYFS_MAX_FILE_SIZE`) - deliberately small, in keeping with "tiny".

### Free-block bitmap

One bit per data block, index 0 = first data block (fs block 10).

## API (`kernel/fs/tinyfs.c`)

`tinyfs_init(read_fn, write_fn)` wires in the block I/O backend (ATA PIO in
the kernel, `fopen`/`fseek`/`fwrite` on the host). `tinyfs_format` is only
ever called by `mkfs_tinyfs`. The kernel calls `tinyfs_mount`, then
`tinyfs_create` / `tinyfs_read` / `tinyfs_write` / `tinyfs_delete` /
`tinyfs_list`.

There is exactly one implementation of the format - `tinyfs.c` is compiled
twice (once freestanding for the kernel, once natively for the host tool)
rather than duplicated.
