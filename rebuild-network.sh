#!/bin/sh
set -eu
export PATH=/usr/local/bin:/usr/bin:/bin
mkdir -p /work/network-build
cd /work/network-build
tar -xzf /src/network-build-tree.tar.gz
cd mbedtls-3.6.7
make -j2 GEN_FILES= CC=gcc AR=ar CFLAGS=-Os lib
cp library/libmbed*.a /lib/
cd ../curl-8.22.0
make -j2
cp src/curl /bin/curl
cp lib/.libs/libcurl.a /lib/libcurl.a
chmod 755 /bin/curl
curl --version
curl -fsS --max-time 30 https://example.com/ -o /work/rebuilt-curl.html
grep 'Example Domain' /work/rebuilt-curl.html
sync
echo AURORA_NETWORK_REBUILT_INSIDE_AURORA
