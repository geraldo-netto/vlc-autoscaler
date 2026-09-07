#!/usr/bin/env bash
set -euo pipefail

build=${1:?profile build directory required}
output=${2:?new output directory required}
ready=${3:?coordination ready-file required}
mode=${4:-all}

if [[ $mode != all && $mode != attribution ]]; then
    echo 'Collection mode must be all or attribution.' >&2
    exit 2
fi

if [[ $(id -u) != 0 || -z ${SUDO_USER:-} || $SUDO_USER == root ]]; then
    echo 'Run through sudo from your normal user account.' >&2
    exit 2
fi
if [[ ! -x $build/profile_pipeline || -e $output ]]; then
    echo 'Build binary is missing or output directory already exists.' >&2
    exit 2
fi
mkdir -m 0755 -- "$output"
finish() {
    local status=$?
    printf '%s\n' "$status" > "$output/status"
    chown -R -- "$SUDO_UID:$SUDO_GID" "$output"
    touch "$output/done"
}
trap finish EXIT
echo "Waiting for the isolated userspace sweep: $ready"
while [[ ! -f $ready ]]; do sleep 2; done
printf 'kernel=%s\n' "$(uname -r)" > "$output/environment.txt"
perf version >> "$output/environment.txt"
perf list --raw-dump > "$output/events.txt"

if [[ $mode == all ]]; then
events=cycles,instructions,cache-references,cache-misses,context-switches,cpu-migrations,page-faults
for workers in 12 32; do
    for height in 1080 2160; do
        width=$((height * 16 / 9))
        label=w${workers}-h${height}
        echo "Hardware counters: $label"
        perf stat -r 3 -x ';' -o "$output/$label.stat" -e "$events" -- \
            runuser -u "$SUDO_USER" -- "$build/profile_pipeline" \
            "$workers" "$workers" "$width" "$height" 10000 1 0 1 0 \
            > "$output/$label.stdout" 2> "$output/$label.stderr"
    done
done
fi

for workers in 12 32; do
    echo "CPU call stacks: $workers workers"
    perf record -o "$output/w$workers.cpu.data" -m 1024 -e cycles:u -F 199 \
        --call-graph dwarf -- \
        runuser -u "$SUDO_USER" -- "$build/profile_pipeline" \
        "$workers" "$workers" 1920 1080 12000 1 0 1 0 \
        > "$output/w$workers.cpu.stdout" 2> "$output/w$workers.cpu.stderr"
    perf report -i "$output/w$workers.cpu.data" --stdio --percent-limit 0.5 \
        > "$output/w$workers.cpu.txt"
    if [[ $mode == all ]]; then
    echo "Scheduler trace: $workers workers"
    perf sched record -o "$output/w$workers.sched.data" -- \
        runuser -u "$SUDO_USER" -- "$build/profile_pipeline" \
        "$workers" "$workers" 1920 1080 3000 1 0 1 0 \
        > "$output/w$workers.sched.stdout" 2> "$output/w$workers.sched.stderr"
    perf sched latency -i "$output/w$workers.sched.data" \
        > "$output/w$workers.sched.txt"
    fi
done

for workers in 12 32; do
echo "Cache-to-cache capture: $workers workers"
if perf c2c record -u -m 1024 -o "$output/w$workers.c2c.data" -- \
    runuser -u "$SUDO_USER" -- "$build/profile_pipeline" \
    "$workers" "$workers" 1920 1080 20000 1 0 1 0 \
    > "$output/w$workers.c2c.stdout" 2> "$output/w$workers.c2c.stderr"; then
    perf c2c report -i "$output/w$workers.c2c.data" --stdio \
        > "$output/w$workers.c2c.txt" 2> "$output/w$workers.c2c-report.stderr"
else
    echo 'Cache-to-cache capture unavailable; its diagnostic was retained.'
fi
done
echo "Capture complete: $output"
