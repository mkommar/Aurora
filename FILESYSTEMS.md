# Filesystem correctness and recovery

Updated 2026-09-18. This describes roadmap item 4: one namespace for legacy and
native processes, complete link and metadata semantics, crash-orphan reclaim,
backup-GPT recovery and a filesystem checker that runs inside Aurora.
`test-filesystems.py` verifies all of it, including power loss during writes.

## Volumes

| Volume | Format | Where | Access |
| --- | --- | --- | --- |
| Boot disk | AuroraFS (32 whole-file slots, 64 KiB each) | ATA primary master | Legacy `SYS_FILE_*` calls; native `/aurorafs`; raw `/dev/boot` |
| Development | ext2 (lwext4, no journal) | GPT partition on the VirtIO or ATA development disk | Native root `/`; legacy fallback to `/work`; raw `/dev/disk` |
| Exchange | FAT32 (FatFs) | Second GPT partition | Native `/exchange` |

## One namespace

Legacy SDK applications and native (musl/GCC) processes previously used
separate stores. They now see the same files:

- **AuroraFS at `/aurorafs`.** Native processes open, read, write, truncate,
  unlink, rename and list AuroraFS files. `stat` reports the slot number as
  `st_ino`, the slot size and a distinct `st_dev`. Directories, symlinks and
  hard links are not possible there (`EPERM`); cross-volume `rename` and
  `link` return `EXDEV` as on Linux.
- **Legacy calls fall back to `/work`.** `SYS_FILE_READ` looks in AuroraFS
  first and then in `/work` on the development volume, so the desktop's
  `cat`, `load` and `ls` show files created by native tools. `SYS_FILE_WRITE`
  updates the file where it already lives and creates new files on AuroraFS.
  `SYS_SPAWN` starts SDK applications from AuroraFS or from `/work`.
- **Sizes.** Legacy calls stay bounded by the 64 KiB AuroraFS slot; the
  development-volume fallback truncates larger files for reads.

`ls` in the desktop terminal lists AuroraFS entries followed by `/work`; the
console keeps 17 lines, so long listings scroll.

## Links and metadata

`stat`, `fstat`, `lstat` and `fstatat` read the raw ext2 inode. They return
the inode number, `st_nlink` (hard links and subdirectory `..` entries),
stored `st_uid`/`st_gid`, mode, size, allocated blocks and independent
`atime`/`mtime`/`ctime`. `chown`, `fchown`, `lchown` and `fchownat` store
owners; `chmod` and the `utime`/`utimes`/`futimesat`/`utimensat` family update
the corresponding timestamps and bump `ctime`. Every process still runs as one
synthetic user, so ownership is recorded but not enforced.

FAT32 entries are stamped from the RTC (`FF_FS_NORTC=0`), and `stat` converts
FAT date/time to Unix epochs. `st_ino` on FAT is a hash of the path and
`st_nlink` is always 1, since FAT has no link counts.

## Open files, unlink and crash orphans

Unlinking or renaming over a file that another descriptor still has open
parks it under a hidden name (`.aurora-orphan-<16 hex digits>` in the volume
root, `/` for ext2 and `/exchange` for FAT32) and removes it when the last
descriptor closes. Descriptors keep reading and writing the parked file.

If the machine stops before those closes happen, the parked names remain on
disk. `vfs_reclaim_orphans()` runs at every mount, scans both roots and
deletes any parked file or directory, logging
`VFS: reclaimed crash orphans count=N`. The `vfs_orphans_reclaimed` counter
records the total.

## ext2 clean and in-use state

Mounting marks the ext2 superblock *in use* (`s_state = 2`). `sync`, `fsync`
and the desktop's `reboot`/`poweroff` flush the cache and mark it *clean*
(`s_state = 1`); the first mutating syscall afterwards marks it in use again.
A boot that finds the in-use mark logs
`EXT2: previous session did not unmount cleanly` and increments
`ext2_unclean_mounts`. The superblock's free block and inode totals are
recomputed from the group descriptors at every mount (lwext4 only writes
them back on unmount, as Linux does), so the checker's totals and the
superblock agree after an unclean stop.

Uncommitted data is still lost on power loss: lwext4 caches writes and there
is no journal or ordering guarantee. Data that `fsync` returned for survives.

