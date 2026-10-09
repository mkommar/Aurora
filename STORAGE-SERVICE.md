# Storage service boundary

The current milestone adds a real, bounded service endpoint without moving the
VirtIO driver out of the kernel. `src/storage_service.h` is usable by a
validated service fixture and defines the boundary a future ring-3 storage
service will consume. Requests are versioned, fixed-size and limited to 1024
sectors (512 KiB); read/write buffers must be mapped through the storage DMA
domain. A broker permits one request at a time and requires the storage
capability, the registered service owner, and the registered VirtIO device.
Completion validates both the service generation and sequence, reports I/O
errors, and releases the request. Quiesce cancels the in-flight request before
a restart; handoff revokes the old DMA domain before the replacement can start.

The VirtIO path submits a request through this broker before building its
descriptor chains, then completes it after the existing IRQ/poll path. This is
an integration seam, not an extraction: the kernel still owns the VirtIO
device, descriptor ring, bounce buffers, ATA fallback, boot filesystem and
filesystem locks. The current AURMOD1 manager cannot yet create fresh page
tables, enter an activated image, or grant a live kernel IPC endpoint. The
software DMA manager still checks mapped ranges, while the hardware backend
remains fail-closed and unimplemented.

`test-storage-service.py` compiles and runs the host fixture. It exercises
version/size/opcode validation, bounds and overflow, capability/owner/device
authorization, DMA rejection, single-flight concurrency, service startup and
dispatch, sequence-safe completion, in-flight cancellation, stale-generation
rejection, DMA revoke, restart handoff, and teardown. This is not a guest
ring-3 execution test. Guest hardware, real module entry, crash containment,
and power-loss behavior remain follow-up acceptance tests.
