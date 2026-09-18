# Shared-memory threads, memory and interrupt-driven I/O

Aurora retains its original kernel, service IPC and SDK ABI. Static applications
reuse the existing toolchain's musl pthread implementation. The kernel supplies
shared address spaces, scheduling, TLS and futex mechanisms; it does not contain
a second implementation of the pthread library.

## Thread and memory mechanisms

- Linux-ABI pthread clone flags share one page-table root and reference-counted
  VM lifetime. Threads share the heap, mappings, descriptor table, cwd, umask and
  signal dispositions. Registers, kernel/user stacks, FS-base TLS, floating-point
  state and signal masks remain per task.
- Futex WAIT/WAKE, bitset waits/wakes and REQUEUE/CMP_REQUEUE suspend tasks.
  Private keys identify a VM and address; process-shared keys identify a backing
  physical page and offset. Check/enqueue/wake is serialized on the single CPU.
  Clear-child-TID wakes joiners; bounded robust-list cleanup reports owner death.
- Native virtual ranges are 512 MiB. Fork shares private pages copy-on-write;
  user write faults and kernel output-buffer writes both resolve COW. Anonymous
  MAP_SHARED mappings retain their physical identity across fork. Thread clones
  share the VM directly. The process remains waitable until its last thread exits.
- `vfork`/the supported `posix_spawn` clone path shares VM and suspends the parent
  until child exec/exit. Exec with other live pthreads currently returns EBUSY.

## Non-breaking kernel changes

Each task has a guarded 64 KiB kernel stack. Explicit kernel continuations allow
a VirtIO disk caller or filesystem-lock waiter to sleep while user services run.
IRQ handlers acknowledge completion and wake the caller; cleanup that can touch
the filesystem runs from syscall context. Filesystem access is serialized.

Transitional VirtIO uses a bounded DMA bounce buffer and one outstanding request.
PCI routing supports MSI-X, MSI and legacy INTx; MSI-X and INTx are exercised in
QEMU. A two-second guest timer deadline resets a stalled device and returns an
I/O error. Early boot and legacy ATA still use bounded polling. This is not an
IOMMU implementation or a migration of storage into a user-space driver.

## Reproduce validation

Build normally with `build.ps1`, and build the isolation image with
`build.ps1 -SelfTest`. Supply the LLVM `llvm-nm.exe` path where indicated.

```powershell
python test-native-gcc.py
python test-native-gcc.py --foundations-only
python test-development.py
python test-development.py --tools-only --intx
python test-applications.py --nm PATH_TO_LLVM_NM
python test-kernel-regressions.py --nm PATH_TO_LLVM_NM
```

Run VM suites sequentially, especially with the polling ATA fallback. Tests use
disposable disk copies and compile `tests/native-foundations.c` with GCC running
inside Aurora. They leave the user's development disk unchanged. The native
suite defaults to the patched development image and saves its pre-reboot
transcript in `build/ext2-tests/foundations-serial.log`.
The development suites save IRQ/wait counters and JSON results in
`build/development-tests` and `build/development-intx-tests`.

The foundation fixture checks mutex/condition synchronization between four
threads, isolated TLS, shared descriptor closure, join, robust owner death,
32 thread-slot reuse cycles, shared brk/cwd/umask, process-shared pthread
synchronization, leader-exit lifetime, poll/select and signals, shared anonymous
futexes, and eight children sharing a 192 MiB COW mapping. It checks both user
writes and kernel reads into COW memory.

## Pinned musl backport

The original toolchain's `_Fork` queried its new TID without registering the
thread-exit futex. A child that creates a worker and then calls `pthread_exit`
could therefore leave that worker blocked during exit. This matches the
[upstream reproducer and proposed fix](https://www.openwall.com/lists/musl/2023/06/01/3).
The default development image backports that one-line fix from pinned musl
1.2.2 source: `SYS_gettid` becomes `SYS_set_tid_address` with
`&__thread_list_lock` as its argument.

`backport-musl.sh` compiles `_Fork.lo` with GCC **inside Aurora**, verifies layout
assertions against the existing x86-64 ABI, and replaces only that archive member
using GNU ar/ranlib. The original archive is retained at
`/lib/libc.a.before-thread-fix`; previously linked static binaries are unchanged.
The source archive includes musl's MIT license. Source and original-libc SHA-256
checksums are enforced by the staging/build scripts, and a source manifest is
written alongside the candidate image.

To reproduce on a new image copy:

```powershell
.\build-image-tool.ps1
python prepare-thread-runtime.py --output build/new-thread-runtime.img
python test-development.py --disk build/new-thread-runtime.img --backport-only
```

The compiled result is in `build/development-tests/development.img`, not the
input candidate. Preserve that output before running another development suite.
The regression includes the original post-fork failure, a fresh-exec case, final
worker exit status, and the desktop waiting for every thread. The original
legacy `build/toolchain.img` remains an unpatched archival fallback; its libc
still has the upstream fork/thread-exit bug.

Inside the patched Aurora image, the complete fixture can be rebuilt with:

```sh
cd /work
gcc -static -pthread foundations.c -o foundations
./foundations
```

## Recorded validation (2026-09-17)

| Suite | Passing checks |
| --- | ---: |
| Native GCC, pthreads, COW and checked shutdown | 24 |
| Original applications and storage | 24 |
| Microkernel isolation | 28 |
| GUI | 12 |
| GNU tools and MSI-X storage | 25 |
| INTx fallback | 11 |
| In-Aurora musl backport | 10 |

All 134 checks pass. Independent read-only `e2fsck -f -n` and `fsck.fat -n`
checks are clean. Both interrupt routes record sleeping disk callers and IRQ
completions with zero timeouts. The compiler harness now explicitly checks
`sync` before reboot and shutdown; previously its abrupt QEMU exit left stale
free-space summaries, reproducing the documented lack of power-loss recovery.

`build/thread-runtime-validation.json` records counts, IRQ counters and image
hashes. `build/development.manifest.json` records source and backport provenance.
The previous default disk and manifest are retained as
`build/development-before-thread-runtime.img` and its companion manifest.

## Limits

This remains a bounded compatibility implementation, not full POSIX
conformance. Multicore, dynamic-linker and alternate-stack additions are
described in [SMP-DYNAMIC.md](SMP-DYNAMIC.md). Thirteen application/thread slots share the existing task pool.
Physical user memory comes from usable E820 pages between 256 MiB and 1 GiB.
There is no swap, priority-inheritance futex support or file-backed MAP_SHARED
coherence. Signals still have one active handler. Broader cancellation, job-control and allocation-failure
coverage remain roadmap work. The device-timeout failure path needs deliberate
fault-injection coverage; zero-timeout successful runs do not validate recovery.
