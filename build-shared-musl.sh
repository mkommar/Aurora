#!/usr/bin/bash
# Run inside Aurora on a candidate development disk.
set -eu
cd /src
printf '%s\n' '9b969322012d796dc23dda27a35866034fa67d8fb67e0e2c45c913c3d43219dd  musl-1.2.2.tar.gz' | sha256sum -c -
printf '%s\n' '87d68678b0996c9fb19aa736c58607d4a43d3bb562c7fc7f4064f0d5b9f1e2d4  /work/musl-Fork-fixed.c' | sha256sum -c -
if test "${1:-}" != resume; then tar -xzf musl-1.2.2.tar.gz; fi
cd musl-1.2.2
cp /work/musl-Fork-fixed.c src/process/_Fork.c
if test "${1:-}" != resume; then
    CC=gcc CFLAGS=-Os /usr/bin/bash ./configure --prefix=/usr --syslibdir=/lib --disable-static --disable-wrapper
fi
test -s config.mak
make --jobserver-style=pipe -j2 lib/libc.so
cp lib/libc.so /lib/libc.so.new
chmod 755 /lib/libc.so.new
mv /lib/libc.so.new /lib/libc.so
ln -sf libc.so /lib/ld-musl-x86_64.so.1
sync
echo AURORA_SHARED_MUSL_BUILT_FROM_PINNED_SOURCE
