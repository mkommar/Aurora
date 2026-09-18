#!/bin/sh
set -eu
apk add --no-cache build-base python3 perl bzip2 curl
mkdir -p /build /opt /netroot/bin /netroot/lib /netroot/include /netroot/src /netroot/etc/ssl
cd /build
for f in native.tgz mbedtls-3.6.7.tar.bz2 curl-8.22.0.tar.gz cacert.pem sources.sha256; do
    wget -q -O "$f" "http://10.0.2.2:8879/$f"
done
sha256sum -c sources.sha256
tar -xzf native.tgz -C /opt
export PATH=/opt/x86_64-linux-musl-native/bin:$PATH
export CC=gcc AR=ar RANLIB=ranlib CFLAGS=-Os LDFLAGS=-static
tar -xjf mbedtls-3.6.7.tar.bz2
cd mbedtls-3.6.7
make -j2 GEN_FILES= lib
cp library/libmbed*.a /netroot/lib/
cp -R include/mbedtls include/psa /netroot/include/
cd /build
tar -xzf curl-8.22.0.tar.gz
cd curl-8.22.0
./configure --prefix=/usr --disable-shared --enable-static --with-mbedtls=/netroot \
    --disable-threaded-resolver --disable-ipv6 --disable-unix-sockets --disable-ldap --disable-ldaps \
    --disable-ftp --disable-file --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smtp --disable-smb --disable-gopher --disable-mqtt \
    --without-zlib --without-brotli --without-zstd --without-libpsl --without-libidn2 \
    --without-librtmp --without-nghttp2 --without-libssh2 --with-ca-bundle=/etc/ssl/cert.pem \
    --with-ca-path=no
make -j2
cp src/curl /netroot/bin/
cp lib/.libs/libcurl.a /netroot/lib/
cp -R include/curl /netroot/include/
cp /build/cacert.pem /netroot/etc/ssl/cert.pem
cp /build/mbedtls-3.6.7.tar.bz2 /build/curl-8.22.0.tar.gz /netroot/src/
tar -czf /build/network-bootstrap.tar.gz -C /netroot .
base64 /build/network-bootstrap.tar.gz > /build/result.b64
/usr/bin/curl -fsS --data-binary @/build/result.b64 http://10.0.2.2:8879/result
echo AURORA_NETWORK_BOOTSTRAP_COMPLETE
