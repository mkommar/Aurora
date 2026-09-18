# Multicore, POSIX and dynamic linking

Aurora retains its original kernel, service ABI, native GCC toolchain and
ext2/FAT32 development disk. `run.ps1` defaults to four CPUs; `-Cpus 1` retains
the single-CPU configuration. The supported range is one through eight CPUs.

## CPU execution and synchronization

ACPI RSDT/MADT discovery selects enabled xAPIC processors. The BSP starts APs
with INIT/SIPI, waits for acknowledgement, and leaves the existing PIC/PIT and
storage interrupt routing on the BSP. Timer IPIs provide AP scheduling points.
Each CPU has its own GDT, TSS, guarded idle stack, current-task pointer and
syscall scratch space. Task kernel stacks remain guarded and ownership prevents
two CPUs from selecting the same task. FS/GS and floating-point state follow
the task. SWAPGS protects the kernel's CPU pointer from user GS changes.

Native applications and pthreads execute concurrently in ring 3. The existing
desktop/input/display services stay on the BSP. Linux ABI `sched_setaffinity`,
`sched_getaffinity` and `getcpu` expose the supported CPU set; pthread creation
and fork inherit affinity.

There is no single lock around every kernel operation. Endpoint IPC has a lock
per endpoint. Read-only identity syscalls use a CPU read-side marker. Native
process/scheduler state has its own lock, and filesystem access has a task-owned
sleeping mutex that survives an I/O continuation. Wait queues, clocks, affinity
and thread bookkeeping do not acquire that filesystem mutex. This allows
service kernel operations to overlap native kernel operations and allows wait
operations to progress while storage sleeps.

Mapping changes issue IPIs only to CPUs executing the affected address space.
Acknowledgement follows the switch to the kernel CR3; restoring a user CR3
flushes translations before execution resumes. COW, protection changes and
page reclamation use this barrier. Unrelated address spaces keep executing.
Dead tasks retain their slot and resources until their owning CPU relinquishes
them. Robust-futex owner death uses compare/exchange, and clear-TID publication
uses a release store.

The lock order is native state followed by an endpoint lock. IPC releases its
endpoint lock before scheduling. CPUs acknowledge VM barriers before waiting
for the native state lock, avoiding an IPI/lock cycle. A task becomes claimable
only after the previous CPU has left its kernel stack.

**Native mutating syscalls still share a coarse state lock.** Kernel concurrency
is split by endpoint, read-side identity calls, native state and the sleeping
filesystem mutex; it is not yet fully fine-grained within the native domain.
Per-address-space mutation locks, separate run queues and independent futex/FD
locks remain work. This implementation does not complete that broader goal.

The initialization and translation-invalidation protocol follows the
[Intel system programming manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
CPU hotplug, x2APIC, NUMA, tickless operation and general NMI recovery are outside
this implementation.

## ELF and libc

The kernel validates and maps the executable and its `PT_INTERP` image before
replacing the old address space. Main images retain the existing lower arena;
PIE uses a 4 MiB bias and the interpreter uses 128 MiB. Auxiliary vectors now
carry the executable's PHDR/entry and the interpreter's `AT_BASE`. Relocations,
library lookup, constructors, DSO TLS, `dlopen` and `dlsym` are musl's code.
The existing static executable path remains available. W+X load segments,
overlapping pages, malformed headers and nested interpreters are rejected.

`build-shared-musl.sh` configures and builds pinned musl 1.2.2 inside Aurora,
including the approved `_Fork` backport, and installs the shared libc and loader.
The bootstrap shared libc and libgcc come from the existing pinned native GCC
archive. The test harness stages them only onto disposable candidate disks.
The original static libc archive keeps its previous backport. The filesystem
metadata cache now holds 16,384 entries in reserved supervisor RAM so source
trees and their object files can remain cached together. GNU Make uses its
pipe jobserver; named FIFO filesystem objects are still unsupported.

Additional ABI support includes alternate signal stacks (`SA_ONSTACK`), group
queries, positioned writes, clock resolution/clock-ID validation, `pselect6`,
4 KiB atomic pipes with capacity queries, up to 4,096 exec arguments in a
bounded 1 MiB string pool (plus the existing bounded environment), null-device
seeking, GS-base controls and CPU affinity. Signal handling still permits one
active handler; file-backed shared mapping coherence, PI futexes and full POSIX
conformance remain unsupported. Interpreter/library addresses are fixed, not
ASLR-randomized.

## Reproduce

```powershell
.\build.ps1
python test-dynamic-posix.py --cpus 4 --rebuild-musl
python test-native-gcc.py --cpus 4
python test-development.py --cpus 4 --nm <path-to-llvm-nm.exe>
```

The dynamic suite compiles its DSO, PIE application and SMP tests using GCC
inside Aurora. It checks constructor/TLS behavior, `dlopen`, malformed ELF
rejection, alternate stacks, affinity, cross-CPU shared atomics, GS isolation,
fork/COW and mapping protection changes. QMP counters record CPU activity and
kernel-domain overlap. `--resume` reuses only the suite's isolated candidate.

Source and boot artifacts from before this change are retained in
`build/before-smp`. The deployed development disk is replaced only after the
candidate passes regression and read-only filesystem checks.

## Validated release

The shared musl build compiled 1,340 object files inside Aurora. Its pinned
source and approved `_Fork` replacement were checksum-verified in the guest.
The installed `libc.so` SHA-256 is
`7b47d6fe2fcf3b3ab84f78c177e6f46283b3b172c07f8242d98f0a5075cc67fe`.

| Suite | Passed checks |
| --- | ---: |
| Native GCC and pthread foundations, 4 CPUs | 26 |
| SDK applications/storage | 24 |
| Isolation and GUI, 128 MiB | 40 |
| GNU/filesystems with MSI-X, 4 CPUs | 25 |
| GNU/filesystems with INTx, 4 CPUs | 25 |
| Shared-runtime build and dynamic/SMP suite, 4 CPUs | 16 |
| Dynamic/POSIX suite, 1 CPU | 10 |
| Dynamic/POSIX/SMP suite, 8 CPUs | 15 |
| Latest-disk runtime installation, dynamic/SMP suite, 4 CPUs | 15 |

These are 196 harness checks across configurations, with additional assertions
inside the guest programs. Tests cover long exec argument lists, relative
symlinks, atomic `pselect` signal masks, alternate signal stacks, malformed ELF,
shared-library constructors/TLS, `dlopen`, and the dynamic fork/thread-exit fix.
The SMP test deliberately attempts a write on a remote CPU after `mprotect`
removes write permission; the expected process fault confirms stale writable
translations are revoked. Four- and eight-CPU runs each recorded 65 acknowledged
remote VM barriers; all eight CPUs executed native applications. Both storage
interrupt routes completed with zero timeouts.

The final disk was staged from the latest development image, preserving its
contents, and passed independent read-only ext2 and FAT32 checks. It is installed
as `build/development.img`; the previous disk and manifest are retained as
`build/development-before-smp.img` and `build/development-before-smp.manifest.json`.
The full guest-built source/object tree remains in
`build/dynamic-tests/toolchain.img`. The deployed disk includes the shared
runtime and `/work/build-shared-musl.sh` for rebuilding from its pinned archive.

`build/smp-runtime-validation.json` records hashes, counts, CPU/IRQ counters,
build logs, deployment provenance and the remaining native-lock limitation.
`stage-shared-runtime.py` stages a validated runtime onto a fresh disk copy;
it never replaces the current development disk itself. `image_access.py` uses
the existing pinned filesystem library for extraction, rejects writes during
read-only access, and verifies GPT bounds/checksums.
