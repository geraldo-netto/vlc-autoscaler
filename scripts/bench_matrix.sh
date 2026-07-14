#!/usr/bin/env bash
# Run bench_usm_pool across the (threads × resolution × fill) matrix.
# Usage: bench_matrix.sh <bench_binary> [frames] [amount] [fill]
# Output CSV: variant,threads,width,height,frames,amount,fill,us_per_frame
set -euo pipefail
BIN="${1:?bench binary path required}"
FRAMES="${2:-100}"
AMOUNT="${3:-20}"
FILL="${4:-rand}"
VARIANT="$(basename "$BIN")"

# (label width height) — 720p, 1080p, 1440p (2k)
RES=(
    "720p  1280  720"
    "1080p 1920 1080"
    "1440p 2560 1440"
)
THREADS=(1 4 8 12 16 20)

for r in "${RES[@]}"; do
    read -r _label W H <<< "$r"
    for t in "${THREADS[@]}"; do
        runs=()
        for _ in 1 2 3; do
            sample="$(
                status=0
                "$BIN" "$t" "$W" "$H" "$FRAMES" "$AMOUNT" "$FILL" || status=$?
                printf '\036'
                exit "$status"
            )"
            if [[ "$sample" != *$'\036' ]]; then
                echo "bench_matrix: failed to capture benchmark output" >&2
                exit 1
            fi
            sample="${sample%$'\036'}"
            if [[ "$sample" == *$'\n' ]]; then
                sample="${sample%$'\n'}"
            fi
            if [[ "$sample" == *$'\n'* ]]; then
                echo "bench_matrix: benchmark emitted multiple lines" >&2
                exit 1
            fi
            separators="${sample//[^,]/}"
            if (( ${#separators} != 6 )); then
                echo "bench_matrix: expected 7 CSV fields, got $((${#separators} + 1))" >&2
                exit 1
            fi
            IFS=',' read -r -a fields <<< "$sample"
            if (( ${#fields[@]} != 7 )); then
                echo "bench_matrix: expected 7 CSV fields, got ${#fields[@]}" >&2
                exit 1
            fi
            if [[ ! "${fields[6]}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
                echo "bench_matrix: invalid timing '${fields[6]}'" >&2
                exit 1
            fi
            runs+=("${fields[6]}")
        done
        sorted_output="$(printf '%s\n' "${runs[@]}" | LC_ALL=C sort -g)"
        readarray -t sorted <<< "$sorted_output"
        if (( ${#sorted[@]} != 3 )); then
            echo "bench_matrix: expected 3 timing samples" >&2
            exit 1
        fi
        med="${sorted[1]}"
        echo "${VARIANT},${t},${W},${H},${FRAMES},${AMOUNT},${FILL},${med}"
    done
done
