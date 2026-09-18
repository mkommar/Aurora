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
suite saves its pre-reboot transcript in `build/native-tests/foundations-serial.log`.
The development suites save IRQ/wait counters and JSON results in
`build/development-tests` and `build/development-intx-tests`.

The foundation fixture checks mutex/condition synchronization between four
threads, isolated TLS, shared descriptor closure, join, robust owner death,
32 thread-slot reuse cycles, shared brk/cwd/umask, process-shared pthread
synchronization, leader-exit lifetime, poll/select and signals, shared anonymous
futexes, and eight children sharing a 192 MiB COW mapping. It checks both user
writes and kernel reads into COW memory.

## Limits

This remains a bounded single-CPU compatibility implementation, not full POSIX
conformance. Thirteen application/thread slots share the existing task pool.
Physical user memory comes from usable E820 pages between 256 MiB and 1 GiB.
There is no swap, SMP, priority-inheritance futex support, dynamic linker or
file-backed MAP_SHARED coherence. Signals still have one active handler and no
alternate stack. Broader cancellation, job-control and allocation-failure
coverage remain roadmap work. The device-timeout failure path needs deliberate
fault-injection coverage; zero-timeout successful runs do not validate recovery.
