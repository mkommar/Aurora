# Storage service boundary

The current implementation adds a built-in storage service intended to run as
a ring-3 task through Aurora's existing process/CR3 model. `src/user/storage.c` reaches the
kernel storage endpoint through `SYS_STORAGE`; the kernel keeps the VirtIO
device, descriptor ring, and DMA mappings. Requests are versioned, fixed-size
and limited to 1024 sectors (512 KiB); read/write buffers must be mapped
through the storage DMA domain. A broker permits one request at a time and
requires the storage capability, the registered service owner, and the
registered VirtIO device. Completion validates both the service generation and
sequence, reports I/O errors, and releases the request.

Quiesce cancels the in-flight request before a restart. A fault or explicit
service exit marks only the storage task dead, revokes its DMA mappings, and
causes the supervisor to create a fresh ring-3 task after a newer-generation
handoff. Completions from the retired generation are rejected. The self-test
image deliberately faults the first storage instance and is intended to verify
that the replacement runs, while normal images only exercise the bounded flush
probe. Current QEMU validation is blocked earlier at the task-return `iretq`
path after expanding the loader for the larger bundle, so this milestone does
not claim guest execution or crash containment yet.

The VirtIO path submits a request through this broker before building its
descriptor chains, then completes it after the existing IRQ/poll path. This is
The intended boundary is real guest ring-3 execution, but not a complete extraction: the kernel
still owns the VirtIO device, descriptor ring, bounce buffers, ATA fallback,
boot filesystem and filesystem locks. AURMOD1 remains a validated, guarded
activation format; arbitrary dynamic module page-table creation and live
endpoint assignment are still separate work. The software DMA manager checks
mapped ranges, while the hardware backend remains fail-closed and
unimplemented.

`test-storage-service.py` compiles and runs the host fixture. It exercises
version/size/opcode validation, bounds and overflow, capability/owner/device
authorization, DMA rejection, single-flight concurrency, service startup and
dispatch, sequence-safe completion, in-flight cancellation, stale-generation
rejection, DMA revoke, restart handoff, and teardown. `test-microkernel.py`
adds guest coverage for the ring-3 storage task's deliberate crash, restart
generation, separate address space, and continued desktop operation. Guest
hardware IOMMU enforcement, arbitrary module entry, and power-loss behavior
remain follow-up acceptance tests.
