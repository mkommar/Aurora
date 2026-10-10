# Aurora: implementation status and next steps

Updated 2026-10-09. The original 20 items retain their numbering. Items 1-6
have working implementations with the limits below; items 7-20 are partial or
planned work. Retain Aurora's original kernel, ext2 for
development, FAT32 for exchange, and the preference for reusable GNU code.
For package/tool sequencing, use the [Linux From Scratch systemd book](https://www.linuxfromscratch.org/lfs/view/systemd/)
as a tooling and dependency reference only. Aurora keeps its musl ABI, custom
kernel, freestanding boot path and Debian-format package target; LFS commands
and package order are not claims of Linux-kernel compatibility.
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
   mutex. Remaining: hardware IOMMU/DMA isolation and moving storage into a service.
   The existing `SERVICE_STORAGE` ABI is only a reserved contract; it is not
   an active storage service and must not be treated as one. The current
   VirtIO block, network, and entropy paths now also use capability-scoped
   software DMA domains with explicit ownership and mapped-range checks. This
   is not hardware DMA protection; hardware programming, fault handling, and
   a negative hardware DMA test remain open.
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
5. **Networking.** Implemented for IPv4 clients and bounded TCP listeners in
    QEMU TCG: modern VirtIO 1.x
   VirtIO-net, pinned lwIP 2.2.1, Ethernet/ARP/ICMP, TCP/UDP, DHCP, musl DNS,
   nonblocking sockets, poll/select, descriptor sharing and eventfd. Network
   processing remains in the kernel under the native compatibility lock.
   `listen`/`accept` are covered by the repository-transfer regression.
   Remaining: Unix-domain sockets, IPv6, DHCP-derived resolver updates,
   DNS-over-TCP, broader options and physical NIC drivers. See
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
   See [PACKAGES.md](PACKAGES.md) for evidence and remaining limits. The
   dependency-ordered LFS reference track is: binutils, GCC, Aurora-compatible
   API headers (not a Linux kernel import), m4, Perl, Autoconf, Automake,
   Libtool, Bison, Flex, Texinfo, and compression/file utilities. Each step
   needs a clean configure/build/test, staged install, package manifest and an
   Aurora regression before the next prerequisite is promoted.
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
     Host lifecycle fixtures now also reject same-version reinstalls without
     mutation. Remaining: validate the pinned Make and diffutils
     download/configure/build/test/package/install/smoke flows inside Aurora,
     upstream dpkg and
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
## Microkernel and tooling sequence

These are the next bounded milestones, in dependency order. A milestone is not
complete until its acceptance test is recorded; a design document or reserved
ABI alone does not count.

1. **Loadable user-service foundation (implemented bounded milestone).** Package
    an already-linked ring-3 service in `AURMOD1`, validate bounds and SHA-256,
    authorize against an external deny-by-default grant set, stage and commit a
    service registration, enforce the grant at each privileged operation, and
    exercise duplicate, malformed, rollback and unload failure paths. The
    self-test build emits `build/selftest/modules/probe.mod`; host acceptance is
    covered by `tests/module-format-host.py` and
    `tests/module-activation-host.py`. This is not arbitrary code execution:
    fresh guest page-table mappings, ring-3 entry, service restart/timeout
    handling, and an IPC endpoint grant remain future runtime work. Capability
    declarations in an image are not authoritative, and no IOMMU/DMA isolation
    is implied.
2. **DMA/IOMMU isolation.** The hardware stage is implemented for Intel VT-d
   and AMD-Vi: Aurora validates ACPI DMAR/DRHD or IVRS/IVHD tables, enables one
   remapping unit,
   builds four-level second-level identity mappings for the three existing
   capability-scoped VirtIO domains, assigns only discovered VirtIO source IDs,
   and flushes context/IOTLB state before use and after unmap, revoke, or
   teardown. A VT-d or AMD-Vi fault disables the DMA backend rather than falling back to
   unrestricted physical DMA. `tests/dma-host.c`, `tests/vtd-host.c`, and
   `tests/amdv-host.c` cover software ownership, ACPI parsing, device IDs, table
   construction and fault decoding; `test-iommu.py`/`test-amd-iommu.py` boot
   the VirtIO block path with QEMU's Intel or AMD IOMMU model, while the paired
   fail-closed tests verify startup refusal without DMAR or IVRS. The
   `test-iommu-dma-fault.py` is a bounded negative harness: only the self-test
   image accepts `--dma-fault-test`, and it publishes a real modern VirtIO 1.x
   PCI block descriptor whose data address is outside the storage second-level
   domain, then waits for the hardware fault latch with a bounded timeout. The
   modern transport is intentionally scoped to the block path used by this
   test; network and entropy remain transitional. The current QEMU 8.2
   validation reaches modern queue setup and descriptor publication but has not
   yet produced the required fault marker, so this negative remains unvalidated.
   The positive boot tests and no-DMAR/no-IVRS or malformed-table
   fail-closed tests remain separate. Interrupt remapping, arbitrary PCI
   functions, multi-device attacks, and physical hardware still require
   independent work and selected hardware must stay fail-closed.
3. **Storage service extraction.** The bounded guest implementation is added:
   the built-in storage service is compiled as a candidate ring-3 task, reaches a
   capability-gated `SYS_STORAGE` endpoint, and uses the existing VirtIO broker
   for bounded flush/read/write dispatch. A storage fault or exit is contained,
   its software DMA mappings are revoked, and a fresh task is admitted only
   after a newer-generation handoff; stale completions are rejected.
   `test-storage-service.py` covers malformed requests, overflow/bounds,
   capability/device denial, DMA rejection, startup/dispatch, cancellation,
   stale generation rejection, handoff, and teardown. `test-microkernel.py`
   is prepared to cover the deliberate guest storage crash, restart, private
   CR3, and continued operation of the other services, but current QEMU runs
   stop at the first invalid user return frame in `iretq` before those checks.
   The VirtIO device, descriptors,
   ATA fallback, and filesystem remain in the kernel. This is not hardware
   IOMMU enforcement, arbitrary AURMOD1 module entry, or a complete
   microkernel extraction. Keep boot recovery and ATA in-kernel until real
   read/write/flush, restart, and power-loss regressions pass on disposable
   images.
4. **Networking and IPRoute2 subset.** Before packaging IPRoute2, implement and
   test `NETLINK_ROUTE`, `RTM_GETLINK`, `RTM_GETADDR`, and `RTM_GETROUTE`, with
   aligned attribute validation, dump sequencing and stable errors. Do not
   claim `ip` compatibility until `ip link`, `ip addr`, and `ip route` fixture
   tests pass; IPv6, mutation commands and qdisc support remain separate.
5. **Debian-format package closure.** Promote the LFS-ordered recipes only
   after each package has an Aurora-native configure/test result. Require
   deterministic `.deb` output, dependency metadata, ownership-aware
   install/upgrade/removal tests, and authenticated Release metadata.

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
2. Validate the new AURMOD1 host gate, then add the first guarded page-table and
   capability hooks needed for a runtime user-service loader. Do not activate
   storage or network drivers through it yet.
3. Extend the completed clean-configure GNU Make build to the LFS-ordered GNU
   ports; retain `config.log`, syscall failures and test output. Add
   prerequisites needed by the next package, then stage its installation and
   record ownership.
4. Define and test the netlink/rtnetlink subset before attempting IPRoute2.
5. Improve terminal scrollback/log capture, then PTYs and job control. Measure
   `make -j1/-j2/-j4`, memory, task limits and lock contention before a GCC rebuild.
6. Expand the boot bundle budget before adding substantial kernel subsystems:
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
