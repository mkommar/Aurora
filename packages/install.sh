#!/bin/bash
# Install, query, or remove a verified Aurora .deb without dpkg/APT semantics.
set -euo pipefail
umask 022
usage(){ echo "usage: $0 [--root ROOT] [--checksum FILE] ARCHIVE" >&2; echo "       $0 [--root ROOT] --url HTTPS_URL --sha256 HASH" >&2; echo "       $0 [--root ROOT] --owner PATH|--list PACKAGE|--remove PACKAGE" >&2; exit 2; }
root=/; checksum=; url=; expected_hash=; mode=install; option_arg=; positional=()
while test "$#" -gt 0; do
    case "$1" in
        --root) test "$#" -ge 2 || usage; root=$2; shift 2;;
        --checksum) test "$#" -ge 2 || usage; checksum=$2; shift 2;;
        --url) test "$#" -ge 2 || usage; url=$2; shift 2;;
        --sha256) test "$#" -ge 2 || usage; expected_hash=$2; shift 2;;
        --owner|--list|--remove) test "$#" -ge 2 || usage; mode=${1#--}; option_arg=$2; shift 2;;
        --) shift; while test "$#" -gt 0; do positional+=("$1"); shift; done;;
        -*) usage;;
        *) positional+=("$1"); shift;;
    esac
done
if test "$mode" = install; then
    test -z "$url" && test "${#positional[@]}" -ge 1 || test -n "$url" || usage
    test "${#positional[@]}" -le 3 || usage
    archive=${positional[0]:-}
    test -n "$checksum" || test "${#positional[@]}" -lt 2 || checksum=${positional[1]}
    test "$root" != / || test "${#positional[@]}" -lt 3 || root=${positional[2]}
else
    test "${#positional[@]}" -eq 0 || usage
