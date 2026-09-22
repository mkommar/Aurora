#!/bin/bash
set -eu
trap 'rc=$?; /usr/bin/sync; exit "$rc"' EXIT
cd /work
gcc -O2 -static path-cache.c -o path-cache
./path-cache
