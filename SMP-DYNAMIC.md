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

**The native process/VM domain is still coarse-grained.** Mutating native
operations rendezvous with CPUs executing native userspace before touching
shared mappings. Acknowledgement occurs after switching to the kernel CR3;
restoring a user CR3 flushes its translations before execution resumes. This
protects COW, unmapping and page reclamation, but stops more CPUs than a per-VM
shootdown would. Independent native mutating syscalls are not yet fully
concurrent. Per-address-space locks, separate run queues, fine-grained futex/FD
locks and targeted TLB generations remain work; this is not completion of that
broader concurrency goal.

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
The original static libc archive keeps its previous backport.

Additional ABI support includes alternate signal stacks (`SA_ONSTACK`), group
queries, positioned writes, clock resolution/clock-ID validation, null-device
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
