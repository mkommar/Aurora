# Isolated driver services

Aurora is migrating from a hybrid kernel toward a capability-based microkernel.
The desktop, input, and display processes already run in ring 3. The display
process now owns GPU policy and scanout through `src/user/gpu_service.h`; the
kernel supplies only the framebuffer mapping and immutable capability bits.

The remaining kernel-side mechanisms are being split behind the same boundary:

| Service | Current kernel responsibility | Migration boundary |
| --- | --- | --- |
| PCI | device discovery and interrupt routing | capability-scoped PCI records and IRQ endpoints |
| GPU/display | Radeon discovery, framebuffer mapping | shared command/complete queues to the display service |
| Network | VirtIO queues, lwIP, socket ABI | shared packet rings and socket broker service |
| Storage | ATA/VirtIO and ext2/FAT32 | block request queues and filesystem service |
| Entropy | VirtIO-rng queue | bounded entropy endpoint |

Until each migration is complete, the kernel retains the minimum compatibility
implementation and the user-facing ABI stays unchanged. This avoids breaking
existing applications while services are moved one subsystem at a time.

The shared contract is defined in `src/service_abi.h`. Requests use bounded
32-entry rings, sequence numbers, explicit status, and opaque arguments. A
service receives only its own ring, device capabilities, and IRQ endpoint;
physical PCI BARs, DMA addresses, and filesystem cache pointers are never
presented to ordinary applications.

Radeon passthrough metadata is defined in `src/radeon_service.h`. The display
service receives the Radeon generation, MMIO BAR, VRAM address when available,
optional PCI IRQ capability, and a 64-entry command ring. The kernel validates
the BAR and PCI capability chain before publishing them. PCIe FLR reset is
available only when the device advertises it; legacy Radeon reset registers are
never guessed. Unsupported devices remain on VBE scanout.

The RX 7800 XT/Navi 32 profile recognizes AMD PCI device `1002:747e`, marks it
as RDNA3, and refuses the command ring for other Radeon IDs. The generic reset
path uses PCIe FLR only when advertised. This profile provides capability
plumbing and safe scanout; a complete accelerated 3D command processor still
requires Navi 32 firmware, VRAM allocation, GPU page tables, scheduling, and
display-mode register programming.
