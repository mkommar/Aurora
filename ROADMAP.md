# Aurora: implementation status and next steps

Updated 2026-10-09. The original 20 items retain their numbering. Items 1-6
have working implementations with the limits below; items 7-20 are partial or
planned work. Retain Aurora's original kernel, ext2 for
development, FAT32 for exchange, and the preference for reusable GNU code.
The 2026-09-18 platform work is described in [PLATFORM.md](PLATFORM.md) and
verified by `test-platform.py`.

Already demonstrated: native GCC builds applications; GNU Make rebuilds and
installs itself from a preconfigured source tree; Bash, GNU text pipelines and
compressed archives work; GPT/ext2/FAT32 pass independent checks. VirtIO-net,
IPv4 client sockets, DNS and verified HTTPS downloads work without a host
download bridge. curl and Mbed TLS can be rebuilt from prepared trees inside
Aurora. The next integration milestone is a pinned source download followed by
native configure, compile, test and staged installation, without imported
configure results. See [NETWORK.md](NETWORK.md) for the networking scope.

1. **Extend interrupt-driven storage.** Implemented: VirtIO IRQ completion,
   MSI-X/MSI/INTx routing, bounded DMA buffers, sleeping callers, timeout handling
   and guarded kernel continuations. 2026-09-18: multiple outstanding VirtIO
   requests (eight 64 KiB slots submitted as one batch; ext2 and FAT32
   multi-sector transfers use it), interrupt-driven ATA on IRQ14 with sequence
   tags and tick deadlines for both the AuroraFS boot volume and the ATA
   development path, and the legacy AuroraFS calls now sleep under the filesystem
   mutex. Remaining: IOMMU/DMA isolation and moving storage into a service.
2. **Complete build-critical POSIX and C-runtime support.** Implemented:
   poll/select, blocking waits, shared-VM musl pthreads, TLS, futex wait/wake/
   requeue, robust mutex cleanup and shared descriptor/filesystem state. A small
   musl fork/thread-exit fix is compiled and installed inside Aurora.
   ELF interpreters, PIE/DSOs, dynamic TLS, alternate signal stacks and CPU
   affinity are now supported. 2026-09-18: nested signal delivery through
   user-stack frames with `rt_sigreturn`, fault signals carrying `si_addr`/
   `si_code`, `sigsuspend`/`pause`/`sigpending`/`sigtimedwait`/`sigqueue`,
   interval timers and `alarm`, `WCONTINUED`/`waitid`, `TOSTOP`, and the
   build-critical calls `link`, `truncate`, `fallocate`, `flock`, `statfs`,
   `msync`/`mlock`, `times`, `prctl`, `setrlimit`, priorities, `sched_*` and
    `membarrier`. Cross-process `flock` exclusion is now covered by the
    package-lock regression, including fork/dup/close lifetime and blocking
    wakeups. Remaining: enforced
   resource limits other than the bounded `RLIMIT_NOFILE` implementation and
   fuller scheduling semantics (some calls currently
   acknowledge requests without implementing their effects), PTYs and full job-control terminal
   semantics, broader pthread APIs (cancellation, barriers) and upstream
   configure coverage. See
   [multicore/dynamic linking](SMP-DYNAMIC.md) and [earlier validation](THREADS.md).
3. **Scale process memory further.** Implemented: 512 MiB address spaces,
   reference-counted pages, copy-on-write fork, shared anonymous mappings and
   kernel-copy COW handling, multicore execution and acknowledged TLB
   rendezvous. 2026-09-18: demand-zero anonymous memory (mmap, brk and the
   process stack commit on first touch), `mremap` growth and relocation, 32 task
   slots with kernel state moved out of the kernel image into reserved RAM, and
   heuristic overcommit admission with fault-in failure counters. Remaining:
   finer native-domain locking, dynamic task/page-table allocation beyond 32
   slots, disk-backed paging and GCC rebuild measurements.
4. **Filesystem correctness and recovery.** Implemented 2026-09-18: one
   namespace for legacy and native processes (AuroraFS at `/aurorafs`; the legacy
   file and spawn calls fall back to `/work` on the development volume), `stat`
   from the raw ext2 inode with hard-link counts, owners and three timestamps,
   `chown`/`lchown`/`utimes`, RTC-stamped FAT32 entries, crash-orphan reclaim at
   mount on ext2 and FAT32, an ext2 clean/in-use superblock state that detects
   unclean stops, backup-GPT recovery with rewrite of the damaged copy, read-only
   `/dev/disk` and `/dev/boot` raw devices, and `fsck-aurora`, a GPT/ext2/FAT32/
   AuroraFS checker compiled and run inside Aurora. `test-filesystems.py` cuts
   power during a metadata-heavy workload, reboots, verifies `fsync`ed data and
   orphan reclaim, damages both GPT copies and runs the checker after each step.
   Hardened 2026-09-22: GPT geometry is bounded by device capacity, all used
   partitions are checked for overlap, conflicting valid copies are rejected,
   and repair flushes the table before publishing the header. Repair errors
   prevent mounting. ext2/FAT32 orphan parking uses protected, marked `AURORARC`
   directories; old root filename lookalikes are preserved. ext2 dirty markers
   must be durable before mutation, data is flushed before clean markers, and
   marker/flush failures block further writes in that session.
   Remaining: an ext2 journal or ordered metadata writes (uncommitted data still
   depends on `sync`), FAT32 dirty-bit handling, and a repairing mode for the
   checker. See [FILESYSTEMS.md](FILESYSTEMS.md).