## Backup-GPT recovery

`native_partitions_init()` validates the protective MBR, then loads the
primary header at LBA 1 and the backup header (from the primary's alternate
LBA, or the last sector when the primary is unreadable; the disk size comes
from the VirtIO configuration or ATA IDENTIFY). Each copy is checked for
signature, header size, header CRC, self-LBA and partition-table CRC.

- Primary bad, backup good: partitions come from the backup, the primary
  header and table are rewritten, `gpt_backup_recoveries` and `gpt_repairs`
  increment (`GPT: primary header damaged; recovered from backup` and
  `GPT: primary header rewritten from backup`).
- Backup bad, primary good: the backup is rewritten from the primary
  (`GPT: backup header damaged; rewritten from primary`).
- Both bad: `GPT: both headers damaged`; the development and exchange
  volumes are not mounted and the boot disk still serves the desktop.

## Raw devices and the checker

Native processes can open `/dev/disk` (the development disk) and `/dev/boot`
(the AuroraFS boot disk) read-only. `read`, `pread`, `lseek` and `fstat` work;
opening for writing fails with `EACCES` and `write` returns `EBADF`. Transfers go through
the kernel's existing interrupt-driven storage paths.

`tools-source/fsck-aurora.c` is compiled inside Aurora with
`gcc -O2 fsck-aurora.c -o fsck-aurora` and reads the raw devices:

```
fsck-aurora [-v] [--no-boot] [--no-disk] [--disk DEV] [--boot DEV]
```

It checks the GPT (both copies, CRCs, overlap, protective MBR), ext2
(superblock, group descriptors, bitmaps against referenced blocks, inode
validity, link counts, directory structure, lost or cross-linked blocks,
unreferenced inodes, state and parked orphans), FAT32 (geometry, FAT copies,
cluster chains, cross-linked or lost clusters, FSInfo free count, orphans)
and AuroraFS (magic, directory entries, sizes, names). Exit status is 0 when
clean, 1 with warnings, 4 with errors and 2 for usage errors.

The checker reports, rather than repairs; repairing is future work.

## Verification

`python test-filesystems.py` (options `--disk`, `--accel`, `--cpus`,
`--qmp-port`) stages the sources onto a copy of `build/development.img` and
runs five phases on hidden QEMU instances:

1. **Fresh copy.** Builds `fsck-aurora`, `tests/filesystem.c` and
   `tests/interrupted-writes.c` in the guest; `fsck-aurora -v` reports clean;
   the regression covers hard links, symlinks, owners, modes, timestamps,
   directory link counts, unlink/rename with open descriptors, `/aurorafs`,
   FAT32 timestamps and parking, and raw devices. The desktop then saves notes
   through the legacy API, a Bash script reads and rewrites them through
   `/aurorafs`, the desktop reads the native-written file, falls back to
   `/work`, and spawns an SDK application from `/work`.
2. **Interrupted writes.** `interrupted-writes` `fsync`s a 1 MiB file, leaves
   an unlinked open file on ext2 and on FAT32, then streams renames and
   metadata-heavy writes while the harness cuts power through QMP `quit`.
3. **Recovery.** The reboot logs the unclean stop, reclaims both orphans, the
   committed file hashes correctly, no orphan names remain, `fsck-aurora`
   finds no structural damage and the regression passes again.
4. **GPT copies.** The host damages the primary header, then the backup
   table, then both, and restores the primary; each boot recovers, rewrites
   the damaged copy and the checker sees both copies valid.
5. **ATA attachment.** The same disk on the ATA primary slave, sized with
   IDENTIFY, passes `fsck-aurora --no-boot`.

47 checks pass. The kernel image with its embedded user services is 226,312
bytes, within the loader's 240 KiB limit.

## Remaining work

- An ext2 journal or ordered metadata writes so unsynced data survives power
  loss; FAT32 dirty-bit handling.
- A repairing mode for `fsck-aurora` (rebuild bitmaps, relink lost inodes,
  fix link counts), and reporting AuroraFS slots that overlap or exceed the
  volume.
- Permission enforcement across users once more than one user exists.
- Hosting the storage stack in a service with DMA isolation (roadmap item 1).
