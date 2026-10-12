#!/bin/bash
# Supervises the existing recipe without changing its security or build policy.
set -u
name="$1"
base=/work/packages
state="$base/state/$name"
log="$base/logs/$name.log"
mkdir -p "$state"

checkpoint() {
    stage="$1"; status="$2"; detail="${3:-}"
    printf '{"timestamp":%s,"stage":"%s","status":"%s","detail":"%s"}\n' \
        "$(date +%s)" "$stage" "$status" "$detail" > "$state/current.json.tmp"
    mv -f "$state/current.json.tmp" "$state/current.json"
    cat "$state/current.json" >> "$state/checkpoints.jsonl"
}

heartbeat() {
    while test -f "$state/heartbeat.pid"; do
        current=unknown
        test -f "$state/current.json" && current="$(cat "$state/current.json")"
        printf '%s stage=%s pid=%s jobs=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            "$current" "$$" "${MAKEFLAGS:-default}" > "$state/heartbeat"
        if test -d /proc/$$; then
            { printf 'pid=%s\n' "$$"; cat "/proc/$$/status"; } > "$state/proc-status"
        fi
        sleep 15
    done
}

checkpoint image-preparation passed "guest recipe supervisor initialized"
checkpoint guest-boot passed "host runner reached guest shell"
checkpoint network-readiness passed "host runner gated DHCP readiness"
checkpoint source-fetch running "waiting for recipe log"
touch "$state/heartbeat.pid"
heartbeat & heartbeat_pid=$!
bash "$base/build.sh" "$name" & build_pid=$!
stage_timeout="${AURORA_STAGE_TIMEOUT:-1800}"
last_line=0
extracted=0
configured=0
tested=0
staged=0
while kill -0 "$build_pid" 2>/dev/null; do
    if test -f "$state/current.json"; then
        checkpoint_time=$(stat -c %Y "$state/current.json" 2>/dev/null || date +%s)
        now=$(date +%s)
        if test $((now - checkpoint_time)) -gt "$stage_timeout"; then
            cp "$state/current.json" "$state/last-observed.json"
            checkpoint recovery timeout "stage checkpoint stale for ${stage_timeout}s; see last-observed.json"
            kill "$build_pid" 2>/dev/null || true
            break
        fi
    fi
    if test -f "$log"; then
        line_count=$(wc -l < "$log")
        if test "$line_count" -gt "$last_line"; then
            tail -n +$((last_line + 1)) "$log" | tee -a "$state/serial-tail.log"
            tail -c 65536 "$state/serial-tail.log" > "$state/serial-tail.log.tmp"
            mv -f "$state/serial-tail.log.tmp" "$state/serial-tail.log"
            last_line="$line_count"
            if test "$extracted" -eq 0 && grep -q 'AURORA_PACKAGE_EXTRACTED' "$log"; then
                extracted=1
                checkpoint source-fetch passed "archive available"
                checkpoint source-verification passed "recipe completed checksum gate"
                checkpoint extraction passed "archive extracted"
                checkpoint configure running "configure output observed"
            fi
            if test "$configured" -eq 0 && grep -qE 'config.status|creating Makefile|checking for' "$log"; then
                configured=1
                checkpoint configure passed "configure output observed"
                checkpoint compile running "compiler output observed"
            fi
            if test "$tested" -eq 0 && grep -qE 'Making check in|check-local|PASS ' "$log"; then
                tested=1
                checkpoint compile passed "compile output observed"
                checkpoint tests running "test output observed"
            fi
            if test "$staged" -eq 0 && grep -q 'AURORA_PACKAGE_STAGED' "$log"; then
                staged=1
                checkpoint tests passed "recipe test stage completed"
                checkpoint package-creation running "staging output observed"
            fi
        fi
    fi
    sleep 2
done
wait "$build_pid"; rc=$?
if test -f "$log"; then
    line_count=$(wc -l < "$log")
    test "$line_count" -le "$last_line" || tail -n +$((last_line + 1)) "$log" | tee -a "$state/serial-tail.log"
fi
if test "$rc" -eq 0; then
    checkpoint package-creation passed "recipe produced package"
    checkpoint install passed "recipe install hook completed where applicable"
    checkpoint lifecycle passed "recipe completed"
else
    checkpoint recovery failed "recipe exit=$rc"
fi
rm -f "$state/heartbeat.pid"
wait "$heartbeat_pid" 2>/dev/null || true
exit "$rc"