5. **Networking.** Implemented for IPv4 clients in QEMU TCG: transitional
   VirtIO-net, pinned lwIP 2.2.1, Ethernet/ARP/ICMP, TCP/UDP, DHCP, musl DNS,
   nonblocking sockets, poll/select, descriptor sharing and eventfd. Network
   processing remains in the kernel under the native compatibility lock.
   Remaining: listen/accept, Unix-domain sockets, IPv6, DHCP-derived resolver
   updates, DNS-over-TCP, broader options and physical NIC drivers. See
   [NETWORK.md](NETWORK.md) and `test-network.py`.
6. **Verified HTTPS downloads.** Implemented: curl 8.22.0 with Mbed TLS 3.6.7,
   VirtIO-rng entropy, RTC time, a pinned CA bundle, certificate/hostname/expiry
   checks, pinned archive downloads and SHA-256 verification. Tests include
   certificate rejection and compiling source fetched over TLS. Prepared
   Mbed TLS/curl trees rebuild inside Aurora with `test-network.py --rebuild`;
   their configure results still originate in the bootstrap environment.
   Remaining: native configure for the remaining ports, integrated download-to-install recipes,
   signature-verification policy and routine CA/source-lock maintenance.
7. **Complete GNU build prerequisites.** In progress: 23 source archives have
   pinned hashes, dependency and license metadata, and native build recipes.
   The runner downloads sources in Aurora and invokes clean configure scripts;
   bounded `#!` interpreter execution and an allocator regression fix support
   this work. Finish and validate the native ports of diffutils, patch, m4,
   Autoconf, Automake, Libtool, Bison, Flex, Perl/Python and compression tools.
   See [PACKAGES.md](PACKAGES.md) for evidence and remaining limits.
8. **Rebuild the compiler and tool suite inside Aurora.** GNU Make 4.4.1 has
   configured from a clean archive, compiled and staged inside Aurora. Progress
   through Bash and other GNU packages, then binutils, the C runtime and GCC with its
   prerequisite libraries. A provisional recipe requires upstream bootstrap
   stage comparison and a runtime corpus; a completed native GCC bootstrap is
   still pending. Pinned C++ bootstrap components passed STL/exception tests;
   path-cache eviction passed a 20,000-path/open-descriptor regression. Validate
   those foundations against the full GCC tree. Keep the bootstrap compiler
   until the replacement passes those gates.
9. **Reproducible package tooling.** Source hashes, dependencies, patches,
     licenses and recipes are recorded. Native recipes produce deterministic
     `.deb` archives with `DESTDIR`. Aurora now has a filesystem installer that
     validates archive members, package identity/version/architecture, safe paths,
     file hashes and ownership state before an all-or-nothing install; it refuses
     unsafe overwrites and deterministic reinstalls, supports ownership/list
     queries and hash-guarded removal, and can download over the existing Aurora
     HTTPS curl interface when given an explicit SHA-256. Host fixtures cover checksum
     rejection, collisions, rollback, traversal, state queries and removal.
     Remaining: validate the pinned Make and diffutils download/configure/build/
     test/package/install/smoke flows inside Aurora, upstream dpkg and
     APT-compatible repository tooling, richer dependency/version metadata,
     maintainer scripts, full metadata semantics and authenticated release policy.
     The bounded installer now resolves comma-separated exact-name dependencies
     from local archives or generated repository metadata, detects cycles and
     missing packages before mutation, upgrades with rollback and rejects
     downgrades by default; alternatives, operators, conflicts, virtual packages,
     maintainer scripts and full Debian version semantics remain unsupported. A
     deterministic APT/source distribution
     tree, release manifest, synthetic-fixture test and configurable Pages
     workflow are implemented; they publish supplied `.deb` files but do not
     mirror source archives. The guest Make run is blocked by the uncached
     source archive and host network egress. Full dpkg/APT lifecycle behavior,
     guest build/install validation and authenticated release policy remain
     open. See [PACKAGES.md](PACKAGES.md).
