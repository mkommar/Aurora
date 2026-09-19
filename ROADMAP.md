# Aurora: 20 remaining action items

Updated 2026-09-18. This replaces the earlier list. Status for items 1–4 is recorded below; the remaining entries are planned work. Retain Aurora's original kernel, ext2 for
development, FAT32 for exchange, and the preference for reusable GNU code.
The 2026-09-18 platform work is described in [PLATFORM.md](PLATFORM.md) and
verified by `test-platform.py`.

Already demonstrated: native GCC builds applications; GNU Make rebuilds and
installs itself from a preconfigured source tree; Bash, GNU text pipelines and
compressed archives work; GPT/ext2/FAT32 pass independent checks. The priority
now is downloading, configuring, compiling and installing source inside Aurora.

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
   `membarrier`. Remaining: PTYs and full job-control terminal semantics, broader
   pthread APIs (cancellation, barriers) and upstream configure coverage. See
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
   Remaining: an ext2 journal or ordered metadata writes (uncommitted data still
   depends on `sync`), FAT32 dirty-bit handling, and a repairing mode for the
   checker. See [FILESYSTEMS.md](FILESYSTEMS.md).
5. **Networking.** Start with VirtIO networking, sockets, Ethernet, ARP, IPv4,
   ICMP, UDP/TCP, DHCP and DNS. Evaluate reusable protocol-stack code. Demonstrate
   connections originating in Aurora without a host download bridge.
6. **Verified HTTPS downloads.** Port a maintained TLS library and curl or GNU
   Wget. Supply entropy, time, CA certificates and checksum/signature verification.
   Download a pinned source archive directly into `/src` inside Aurora.
7. **Complete GNU build prerequisites.** Add diffutils, patch, m4, Autoconf,
   Automake, Libtool, Bison and Flex, plus Perl/Python and compression tools where
   required. Run configure inside Aurora instead of importing its output.
8. **Rebuild the compiler and tool suite inside Aurora.** Progress from Make to
   Bash and other GNU packages, then binutils, the C runtime and GCC with its
   prerequisite libraries. Compare successive compiler builds and run a test
   corpus before replacing the bootstrap compiler.
9. **Reproducible package tooling.** Record source hashes, dependencies, patches,
   licenses and recipes. Add staged installation, file ownership records,
   removal and rollback. Demonstrate download-to-install entirely in Aurora.
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
18. **AMD Radeon support.** Select a GPU family and reuse suitable upstream
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
