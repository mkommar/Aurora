#!/bin/bash
set -eu
port=${1:?QEMU host-forward port required}
version=$(curl --version)
case "$version" in *mbedTLS*) ;; *) echo 'curl TLS backend missing' >&2; exit 1;; esac
packages=$(curl -fsS --max-time 30 "http://10.0.2.2:$port/dists/stable/main/binary-musl/Packages")
case "$packages" in *aurora-apt-probe*|*aurora-repo-probe*) ;; *) echo 'APT Packages index invalid' >&2; exit 2;; esac
curl -fsS --max-time 30 "http://10.0.2.2:$port/dists/stable/Release" -o /work/curl-Release
curl -fsS --max-time 30 "http://10.0.2.2:$port/pool/aurora-apt-probe_1.0-1_musl-linux-amd64.deb" -o /work/curl-probe.deb
curl -fsS --max-time 30 --cacert /work/test-ca.pem https://10.0.2.2:8881/source.c -o /work/curl-tls.c
test -s /work/curl-Release && test -s /work/curl-probe.deb && test -s /work/curl-tls.c
echo APT_REPO_BASH_CURL_TLS_PASS
