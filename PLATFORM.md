# Base platform: storage, POSIX runtime and process memory

Updated 2026-09-18. This completes roadmap items 1–3 far enough that
filesystems, networking, packaging, fonts and display work can build on the
kernel without further changes to its computation and memory model. The
kernel remains Aurora's own; nothing here imports Linux code.

## Storage

- **Batched VirtIO requests.** The block ring has a 16 KiB descriptor area
  and eight 64 KiB bounce slots at `0x0d000000`. One submission carries up to
  eight chains (512 KiB); the caller sleeps once and the completion interrupt
  wakes it when the whole batch has been used. ext2 (lwext4) and FAT32 (FatFs)
  multi-sector transfers go through `native_raw_disk_range`, so directory
  scans and large copies no longer issue one request per sector.
- **Interrupt-driven ATA.** IRQ14 is unmasked once the scheduler runs. Reads
  and flushes sleep until the drive interrupts; writes poll DRQ (no interrupt
  precedes the data phase) and then sleep for completion. Every command carries
  a sequence number so a late interrupt cannot complete a newer command, and
  each sleep has a tick deadline. The same code serves the AuroraFS boot volume
  (primary master) and the ATA development-disk fallback (primary slave).
  Before scheduling starts, PIO polls with bounded spins.
- **Legacy calls sleep too.** `SYS_FILE_READ`, `SYS_FILE_WRITE` and
  `SYS_SPAWN` take the filesystem mutex like the native calls, so a legacy
  application's disk wait no longer blocks other tasks.

## POSIX and C-runtime support

- **Signals.** Handlers nest: each delivery pushes a `ucontext`/`siginfo` frame
  on the user stack (or the `SA_ONSTACK` alternate stack) and `rt_sigreturn`
  restores registers, FPU state and the saved mask. Faults deliver `SIGSEGV`,
  `SIGBUS`, `SIGFPE` and `SIGILL` with `si_addr` and `si_code`; `siglongjmp`
  out of a fault handler works. `rt_sigsuspend`, `pause`, `rt_sigpending`,
  `rt_sigtimedwait`, `rt_sigqueueinfo`, `tkill`, `alarm`, `getitimer` and
  `setitimer` are implemented. `wait4`/`waitid` report
  `WCONTINUED`, and background writes honor `TOSTOP`. A wait whose condition
  is already satisfied completes even when a handled signal is pending, so
  `SIGCHLD` handlers do not turn `waitpid` into `EINTR`.
- **Build-critical syscalls.** `link`/`linkat`, `truncate`, `fallocate`,
  `flock`, `statfs`/`fstatfs`, `msync`, `mlock`/`munlock`/`mlockall`,
  `times`, `prctl` (`PR_SET_NAME`/`PR_GET_NAME` and common queries),
  `setrlimit`/`getrlimit`/`prlimit` for `RLIMIT_NOFILE`, `getpriority`/`setpriority`, the `sched_*` family and
  `membarrier`. musl's `sched_getscheduler`/`sched_getparam` wrappers return
  `ENOSYS` on their own; the raw syscalls succeed.

- **File-descriptor limit.** Native processes start with a soft and hard
  `RLIMIT_NOFILE` of 64, matching the fixed descriptor-table capacity. The
  supported `getrlimit`, `setrlimit` and `prlimit64` paths validate
  `soft <= hard`, permit only lowering the hard limit (Aurora has no privilege
  transition that can raise it), and enforce the soft limit for open, pipe,
  socket, eventfd, `dup*` and `F_DUPFD*` allocation. Existing descriptors stay
  usable after a reduction; newly requested descriptors at or above the limit
  fail without consuming a slot. Limits are copied by fork/exec and shared by
  Aurora's native threads. Other resource numbers retain their existing
  bounded/advisory behavior and are not represented as Linux host limits.

## Process memory

- **Demand-zero anonymous memory.** `MAP_PRIVATE|MAP_ANONYMOUS`, `brk` growth
  and the 2 MiB process stack (except the top 256 KiB holding arguments) are
  committed on first touch. A software page-table bit marks owed pages; the
  page-fault handler allocates and zeroes them. `PROT_NONE` lazy pages fault
  as protection errors. `mincore` reports mapped-but-unbacked pages as not
  resident. `mremap` shrinks, grows in place or relocates (`MREMAP_MAYMOVE`).
- **Admission.** A single request larger than free RAM fails with `ENOMEM`
  up front; outstanding lazy commitments are not summed (Linux's default
  heuristic), so forking a process with a large untouched heap succeeds. A
  fault-in that finds no RAM raises `SIGSEGV` and counts in
  `native_lazy_commit_failures`.
