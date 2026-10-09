#!/bin/bash
# Install, query, or remove bounded Aurora .deb packages.
# This is a filesystem transaction tool, not dpkg or APT.
set -euo pipefail
umask 022

usage() {
    echo "usage: $0 [--root ROOT] [--repository DIR] [--allow-downgrade] ARCHIVE..." >&2
    echo "       $0 [--root ROOT] --url HTTPS_URL --sha256 HASH" >&2
    echo "       $0 [--root ROOT] --owner PATH|--list PACKAGE|--remove PACKAGE" >&2
    exit 2
}

root=/
repository=
checksum=
url=
expected_hash=
allow_downgrade=0
mode=install
option_arg=
positional=()
while test "$#" -gt 0; do
    case "$1" in
        --root) test "$#" -ge 2 || usage; root=$2; shift 2;;
        --repository) test "$#" -ge 2 || usage; repository=$2; shift 2;;
        --checksum) test "$#" -ge 2 || usage; checksum=$2; shift 2;;
        --url) test "$#" -ge 2 || usage; url=$2; shift 2;;
        --sha256) test "$#" -ge 2 || usage; expected_hash=$2; shift 2;;
        --allow-downgrade) allow_downgrade=1; shift;;
        --owner|--list|--remove) test "$#" -ge 2 || usage; mode=${1#--}; option_arg=$2; shift 2;;
        --) shift; while test "$#" -gt 0; do positional+=("$1"); shift; done;;
        -*) usage;;
        *) positional+=("$1"); shift;;
    esac
