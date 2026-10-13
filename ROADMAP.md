# Aurora roadmap

Updated 2026-10-13. This document is an implementation roadmap, not a claim that every
LFS or Linux interface is present. The Linux From Scratch systemd book is used for
userspace dependency order and tool vocabulary only. Aurora keeps its custom
microkernel, musl ABI, freestanding boot path, capability model, IOMMU/DMA policy,
and Debian-format package target.

## Status at a glance

| Area | Status | Evidence boundary |
| --- | --- | --- |
| POSIX/runtime foundations | Implemented broadly | Native and recovery regressions pass; complete scheduling, resource-limit, PTY, and pthread semantics remain open. |
| Filesystems | Recovery and validation implemented | GPT/ext2/FAT32 recovery, fsck checks, dirty-state handling, and mounts work; journaling or ordered metadata writes remain open. |
| Networking | QEMU IPv4 path validated | Modern VirtIO-net, ARP/IPv4, UDP/TCP, DHCP, DNS, HTTPS, and socket readiness pass in QEMU. DHCP is still in the kernel compatibility path; the modular userspace stack is open. |
| DMA and IOMMU | Intel and AMD setup implemented | VT-d and AMD-Vi discovery, mappings, fail-closed startup, and positive QEMU paths pass. Deterministic AMD malicious-DMA fault quarantine remains open. |
| Storage service | Bounded ring-3 boundary implemented | Capability-gated storage execution, crash containment, restart handoff, and stale-completion rejection pass in the microkernel suite. Full production service extraction remains open. |
| Modules | Format and guarded activation implemented | AURMOD1 validation, authorization, rollback, and unload checks pass. Arbitrary runtime module loading is not implemented. |
| Packages | Host lifecycle and publication implemented | Installer, dependency planning, upgrades/removal, deterministic Pages output, and source mirror pass host checks. Guest package transaction is open. |
| Native toolchain | Bootstrap reproducible; guest closure open | GNU bootstrap inputs and Linux image preparation work. Guest xz, libgcc_s, and diffutils package evidence is still missing. |

## Current priorities

These are ordered by dependency and expected leverage. A priority is complete only
when its acceptance evidence is recorded in the repository; host fixtures do not
substitute for guest evidence.

### 1. Complete the observable guest package transaction

Finish the static xz and GCC runtime path, then run the complete diffutils
transaction inside Aurora using the observable/resumable runner.

Required evidence:

- Boot a prepared development image with multiple vCPUs and record the stage checkpoints.
- Verify the locked diffutils and xz source hashes inside Aurora.
- Provide executable awk, cmp, diff, xz, curl, and the other recipe prerequisites from verified bootstrap inputs.
- Build xz with its recipe-local static mode without requiring libgcc_s.
- Build and install the libgcc_s runtime for the existing x86_64-linux-musl ABI, then prove dynamic loader resolution with a guest smoke program.
- Configure, compile, test, stage, and package diffutils inside Aurora.
- Record the generated .deb path, size, and SHA-256.
- Exercise install, ownership/list state, same-version policy, upgrade or replacement, removal, and interrupted-transaction recovery on a disposable root.

Current known gap: the guest reaches source download and hash verification, but
static xz configure still needs verified awk/cmp/diff commands and no guest .deb
or libgcc_s load result has been recorded.

### 2. Turn the package runner into a usable base-system pipeline

Use the diffutils recipe as the reference and promote the LFS-ordered packages
only after each has guest evidence:

1. m4
2. patch
3. gawk, findutils, grep, sed, tar, and compression tools
4. Perl
5. Autoconf, Automake, and Libtool
6. Bison, Flex, and Texinfo
7. binutils, GCC runtime components, and the next compiler stage
8. IPRoute2 after the netlink subset below is implemented

Every promoted package needs a locked source, exact dependencies, clean native
configure/build/test logs, deterministic .deb output, ownership metadata, an
installed-command smoke test, and a recovery result. Keep the bootstrap compiler
until the replacement compiler passes the existing corpus and upstream gates.

### 3. Split networking into driver, service, and userspace client

The VirtIO-net driver should own PCI transport, queues, DMA/IOMMU mappings,
interrupts, and raw frames only. Move protocol and policy into a restartable
network service:

- Ethernet, ARP, IPv4, checksums, routing, UDP, TCP, and socket IPC.
- A ring-3 dhcpclient using UDP sockets, rather than DHCP logic in the driver.
- Startup scripts or a supervisor that start the network service, then dhcpclient.
- QEMU tests for raw TX/RX, malformed packets, DHCP lease configuration, and a
  post-lease UDP/TCP smoke test.

Keep the current kernel compatibility path until the userspace path passes, then
remove DHCP-specific driver behavior and its acceptance markers.

### 4. Implement the read-only NETLINK_ROUTE subset

Before packaging IPRoute2, add and test:

- NETLINK_ROUTE socket creation and binding.
- RTM_GETLINK, RTM_GETADDR, and RTM_GETROUTE.
- Multipart dump sequencing.
- Strict netlink attribute length, alignment, family, and truncation checks.
- Fixture-backed ip link, ip addr, and ip route output.

IPv6, route/address mutation, qdisc, policy routing, and physical NIC support are
separate milestones.

### 5. Promote the storage service from boundary to system service

The current ring-3 storage path proves the execution and restart boundary, but the
VirtIO device, descriptor construction, ATA fallback, and filesystem integration
still retain kernel responsibilities. Next:

