#!/bin/bash
# Install the first candidate .deb into /opt/aurora without overwriting files.
set -euo pipefail
umask 022
archive=${1:?package archive required}
checksum=${2:?package SHA-256 sidecar required}
root=${3:-/}
root=$(cd "$root" && pwd);root=${root%/};test -n "$root" || root=/
test -f "$archive" && test -f "$checksum"
read -r expected _ < "$checksum"
case "$expected" in *[!a-f0-9]*|'') echo 'Invalid package checksum sidecar' >&2;exit 2;;esac
test "${#expected}" -eq 64
actual=$(sha256sum "$archive");actual=${actual%% *}
test "$actual" = "$expected" || { echo 'Package archive checksum mismatch' >&2;exit 1; }

members=$(ar t "$archive")
test "$(printf '%s\n' "$members" | sort | tr '\n' ' ')" = 'control.tar.gz data.tar.gz debian-binary ' || {
    echo 'Unexpected Debian archive members' >&2;exit 1;
}
test "$(ar p "$archive" debian-binary)" = '2.0'

tmp=$(mktemp -d "${TMPDIR:-/tmp}/aurora-package-install.XXXXXX")
installed=();created_dirs=();committed=0;record_tmp=;record=
rollback(){
    rc=$?
    if test "$committed" -ne 1; then
        for ((i=${#installed[@]}-1;i>=0;i--));do rm -f -- "${installed[i]}";done
        test -z "$record_tmp" || rm -f -- "$record_tmp"
        test -z "$record" || rm -f -- "$record"
        for ((i=${#created_dirs[@]}-1;i>=0;i--));do rmdir -- "${created_dirs[i]}" 2>/dev/null || true;done
    fi
    rm -rf -- "$tmp"
    exit "$rc"
}
trap rollback EXIT
mkdir "$tmp/control" "$tmp/data"
ar p "$archive" control.tar.gz > "$tmp/control.tar.gz"
ar p "$archive" data.tar.gz > "$tmp/data.tar.gz"
while IFS= read -r listing;do
    case "${listing:0:1}" in -|d);; *) echo 'Unsupported package control entry type' >&2;exit 2;;esac
done < <(tar -tvzf "$tmp/control.tar.gz")
while IFS= read -r entry;do
    entry=${entry#./};test -z "$entry" && continue
    case "$entry" in .|control|files|sha256sums|md5sums);;
        *) echo "Unexpected package control path: $entry" >&2;exit 2;;esac
done < <(tar -tzf "$tmp/control.tar.gz")
tar --no-same-owner -xzf "$tmp/control.tar.gz" -C "$tmp/control"

control=$tmp/control/control
test -f "$control" && test -f "$tmp/control/files" && test -f "$tmp/control/sha256sums"
package=$(sed -n 's/^Package: //p' "$control")
version=$(sed -n 's/^Version: //p' "$control")
test "$package" = aurora-make || { echo 'This initial installer accepts only aurora-make' >&2;exit 2; }
test -n "$version" && grep -qx 'Architecture: musl-linux-amd64' "$control"
case "$version" in *[!a-zA-Z0-9.+:~-]*|'') echo 'Invalid package version' >&2;exit 2;;esac
files=();archive_files=()
while IFS= read -r listing;do
    case "${listing:0:1}" in -|d);; *) echo 'Unsupported package data entry type' >&2;exit 2;;esac
done < <(tar -tvzf "$tmp/data.tar.gz")
while IFS= read -r entry;do
    entry=${entry#./};test -z "$entry" && continue
    case "$entry" in /*|../*|*/../*|*/..|*//* ) echo "Unsafe package path: $entry" >&2;exit 2;;esac
    if [[ "$entry" != */ ]];then
        case "$entry" in opt/aurora/*) archive_files+=("$entry");;
            *) echo "Package file escapes /opt/aurora: $entry" >&2;exit 2;;esac
    else
        entry=${entry%/}
        case "$entry" in opt|opt/aurora|opt/aurora/*);;
            *) echo "Package directory escapes /opt/aurora: $entry" >&2;exit 2;;esac
    fi
done < <(tar -tzf "$tmp/data.tar.gz")
while IFS= read -r entry;do
    test -n "$entry" || continue
    case "$entry" in opt/aurora/*) files+=("$entry");;
        *) echo "Invalid package file manifest entry: $entry" >&2;exit 2;;esac
done < "$tmp/control/files"
test "$(printf '%s\n' "${archive_files[@]}" | sort)" = "$(printf '%s\n' "${files[@]}" | sort)" || {
    echo 'Package archive and DEBIAN/files differ' >&2;exit 1;
}
tar --no-same-owner -xzf "$tmp/data.tar.gz" -C "$tmp/data"
(cd "$tmp/data" && sha256sum -c "$tmp/control/sha256sums")

for file in "${files[@]}";do
    test -f "$tmp/data/$file" || { echo "Package file missing: $file" >&2;exit 1; }
    target=${root%/}/$file;test -n "${root%/}" || target=/$file
    if test -e "$target" || test -L "$target";then echo "Refusing to overwrite $target" >&2;exit 1;fi
done
record=${root%/}/var/lib/aurora/packages/${package}_${version}.files;test -n "${root%/}" || record=/var/lib/aurora/packages/${package}_${version}.files
if test -e "$record";then echo "Package is already installed: $package $version" >&2;exit 1;fi
ensure_dir(){
    local path=$1 parent
    if test -d "$path";then return 0;fi
    if test -e "$path" || test -L "$path";then echo "Not a directory: $path" >&2;return 1;fi
    parent=${path%/*};test -n "$parent" || parent=/
    if test "$parent" != "$path";then ensure_dir "$parent";fi
    mkdir "$path";created_dirs+=("$path")
}
for file in "${files[@]}";do
    parent=${file%/*};target=${root%/}/$parent;test -n "${root%/}" || target=/$parent
    ensure_dir "$target"
done
ensure_dir "${record%/*}"
record_tmp=$record.partial.$$
printf '%s\n' "${files[@]}" > "$record_tmp"
for file in "${files[@]}";do
    target=${root%/}/$file;test -n "${root%/}" || target=/$file
    mv -- "$tmp/data/$file" "$target";installed+=("$target")
done
mv -- "$record_tmp" "$record";record_tmp=
committed=1
echo "AURORA_PACKAGE_INSTALLED $package $version"