done
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
    for dependency_file in "$state"/*.depends; do
        test -f "$dependency_file" || continue
        if grep -qxF "$option_arg" "$dependency_file"; then
            echo "Refusing to remove required package: $option_arg" >&2; exit 1
        fi
    done
    read -r installed_version < "$state/$option_arg.current"
    record=$state/${option_arg}_${installed_version}.files
    meta=$state/${option_arg}_${installed_version}.sha256sums
    test -f "$record" && test -f "$meta" || { echo "Package is not installed: $option_arg" >&2; exit 1; }
    while IFS=' ' read -r digest file; do
        test -n "$file" || continue
        target=$root/$file
        test -f "$target" && printf '%s  %s\n' "$digest" "$target" | sha256sum -c - >/dev/null || {
            echo "Refusing to remove modified or missing file: /$file" >&2; exit 1;
        }
    done < "$meta"
    while IFS=' ' read -r digest file; do test -n "$file" || continue; rm -f -- "$root/$file"; done < "$meta"
    rm -f -- "$record" "$meta" "$state/$option_arg.current" "$state/$option_arg.depends" "$state/$option_arg.control"
    echo "AURORA_PACKAGE_REMOVED $option_arg"; exit 0
fi

test "$mode" = install || usage
test -z "$url" && test "${#positional[@]}" -ge 1 || test -n "$url" || usage
test "$url" = "" || test "${#positional[@]}" -eq 0 || usage
if test -z "$url" && test "${#positional[@]}" -ge 2 && test -f "${positional[1]}" && case "${positional[1]}" in *.sha256) true;; *) false;; esac; then
    # Preserve the original ARCHIVE CHECKSUM ROOT calling convention.
    test "${#positional[@]}" -le 3 || usage
    test -n "$checksum" || checksum=${positional[1]}
    if test "${#positional[@]}" -eq 3 && test "$root" = /; then root=${positional[2]}; case "$root" in /*) ;; *) echo 'Install root must be absolute' >&2; exit 2;; esac; state=$root/var/lib/aurora/packages; fi
    positional=("${positional[0]}")
fi
if test -n "$url"; then
    case "$url" in https://*) ;; *) echo 'Package URL must use HTTPS' >&2; exit 2;; esac
    case "$expected_hash" in [a-f0-9][a-f0-9][a-f0-9][a-f0-9]*) test "${#expected_hash}" -eq 64 || usage;; *) echo 'Download mode requires a SHA-256 hash' >&2; exit 2;; esac
    download_dir=$(mktemp -d)
    download_archive=$download_dir/package.deb
    curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 --output "$download_archive" "$url"
    printf '%s  %s\n' "$expected_hash" "$download_archive" | sha256sum -c - >/dev/null || { echo 'Package archive checksum mismatch' >&2; rm -rf "$download_dir"; exit 1; }
    positional=("$download_archive")
else
    download_dir=
fi

tmp=$(mktemp -d)
committed=0
installed=()
backups=()
state_created=()
created_dirs=()
rollback() {
    rc=$?
    if test "$committed" -ne 1; then
        for ((i=${#installed[@]}-1;i>=0;i--)); do rm -f -- "${installed[i]}"; done
        for ((i=${#backups[@]}-1;i>=0;i-=2)); do
            target=${backups[i]}; backup=${backups[i+1]}; rm -f -- "$target"; mv -- "$backup" "$target" 2>/dev/null || true
        done
        for ((i=${#state_created[@]}-1;i>=0;i--)); do rm -f -- "${state_created[i]}"; done
        for ((i=${#created_dirs[@]}-1;i>=0;i--)); do rmdir -- "${created_dirs[i]}" 2>/dev/null || true; done
    fi
    rm -rf -- "$tmp" "$download_dir"
    exit "$rc"
}
trap rollback EXIT

declare -A archive_for version_for control_for files_for sums_for depends_for package_dir_for
names=()
compare_version() {
    # Bounded ordering: numeric dot/colon/dash components, then ASCII suffixes;
    # '~' sorts before all other suffixes. This is not full Debian ordering.
    awk -v a="$1" -v b="$2" 'function tok(v, x,n,i,r) { n=split(v,x,/[.:-+~]/); for(i=1;i<=n;i++) r[i]=x[i]; return n }
        function c(x,y, nx,ny,i,xx,yy) { nx=tok(x,xx); ny=tok(y,yy); for(i=1;i<=nx || i<=ny;i++){ if(i>nx)return -1; if(i>ny)return 1; if(xx[i] ~ /^[0-9]+$/ && yy[i] ~ /^[0-9]+$/){ if((xx[i]+0)>(yy[i]+0))return 1; if((xx[i]+0)<(yy[i]+0))return -1 } else if(xx[i]!=yy[i]) return (xx[i]>yy[i]?1:-1) } return 0 }
        BEGIN{print c(a,b)}'
}
validate_dependency_field() {
    field=$1
    test -z "$field" && return 0
    case "$field" in *'|'*|*'<'*|*'>'*|*'='*|*'('*|*')'*|*':'*)
        echo "Unsupported dependency expression: $field" >&2; return 1;; esac
    oldIFS=$IFS; IFS=,
    read -ra dependency_words <<< "$field"
    IFS=$oldIFS
    for dependency in "${dependency_words[@]}"; do
        dependency=${dependency#${dependency%%[![:space:]]*}}
        dependency=${dependency%${dependency##*[![:space:]]}}
        case "$dependency" in aurora-[a-z0-9+.-]*) ;; *) echo "Unsupported dependency name: $dependency" >&2; return 1;; esac
    done
}
read_archive() {
    local archive key dir control package version architecture depends field entry
    archive=$1; key=$2; dir=$tmp/pkg-$key
    test -f "$archive" || { echo "Package archive is missing: $archive" >&2; return 2; }
    mkdir -p "$dir/control" "$dir/data"
    ar p "$archive" debian-binary > "$dir/debian-binary"
    ar p "$archive" control.tar.gz > "$dir/control.tar.gz"
    ar p "$archive" data.tar.gz > "$dir/data.tar.gz"
    ar t "$archive" > "$dir/members"
    test "$(sort "$dir/members" | tr '\n' ' ')" = 'control.tar.gz data.tar.gz debian-binary ' || { echo 'Unexpected Debian archive members' >&2; return 2; }
    test "$(cat "$dir/debian-binary")" = '2.0' || { echo 'Unsupported Debian archive version' >&2; return 2; }
    tar -tzf "$dir/control.tar.gz" > "$dir/control.paths"
    tar -tzf "$dir/data.tar.gz" > "$dir/data.paths"
    while IFS= read -r entry; do entry=${entry#./}; case "$entry" in ''|.|control|files|sha256sums|md5sums) ;; *) echo "Unexpected package control path: $entry" >&2; return 2;; esac; done < "$dir/control.paths"
    while IFS= read -r entry; do entry=${entry#./}; case "$entry" in ''|opt|opt/|opt/aurora|opt/aurora/|opt/aurora/*) ;; *) echo "Unsafe package path: $entry" >&2; return 2;; esac; done < "$dir/data.paths"
    tar --no-same-owner -xzf "$dir/control.tar.gz" -C "$dir/control"
    tar --no-same-owner -xzf "$dir/data.tar.gz" -C "$dir/data"
    control=$dir/control/control
    for field in Package Version Architecture; do test "$(grep -c "^$field: " "$control")" -eq 1 || { echo "Invalid package metadata: $field" >&2; return 2; }; done
    package=$(grep '^Package: ' "$control" | cut -d' ' -f2-); version=$(grep '^Version: ' "$control" | cut -d' ' -f2-); architecture=$(grep '^Architecture: ' "$control" | cut -d' ' -f2-)
    case "$package" in aurora-[a-z0-9+.-]*) ;; *) echo 'Invalid Aurora package identity' >&2; return 2;; esac
    case "$version" in *[!a-zA-Z0-9.+:~-]*|'') echo 'Invalid package version' >&2; return 2;; esac
    test "$architecture" = musl-linux-amd64 || { echo 'Unsupported package architecture' >&2; return 2; }
    depends=$(grep '^Depends: ' "$control" | cut -d' ' -f2- || true); validate_dependency_field "$depends"
    # Dependency metadata is retained in the transaction plan and state.
    files_for[$package]=$dir/regular.files; sums_for[$package]=$dir/control/sha256sums; control_for[$package]=$control; version_for[$package]=$version
    archive_for[$package]=$archive; depends_for[$package]=$depends; package_dir_for[$package]=$dir
    while IFS= read -r entry; do entry=${entry#./}; case "$entry" in ''|opt|opt/|opt/aurora|opt/aurora/|opt/aurora/*) ;; *) echo 'Invalid package file path' >&2; return 2;; esac; done < "$dir/control/files"
    : > "$dir/regular.files"; while IFS= read -r entry; do test -n "$entry" && test -f "$dir/data/$entry" && printf '%s\n' "$entry" >> "$dir/regular.files"; done < "$dir/control/files"
    while IFS=' ' read -r digest file; do
        case "$digest" in [a-f0-9][a-f0-9][a-f0-9][a-f0-9]*) test "${#digest}" -eq 64 || { echo 'Invalid package file checksum' >&2; return 2; };; *) echo 'Invalid package file checksum' >&2; return 2;; esac
        grep -qxF "$file" "$dir/regular.files" || { echo "Checksum path is not packaged: $file" >&2; return 2; }
    done < "$dir/control/sha256sums"
    (cd "$dir/data" && sha256sum -c "$dir/control/sha256sums") || return 1
    : > "$dir/archive.files"; while IFS= read -r entry; do entry=${entry#./}; test -f "$dir/data/$entry" && printf '%s\n' "$entry" >> "$dir/archive.files"; done < "$dir/data.paths"
    : > "$dir/manifest.files"; while IFS= read -r entry; do test -f "$dir/data/$entry" && printf '%s\n' "$entry" >> "$dir/manifest.files"; done < "$dir/control/files"
    sort -o "$dir/archive.files" "$dir/archive.files"; sort -o "$dir/manifest.files" "$dir/manifest.files"
    test "$(cat "$dir/archive.files")" = "$(cat "$dir/manifest.files")" || { echo 'Package archive and DEBIAN/files differ' >&2; return 1; }
    names+=("$package")
}

for archive in "${positional[@]}"; do
    if test -n "$checksum"; then read -r expected _ < "$checksum"; test "${#expected}" -eq 64 || { echo 'Invalid package checksum sidecar' >&2; exit 2; }; printf '%s  %s\n' "$expected" "$archive" | sha256sum -c - >/dev/null || { echo 'Package archive checksum mismatch' >&2; exit 1; }; checksum=; fi
    key=$(basename "$archive" | tr -cd 'a-zA-Z0-9')
    read_archive "$archive" "$key"
done

declare -A repo_path repo_version resolving
if test -n "$repository"; then
    repo_base=$repository
    repo_index=$repository/Packages
    if test ! -f "$repo_index"; then repo_base=$repository/apt; repo_index=$repo_base/dists/aurora/main/binary-musl-linux-amd64/Packages; fi
    test -f "$repo_index" || { echo 'Repository Packages metadata is missing' >&2; exit 2; }
    repo_records=$tmp/repository.records
    awk 'BEGIN{p=v=f=a=""} /^Package: /{p=$0;sub(/^Package: /,"",p)} /^Version: /{v=$0;sub(/^Version: /,"",v)} /^Filename: /{f=$0;sub(/^Filename: /,"",f)} /^Architecture: /{a=$0;sub(/^Architecture: /,"",a)} /^$/{if(p!="")print p"|"v"|"f"|"a; p=v=f=a=""}' "$repo_index" > "$repo_records"
    while IFS='|' read -r package version filename architecture; do
        test "$architecture" = musl-linux-amd64 || continue
        case "$package" in aurora-[a-z0-9+.-]*) ;; *) continue;; esac
        candidate=$repo_base/$filename
        test -f "$candidate" || continue
        if test -z "${repo_version[$package]:-}" || test "$(compare_version "$version" "${repo_version[$package]}")" -gt 0; then repo_version[$package]=$version; repo_path[$package]=$candidate; fi
    done < "$repo_records"
fi

resolve() {
    local package=$1 chain=$2 dependency oldIFS
    if test -z "${archive_for[$package]:-}" && test -f "$state/$package.current"; then return 0; fi
    test "${resolving[$package]:-0}" -eq 0 || { echo "Dependency cycle: $chain,$package" >&2; return 1; }
    resolving[$package]=1
    if test -z "${archive_for[$package]:-}" && test -n "${repo_path[$package]:-}"; then
        case ",$chain," in *,"$package",*) echo "Dependency cycle: $chain,$package" >&2; return 1;; esac
        read_archive "${repo_path[$package]}" "repo$(printf '%s' "$package" | tr -cd 'a-zA-Z0-9')"
    elif test -z "${archive_for[$package]:-}"; then
        echo "Missing dependency: $package (required by $chain)" >&2; return 1
    fi
    oldIFS=$IFS; IFS=,
    read -ra dependency_words <<< "${depends_for[$package]}"
    IFS=$oldIFS
    for dependency in "${dependency_words[@]}"; do
        dependency=${dependency#${dependency%%[![:space:]]*}}; dependency=${dependency%${dependency##*[![:space:]]}}
        test -n "$dependency" || continue
        resolve "$dependency" "$chain,$package" || { unset 'resolving[$package]'; return 1; }
    done
    unset 'resolving[$package]'
}
for package in "${names[@]}"; do resolve "$package" "$package" || exit 1; done

# Resolve dependencies before any filesystem mutation, then topologically order them.
ordered=(); declare -A seen active; order() { local package=$1 dependency oldIFS; test "${active[$package]:-0}" -eq 0 || { echo "Dependency cycle involving $package" >&2; return 1; }; test "${seen[$package]:-0}" -eq 1 && return 0; if test -z "${archive_for[$package]:-}" && test -f "$state/$package.current"; then seen[$package]=1; return 0; fi; active[$package]=1; oldIFS=$IFS; IFS=,; read -ra dependency_words <<< "${depends_for[$package]:-}"; IFS=$oldIFS; for dependency in "${dependency_words[@]}"; do dependency=${dependency#${dependency%%[![:space:]]*}}; dependency=${dependency%${dependency##*[![:space:]]}}; test -n "$dependency" && order "$dependency" || return 1; done; active[$package]=0; seen[$package]=1; ordered+=("$package"); }
for package in "${names[@]}"; do order "$package" || exit 1; done

mkdir -p "$tmp/backup"
for package in "${ordered[@]}"; do
    new_version=${version_for[$package]}; current_file=$state/$package.current
    if test -f "$current_file"; then
        read -r old_version < "$current_file"
        relation=$(compare_version "$new_version" "$old_version")
        test "$relation" -ne 0 || { echo "Refusing to overwrite installed package: $package $old_version" >&2; exit 1; }
        test "$relation" -ge 0 || test "$allow_downgrade" -eq 1 || { echo "Refusing downgrade of $package from $old_version to $new_version" >&2; exit 1; }
        old_meta=$state/${package}_${old_version}.sha256sums
        old_files=$state/${package}_${old_version}.files
        while IFS=' ' read -r digest file; do test -n "$file" || continue; target=$root/$file; if test -f "$target"; then printf '%s  %s\n' "$digest" "$target" | sha256sum -c - >/dev/null || { echo "Refusing to replace modified file: /$file" >&2; exit 1; }; fi; done < "$old_meta"
        while IFS= read -r file; do test -n "$file" || continue; grep -qxF "$file" "${files_for[$package]}" || rm -f -- "$root/$file"; done < "$old_files"
    fi
    while IFS= read -r file; do test -n "$file" || continue; target=$root/$file
        if test -e "$target" || test -L "$target"; then
            owner_found=0
            for owner_record in "$state"/*.files; do test -f "$owner_record" || continue; grep -qxF "$file" "$owner_record" || continue; owner_found=1; done
            test "$owner_found" -eq 1 || { echo "Refusing to overwrite unmanaged path: $target" >&2; exit 1; }
        fi
    done < "${files_for[$package]}"
done

ensure_dir() {
    local path=$1 parent
    test -d "$path" && return 0
    test ! -e "$path" && test ! -L "$path" || { echo "Not a directory: $path" >&2; return 1; }
    mkdir -p -- "$path"
    created_dirs+=("$path")
}
ensure_dir "$state"
for package in "${ordered[@]}"; do
    new_version=${version_for[$package]}; dir=${package_dir_for[$package]}
    while IFS= read -r file; do test -n "$file" || continue; target=$root/$file; ensure_dir "${target%/*}"; if test -e "$target" || test -L "$target"; then backup=$tmp/backup/$(printf '%s' "$file" | tr '/' '_'); cp -p -- "$target" "$backup"; backups+=("$target" "$backup"); rm -f -- "$target"; fi; mv -- "$dir/data/$file" "$target"; installed+=("$target"); done < "${files_for[$package]}"
    old_version_file=$state/$package.current
    new_state_files=("$state/${package}_${new_version}.files" "$state/${package}_${new_version}.sha256sums" "$state/$package.control" "$state/$package.depends" "$old_version_file")
    for state_file in "${new_state_files[@]}"; do
        if test -e "$state_file"; then state_backup=$tmp/backup/state-$(printf '%s' "$state_file" | tr '/' '_'); cp -p -- "$state_file" "$state_backup"; backups+=("$state_file" "$state_backup"); else state_created+=("$state_file"); fi
    done
    if test -f "$old_version_file"; then read -r old_version < "$old_version_file"; old_record=$state/${package}_${old_version}.files; old_meta=$state/${package}_${old_version}.sha256sums; rm -f -- "$old_record" "$old_meta" "$state/$package.depends" "$state/$package.control"; fi
    cp -- "${files_for[$package]}" "$state/${package}_${new_version}.files"
    cp -- "${sums_for[$package]}" "$state/${package}_${new_version}.sha256sums"
    cp -- "${control_for[$package]}" "$state/$package.control"
    printf '%s\n' "$new_version" > "$state/$package.current"
    : > "$state/$package.depends"; oldIFS=$IFS; IFS=,; read -ra dependency_words <<< "${depends_for[$package]}"; IFS=$oldIFS; for dependency in "${dependency_words[@]}"; do dependency=${dependency#${dependency%%[![:space:]]*}}; dependency=${dependency%${dependency##*[![:space:]]}}; test -n "$dependency" && printf '%s\n' "$dependency" >> "$state/$package.depends"; done
done
committed=1
for package in "${ordered[@]}"; do echo "AURORA_PACKAGE_INSTALLED $package ${version_for[$package]}"; done
