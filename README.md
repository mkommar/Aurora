# Aurora OS 0.2 â€” microkernel

An original, bootable x86-64 hobby OS with a graphical desktop. The kernel now
handles memory protection, scheduling, traps, IPC, and capability checks.
The GUI, applications, PS/2 driver, RTC, and framebuffer driver run in separate
ring-3 service processes. No existing kernel is used.

## Run

Double-click **Launch Aurora.cmd** to boot the built image in portable QEMU.
Close the existing Aurora instance before starting another: the disk image and
local QMP port are shared.

**Native GCC and GNU tools are available:** the launcher prefers
`build/development.img` (GPT, ext2 and FAT32 over VirtIO) and uses 1 GiB RAM.
It falls back to the original `build/toolchain.img` when needed.
In F2 Terminal, run `gcc -static demo.c -o demo`,
then `./demo`. Compilation and linking happen inside Aurora. See
[Native GCC guide](NATIVE-GCC.md) for source editing, installation, and limits.
GNU Bash, Make and the source-bootstrap workflow are described in
[Development foundations](FOUNDATIONS.md), including the remaining work in
steps 1â€“7. GNU Make can now rebuild and install itself inside Aurora.
Musl POSIX threads use shared address spaces and futex synchronization.
The launcher now starts four CPUs; use `-Cpus 1` for a single-CPU boot.
ELF interpreters, PIE applications and shared-library loading are supported.
After installing the shared runtime, `gcc demo.c -o demo` builds dynamically;
`-static` keeps the existing self-contained executable path.
See [multicore and dynamic linking](SMP-DYNAMIC.md) and the earlier
[thread, memory and IRQ validation](THREADS.md).
The base platform now has demand-paged anonymous memory, 32 task slots,
nested signal delivery with `siginfo`, interval timers, batched VirtIO and
interrupt-driven ATA storage, and the build-critical syscalls that GNU
configure scripts and build tools expect; see [PLATFORM.md](PLATFORM.md) and
`python test-platform.py`.
Legacy and native processes now share one filesystem namespace (AuroraFS at
`/aurorafs`, `/work` fallback for the legacy calls), hard links, owners and
timestamps are stored, crash orphans are reclaimed at mount, a damaged GPT
copy is recovered from the other one, and `fsck-aurora` checks GPT, ext2,
FAT32 and AuroraFS from inside Aurora; see [FILESYSTEMS.md](FILESYSTEMS.md)
and `python test-filesystems.py`, which cuts power mid-write and verifies
recovery.

See the updated [20-item roadmap](ROADMAP.md) for remaining work, including
network downloads, native source builds, USB, GTK, sound, Radeon and Nouveau.

```powershell
.\run.ps1 -NoBuild       # Boot build/aurora.img
.\run.ps1                # Rebuild and boot
.\run.ps1 -Headless      # Rebuild and boot without a window
.\run.ps1 -Minimal       # Original 128 MiB environment, no GCC disk
```

Click inside QEMU to capture input; **Ctrl+Alt+G** releases it. Close the QEMU
window to stop, or type `poweroff` in the terminal.

| Control | Action |
| --- | --- |
| F1 / Welcome | Welcome screen |
| F2 / Terminal | Console; type `help` |
| F3 / Notes | Type, Enter for newline, Backspace to erase |
| F4 / Settings | Mint/amber palette |
| Drag title bar | Move the current application window |
| x / Escape | Close the current window |

One app window is shown at a time. Use terminal `save` and `load` to persist and
restore Notes as `notes.txt`; unsaved changes and terminal output are lost on
reboot. Notes use append/backspace editing with
wrapping and scrolling. The keyboard layout is US. The clock reflects QEMU's
RTC, normally UTC.

Terminal commands: `help`, `about`, `mem`, `clear`, `theme`, `reboot`, `poweroff`,
`ls`, `cat FILE`, `save`, `load`, `run APP [args]`. Application names also work
directly: `hello aurora`, `calc 12 30`, `filedemo`.

## C applications and storage

Aurora now loads static ELF64 applications from a persistent AuroraFS filesystem.
Each application has its own protected address space, stack guard, and exit status.
The C SDK supplies terminal output, basic strings, a reusable heap allocator, and
whole-file reads/writes. The sample `filedemo` writes `message.txt`; view it with
`cat message.txt`, including after reboot.

Compile and install your own application while QEMU is stopped:

```powershell
.\build-app.ps1 -Source apps/myapp.c -Name myapp -Image build/aurora.img
.\run.ps1 -NoBuild
```

See [SDK guide](sdk/README.md) for an example, supported C functions, and limits.
The SDK path compiles on Windows. The optional native GCC environment compiles
inside Aurora using a separate compatibility ABI and development volume.
Filesystem limits are 32 files, 64 KiB per file, and a flat directory. Builds
preserve existing files while refreshing the bundled examples. Disk I/O is
synchronous ATA PIO inside the kernel in this first implementation; moving it to
a separate storage service is future work.

## What changed from 0.1

| Component | Privilege | Responsibility |
| --- | --- | --- |
| Microkernel | Ring 0 | Private page tables, timer preemption, syscall/trap entry, scheduling, IPC, capability enforcement |
| Desktop process | Ring 3 | Window UI, text rendering, terminal, notes, settings |
| Input/platform process | Ring 3 | PS/2 controller protocol, input packets, RTC, reboot/shutdown requests |
| Display process | Ring 3 | Copy completed desktop frames to the hardware framebuffer |

Each process has its own CR3 (page-table root), 2 MiB private region, read-only
executable code, non-executable writable data, and a stack guard page. Messages
are copied through bounded kernel queues; receivers can block. Senders and
device ports are checked against fixed startup capabilities.

