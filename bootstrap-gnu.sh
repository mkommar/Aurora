#!/bin/sh
# Runs only in the disposable Linux builder, never in the Aurora kernel.
set -eu
apk add --no-cache build-base linux-headers bash perl xz bison flex
mkdir -p /build /stage/bin /stage/share/licenses
mount -t tmpfs -o size=640m tmpfs /build
mount -t tmpfs -o size=256m tmpfs /stage
mkdir -p /stage/bin /stage/share/licenses /stage/share/aurora
if wget -q http://10.0.2.2:8879/gnu-bootstrap.tar.gz -O /build/checkpoint.tar.gz; then
    tar xzf /build/checkpoint.tar.gz -C /stage
fi
apk info -v > /stage/share/builder-packages.txt
cd /build
export CFLAGS='-O2 -fno-pie -std=gnu17' LDFLAGS='-static -no-pie' FORCE_UNSAFE_CONFIGURE=1
build() {
    package=$1; archive=$2; shift 2
    if test -f /stage/share/bootstrap-packages.txt && grep -qx "$package" /stage/share/bootstrap-packages.txt && { test "$package" != make-4.4.1 || test -f /stage/share/aurora/make-configured.tar.gz; }; then
        echo "AURORA BOOTSTRAP REUSE $package"
        return
    fi
    echo "AURORA BOOTSTRAP BUILD $package"
    wget -q "http://10.0.2.2:8879/$archive" -O "$archive"
    tar xf "$archive"
    cd "$package"
    ./configure --prefix=/usr --disable-nls "$@" > /build/configure.log 2>&1 || { cat /build/configure.log; return 1; }
    make -j2 > /build/make.log 2>&1 || { tail -100 /build/make.log; return 1; }
    make DESTDIR=/stage install > /build/install.log 2>&1 || { tail -100 /build/install.log; return 1; }
    cp COPYING "/stage/share/licenses/$package-COPYING"
    if test "$package" = make-4.4.1; then
        make clean > /build/clean.log 2>&1
        tar czf /stage/share/aurora/make-configured.tar.gz -C /build make-4.4.1
    fi
    echo "$package" >> /stage/share/bootstrap-packages.txt
    cd /build
    # Archive is retained by the host; free disposable guest memory after build.
    rm -rf "$package" "$archive"
    tar czf /build/gnu-bootstrap.tar.gz -C /stage .
    base64 /build/gnu-bootstrap.tar.gz > /build/result.base64
    wget -q --post-file=/build/result.base64 http://10.0.2.2:8879/result -O /dev/null
}
build bash-5.2.37 bash-5.2.37.tar.gz --without-bash-malloc --disable-readline
build make-4.4.1 make-4.4.1.tar.gz --disable-load --without-guile
build coreutils-9.5 coreutils-9.5.tar.xz --without-gmp --without-libcap --without-selinux --disable-libsmack --enable-no-install-program=stdbuf
build sed-4.9 sed-4.9.tar.xz --disable-acl
build grep-3.11 grep-3.11.tar.xz --disable-perl-regexp
build gawk-5.3.1 gawk-5.3.1.tar.xz --disable-extensions --without-mpfr --without-gmp
build findutils-4.10.0 findutils-4.10.0.tar.xz
build tar-1.35 tar-1.35.tar.xz
build gzip-1.14 gzip-1.14.tar.xz
tar czf /build/gnu-bootstrap.tar.gz -C /stage .
base64 /build/gnu-bootstrap.tar.gz > /build/result.base64
wget -q --post-file=/build/result.base64 http://10.0.2.2:8879/result -O /dev/null
echo 'AURORA GNU BOOTSTRAP COMPLETE'
