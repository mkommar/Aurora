#!/bin/bash
set -eu
export PATH=/usr/bin:/bin:/usr/local/bin
trap 'rc=$?; echo "AURORA_BUILD_SHELL_EXIT=$rc PATH=$PATH"; /usr/bin/sync; exit "$rc"' EXIT
for ((i=0;i<80;i++)); do
    result=$(/usr/bin/bash -c 'printf "%s\n" alphabet | sed s/alpha/beta/ | grep betabet')
    test "$result" = betabet
    test "$PATH" = /usr/bin:/bin:/usr/local/bin
done
echo AURORA_BUILD_SHELL_PIPELINES_PASS