fi
case "$root" in /*) ;; *) echo 'Install root must be absolute' >&2; exit 2;; esac
state=$root/var/lib/aurora/packages

if test "$mode" = owner; then
    case "$option_arg" in /*) ;; *) echo 'Owner query must be absolute' >&2; exit 2;; esac
    found=0
    if test -d "$state"; then
        for record in "$state"/*.files; do
            test -f "$record" || continue
            if grep -qxF "${option_arg#/}" "$record"; then basename "$record" .files; found=1; fi
        done
    fi
    test "$found" -eq 1
    exit 0
fi
if test "$mode" = list; then
    test -f "$state/$option_arg.current" || { echo "Package is not installed: $option_arg" >&2; exit 1; }
    read -r installed_version < "$state/$option_arg.current"
    record=$state/${option_arg}_${installed_version}.files
    test -f "$record" || { echo "Package is not installed: $option_arg" >&2; exit 1; }
    cat "$record"; exit 0
fi
if test "$mode" = remove; then
    case "$option_arg" in aurora-[a-z0-9+.-]*) ;; *) echo 'Invalid Aurora package name' >&2; exit 2;; esac
    test -f "$state/$option_arg.current" || { echo "Package is not installed: $option_arg" >&2; exit 1; }
    read -r installed_version < "$state/$option_arg.current"
    record=$state/${option_arg}_${installed_version}.files; meta=$state/${option_arg}_${installed_version}.meta
    test -f "$record" && test -f "$meta" || { echo "Package is not installed: $option_arg" >&2; exit 1; }
    while IFS=' ' read -r digest file; do
        test -n "$file" || continue
        target=$root/$file
        test -f "$target" && printf '%s  %s\n' "$digest" "$target" | sha256sum -c - >/dev/null || { echo "Refusing to remove modified or missing file: /$file" >&2; exit 1; }
    done < "$meta"
    while IFS=' ' read -r digest file; do test -n "$file" || continue; rm -- "$root/$file"; done < "$meta"
    rm -- "$record" "$meta" "$state/$option_arg.current"; echo "AURORA_PACKAGE_REMOVED $option_arg"; exit 0
fi

download_archive=
if test -n "$url"; then
    case "$url" in https://*) ;; *) echo 'Package URL must use HTTPS' >&2; exit 2;; esac
    case "$expected_hash" in [a-f0-9][a-f0-9][a-f0-9][a-f0-9]* ) test "${#expected_hash}" -eq 64 || usage;; *) echo 'Download mode requires a SHA-256 hash' >&2; exit 2;; esac
    download_archive=$(mktemp "${TMPDIR:-/tmp}/aurora-package-download.XXXXXX")
    curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 --output "$download_archive" "$url"
    archive=$download_archive
    trap 'rm -f -- "$download_archive"' EXIT
fi
test -f "$archive" || { echo 'Package archive is missing' >&2; exit 2; }
if test -n "$expected_hash"; then printf '%s  %s\n' "$expected_hash" "$archive" | sha256sum -c - >/dev/null || { echo 'Package archive checksum mismatch' >&2; exit 1; }
fi
if test -n "$checksum"; then
    read -r expected _ < "$checksum"
    case "$expected" in *[!a-f0-9]*|'') echo 'Invalid package checksum sidecar' >&2; exit 2;; esac
    test "${#expected}" -eq 64 || { echo 'Invalid package checksum sidecar' >&2; exit 2; }
    actual=$(sha256sum "$archive"); actual=${actual%% *}
    test "$actual" = "$expected" || { echo 'Package archive checksum mismatch' >&2; exit 1; }
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/aurora-package-install.XXXXXX")
installed=(); created_dirs=(); record_tmp=; meta_tmp=; committed=0
rollback(){
    rc=$?
    if test "$committed" -ne 1; then
        for ((i=${#installed[@]}-1;i>=0;i--)); do rm -f -- "${installed[i]}"; done
        test -z "$record_tmp" || rm -f -- "$record_tmp"
        test -z "$meta_tmp" || rm -f -- "$meta_tmp"
        for ((i=${#created_dirs[@]}-1;i>=0;i--)); do rmdir -- "${created_dirs[i]}" 2>/dev/null || true; done
    fi
    rm -rf -- "$tmp"
    test -z "$download_archive" || rm -f -- "$download_archive"
    exit "$rc"
}
trap rollback EXIT
mkdir "$tmp/control" "$tmp/data"
ar p "$archive" debian-binary > "$tmp/debian-binary"
ar p "$archive" control.tar.gz > "$tmp/control.tar.gz"
ar p "$archive" data.tar.gz > "$tmp/data.tar.gz"
ar t "$archive" > "$tmp/members"
test "$(sort "$tmp/members" | tr '\n' ' ')" = 'control.tar.gz data.tar.gz debian-binary ' || { echo 'Unexpected Debian archive members' >&2; exit 2; }
test "$(cat "$tmp/debian-binary")" = '2.0' || { echo 'Unsupported Debian archive version' >&2; exit 2; }
validate_tar(){
    tar -tvzf "$1" > "$2"
    while IFS= read -r line; do case "${line:0:1}" in -|d) ;; *) echo 'Unsupported package archive member type' >&2; exit 2;; esac; done < "$2"
}
validate_tar "$tmp/control.tar.gz" "$tmp/control.list"
validate_tar "$tmp/data.tar.gz" "$tmp/data.list"
tar -tzf "$tmp/control.tar.gz" > "$tmp/control.paths"
while IFS= read -r entry; do
    entry=${entry#./}
    case "$entry" in ''|.|control|files|sha256sums|md5sums) ;; *) echo "Unexpected package control path: $entry" >&2; exit 2;; esac
done < "$tmp/control.paths"
tar -tzf "$tmp/data.tar.gz" > "$tmp/data.paths"
while IFS= read -r entry; do
    entry=${entry#./}
    case "$entry" in ''|opt|opt/|opt/aurora|opt/aurora/|opt/aurora/*) ;; /*|../*|*/../*|*/..|*//* ) echo "Unsafe package path: $entry" >&2; exit 2;; *) echo "Package path escapes /opt/aurora: $entry" >&2; exit 2;; esac
