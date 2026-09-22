#!/bin/bash
# Create a flat APT repository from Aurora-built .deb files, inside Aurora.
# The caller publishes this immutable directory and authenticates Release.
set -euo pipefail
export LC_ALL=C TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1720000000}
destination=${1:?new repository directory required}; shift
test "$#" -gt 0
test ! -e "$destination"
mkdir -p "$destination/pool"
for archive in "$@"; do
    name=${archive##*/}
    case "$name" in aurora-*_musl-linux-amd64.deb) ;; *) echo "Invalid package name: $name" >&2; exit 2;; esac
    case "$name" in *[!a-zA-Z0-9.+_~-]*) echo 'Unsafe package filename' >&2; exit 2;; esac
    test ! -e "$destination/pool/$name"
    cp "$archive" "$destination/pool/$name"
done
cd "$destination"
for archive in pool/*.deb; do
    test "$(ar p "$archive" debian-binary)" = 2.0
    ar p "$archive" control.tar.gz | gzip -dc | tar -xOf - ./control
    printf 'Filename: %s\nSize: %s\nSHA256: %s\n\n' "$archive" \
        "$(wc -c < "$archive" | tr -d ' ')" "$(sha256sum "$archive" | cut -d ' ' -f 1)"
done > Packages
gzip -nc Packages > Packages.gz
{
    printf 'Origin: Aurora\nLabel: Aurora native packages\nSuite: native\nArchitectures: musl-linux-amd64\n'
    printf 'Date: %s\nDescription: Native Aurora candidate packages\nSHA256:\n' "$(date -Ru -d "@$SOURCE_DATE_EPOCH")"
    for index in Packages Packages.gz; do
        printf ' %s %s %s\n' "$(sha256sum "$index" | cut -d ' ' -f 1)" \
            "$(wc -c < "$index" | tr -d ' ')" "$index"
    done
} > Release
sha256sum Release > Release.sha256
sync
echo AURORA_APT_REPOSITORY_CREATED
echo 'Release is unsigned. Authenticate its hash or sign it before distribution.'
