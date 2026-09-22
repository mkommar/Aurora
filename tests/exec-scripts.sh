#!/bin/bash
set -eu
trap 'rc=$?; sync; exit "$rc"' EXIT
cd /work
gcc -O2 -static exec-scripts.c -o exec-scripts
./exec-scripts