The desktop writes a shared presentation surface; the display service can read
it and is the only process with a user-accessible hardware framebuffer mapping.
The input service is the only process granted PS/2/RTC/power port operations.
All services execute with IOPL 0, so direct `in`/`out` instructions are forbidden.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the memory map, syscall ABI, IPC
protocol, and precise limitations.

## Build

Requires Windows PowerShell, NASM, and LLVM (`clang`, `ld.lld`, `llvm-objcopy`,
`llvm-nm`). QEMU is required to run it. `setup-tools.ps1` downloads pinned NASM
and QEMU distributions and extracts them locally using 7-Zip.

LLVM is detected on PATH, via `AURORA_LLVM`, or at the Unity Android NDK
installation found on this computer. To choose another directory:

```powershell
.\build.ps1 -LlvmBin 'C:\path\to\llvm\bin'
```

The build separately compiles and links each user service, derives page
permissions from its linker symbols, and embeds its flat image into the boot
bundle. The kernel copies the services into distinct private memory at startup.
The disk image is 16 MiB, and the loader accepts a bundle up to 240 KiB.
ELF files retain symbols for debugging. Generated files and tools are Git-ignored.

Equivalent QEMU command on another machine:

```sh
qemu-system-x86_64 -machine pc -accel tcg -cpu qemu64 -m 128M -vga std -drive format=raw,file=build/aurora.img -net none
```

## Tests

Python 3 and Pillow are used by the QMP integration tests. QMP listens only on
`127.0.0.1:4444`. With a normal VM running:

```powershell
python test-smoke.py --nm 'C:\path\to\llvm\bin\llvm-nm.exe'
```

For the fault-injection image, close the normal VM and run:

```powershell
.\run.ps1 -SelfTest -Headless
```

Then, in another PowerShell terminal:

```powershell
python test-microkernel.py --nm 'C:\path\to\llvm\bin\llvm-nm.exe'
python test-smoke.py --build-dir build/selftest --nm 'C:\path\to\llvm\bin\llvm-nm.exe'
```

Verified: **26 native GCC/thread checks**, **24 application/storage checks**,
**28 microkernel checks**, and **12 GUI checks**, including GUI
operation while a hostile process spins continuously. The protection checks
cover private address spaces, page permissions, stack guards, hardware access,
invalid syscall buffers, exception containment, and timer preemption. The six
intentional faults are present only in the self-test image.

Multicore, dynamic linking and the expanded POSIX runtime are validated on
**1, 4 and 8 CPUs**. The patched shared musl runtime was compiled inside Aurora.
See [multicore/runtime validation](SMP-DYNAMIC.md) for the full 196-check matrix,
filesystem checks, deployment records and remaining concurrency limits.

The application/storage suite also tests malformed ELF files, runtime and memory
allocation, invalid file buffers, maximum-size files, crash recovery, and persistence
across a complete VM restart. Run it using the commands in [SDK guide](sdk/README.md).
Its results are saved to `build/application-tests/results.json`.

Results: `build/test-results.json`, `build/selftest/microkernel-results.json`,
and `build/selftest/test-results.json`. Screenshots go to `build/`; serial
diagnostics go beside the selected image. `tools-qmp.py` supports `capture NAME`,
`status`, `key f2 h e l p ret`, and `quit`.

## Current scope

This is a small OS with microkernel-style service isolation and an initial
in-kernel storage implementation. Native applications use a BIOS-backed page
allocator and musl's dynamic linker. The development profile includes VirtIO-net,
IPv4/DHCP, DNS, and curl with verified TLS; see [NETWORK.md](NETWORK.md).
Automatic service restart remains unimplemented. Radeon PCI display controllers are detected at boot; known-safe hardware uses the aligned accelerated scanout path while unknown hardware keeps the VBE framebuffer fallback. Generation-specific Radeon command processors are not enabled yet. The separate SDK runtime supplies a fixed 128 KiB heap. The terminal and notes
share the desktop process. Scheduling is
preemptive, but PS/2 input still uses polling and can consume a host CPU core.
It targets BIOS QEMU with one to eight CPUs, 128 MiB RAM (1 GiB for GCC) and standard VGA, not UEFI or
general physical hardware.

Tool sources: [NASM](https://www.nasm.us/),
[QEMU Windows builds](https://qemu.weilnetz.de/), [LLVM](https://llvm.org/).
CPU reference: [Intel system programming manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
QEMU supplies the BIOS and VBE font; no kernel implementation from another OS
is embedded in Aurora.

## Bootable ISO artifact

Run `./make-iso.ps1` to create `build/aurora.iso`. This is a bootable Aurora
BIOS hybrid disk artifact: it preserves the exact 512-byte boot sectors and
GPT layout, so the BIOS loader and kernel remain byte-for-byte compatible.
The generated `build/aurora.iso.json` records the SHA-256 and launch command.

```powershell
./make-iso.ps1
tools/qemu/qemu-system-x86_64.exe -drive format=raw,file=build/aurora.iso -m 1G -smp 4
```

The ISO boots the kernel and desktop by itself. Aurora’s development volume is
accessed through ATA PIO, so attach the development disk as a second drive when
you need ext2/FAT32 storage, GCC, or networking tools:

```powershell
tools/qemu/qemu-system-x86_64.exe `
  -drive format=raw,file=build/aurora.iso `
  -drive format=raw,file=build/development.img,if=none,id=development `
  -device virtio-blk-pci,drive=development,disable-modern=on -m 1G -smp 4
```
