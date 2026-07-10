#!/usr/bin/env bash
# Run bench_usm_pool across the (threads × resolution × fill) matrix.
# Usage: bench_matrix.sh <bench_binary> [frames] [amount] [fill]
# Output CSV: variant,threads,width,height,frames,amount,fill,us_per_frame
set -eu
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
        readarray -t runs < <(for _ in 1 2 3; do "$BIN" "$t" "$W" "$H" "$FRAMES" "$AMOUNT" "$FILL" | cut -d, -f7; done)
        IFS=$'\n' sorted=($(sort -g <<<"${runs[*]}"))
        unset IFS
        med="${sorted[1]}"
        echo "${VARIANT},${t},${W},${H},${FRAMES},${AMOUNT},${FILL},${med}"
    done
done
