# Storage service boundary

The current milestone adds a real, bounded transport seam without moving the
driver out of the kernel. `src/storage_service.h` is the contract a future
ring-3 storage service will consume. Requests are versioned, fixed-size and
limited to 1024 sectors (512 KiB); read/write buffers must be mapped through
the storage DMA domain. A broker permits one request at a time and requires the
storage capability, the registered service owner, and the registered VirtIO
device. Completion validates the sequence, reports I/O errors, and releases
the request. Timeout cancellation and teardown reject late completions.

The VirtIO path submits a request through this broker before building its
descriptor chains, then completes it after the existing IRQ/poll path. This is
an integration seam, not an extraction: the kernel still owns the VirtIO
device, descriptor ring, bounce buffers, ATA fallback, boot filesystem and
filesystem locks. The software DMA manager still checks mapped ranges, while
the hardware backend remains fail-closed and unimplemented.

`test-storage-service.py` compiles and runs the host fixture. It exercises
version/size/opcode validation, bounds and overflow, capability/owner/device
authorization, DMA rejection, single-flight concurrency, sequence-safe
completion, cancellation and teardown. Guest hardware, service restart and
power-loss behavior remain follow-up acceptance tests and are not claimed by
this milestone.
