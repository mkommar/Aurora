#!/usr/bin/bash
# Compile the small musl _Fork backport inside Aurora, preserving the old archive.
set -eu
cd /src
printf '%s\n' '9b969322012d796dc23dda27a35866034fa67d8fb67e0e2c45c913c3d43219dd  musl-1.2.2.tar.gz' | sha256sum -c -
tar -xzf musl-1.2.2.tar.gz musl-1.2.2/COPYRIGHT musl-1.2.2/arch/x86_64 musl-1.2.2/arch/generic musl-1.2.2/src/internal musl-1.2.2/src/include musl-1.2.2/include musl-1.2.2/src/process/_Fork.c
cd musl-1.2.2
cp /work/musl-Fork-fixed.c src/process/_Fork.c
gcc -O2 -std=c11 -ffreestanding -fPIC -fno-stack-protector -D_XOPEN_SOURCE=700 -Iarch/x86_64 -Iarch/generic -Isrc/include -Isrc/internal -Iinclude -include /work/musl-fork-abi.h -c src/process/_Fork.c -o _Fork.lo
if test ! -f /lib/libc.a.before-thread-fix; then cp /lib/libc.a /lib/libc.a.before-thread-fix; fi
printf '%s\n' '3372ecd3cf1717fd1a78a447c61793b82ebc87a42910c990341c4059b9f29e23  /lib/libc.a.before-thread-fix' | sha256sum -c -
cp /lib/libc.a.before-thread-fix /lib/libc.a.thread-fix
ar r /lib/libc.a.thread-fix _Fork.lo
ranlib /lib/libc.a.thread-fix
mv /lib/libc.a.thread-fix /lib/libc.a
sync
echo AURORA_MUSL_FORK_BACKPORT_INSTALLED
