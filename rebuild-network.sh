#!/bin/sh
set -eu
export PATH=/usr/local/bin:/usr/bin:/bin
mkdir -p /work/network-build
cd /work/network-build
tar -xzf /src/network-build-tree.tar.gz
cd mbedtls-3.6.7
if [ ! -f library/libmbedcrypto.a ] || [ ! -f library/libmbedtls.a ] || [ ! -f library/libmbedx509.a ]; then
    make -j2 GEN_FILES= CC=gcc AR=ar CFLAGS=-Os lib
fi
cp library/libmbed*.a /lib/
cd ../curl-8.22.0
# The source tree may have been prepared by the pinned bootstrap.  Keep the
# archive recipe deterministic even when resuming a disposable native build.
sed -i '/libcurl_la_LINK.*libcurl_la_OBJECTS/c\	@/work/archive-libcurl.sh' lib/Makefile
# Build only the production libcurl and command-line targets.  The default
# aggregate target also builds curl's optional libcurlu helper library, which
# is not needed by Aurora and doubles the native compile time.
objects=$(find lib -type f -name 'libcurl_la-*.o' | wc -l)
if [ "$objects" -lt 50 ]; then
    make -C lib -j2 libcurl.la
else
    /work/archive-libcurl.sh
fi
make -C src -j2 curl
cp src/curl /bin/curl
cp lib/.libs/libcurl.a /lib/libcurl.a
chmod 755 /bin/curl
sha256sum /bin/curl /lib/libcurl.a /lib/libmbedcrypto.a /lib/libmbedtls.a /lib/libmbedx509.a
curl --version
curl -fsS --max-time 30 https://example.com/ -o /work/rebuilt-curl.html
grep 'Example Domain' /work/rebuilt-curl.html
sync
echo AURORA_NETWORK_REBUILT_INSIDE_AURORA
