# Aurora loadable modules

Aurora's first module milestone is a bounded transport and validation format
for already-linked ring-3 user services. It is intentionally not a kernel
module loader: the current kernel has no safe relocation, capability-grant,
page-table mapping, restart, or unload path for privileged code.

## ABI v1

`tools/module-format.py` writes an `AURMOD1` header followed by a flat service
image. The header records the ABI version, user-service kind, IPC and unload
flags, payload size, entry offset, RX boundary, read-only boundary, and a
SHA-256 digest. Payloads are limited to 512 KiB and must satisfy:

- entry `<` RX boundary `<=` read-only boundary `<=` payload size;
- no unknown flags, relocations, or privileged module kind;
- exact file length and SHA-256 match before any future staging or activation.

The service image is linked before packaging, so module offsets are relative to
the flat payload and do not imply that the image may execute at an arbitrary
address. ABI v1's unload policy requires an explicit unload flag and zero
active calls. There is no runtime unload implementation yet; this guard is the
contract a future service manager must enforce.

The self-test build emits `modules/probe.mod` as a concrete module artifact.
It remains a test artifact and is not activated by the kernel. Activation must
first add a capability-scoped loader, fresh page-table mappings with RX/RW/NX
permissions, service restart/timeout handling, and an IPC endpoint grant.

## Storage-service boundary

The storage IPC header now includes a host/fixture service boundary. A service
must start with the broker's current generation, registered owner, and granted
storage capability before it can dispatch a request. The broker validates
device ownership, DMA-domain identity, request ranges and permissions, and
single-flight sequencing. Quiesce cancels the outstanding request; handoff
requires a newer generation, invokes DMA revocation before admitting the
replacement, and rejects completions from the old generation. This is a
lifecycle and ABI regression boundary, not proof that an AURMOD1 payload has
entered ring 3 inside Aurora.

The kernel still lacks dynamic page-table construction and a safe entry/exit
path for an activated module's storage endpoint. VirtIO descriptors, DMA
mappings, and device ownership therefore remain supervisor-owned. A future
guest implementation must connect this contract to the existing process model
without allowing module metadata to grant capabilities.

## Hardware boundaries

This format does not provide IOMMU or DMA isolation. The kernel now has a
capability-scoped software DMA manager: each VirtIO block, network, and entropy
device is assigned to one supervisor-owned domain, and every fixed ring/request
buffer used by the drivers is explicitly mapped. Descriptor construction is
rejected for unmapped, misaligned, overflowing, or foreign ranges; unmap,
revocation, duplicate ownership, and teardown are covered by a host regression.
This is software range enforcement only: it cannot stop a malicious device from
issuing a physical transaction. The hardware backend is intentionally
non-operational and fails closed. VT-d/AMD-Vi page-table programming, fault
handling, and a negative hardware DMA test remain prerequisites for claiming
hardware isolation. Storage is still in the kernel and is not a service claim.

## Build and test

```sh
python3 tests/module-format-host.py
python3 tests/module-activation-host.py
python3 tools/module-format.py validate build/selftest/modules/probe.mod
```