- Start the storage service through the system supervisor, not a self-test-only path.
- Route real read, write, and flush operations through the service.
- Regrant capabilities and DMA mappings after restart.
- Test interrupted I/O, power loss, service failure, and stale completions.
- Preserve ATA fallback until the service path is reliable on disposable images.

### 6. Add a minimal system supervisor and shutdown contract

Define a boot sequence that mounts procfs and device/filesystem namespaces, starts
the storage and network services, runs dhcpclient, reports service state, starts
Bash after prerequisites are ready, and performs bounded shutdown plus sync.
Service restart, timeout, generation, and failure reporting should use the existing
capability and storage handoff mechanisms.

### 7. Make filesystem durability adequate for package operations

Implement ordered ext2 metadata writes or a small journal, FAT32 dirty-bit set and
clear handling, and a repairing mode for fsck-aurora. Add crash tests for package
install, upgrade, removal, rename, directory updates, and ownership-state updates.
Do not claim package durability from a clean shutdown-only test.

### 8. Complete runtime module loading and reduce boot pressure

AURMOD1 validation and guarded activation are foundations, not a general module
loader. Add runtime page-table construction, actual authorized ring-3 entry, IPC
endpoint grants, timeout/restart policy, and full resource cleanup after faults.
Migrate one noncritical service out of the boot bundle and measure the resulting
loader-size reduction before expanding kernel features.

### 9. Resolve AMD malicious-DMA fault determinism

Keep the focused QEMU reproducer. Require command-ring consumption, a nonzero fault
record identifying the injected requester and target, mapping revocation, device
quarantine, and stale-completion rejection. Do not treat a zeroed or unrelated AMD
fault record as success. Intel fault quarantine remains the comparison baseline.

### 10. Improve the development terminal

Add PTYs, process groups, controlling-terminal semantics, terminal-generated
signals, Readline integration, ANSI/VT behavior, scrollback, and an editor. Use
this work to validate job control with native configure/build/test workloads.

## Kernel and platform roadmap

These items remain after the current system-closure priorities and should not be
started by bypassing the package, service, or DMA contracts above.

11. **Storage and physical media:** complete AHCI/SATA and NVMe discovery, queued I/O,
    removable media, formatting, and safe device identity checks.
12. **USB host:** implement xHCI enumeration, descriptors, control/bulk/interrupt
    transfers, hubs, hotplug, and disconnect handling in QEMU first.
13. **USB devices:** add HID and mass-storage drivers, then USB audio and network
    adapters; add UAS after bulk-only storage is stable.
14. **Text and image rendering:** port FreeType, Fontconfig, HarfBuzz, Pango, Cairo,
    and image codecs in dependency order.
15. **Display/window system:** define modes, buffers, cursors, input, clipboard,
    compositor, and window protocols; improve software rendering and VirtIO-GPU.
16. **GTK applications:** port the selected GLib/GObject/GIO and GTK dependency set,
    then demonstrate a compiled GTK application with software rendering.
17. **Audio:** bring up QEMU-supported Intel HDA or VirtIO sound, playback/recording,
    mixing, volume, and an application API.
18. **Radeon:** validate one GPU family, firmware, display modes, scanout, and only
    then command submission and acceleration.
19. **Nouveau/NVIDIA:** select a generation, pin reusable Nouveau code and firmware,
    and prove stable display before GPU execution.
20. **Mesa:** port suitable OpenGL/EGL components and later Vulkan only after the
    Aurora driver, synchronization, reset, and isolation interfaces are stable.

## Completed milestones

The following are implemented or have bounded validation in the current tree:

- Linux build scripts, GNU bootstrap refresh, hash-verified source mirror, and
  observable/resumable guest transaction state.
- RLIMIT_NOFILE enforcement and regression coverage.
- Kernel-backed read-only procfs entries for processes, descriptors, meminfo,
  uptime, and mounts.
- Deterministic GPT/development-image creation, native partition discovery,
  ext2/FAT32 mounts, and recovery checks.
- Modern VirtIO 1.x block, network, and entropy transport with IOMMU-aware DMA
  mappings and correct modern net framing.
- Intel VT-d and AMD-Vi discovery, translation setup, invalidation, and fail-closed
  startup for supported VirtIO devices.
- Capability-scoped software DMA domains, hardware fault latching, and device
  quarantine plumbing.
- AURMOD1 format validation and guarded activation with deny-by-default grants.
- Storage IPC, built-in ring-3 storage execution, crash containment, generation-safe
  restart handoff, and stale-completion rejection.
- Package manifests, deterministic .deb generation, local installation, dependency
  ordering, upgrade/removal policy, Pages distribution, and a QEMU NAT source mirror.
- Bash startup, modern VirtIO DHCP/IPv4 readiness under QEMU, verified HTTPS support,
  and the Linux archive-member path normalization fix.

## Evidence rules

- A host fixture proves host behavior only; it does not prove guest execution.
- A positive boot does not prove malicious-device blocking.
- A format validator does not prove runtime module activation.
- A storage IPC seam does not prove complete storage-service extraction.
- A package recipe or source lock does not prove a guest .deb or install lifecycle.
- QEMU evidence must retain serial output, stage checkpoints, relevant hashes, and
  the exact launch configuration.

Relevant entry points include native-packages.py, packages/build.sh,
packages/install.sh, packages/generate-pages.py, build-gnu-bootstrap.py,
test-network.py, test-iommu.py, test-amd-iommu.py, test-iommu-dma-fault.py,
test-storage-service.py, test-microkernel.py, test-recovery.py,
test-filesystems.py, and the host package fixtures under tests/. The exact
command and artifact path for each completed milestone should be recorded beside
its implementation rather than inferred from this roadmap.
