# Aurora networking

The development profile uses transitional VirtIO-net with QEMU user-mode NAT.
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
There is no host port forwarding in the default launch configuration.

## Runtime and isolation

- IPv4, Ethernet, ARP, ICMP, DHCP, UDP, and TCP are supplied by pinned lwIP.
- DNS uses musl's resolver and `/etc/resolv.conf`, through Aurora UDP sockets.
- The Linux-compatible socket ABI covers client socket creation, bind/connect,
  send/receive, scatter/gather, names, selected options, shutdown, nonblocking
  operation, poll/select, and shared open descriptions across dup/fork.
- `eventfd`/`eventfd2` supply shared counters and readiness for curl's wakeups.
- VirtIO RX/TX buffers remain in supervisor memory. Queue indices, descriptor
  IDs, packet lengths, and user pointers are checked. Processing is limited
  to 64 received packets per scheduler pass. There are 32 socket handles;
  each UDP socket retains at most eight datagrams and 64 KiB of payload.
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

TLS verification uses `/etc/ssl/cert.pem` and the boot-time RTC. Certificate
chain, hostname, and expiry verification remain enabled. The pinned Mozilla
CA snapshot is dated August 13, 2026; update the bundle and its lock-file hash
together when maintaining the system.

## Build and provenance

`network-sources.lock.json` records archive URLs and SHA-256 values.
`fetch-network-sources.py` verifies downloads; the exact CA snapshot is also
vendored under `third_party/network-ca` because the upstream current-bundle URL
changes over time. lwIP's source and BSD license are in `third_party`.

`build-network-bootstrap.py` runs `bootstrap-network.sh` in the temporary
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

This release targets IPv4 client traffic in QEMU TCG. IPv6, listening/accept,
Unix-domain sockets, general hardware NICs, DHCP-derived resolver updates,
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
