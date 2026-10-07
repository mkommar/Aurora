# GCC running inside Aurora

The compiler now executes as Aurora user processes. GCC's driver starts `cc1`,
GNU `as`, `collect2`, and GNU `ld` inside the guest. Generated static and dynamic C executables
run there too. No Windows compiler, host compilation bridge, Linux kernel, or
nested Linux VM is involved in this workflow.

This is a **binary compatibility port**, not a GCC source port defining a new
`x86_64-aurora` target. Aurora implements the subset of the Linux x86-64 syscall
ABI used by a pinned, statically linked GCC/musl toolchain. The compiler reports
the target `x86_64-linux-musl`; its generated programs use that same compatibility
interface. Aurora's original application ABI and Windows-hosted SDK still work.

## Try it

Double-click `Launch Aurora.cmd`. The launcher prefers `build/development.img`,
falls back to `build/toolchain.img`, and selects 1 GiB RAM. In F2 Terminal:

```text
gcc --version
gcc -static demo.c -o demo
./demo
```

Expected output:

```text
Compiled by GCC inside Aurora!
Application exited: 0
```

Other included sources are `compute.c` (allocation, a loop and integer output),
`math.c` (floating point), and `broken.c` (intentional compiler diagnostics).
The updated development disk supports default dynamic linking, PIE and shared
libraries through the guest-built musl loader. `gcc demo.c -o demo` works;
use `gcc -fPIE -pie` for PIE and `gcc -shared -fPIC` for a DSO. The older
`-NativeGcc` disk still requires `-static`. `gcc -c` and `gcc -S` remain available
for intermediate output. See [multicore and dynamic linking](SMP-DYNAMIC.md).

To edit a source file in Aurora:

1. In the terminal, enter `load demo.c`.
2. Press F3 to edit it in Notes. The existing editor supports append/backspace.
3. Return to F2 and enter `save example.c`.
4. Run `gcc -static example.c -o example`, then `./example`.

Named `save FILE` / `load FILE` use the development volume's `/work` directory.
Plain `save` / `load` still save and restore `notes.txt` on the original AuroraFS
volume. `ls` includes development files; `cat FILE` can read them. Notes still has
a 1,598-character editing limit. The desktop command prompt splits arguments
on spaces. Run `bash --noprofile --norc` for quoting, redirection and pipelines.

## Installation and launch options

The current workspace already has the compiler disk. To reproduce it elsewhere,
with Python 3 available:

```powershell
python setup-native-gcc.py
.\build.ps1
.\run.ps1 -NoBuild -NativeGcc
```

On Linux, use the portable image/build path and the distro QEMU binary:

```sh
sudo apt install clang lld llvm nasm binutils qemu-system-x86 python3-pil musl-tools e2fsprogs
python3 setup-native-gcc.py
python3 build-linux.py
python3 test-native-gcc.py --disk build/toolchain.img --cpus 1 --compile-only
python3 prepare-native-gcc-threadfix.py
python3 test-native-gcc.py --disk build/toolchain-threadfix.img --cpus 1 --foundations-only
```

The Linux runner now resolves QEMU from `AURORA_QEMU` or `PATH` and does not
use Windows process-creation flags. The native-GCC test still exercises the
compiler inside Aurora; if the guest test times out after compilation, retain
the serial log because that indicates a guest runtime regression rather than a
host toolchain setup failure. `--compile-only` validates the in-guest compiler
and linker without entering the longer foundations runtime suite; it reports
the full suite exercises the runtime, thread-exit and filesystem-sync paths.
The original AURDEV image remains unpatched; its musl `_Fork` implementation
does not register the thread-exit futex in the child. `prepare-native-gcc-threadfix.py`
creates a separate copy with Aurora's pinned musl `_Fork` fix after checking
both source and original libc hashes. The copy is for validating thread-exit
behavior and does not modify the source compiler image. This Linux fallback
builds the pinned musl archive member on the host; the documented full
development-image workflow still compiles the backport inside Aurora.

Setup downloads an 89 MB archive, verifies the pinned SHA-512, resolves archive
links in memory, and copies selected file contents into a 512 MiB raw disk. It
never extracts archive paths onto the host filesystem and refuses to replace an
existing compiler disk. The disk is Git-ignored and preserved across OS rebuilds.
Its source/version/hash manifest is `build/toolchain.manifest.json`.

Use `.\run.ps1 -Minimal` for the original 128 MiB configuration without the
development disk. Self-test runs stay minimal unless `-NativeGcc` is specified.
Keep QEMU closed while rebuilding or changing disk images from Windows.

## Current development environment

The original GCC/musl toolchain now also runs on a GPT ext2 development disk
with a FAT32 exchange partition and a VirtIO block driver. GNU Bash, Make,
coreutils, sed, grep, gawk, findutils, tar and gzip are installed from pinned
source builds. GNU Make can compile and install itself inside Aurora.

See [Development foundations](FOUNDATIONS.md) for the current implementation,
commands, source versions, validation and remaining work. Native processes now
use allocated physical pages, shared open-file descriptions, interactive TTY
input and nested signal delivery. The normal kernel has 29 application slots
(13 shared with SDK applications, 16 native-only), with 512 MiB native virtual
spaces, first-touch commitment of anonymous memory and copy-on-write fork.

The older AURDEV01 disk remains available through `run.ps1 -NativeGcc`.
Its flat metadata table, 1 MiB file reservations and 32 MiB growth limit apply
only to that older format. The ext2 development disk supports directories,
symlinks, metadata and normal file allocation. VirtIO and ATA requests both
suspend their caller until IRQ completion; see [PLATFORM.md](PLATFORM.md).

This remains a limited Linux ABI, without networking or full POSIX compatibility.
GCC itself has not been rebuilt inside Aurora. Static and dynamic musl pthreads
use shared VM and futex mechanisms and can execute on different CPUs; see
[Thread validation](THREADS.md). The original compiler package remains GCC 11.2.1 with binutils 2.37.

## Tests and provenance

```powershell
python test-native-gcc.py
```

This test copies both disks, runs a hidden QEMU instance on QMP port 4446, and
invokes no host C compiler. It verifies the driver version, compiler/assembler/
linker execution, generated programs, malloc/integer/floating-point output,
invalid-source diagnostics, recovery, shared-memory pthreads, COW memory,
reboot persistence, and original SDK apps. It defaults to the patched VirtIO
development image and stages the current regression source onto its copy.
Results: `build/ext2-tests/results.json`. Screenshot: `build/native-gcc-tested.png`.

Toolchain source: [musl.cc static native toolchains](https://musl.cc/), specifically
`x86_64-linux-musl-native.tgz`: GCC 11.2.1 (20211120), Binutils 2.37, and musl.
The distributor links its build scripts, configuration, component sources and
checksums. Downloaded binaries remain outside the tracked source tree; their
upstream licenses apply. Aurora's compatibility implementation is in
`src/native.h` and `src/native_fs.h`.

GCC background: [build/host/target terminology](https://gcc.gnu.org/onlinedocs/gccint/Configure-Terms.html)
and [GCC build prerequisites](https://gcc.gnu.org/install/prerequisites.html).
