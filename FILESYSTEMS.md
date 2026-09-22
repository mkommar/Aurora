# Filesystem correctness and recovery

Updated 2026-09-22. This describes roadmap item 4: one namespace for legacy and
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

Unlinking or renaming over an open ext2/FAT32 file parks it as
`.aurora-orphan-<16 lowercase hex digits>` inside `/AURORARC` or
`/exchange/AURORARC`. Descriptors continue to use it until the last close.
These directories contain a versioned `OWNER` marker and are reserved by the
kernel. User path handling rejects the `AURORARC` component, including FAT
case/trailing-dot/space variants and symlink targets. The short directory name
has no separate FAT 8.3 alias. Raw devices remain read-only.

At mount, recovery uses only directories with the expected marker. A preexisting
collision, absent/invalid marker or I/O failure disables parking for that volume;
existing content is preserved. Initialization interrupted before the marker is
durable also fails closed on the next boot and requires offline inspection.
Reclamation matches the complete orphan name, only removes empty directories,
and leaves an entry cached if last-close deletion fails; explicit `close()`
returns the deletion error. Unknown entries are preserved. The `vfs_orphans_reclaimed` counter and serial log report deletions.

**Upgrade behavior:** old root-level `.aurora-orphan-*` names are no longer
reclaimed automatically. Their names cannot distinguish old parked files from
ordinary user data. Inspect these files offline before removing them. AuroraFS
still uses its existing flat-file parking behavior; this recovery-directory
scheme applies to ext2 and FAT32.

## ext2 clean and in-use state

Mounting writes the ext2 in-use marker (`s_state = 2`) and flushes the
selected development device before exposing the volume. Recount or flush
failure disables the ext2 mount. A boot that sees the in-use marker logs
`EXT2: previous session did not unmount cleanly` and increments
`ext2_unclean_mounts`.

`sync`/`fsync` flush cached filesystem writes and the device **before** writing
and flushing the clean marker (`s_state = 1`). The next mutating syscall must
write and flush the in-use marker before proceeding. Last-close orphan cleanup
also follows this rule. State-write or flush failures propagate as I/O errors
and latch the session against further mutations; reboot and inspect the volume
before resuming writes. A failed sync never reports success. ATA flushes select
the intended master/slave explicitly, and global sync flushes the boot disk too.

The superblock's free totals are recomputed from group descriptors at mount.
This repairs stale summary totals, not damaged bitmaps or directory structures.
There is still no journal or general metadata transaction ordering. Successful
fsync has a device durability boundary, but later interrupted metadata operations
can still damage an unjournaled filesystem. Unsynced writes are not guaranteed
to survive; the tests cover particular interruption points, not every crash.

## Backup-GPT recovery

`native_partitions_init()` validates GPT headers at LBA 1 and the actual last
device sector (from VirtIO capacity or ATA IDENTIFY). It checks signature,
revision, reserved fields, header/table CRCs, reciprocal header locations,
metadata bounds and the usable range. All used entries, including unknown
partition types, must fit that range without overlap. The current driver limit
is 28-bit sector addressing; unsupported GPT geometry is rejected.

- One invalid copy: use the other, validate the repair destination, write and
  flush the replacement table, then write and flush the replacement header.
  Only completed repairs increment `gpt_repairs`.
- Repair write/flush failure: increment `gpt_repair_failures`, report the error
  and leave the development disk unmounted. The surviving copy is untouched.
- Two valid but inconsistent copies: refuse mounting and preserve both for
  offline inspection rather than guessing which is authoritative.
- Both invalid: report `GPT: both headers damaged`; development/exchange remain
  unmounted while the boot disk still serves the desktop.

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
`--qmp-port`, `--cut-rounds`, `--stress-cut`) stages the sources onto a copy of `build/development.img` and
runs six phases on hidden QEMU instances:

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
   The default pauses without sync at completed operation boundaries in rounds
   0, 5 and 12, cuts power and checks recovery after each. `--stress-cut` leaves
   the workload running and can cut inside the next syscall; this is a separate
   diagnostic mode and is not guaranteed to pass without metadata transactions.
3. **Recovery.** The reboot logs the unclean stop, reclaims both orphans, the
   committed file hashes correctly, ordinary root filename lookalikes survive,
   `fsck-aurora` finds no structural damage and the regression passes again.
4. **GPT copies.** The host damages the primary header, then the backup
   table, then both, and restores the primary; each boot recovers, rewrites
   the damaged copy and the checker sees both copies valid.
5. **ATA attachment.** The same disk on the ATA primary slave, sized with
   IDENTIFY, passes `fsck-aurora --no-boot`.
6. **Recovery-directory collision.** Invalid marker content disables parking;
   unlink of an open file fails without losing its data. Host read-only checks
   confirm that the marker and orphan-looking directory contents are unchanged.

`python test-recovery.py` compiles the production GPT and ext2 recovery
routines into a host test library backed by an in-memory block device. It tests
CRC-valid hostile geometry, partition overlap, conflicting copies, every GPT
repair write/flush failure and restart boundary, and ext2 dirty/clean ordering
with injected failures. It writes `build/recovery-tests/results.json`.
Guest results are written to the selected test folder's `results.json`.
An unrestricted cut after round 0 during this work reproduced an inode marked
free while its on-disk mode/link fields remained populated; `fsck-aurora`
reported an error. The failed run is retained in `build/recovery-final-tests/`.
This is evidence of the outstanding mid-operation crash-consistency limitation,
not a claim that marker ordering provides filesystem transactions.
Validation on 2026-09-22: **305 host recovery checks**, **69 guest filesystem
checks** (three operation-boundary cuts, GPT repair, ATA and collision
preservation), and **28 microkernel + 12 GUI checks** passed. The final guest
run is recorded in `build/recovery-release-tests/results.json`. The normal
kernel/service bundle is 229,256 bytes. `build.ps1` continues to enforce the
loader's 240 KiB bundle limit.

## Remaining work

- An ext2 journal or ordered metadata writes for structural consistency across
  interrupted mutations; FAT32 dirty-bit handling. Unsynced data durability
  remains a separate guarantee.
- A repairing mode for `fsck-aurora` (rebuild bitmaps, relink lost inodes,
  fix link counts), and reporting AuroraFS slots that overlap or exceed the
  volume.
- Permission enforcement across users once more than one user exists.
- Hosting the storage stack in a service with DMA isolation (roadmap item 1).
