# Aurora networking

The development profile uses modern VirtIO 1.x net with QEMU user-mode NAT.
Aurora retains its original kernel. The transport reuses lwIP 2.2.1; curl 8.22.0
and Mbed TLS 3.6.7 provide the userspace HTTP/TLS implementation.

## Use

Run `./run.ps1` normally, open the terminal with F2, and wait for DHCP to finish.
The serial log records `NET: DHCP IPv4 address, gateway and TCP/UDP ready`.

```sh
curl --version
curl -fL https://example.com/ -o /work/example.html
```

For longer commands, start `bash --noprofile --norc`; the desktop command line
has a shorter length limit. Download source, verify its expected digest, then
use the installed GCC and GNU Make. For example:

```sh
curl -fL https://curl.se/download/curl-8.22.0.tar.gz -o /work/curl-source.tar.gz
echo 'd54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1  /work/curl-source.tar.gz' | sha256sum -c -
gcc -static /work/http-demo.c -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -o /work/http-demo
/work/http-demo
sync
```

`./run.ps1 -Offline` omits the NIC while retaining the entropy device. The
128 MiB minimal and self-test profiles do not initialize the network DMA area.
QEMU exposes the host through `10.0.2.2`; the configured DNS proxy is `10.0.2.3`.
There is no host port forwarding in the default launch configuration. A test
or custom QEMU launch can forward a host port to a guest TCP listener; a guest
client reaches that service at `10.0.2.2:<host-port>`.

The native package runner uses the same bounded mechanism for a Linux-hosted
source mirror. It binds the verified cache on a host service port and uses the
QEMU user-NAT host gateway, making the mirror available to Aurora at
`http://10.0.2.2:8080`. The server starts only after every requested archive
matches the package source lock and refuses missing or mismatched files; it
does not substitute host-built or placeholder inputs.

## Runtime and isolation

- IPv4, Ethernet, ARP, ICMP, DHCP, UDP, and TCP are supplied by pinned lwIP.
- DNS uses musl's resolver and `/etc/resolv.conf`, through Aurora UDP sockets.
- The Linux-compatible socket ABI covers socket creation, bind/connect/listen/accept,
  send/receive, scatter/gather, names, selected options, shutdown, nonblocking
  operation, poll/select, and shared open descriptions across dup/fork.
- TCP listeners queue up to eight pending accepted connections. Listen backlog
  is clamped to that bounded queue; this is enough for the QEMU repository
  fixture and is not a general high-load server implementation.
- `eventfd`/`eventfd2` supply shared counters and readiness for curl's wakeups.
- VirtIO RX/TX buffers remain in supervisor memory. Queue indices, descriptor
  IDs, packet lengths, and user pointers are checked. Processing is limited
  to 64 received packets per scheduler pass. There are 32 socket handles;
  each UDP socket retains at most eight datagrams and 64 KiB of payload.
- The VirtIO network and entropy devices have explicit capability-scoped DMA
  domains. Their fixed descriptor rings and data buffers must be registered in
  those domains before use; software range checks reject foreign, unaligned,
  overflowing, unmapped, or revoked ranges. The operational VT-d/AMD-Vi
  backend provides hardware translation for the configured QEMU VirtIO
  domains. The dedicated
  DMA-fault QEMU test only targets the transitional VirtIO block device; it does
  not establish protection for arbitrary physical network hardware, interrupt
  remapping, or concurrent multi-device attacks.
- lwIP's raw `NO_SYS` API runs under the existing native-state lock. It never
  keeps syscall user pointers. NIC IRQs acknowledge completion; scheduler
  boundaries perform protocol work and wake socket waiters. This is an
  initial in-kernel network implementation, not an isolated userspace service
  or a fully parallel network stack.
- VirtIO-rng backs `getrandom`, `/dev/random`, `/dev/urandom`, TCP initial
  sequence numbers, and native ELF auxiliary randomness. The prior timer-based
  pseudo-random fallback has been removed. Networking requires this device;
  entropy failures stop transmission. The launcher uses QEMU `rng-builtin`
  without deterministic `-seed` or replay options. Its randomness comes from
  QEMU's crypto backend; on Windows the platform implementation uses
  `CryptGenRandom`. The host and emulator remain trusted components.

Modern VirtIO-net discovers the common, notify, and device PCI capabilities,
maps each capability through its BAR, and writes queue notifications at
`notify_base + queue_notify_off * notify_off_multiplier`. Invalid capability
ranges fail closed before a queue is enabled.

## IPRoute2 boundary

IPRoute2 is a later userspace package, not a current claim of Linux networking
compatibility. Its first Aurora target is a read-only discovery subset backed by
`NETLINK_ROUTE`: `RTM_GETLINK`, `RTM_GETADDR`, and `RTM_GETROUTE`. The kernel
must first provide netlink-family socket creation and bind, aligned netlink
header/attribute validation, multipart dump sequencing, interface/address/route
records, and deterministic errors. Acceptance fixtures will exercise `ip link`,
`ip addr`, and `ip route` against QEMU's DHCP interface and reject truncated,
misaligned, overlong, or wrong-family messages.

