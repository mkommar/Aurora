#!/bin/bash
set -eu
gcc -O2 -static /work/package-locks.c -o /work/package-locks
/work/package-locks
sync
