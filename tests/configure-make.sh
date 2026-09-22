#!/bin/bash
set -eu
trap 'rc=$?; echo "AURORA_CONFIGURE_EXIT=$rc PATH=$PATH"; /usr/bin/sync; exit "$rc"' EXIT
export PATH=/usr/bin:/bin:/usr/local/bin CONFIG_SHELL=/usr/bin/bash SHELL=/usr/bin/bash
export CC=gcc CFLAGS=-O1 LDFLAGS=-static
mkdir -p /work/native-configure /work/build-logs
cd /work/native-configure
if test ! -d make-4.4.1; then tar xzf /src/make-4.4.1.tar.gz; fi
mkdir -p make-build
cd make-build
echo AURORA_CONFIGURE_START
bash ../make-4.4.1/configure --build=x86_64-linux-musl --host=x86_64-linux-musl \
    --prefix=/opt/aurora/make --disable-nls --disable-load --without-guile \
    > /work/build-logs/make-configure.log 2>&1 || { tail -80 /work/build-logs/make-configure.log; exit 1; }
echo AURORA_CONFIGURE_PASS
make -j1 > /work/build-logs/make-build.log 2>&1 || { tail -80 /work/build-logs/make-build.log; exit 1; }
./make --version
mkdir -p /work/make-stage
make DESTDIR=/work/make-stage install > /work/build-logs/make-install.log 2>&1
echo AURORA_NATIVE_MAKE_CONFIGURED_BUILT_STAGED