Mutation commands, IPv6, qdisc, policy routing and physical-NIC discovery are
not part of that subset. Until those fixtures pass, the LFS systemd-book
package order is a reference for building prerequisites, not evidence that
the Linux `ip` command can run on Aurora.

TLS verification uses `/etc/ssl/cert.pem` and the boot-time RTC. Certificate
chain, hostname, and expiry verification remain enabled. The pinned Mozilla
CA snapshot is dated August 13, 2026; update the bundle and its lock-file hash
together when maintaining the system.

## Build and provenance

`network-sources.lock.json` records archive URLs and SHA-256 values.
`fetch-network-sources.py` verifies downloads; the exact CA snapshot is also
vendored under `third_party/network-ca` because the upstream current-bundle URL
changes over time. lwIP's source and BSD license are in `third_party`.

`fetch-network-sources.py` first downloads or reuses only the exact entries in
`network-sources.lock.json`; it rejects changed content. On Linux,
`build-network-bootstrap-linux.py` then deterministically builds the disposable
bootstrap payload and writes the generated archive and manifest under
`tools/network-bootstrap/`. These are generated artifacts, not source locks;
the manifest records the archive hash, source lock, recipe hash, and compiler.
The Linux path is:

```sh
python3 fetch-network-sources.py
python3 build-network-bootstrap-linux.py
```

The VM path, `build-network-bootstrap.py`, runs `bootstrap-network.sh` in the temporary
Linux build VM using the already pinned native musl GCC 11.2.1. It verifies
inputs, builds static libraries and curl, and exports
`tools/network-bootstrap/network-bootstrap.tar.gz` with a hash manifest.
Original curl and Mbed TLS source archives, including their licenses, are
installed at `/src`. curl uses its curl license; Mbed TLS offers Apache-2.0
or GPL-2.0-or-later licensing. The CA bundle carries its upstream licensing
information in its header.

`stage-network.py` writes these artifacts onto a new copy of the development
disk. It refuses an existing output or the source path. It also installs the
sample client and `/work/rebuild-network.sh`. The prepared build-tree archive
contains sources and generated configure files, with all object files and
libraries removed. Its configure results describe the pinned musl ABI; it
does not claim to run curl's configure probes inside Aurora.

```sh
bash /work/rebuild-network.sh
```

This script compiles Mbed TLS and curl with Aurora's own GCC and GNU Make,
installs their libraries/client, performs a verified HTTPS request, and syncs.

## Validation and current limits

`test-network.py` uses disposable disks and loopback-bound HTTP, TLS, TCP, and
UDP fixtures. It checks binary hashes, redirects, chunked transfers, verified
HTTPS, three certificate rejection cases, DNS/NXDOMAIN, eventfd, socket buffer
validation, nonblocking connect, dup, half-close, shutdown/reuse, bounded
connection failure, and public pinned-source download. C source fetched over
TLS is compiled and executed inside Aurora. `--rebuild` additionally rebuilds
the client and TLS libraries before running these checks. Always sync before
stopping a VM; ext2 is not journaled, and an abruptly stopped disposable test
disk must not be reused as a clean baseline.

`test-apt-repository-pair.py` starts two Aurora guests with separate writable
development disks. The server guest provides repository `Packages`, `Release`,
and `.deb` files; the client retrieves them through QEMU's host-forwarded user
network, then the staged curl performs the same downloads and verifies an HTTPS
payload using the test CA. The harness compares the fetched bytes with the
fixture contents after the client VM stops. On Linux, prepare the helper and
development image with `python3 build-image-tool-linux.py` and
`python3 setup-development.py --partitioned --image build/development-pair.img`;
build the pinned curl bundle from the verified cached source archives with
`python3 build-network-bootstrap-linux.py`, then run:

```sh
python3 test-apt-repository-pair.py --disk build/development-pair.img
```

When a compatible guest Bash executable is available, pass it with
`--bash-binary /path/to/bash`; the harness then runs the Bash/curl repository
and verified-TLS script inside the client guest as well.

This verifies the QEMU NAT host-forwarding path and bounded TCP listener support;
it is an apt-style repository transfer test, not package-manager installation.

This release targets IPv4 traffic in QEMU TCG. IPv6, Unix-domain sockets,
general hardware NICs, DHCP-derived resolver updates,
DNS-over-TCP fallback, and most advanced socket options are not implemented.
Unsupported operations return errors. QEMU NAT may time out rather than
immediately reject an unavailable host port. The tested WHPX configuration
did not advance Aurora's PIT clock and did not complete DHCP; use TCG.

Upstream references: [lwIP source](https://github.com/lwip-tcpip/lwip),
[curl source releases](https://curl.se/download.html),
[Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7),
[QEMU entropy backend](https://github.com/qemu/qemu/blob/master/backends/rng-builtin.c),
[QEMU guest randomness](https://github.com/qemu/qemu/blob/master/util/guest-random.c),
[QEMU platform randomness](https://github.com/qemu/qemu/blob/master/crypto/random-platform.c).