10. **Development terminal and editor.** Add ANSI/VT behavior, scrollback, PTYs,
    Readline, complete job control, multiple terminals and an editor such as
    GNU nano. Make compiler output and source editing practical.
11. **Physical disk drivers and tools.** Add AHCI/SATA and NVMe, queued I/O,
    device discovery, removable media and formatting utilities. Reuse the block
    and VFS interfaces and expose disk identity before destructive operations.
12. **USB host stack.** Implement xHCI first: enumeration, descriptors,
    control/bulk/interrupt transfers, hubs, hotplug and disconnect handling.
    Test in QEMU and on selected hardware; add older controllers when required.
    Evaluate reusable upstream components behind Aurora-specific adapters.
13. **USB device drivers.** Prioritize HID keyboards/mice and mass storage,
    including FAT32 exchange drives and safe removal. Add USB audio and selected
    network adapters later; add UAS after basic bulk-only storage works.
14. **FreeType and text/image rendering.** Port FreeType, Fontconfig, HarfBuzz,
    Pango, Cairo and common image codecs in dependency order. Demonstrate scalable
    fonts, Unicode shaping and image rendering in an Aurora application.
15. **Display and window-system interfaces.** Define modes, buffers, cursors,
    input events, clipboard and compositor/window protocols. Improve software
    rendering and VirtIO-GPU support before relying on physical GPU acceleration.
16. **GTK applications.** Port GLib/GObject/GIO and the selected GTK version's
    dependencies. Choose an Aurora GDK backend or compatible display protocol.
    First target: a GTK window with text, controls, input and a file chooser,
    compiled inside Aurora with software rendering available.
17. **Sound drivers and audio service.** Start with QEMU-supported Intel HDA or
    VirtIO sound, then playback/recording, mixing, volume and an application API.
    Integrate USB audio after the USB stack is ready.
18. **AMD Radeon support.** Partial: PCI discovery, an RX 7800 XT/RDNA3
    capability handoff and a display-service command-ring interface exist.
    Presentation still uses CPU framebuffer copies; GPU command processors
    and hardware acceleration remain disabled. Select and validate a GPU
    family and reuse suitable upstream
    Radeon/AMDGPU source and definitions. Supply memory, firmware and
    synchronization interfaces. Bring up display modes and scanout first,
    then acceleration; validate specific hardware rather than all generations.
19. **NVIDIA support using Nouveau source.** Select a GPU generation, pin a
    Nouveau revision and inventory reusable discovery, VBIOS, display, memory
    management and command-submission code. Adapt required kernel/DRM services
    and firmware loading to Aurora. Prove stable display output first, then
    buffer management and GPU execution; check target-specific power management.
20. **Mesa graphics acceleration.** Port suitable Mesa components for OpenGL/EGL
    and later Vulkan once driver interfaces are ready. Evaluate NVK separately
    from the Nouveau kernel-driver port. Test synchronization, context isolation,
    GPU reset/recovery and real applications.

## Next milestones

1. Keep storage recovery regressions passing on disposable VirtIO and ATA
   images, including malformed GPTs, failed writes/flushes and interrupted
   recovery. `test-recovery.py` exercises the production recovery routines
   against an in-memory device; `test-filesystems.py` covers guest behavior.
2. Extend the completed clean-configure GNU Make build to the other GNU ports;
   retain `config.log`, syscall failures and test output. Add prerequisites
   needed by the next package, then stage its installation and record ownership.
3. Improve terminal scrollback/log capture, then PTYs and job control. Measure
   `make -j1/-j2/-j4`, memory, task limits and lock contention before a GCC rebuild.
4. Expand the boot bundle budget before adding substantial kernel subsystems:
   `build.ps1` currently enforces the loader's 240 KiB limit.

## Reuse and decisions

Nouveau capabilities vary by GPU generation and feature. Its upstream
[feature matrix](https://nouveau.freedesktop.org/FeatureMatrix.html) records
firmware requirements and differing power-management support. Linux support
there does not imply support in Aurora. Mesa documents
[NVK](https://docs.mesa3d.org/drivers/nvk.html) separately; display output alone
does not provide Vulkan acceleration.

USB host controllers, enumeration/hubs and device-class drivers are separate
parts of the stack, reflected in the upstream
[USB host API documentation](https://www.kernel.org/doc/html/latest/driver-api/usb/usb.html).
Items 12 and 13 deliberately separate those milestones. Linux drivers should
not be assumed to compile unchanged against Aurora.

Before implementing the affected items, confirm the physical machine and GPU
model, required USB-controller coverage, GTK/display-backend choice and scope
of any Linux-driver compatibility layer. Inspect licenses and retain attribution
for reused components. None of these choices replaces Aurora's kernel.
