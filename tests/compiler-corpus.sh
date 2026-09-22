#!/bin/bash
set -eu
compiler=${1:?candidate compiler path required}
root=${2:-/work/compiler-corpus-results}
mkdir -p "$root"
"$compiler" -v > "$root/compiler.txt" 2>&1
for optimization in 0 2 3; do
    "$compiler" -O"$optimization" -static -pthread /work/compiler-corpus.c -o "$root/corpus-O$optimization"
    "$root/corpus-O$optimization" > "$root/O$optimization.txt"
    grep -qx AURORA_COMPILER_CORPUS_PASS "$root/O$optimization.txt"
done
test "$(cat "$root/O0.txt")" = "$(cat "$root/O2.txt")"
test "$(cat "$root/O0.txt")" = "$(cat "$root/O3.txt")"
echo AURORA_CANDIDATE_COMPILER_CORPUS_PASS
