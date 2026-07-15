#!/usr/bin/env bash
# Run bench_usm_pool across the (threads × resolution × fill) matrix.
# Usage: bench_matrix.sh <bench_binary> [frames] [amount] [fill]
# Output CSV: variant,requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame
set -euo pipefail
export LC_ALL=C
BIN="${1:?bench binary path required}"
FRAMES="${2:-100}"
AMOUNT="${3:-20}"
FILL="${4:-rand}"
VARIANT="$(basename "$BIN")"

die() {
    echo "bench_matrix: $*" >&2
    exit 1
}

decimal_gt() {
    local left=$1
    local right=$2

    if (( ${#left} > ${#right} )); then
        return 0
    fi
    if (( ${#left} < ${#right} )); then
        return 1
    fi
    [[ "$left" > "$right" ]]
}

capture_sample() {
    local requested=$1
    local width=$2
    local height=$3
    local sample status

    if sample="$(
        status=0
        "$BIN" "$requested" "$width" "$height" \
            "$FRAMES" "$AMOUNT" "$FILL" || status=$?
        printf '\036'
        exit "$status"
    )"; then
        :
    else
        status=$?
        die "benchmark exited with status $status"
    fi
    if [[ "$sample" != *$'\036' ]]; then
        die "failed to capture benchmark output"
    fi
    sample="${sample%$'\036'}"
    if [[ "$sample" == *$'\n' ]]; then
        sample="${sample%$'\n'}"
    fi
    if [[ "$sample" == *$'\n'* ]]; then
        die "benchmark emitted multiple lines"
    fi
    printf '%s' "$sample"
}

parse_sample() {
    local sample=$1
    local requested=$2
    local width=$3
    local height=$4
    local separators
    local -a fields

    separators="${sample//[^,]/}"
    if (( ${#separators} != 7 )); then
        die "expected 8 CSV fields, got $((${#separators} + 1))"
    fi
    IFS=',' read -r -a fields <<< "$sample"
    if (( ${#fields[@]} != 8 )); then
        die "expected 8 CSV fields, got ${#fields[@]}"
    fi

    [[ "${fields[0]}" == "$requested" ]] ||
        die "requested_threads mismatch: expected '$requested', got '${fields[0]}'"
    [[ "${fields[1]}" =~ ^[1-9][0-9]*$ ]] ||
        die "invalid effective_threads '${fields[1]}'"
    if decimal_gt "${fields[1]}" "$requested"; then
        die "effective_threads ${fields[1]} exceeds requested_threads $requested"
    fi
    [[ "${fields[2]}" == "$width" ]] ||
        die "width mismatch: expected '$width', got '${fields[2]}'"
    [[ "${fields[3]}" == "$height" ]] ||
        die "height mismatch: expected '$height', got '${fields[3]}'"
    [[ "${fields[4]}" == "$FRAMES" ]] ||
        die "frames mismatch: expected '$FRAMES', got '${fields[4]}'"
    [[ "${fields[5]}" == "$AMOUNT" ]] ||
        die "amount mismatch: expected '$AMOUNT', got '${fields[5]}'"
    [[ "${fields[6]}" == "$FILL" ]] ||
        die "fill mismatch: expected '$FILL', got '${fields[6]}'"
    [[ "${fields[7]}" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
        die "invalid timing '${fields[7]}'"

    SAMPLE_EFFECTIVE=${fields[1]}
    SAMPLE_TIMING=${fields[7]}
}

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
        effective=
        for _ in 1 2 3; do
            sample=$(capture_sample "$t" "$W" "$H")
            parse_sample "$sample" "$t" "$W" "$H"
            if [[ -n "$effective" && "$SAMPLE_EFFECTIVE" != "$effective" ]]; then
                die "effective_threads changed from $effective to $SAMPLE_EFFECTIVE"
            fi
            effective=$SAMPLE_EFFECTIVE
            runs+=("$SAMPLE_TIMING")
        done
        sorted_output="$(printf '%s\n' "${runs[@]}" | LC_ALL=C sort -g)"
        readarray -t sorted <<< "$sorted_output"
        if (( ${#sorted[@]} != 3 )); then
            die "expected 3 timing samples"
        fi
        med="${sorted[1]}"
        echo "${VARIANT},${t},${effective},${W},${H},${FRAMES},${AMOUNT},${FILL},${med}"
    done
done
