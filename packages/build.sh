#!/bin/bash
# Native source-to-.deb builder. Invoked with a generated, hash-pinned manifest.
set -euo pipefail
umask 022
name=${1:?package name required}
case "$name" in *[!a-z0-9+-]*|'') exit 2;; esac
base=/work/packages
source "$base/$name.env"
export PATH=/opt/aurora/bin:/opt/aurora/sbin:/usr/local/bin:/usr/bin:/bin
export CONFIG_SHELL=/usr/bin/bash SHELL=/usr/bin/bash LC_ALL=C TZ=UTC
export CC=gcc CXX=g++ CFLAGS='-O2' CXXFLAGS='-O2' LDFLAGS='-static -L/opt/aurora/lib'
export CPPFLAGS=-I/opt/aurora/include SOURCE_DATE_EPOCH=1720000000
mkdir -p "$base/cache" "$base/logs" "$base/out" "$base/build" "$base/stage"
log=$base/logs/$name.log
trap 'rc=$?; sync; if test "$rc" != 0; then tail -60 "$log"; fi; exit "$rc"' EXIT
echo "AURORA_PACKAGE_START $name $version"
exec 3>&1
(
    cd "$base/cache"
    if ! test -f "$archive"; then
        curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 \
            --output "$archive.partial" "$url"
        printf '%s  %s\n' "$sha256" "$archive.partial" | sha256sum -c -
        mv "$archive.partial" "$archive"
    fi
    printf '%s  %s\n' "$sha256" "$archive" | sha256sum -c -
    # A failed work directory is kept for diagnosis. Never reuse config.cache
    # or host-generated configure results on a later attempt.
    work=$base/build/$name-$version
    stage=$base/stage/$name-$version
    test ! -e "$work"; test ! -e "$stage"
    mkdir "$work" "$stage"
    tar xf "$base/cache/$archive" -C "$work" --strip-components=1
    echo "AURORA_PACKAGE_EXTRACTED $name" >&3
    cd "$work"
    if test -n "${patch_specs:-}"; then
        while read -r digest strip filename; do
            printf '%s  %s\n' "$digest" "$filename" | sha256sum -c -
            patch --batch --forward -p"$strip" < "$filename"
        done <<< "$patch_specs"
    fi
    prefix=/opt/aurora
    case "$recipe" in
        musl)
            LDFLAGS='' bash configure --prefix="$prefix" --syslibdir="$prefix/lib" --disable-wrapper
            make -j1
            make DESTDIR="$stage" install
            ;;
        perl)
            bash Configure -des -Dprefix="$prefix" -Dcc=gcc -Dccflags=-O2 \
                -Dldflags=-static -Uusedl -Uuseshrplib -Dusethreads
            make -j1
            make test
            make DESTDIR="$stage" install
            ;;
        bzip2)
            make -j1 CC=gcc CFLAGS='-O2 -static' test
            make PREFIX="$stage$prefix" install
            ;;
        zlib)
            bash configure --prefix="$prefix" --static
            make -j1; make check
            make DESTDIR="$stage" install
            ;;
        *)
            options=(--prefix="$prefix" --build=x86_64-linux-musl --host=x86_64-linux-musl)
            case "$name" in
                make) options+=(--disable-nls --disable-load --without-guile);;
                bash) options+=(--without-bash-malloc --disable-nls --disable-readline);;
                xz) options+=(--disable-shared --disable-nls);;
                libmd|gmp|mpfr|mpc) options+=(--disable-shared --enable-static);;
                dpkg) options+=(--disable-nls --disable-dselect --disable-start-stop-daemon \
                    --disable-update-alternatives --disable-devel-docs --without-libselinux \
                    --with-dpkg-deb-compressor=gzip);;
                binutils) options+=(--disable-nls --disable-werror --disable-gprofng --disable-shared);;
                gcc) options+=(--enable-languages=c,c++ --disable-multilib --disable-nls \
                    --disable-libsanitizer --disable-libquadmath --without-isl \
                    --with-gmp="$prefix" --with-mpfr="$prefix" --with-mpc="$prefix");;
                python) options+=(--without-ensurepip --disable-shared);;
                *) options+=(--disable-nls);;
            esac
            mkdir obj; cd obj
            bash ../configure "${options[@]}"
            if test "$recipe" = gcc; then
                # Upstream bootstrap compares stage 2 and stage 3 before install.
                make -j1 bootstrap
            else
                make -j1
            fi
            make DESTDIR="$stage" install
            ;;
    esac
    echo "AURORA_PACKAGE_STAGED $name" >&3
    if test "$recipe" = gcc; then
        bash /work/compiler-corpus.sh "$stage$prefix/bin/gcc" "$base/logs/gcc-corpus"
    fi
    case "$name" in
        make|bash|xz|m4|patch|bison|flex) "$stage$prefix/bin/$name" --version;;
        diffutils) "$stage$prefix/bin/diff" --version;;
    esac
    # Real Debian binary package format, available before dpkg bootstraps.
    # gzip -n and sorted fixed-mtime tar entries keep archive metadata stable.
    mkdir -p "$stage$prefix/share/doc/$name" "$stage/DEBIAN"
    find "$work" -maxdepth 1 -type f \( -iname 'copying*' -o -iname 'copyright*' \
        -o -iname 'license*' -o -iname 'licence*' -o -iname 'artistic*' -o -iname 'notice*' \) \
        -exec cp '{}' "$stage$prefix/share/doc/$name/" ';'
    cp "$base/sources.lock.json" "$stage$prefix/share/doc/$name/aurora-sources.json"
    cp "$base/build.sh" "$stage$prefix/share/doc/$name/aurora-build.sh"
    if test -n "${patch_specs:-}"; then
        mkdir "$stage$prefix/share/doc/$name/patches"
        while read -r digest strip filename; do
            cp "$filename" "$stage$prefix/share/doc/$name/patches/"
        done <<< "$patch_specs"
    fi
    printf 'Package: aurora-%s\nVersion: %s-1\nArchitecture: musl-linux-amd64\nMaintainer: Aurora Developers\nDescription: %s built from pinned source inside Aurora\n' \
        "$name" "$version" "$name" > "$stage/DEBIAN/control"
    if test -n "$depends"; then printf 'Depends: %s\n' "$depends" >> "$stage/DEBIAN/control"; fi
    printf 'Source-SHA256: %s\nRecipe-SHA256: %s\nLicense: %s\n' "$sha256" "$recipe_sha256" "$license" \
        > "$stage$prefix/share/doc/$name/aurora-buildinfo"
    cd "$stage"
    find opt -type f -print0 | sort -z | xargs -0 sha256sum > DEBIAN/sha256sums
    find opt -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
    find opt -print | sort > DEBIAN/files
    tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner -cf "$work/control.tar" -C DEBIAN .
    tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner -cf "$work/data.tar" opt
    gzip -n "$work/control.tar" "$work/data.tar"
    cd "$work"; printf '2.0\n' > debian-binary
    ar crD package.deb debian-binary control.tar.gz data.tar.gz
    output="$base/out/aurora-${name}_${version}-1_musl-linux-amd64.deb"
    # Never update a published ar archive in place or discard a previous build.
    if test -e "$output"; then
        previous=$(sha256sum "$output"); previous=${previous%% *}
        mkdir -p "$base/out/history"
        if ! test -e "$base/out/history/$previous.deb"; then
            cp "$output" "$base/out/history/$previous.deb"
        fi
    fi
    mv package.deb "$output"
    cd "$base/out"; sha256sum "aurora-$name"_"$version-1_musl-linux-amd64.deb" > "$name.sha256.partial"
    mv "$name.sha256.partial" "$name.sha256"
) > "$log" 2>&1
echo "AURORA_PACKAGE_BUILT $name $version"