- **32 task slots.** The kernel's large per-task arrays (`tasks`, FPU save
  areas, native process and descriptor tables, the executable staging buffer)
  moved out of the 240 KiB kernel image into reserved RAM at `0x4000000`,
  which only the kernel root maps. Because kernel code always runs under the
  kernel root, the trap return path now copies the outgoing frame onto the
  task's kernel stack (mapped in every root) before switching CR3. Slots 16–31
  have their own guarded kernel stacks at virtual `0x3f000000`, backed by RAM
  at `0x4100000`, and are used only by native processes; legacy SDK
  applications keep the original 13 slots. Descriptions rose to 1,024 and
  pipes to 64. The kernel image is 217 KB; services build with `-Os`.

## Reproduce

```powershell
.\build.ps1
python test-platform.py --cpus 2            # VirtIO development disk
python test-platform.py --cpus 2 --ata      # ATA primary-slave fallback
python test-kernel-regressions.py --nm <path-to-llvm-nm.exe>
python test-native-gcc.py --virtio --cpus 2
python test-dynamic-posix.py --virtio --cpus 2
```

Linux hosts use the portable builder and PATH-resolved QEMU/LLVM tools:

```sh
python3 build-linux.py
python3 test-platform.py --cpus 2 --nm "$(command -v llvm-nm)"
```

Install `clang`, `lld`, `llvm`, `nasm`, `qemu-system-x86`, `python3`,
`python3-pil`, `musl-tools` and `e2fsprogs` first. The normal command needs
`build/development.img` prepared with the native GCC toolchain. For a raw ext2
fallback that still runs the guest regression, use:

```sh
truncate -s 512M build/development.img
mkfs.ext2 -F build/development.img
debugfs -w -R 'mkdir /work' build/development.img
musl-gcc -static -O2 -pthread tests/platform.c -o build/platform-musl
python3 test-platform.py --cpus 1 --platform-binary build/platform-musl \
  --nm "$(command -v llvm-nm)"
musl-gcc -static -O2 -pthread tests/package-locks.c -o build/package-locks-musl
python3 test-package-locks.py --folder build/package-lock-tests-linux \
  --package-binary build/package-locks-musl
```

The fallback validates both the platform and package-lock binaries inside
Aurora but does not replace the default in-guest GCC compilation check.

`test-platform.py` stages `tests/platform.c` into a copy of the development
disk, compiles it with the native GCC inside Aurora and runs it. The program
checks nested and fault signals, `sigaltstack`, signal waits and queues,
interval timers, job control, demand paging with `mremap`, the build-critical
syscalls and a 4 MiB storage round trip. The harness then reads kernel
counters through QMP: on VirtIO, `virtio_batches > 0` and
`virtio_max_batch >= 2` with `virtio_timeouts == 0`; on ATA,
`ata_interrupts > 0`, `ata_suspensions > 0` and `ata_timeouts == 0`. A recent
run recorded 9,863 demand faults, 74 batches reaching eight chains, and
111,483 ATA interrupts with none stale or timed out. The kernel regression,
native GCC and dynamic POSIX suites pass unchanged except for
`tests/native-foundations.c`, whose residency check now reflects first-touch
commitment. The 128 MiB smoke configuration boots because the reserved kernel
state and stacks lie below 128 MiB.

## Limits

Storage still runs inside the kernel. QEMU and supported Intel systems with an
ACPI DMAR include-all VT-d unit or a validated ACPI IVRS/IVHD AMD-Vi unit now
get hardware translation for the existing
VirtIO block, network and entropy windows. Run `python test-iommu.py
--build-dir build` with QEMU's `-machine q35` and
`-device intel-iommu,intremap=on`; the harness injects a checksum-valid
include-all DMAR/DRHD table at the emulated unit's `0xfed90000` because the
stock SeaBIOS used by QEMU 8.2 does not publish one. The paired AMD harness
uses `amd-iommu,pt=off`, injects IVRS at QEMU's `0xfed80000` AMD-Vi BAR, and
checks explicit VirtIO source IDs. The fail-closed tests verify that a
guest without DMAR or IVRS refuses DMA-backed startup. The VirtIO path now crosses the
versioned `storage_service.h` broker seam, which validates a
single bounded request and owns its completion/cancel lifecycle; it is not yet
a ring-3 service and does not transfer device ownership out of the kernel. Only one
batch is in flight per caller. There are no PTYs, so full job-control terminal
semantics are pending. `ITIMER_VIRTUAL` and `ITIMER_PROF` count wall-clock
ticks rather than consumed CPU time. Task and page-table storage is fixed at
32 slots. There is no
disk-backed paging; overcommitted memory that is finally touched without free
RAM ends the faulting process rather than reclaiming pages. AMD-Vi uses the
bounded device-table/page-table path and command-buffer invalidation; interrupt
remapping, arbitrary PCI functions, and broader physical-device coverage remain
unsupported and fail closed.