done < "$tmp/data.paths"
tar --no-same-owner -xzf "$tmp/control.tar.gz" -C "$tmp/control"
tar --no-same-owner -xzf "$tmp/data.tar.gz" -C "$tmp/data"
control=$tmp/control/control; test -f "$control" && test -f "$tmp/control/files" && test -f "$tmp/control/sha256sums"
for field in Package Version Architecture; do test "$(grep -c "^${field}: " "$control")" -eq 1 || { echo "Invalid package metadata: $field" >&2; exit 2; }; done
package=$(awk -F': ' '$1=="Package"{print $2}' "$control")
version=$(awk -F': ' '$1=="Version"{print $2}' "$control")
architecture=$(awk -F': ' '$1=="Architecture"{print $2}' "$control")
case "$package" in aurora-[a-z0-9+.-]*) ;; *) echo 'Invalid Aurora package identity' >&2; exit 2;; esac
case "$version" in *[!a-zA-Z0-9.+:~-]*|'') echo 'Invalid package version' >&2; exit 2;; esac
test "$architecture" = musl-linux-amd64 || { echo 'Unsupported package architecture' >&2; exit 2; }
mapfile -t files < "$tmp/control/files"; mapfile -t archive_files < "$tmp/data.paths"
for i in "${!archive_files[@]}"; do archive_files[i]=${archive_files[i]#./}; done
archive_files=($(printf '%s\n' "${archive_files[@]}" | awk 'substr($0,length($0),1)!="/"'))
test "$(printf '%s\n' "${archive_files[@]}" | sort)" = "$(printf '%s\n' "${files[@]}" | sort)" || { echo 'Package archive and DEBIAN/files differ' >&2; exit 1; }
while IFS=' ' read -r digest file; do
    case "$digest" in [a-f0-9][a-f0-9][a-f0-9][a-f0-9]*) test "${#digest}" -eq 64 || { echo 'Invalid package file checksum' >&2; exit 2; };; *) echo 'Invalid package file checksum' >&2; exit 2;; esac
    case "$file" in opt/aurora/*) ;; *) echo 'Invalid package checksum path' >&2; exit 2;; esac
    grep -qxF "$file" "$tmp/control/files" || { echo "Checksum path is not packaged: $file" >&2; exit 2; }
done < "$tmp/control/sha256sums"
(cd "$tmp/data" && sha256sum -c "$tmp/control/sha256sums")
record=$state/${package}_${version}.files; meta=$state/${package}_${version}.meta
test ! -e "$state/$package.current" || { echo "Refusing to overwrite installed package: $package $version" >&2; exit 1; }
for file in "${files[@]}"; do
    test -n "$file" || continue; target=$root/$file
    if test -e "$target" || test -L "$target"; then echo "Refusing to overwrite $target" >&2; exit 1; fi
done
ensure_dir(){
    local path=$1 parent
    if test -d "$path"; then return 0; fi
    test ! -e "$path" && test ! -L "$path" || { echo "Not a directory: $path" >&2; return 1; }
    parent=${path%/*}; test "$parent" = "$path" || ensure_dir "$parent"
    if test -d "$path"; then return 0; fi
    mkdir "$path"; created_dirs+=("$path")
}
for file in "${files[@]}"; do test -n "$file" || continue; ensure_dir "$root/${file%/*}"; done
ensure_dir "$state"
record_tmp=$(mktemp "$state/.files.XXXXXX"); meta_tmp=$(mktemp "$state/.meta.XXXXXX")
printf '%s\n' "${files[@]}" | sed '/^$/d' > "$record_tmp"
awk '/^[0-9a-f]{64}  /{print}' "$tmp/control/sha256sums" > "$meta_tmp"
for file in "${files[@]}"; do test -n "$file" || continue; target=$root/$file; mv -- "$tmp/data/$file" "$target"; installed+=("$target"); done
mv -- "$record_tmp" "$record"; record_tmp=
mv -- "$meta_tmp" "$meta"; meta_tmp=
printf '%s\n' "$version" > "$state/$package.current"
committed=1; echo "AURORA_PACKAGE_INSTALLED $package $version"
