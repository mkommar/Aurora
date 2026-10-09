# Native GNU builds and Debian-format packages

This work is in progress. A source lock and native build runner are implemented;
they do not mean that all listed packages have been ported. The installed C
compiler remains the bootstrap GCC. Upstream dpkg and APT are not yet installed.

Device-facing service extraction is deliberately separate from package work.
The kernel's current VirtIO paths use explicit software DMA domains and
deny-by-default device ownership. `src/storage_service.h` now provides the
bounded transport seam and host regression, but the broker still runs in the
kernel and `SERVICE_STORAGE` is not an extracted ring-3 service. Package
tooling must not be used as evidence of storage isolation or hardware IOMMU
support.

The [LFS systemd book](https://www.linuxfromscratch.org/lfs/view/systemd/)
is the reference for dependency order and host-tool vocabulary. Aurora retains
musl, its custom kernel and its own syscall/device boundaries; LFS does not
make Linux kernel interfaces or glibc packages available here.

## Build inputs and execution

`packages/sources.lock.json` pins 23 source archives, versions, HTTPS locations,
SHA-256 hashes, build dependencies, patch lists and main-project license
identifiers. Hashes were acquired over HTTPS; upstream signatures have not been
verified. License identifiers are summaries: redistributed packages must retain
the complete upstream notices and exceptions.

`native-packages.py` stages the lock and reviewed recipes, not source archives
or configure output. `packages/build.sh` downloads through Aurora's curl,
verifies the source hash, unpacks into a fresh build directory and invokes the
upstream configure script under Aurora's Bash. It retains logs, uses `DESTDIR`,
and has recipes for creating real Debian binary archives with `ar`, `tar` and
`gzip`. No host compiler or Linux kernel executes the package build.

The `.deb` recipe records source and combined build/install recipe hashes,
copies license files and the source lock, and includes a file list and SHA-256
manifest. Sorted tar entries, fixed archive timestamps, deterministic `ar` mode
and `gzip -n` remove variable archive metadata. Bit-for-bit reproducibility of
successive package builds still needs testing. Build dependencies are recorded;
runtime dependencies need to be audited per package before release. This runner
is not a dependency solver.

The diffutils entry is the reference GNU recipe for future m4 and related
ports: add a verified source-lock record, list only prerequisites already
available in the candidate image, add a dedicated `build.sh` case when the
upstream test/install sequence needs policy, and keep configure, `make`,
`make check`, `DESTDIR` staging, package metadata and a `--version` smoke check
inside Aurora. Never add a source checksum unless it is established from an
authoritative source or an existing repository cache.

The planned package track follows the LFS ordering in bounded Aurora steps:
binutils and GCC foundations; Aurora-compatible API headers; m4; Perl;
Autoconf; Automake; Libtool; Bison/Flex; Texinfo; compression and file tools;
then IPRoute2 after the `NETLINK_ROUTE`/rtnetlink subset is implemented and
tested. Each recipe must record exact build dependencies, pass clean native
configure/build/test, stage with `DESTDIR`, and produce a deterministic Debian
archive before being used as another package's prerequisite.

The GNU Make recipe now has an installer path after package creation, and
diffutils is the first explicit native prerequisite recipe using its existing
pinned GNU source URL and SHA-256.
`packages/install.sh` verifies the package sidecar or an explicitly supplied
download hash, exact Debian archive members and types, package identity/version/
architecture, data paths under `/opt/aurora`, the package file list, and every
installed file hash. It rejects traversal, symlink/hard-link members and unsafe
overwrites, records ownership under `/var/lib/aurora/packages`, and rolls back
created files/state on failure. Supported commands are `install.sh ARCHIVE
CHECKSUM ROOT`, multiple local archives, `--repository DIR` for generated
`Packages` plus `pool/` metadata, `install.sh --url URL --sha256 HASH --root
ROOT`, `--owner /opt/aurora/path`, `--list aurora-name`, and `--remove
aurora-name`. Dependencies support only comma-separated exact Aurora package
names. Alternatives, version operators, conflicts, virtual packages,
architecture qualifiers, maintainer scripts, conffiles, permissions/owners and
triggers are unsupported. Cycles and missing packages are diagnosed before
mutation; upgrades replace only unchanged owned files, downgrades require
`--allow-downgrade`, and removal refuses packages still required by installed
dependents. Version ordering is a bounded numeric-component/ASCII-suffix
comparator, not full Debian version semantics. This is not dpkg or APT.

Local patches are SHA-256 pinned, verified before staging and again in Aurora,
applied with GNU patch, and copied into the package's documentation. The musl
entry records the existing fork-child TID-registration backport; the new musl
package recipe has not yet completed its validation run.

Candidate packages use `/opt/aurora` and `musl-linux-amd64`, separate from the
bootstrap tools. Stock Debian glibc packages are not compatible with this image.
The initial archive writer bootstraps the `.deb` format. The installer uses
Aurora's existing HTTPS-enabled `curl` for optional download-then-install, but does not
fetch dependencies or claim APT compatibility. It consumes only the generated
local `Packages` and `pool/` tree for dependency selection. An authenticated
index policy remains follow-up work. Archive validation and repository parsing
use ordinary temporary files, not `/proc/self/fd` or process substitution, so
procfs is not required.

## GitHub Pages distribution

`packages/generate-pages.py` creates a deterministic static distribution tree
without downloading source archives. It accepts a directory of already-built
`.deb` files and checks each filename and control record against the source
lock. The output contains `apt/pool/`, APT metadata at
`apt/dists/aurora/main/binary-musl-linux-amd64/`, `sources/index.json`, one
source manifest per locked package, `release-manifest.json`, and `index.html`.
The release manifest records package sizes and SHA-256 hashes, the source-lock
hash, and stable paths; it deliberately has no generation timestamp.

Reproduce a local publication with:

```sh
rm -rf build/aurora-pages
python3 packages/generate-pages.py --packages build/package-output \
  --output build/aurora-pages
python3 -m http.server 8000 --directory build/aurora-pages
```

The package directory must contain archives named like
`aurora-make_4.4.1-1_musl-linux-amd64.deb`; the package control fields,
source-lock version, architecture and package namespace must agree. The
dedicated Pages URL shape is `https://mkommar.github.io/Aurora-packages/`,
with the APT release at
`https://mkommar.github.io/Aurora-packages/apt/dists/aurora/Release`. A
package download example is
`curl -fLO https://mkommar.github.io/Aurora-packages/apt/pool/<package>.deb`.
After adding the release to an authenticated APT configuration, the package
install example is `apt install aurora-make`.
The unsigned `Release` hash must be authenticated or replaced by a signed
release policy before use. `.deb` files are supplied by the package build
workflow; source archives are not downloaded or mirrored by this generator.

`.github/workflows/publish-packages.yml` accepts a workflow artifact containing
the `.deb` files and defaults to `mkommar/Aurora-packages`. It bootstraps an
empty target with an orphan `gh-pages` branch and pushes it using the
`AURORA_PAGES_TOKEN` secret. Set the input to an empty value only when using
this repository's Pages deployment instead. The dedicated repository still
needs Pages enabled for its `gh-pages` branch and a token with write access;
the generator itself never creates repositories or configures Pages.

## Reproduction

Run these host commands with the development disk stopped:

```powershell
.\build.ps1
.\build-image-tool.ps1
python prepare-build-volume.py
python native-packages.py make
```

The host-only publication and fixture test is:

```sh
python3 test-package-pages.py
```

`python tests/package-install-host.py` builds synthetic Debian archives and
checks successful install, ownership/list queries, safe removal, checksum
rejection, duplicate/overwrite refusal, preflight collision behavior, install
rollback and path traversal rejection.
`python tests/package-lifecycle-host.py` covers dependency ordering, missing
dependencies, cycles, upgrades, same-version reinstall rejection, bounded
version ordering, downgrade rejection, file replacement and dependency-aware
removal. These are host transaction fixtures; they do not establish guest
package evidence.
The Make package's guest download/configure/build/package/install/smoke flow
still requires a prepared development image with the bootstrap GNU tools and
network access. No pinned Make 4.4.1 source archive is currently cached in the
host workspace, and the latest host fetch attempt failed with `Network is
unreachable`; a full guest Make package run has not yet been recorded.

The volume preparer refuses existing destinations and grows a **new copy** to
8 GiB ext2, preserving the FAT32 exchange partition. A temporary Linux VM runs
offline e2fsck/resize2fs only. Its automatic preen repairs apply to the copy;
serious filesystem errors stop preparation. Failed volumes retain a `.partial`
suffix. The original development image is not resized or formatted.

The native runner uses another disposable image in `build/native-package-tests`.
It keeps `serial.log`, `results.json`, the disk, and the generated job script.
Guest logs and output packages live under `/work/packages/logs` and
`/work/packages/out`. Use `--resume` only when deliberately reusing that test
disk. A failed source build directory is preserved and not silently reused;
inspect its log and choose a fresh candidate for a clean retry.

For offline or restricted-host validation, `native-packages.py` can serve a
verified source cache through a Linux-hosted mirror fixture and QEMU user-mode
NAT:

```sh
python3 native-packages.py diffutils --mirror-cache build/native-mirror \
  --disk build/native-build.img --folder build/native-package-tests
```

The cache must contain the exact archive filename from
`packages/sources.lock.json`; the runner verifies its SHA-256 before starting
the HTTP server. QEMU forwards a loopback host port to guest `10.0.2.2:8080`,
and the guest build uses that mirror instead of upstream internet. The mirror
does not create missing bootstrap or source inputs. `--retry` remains bounded
to a resumed disposable disk and preserves failed build trees/logs; it does
not reuse a failed source tree.

The fixture has been host-tested with the exact diffutils 3.10 archive. A
guest package result still requires the prepared GNU bootstrap image and all
of its locked inputs; a working source mirror does not replace that
prerequisite.

Only the bootstrap tools are initially available. Later recipes require their
listed prerequisites to have been installed on the candidate. Passing a list of
package names does not install dependencies automatically. Recipes beyond the
tested milestones are provisional and must not be treated as successful ports.

## Compiler gates and platform findings

`test-bootstrap-cxx.py` restores C++ components from the existing SHA-512-pinned
musl.cc archive on a disposable disk and tests STL and exception handling. This
is an **imported bootstrap**, not the requested native GCC rebuild.

The GCC recipe runs upstream `make bootstrap`, including its stage comparison,
then invokes `tests/compiler-corpus.sh` against the staged candidate. The corpus
checks integer arithmetic, floating point, varargs, sorting, allocation,
setjmp/longjmp, atomics and pthreads at three optimization levels. These gates
do not replace the upstream GCC testsuite. No compiler activation is performed.

Platform fixes found while starting clean configure runs:

* Kernel `execve` now resolves `#!` scripts with bounded recursion, executable
  permission checks and one optional interpreter argument. Direct execve tests
  avoid Bash's fallback and exercise errors as well as successful execution.
* Repeated compiler probes reached the virtual window used for guarded kernel
  stacks. That window is now excluded from the identity-addressed physical page
  allocator. The regression cycles through 960 MiB of allocations and checks
  contents after writing each page.
* Creating a read-only file through a writable `O_CREAT` descriptor now permits
  the initial writes. Later opens still enforce its mode. This fixes extraction
  of Bash's read-only test fixtures and has a direct syscall regression.
* Filesystem entry-cache eviction runs between operations and pins open
  descriptors and recovery entries. A guest regression traverses 20,000 paths
  and checks an open descriptor across eviction; a host fixture checks the
  production eviction routine. The raw AuroraFS directory is never evicted.
* VirtIO checks the used ring before resetting a device on timeout, including
  counter wrap and partial batches. A truly incomplete request still disables
  the volume. The bounded watchdog allows 30 seconds plus batch allowance;
  the previous 2.5-second single-request deadline failed during source extraction.
  The ext2 block cache is now 128 blocks (512 KiB), reducing metadata rereads.

Run `python test-exec-scripts.py` for these regressions. The clean Make configure
probe is `python aurora_vm.py tests/configure-make.sh --folder build/configure-probe`.

Validated on disposable images: interpreter/error cases and 960 MiB allocator
cycling (`build/exec-script-tests`), C++ STL/exceptions (`build/bootstrap-cxx-tests`),
the bootstrap compiler corpus at `-O0/-O2/-O3` (`build/compiler-corpus-tests`), and
80 Bash/sed/grep pipelines on four CPUs (`build/build-shell-tests`). GNU Make
4.4.1 completed clean native configure, compilation, execution and staged
installation on one CPU (`build/configure-make-single`); its `config.log` and
configure/build/install logs are retained there. This run used the source archive
already present on the bootstrap image. The separate download-to-package run
must pass before claiming the full network workflow.

The pinned GCC archive contains 114,451 entries, including its testsuites;
the bounded cache now evicts entries, but full-tree bootstrap remains untested.
Earlier long builds failed, and a one-CPU Perl extraction captured a genuinely
incomplete VirtIO request. Larger-build reliability must be validated after the
cache/watchdog changes; smaller passing tests do not establish it. Memory/task limits,
real cross-process file locks, the existing patched musl runtime, native dpkg,
and interruption recovery must all be validated before replacing bootstrap
tools or claiming transactional package installation. ext2 still lacks a
journal or general ordered-metadata recovery; see [FILESYSTEMS.md](FILESYSTEMS.md).
