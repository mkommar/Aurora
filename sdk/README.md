# Aurora C application SDK

Build on Windows with the same NASM/LLVM tools used by the OS. Programs run
inside Aurora using its original application ABI. There is also a separate
[native GCC environment](../NATIVE-GCC.md) for compilation inside the guest.

```c
#include <stdio.h>

int main(int argc, char **argv) {
    puts("Hello from my application!");
    if (argc > 1) printf("Argument: %s\n", argv[1]);
    return 0;
}
```

Save as `apps/myapp.c`. Stop QEMU, then compile and install it without rebuilding
the kernel:

```powershell
.\build-app.ps1 -Source apps/myapp.c -Name myapp -Image build/aurora.img
.\run.ps1 -NoBuild
```

In Aurora, press F2 and type `myapp world` or `run myapp world`. The shell displays
the exit status. One application can be launched at a time through this shell;
desktop/input/display services continue running. `ls` lists files and `cat FILE`
prints a text file. Names are case-sensitive. The shell accepts up to 70 characters;
the runtime tokenizes arguments on spaces, with at most 16 arguments, including
the program name. Quoting, pipes, interactive stdin, and redirection are not
implemented.

`build-app.ps1` accepts `-LlvmBin` and `-OutputDirectory`. Without `-Image`, it only
builds an ELF. Install arbitrary files with:

```powershell
.\pack-files.ps1 -Image build/aurora.img -Files @{'example.txt'='path/to/example.txt'}
```

The packer opens the disk exclusively and refuses disks currently used by QEMU.
Ordinary OS builds preserve the filesystem and refresh the bundled `hello`,
`calc`, `filedemo`, and `readme.txt` files. Avoid using those names for your own
files. Back up the image before experiments that modify valuable data.

## Runtime subset

This is a small freestanding C runtime, not a complete ISO C/POSIX library.
Headers `stdio.h`, `stdlib.h`, `string.h`, and `stddef.h` expose the supported
declarations; `aurora.h` additionally documents file operations.

| API | Support |
| --- | --- |
| `main`, `exit` | `argc`/`argv`, integer exit status; returning from main exits |
| `puts`, `putchar`, `printf` | Terminal and serial output; `%s`, `%c`, `%d`, `%u`, `%x`, `%%`; no widths, precision, length modifiers or floating point |
| `strlen`, `strcmp`, `atoi` | Basic strings and decimal integer conversion |
| `memcpy`, `memset` | Memory operations; memcpy requires non-overlapping regions |
| `malloc`, `free` | 128 KiB per-process heap, 16-byte alignment, reuse and coalescing; allocation failure returns NULL |
| `aurora_readfile` | Read from the beginning, up to buffer capacity; returns bytes copied or a negative error; does not append a NUL |
| `aurora_writefile` | Create/replace an entire file; return bytes written or a negative error; zero length creates/truncates an empty file |

Files are limited to 64 KiB each and the flat directory holds 32 files. Names
contain 1–31 ASCII letters, digits, dots, underscores or hyphens. There are no
directories, file descriptors, delete/rename, permissions or transactional writes.
An overwrite can leave partial data after power loss or an I/O error. Files are
shared by all applications; process memory protection does not isolate files.

## Executable contract

Static x86-64 little-endian ELF64 `ET_EXEC`, entry inside executable file-backed
bytes, at most 16 program headers. Loadable segments must have 4 KiB alignment,
page-aligned virtual addresses/file offsets, disjoint pages, and fit between
`0x400000` and `0x5c0000`. Segments must be readable and cannot be writable and
executable simultaneously. The loader rejects malformed sizes, offsets, overlap,
dynamic linking, interpreters and TLS. The linker script enforces the memory
ceiling. Executable files themselves must fit the filesystem's 64 KiB limit.

There is a 64 KiB stack with a guard page. Unused application pages are unmapped;
ELF BSS is zero-initialized. The supplied SDK build retains `-mgeneral-regs-only`;
the kernel now also saves x87/SSE state for the native GCC environment. This SDK
loader accepts Aurora ABI applications; selected static Linux-ABI binaries use
the separate compatibility loader described in the native GCC guide.

## Verification

After `.\build.ps1`, build the test-only applications and run the isolated test
suite. Python and Pillow are needed; pass the path to `llvm-nm.exe`:

```powershell
.\build-app.ps1 -Source tests/appcheck.c -Name appcheck -OutputDirectory build/tests
.\build-app.ps1 -Source tests/crash.c -Name crash -OutputDirectory build/tests
python test-applications.py --nm 'C:\path\to\llvm\bin\llvm-nm.exe'
```

The test makes its own disk copy under `build/application-tests`, uses QMP port
4445, and closes its hidden QEMU instance. It verifies runtime behavior, boundary
file I/O, invalid pointers, malformed ELF rejection, application crash containment,
slot reuse, terminal output, and persistence across a full VM restart.
