#!/bin/sh
set -eu
cd /work/network-build/curl-8.22.0/lib
rm -f .libs/libcurl.a .libs/libcurl.la .libs/libcurl.lai
find . -type f -name 'libcurl_la-*.o' -print | while IFS= read -r object; do
    ar r .libs/libcurl.a "$object"
done
if [ ! -f .libs/libcurl.a ]; then
    echo "archive-libcurl: no libcurl objects" >&2
    exit 1
fi
ranlib .libs/libcurl.a
cat > .libs/libcurl.la <<'EOF'
# libcurl.la - a libtool library file
dlname=''
library_names=''
old_library='libcurl.a'
dependency_libs=''
current=0
age=0
revision=0
installed=no
shouldnotlink=no
dlopen=''
dlpreopen=''
libdir='/usr/lib'
EOF
cp .libs/libcurl.la libcurl.la
