#!/bin/bash
set -euo pipefail
archive=/work/packages/out/aurora-zlib_1.3.1-1_musl-linux-amd64.deb
for name in one two; do
    bash /work/packages/repository.sh "/work/repository-$name" "$archive"
done
for file in Packages Packages.gz Release; do
    first=$(sha256sum "/work/repository-one/$file"); second=$(sha256sum "/work/repository-two/$file")
    test "${first%% *}" = "${second%% *}"
done
grep -q '^Package: aurora-zlib$' /work/repository-one/Packages
grep -q '^Architecture: musl-linux-amd64$' /work/repository-one/Packages
grep -q '^Filename: pool/aurora-zlib_1.3.1-1_musl-linux-amd64.deb$' /work/repository-one/Packages
mkdir /work/package-inspection
cd /work/package-inspection
ar p "$archive" control.tar.gz | gzip -dc | tar xf -
ar p "$archive" data.tar.gz | gzip -dc | tar xf -
sha256sum -c sha256sums
sync
echo AURORA_DEB_AND_REPOSITORY_PASS
