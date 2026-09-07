#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/vlc-autoscaler-bench-recipes.XXXXXX")

cleanup() {
    rm -rf -- "$tmp"
}
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

cat >"$tmp/bench_worker_pool" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$1" >>"${BENCH_CALLS:?}"
test "$1" -ne "${BENCH_FAIL_AT:?}" || exit 23
printf '%s,1,1,1\n' "$1"
EOF
chmod 0755 "$tmp/bench_worker_pool"
cp "$tmp/bench_worker_pool" "$tmp/bench_pipeline"

run_recipe() {
    : >"$tmp/calls"
    BENCH_FAIL_AT=$1 BENCH_CALLS="$tmp/calls" \
        make -s --no-print-directory -C "$repo_root" -o "$binary" \
        BUILD="$tmp" "$target" >"$tmp/stdout" 2>"$tmp/stderr"
}

for target in bench-worker-pool bench-pipeline; do
    case $target in
        bench-worker-pool) binary="$tmp/bench_worker_pool"; count=8 ;;
        bench-pipeline) binary="$tmp/bench_pipeline"; count=5 ;;
    esac
    run_recipe 0
    test "$(wc -l <"$tmp/calls")" -eq "$count"
    for fail_at in 1 4 16; do
        if run_recipe "$fail_at"; then
            echo "$target ignored failure at $fail_at workers" >&2
            exit 1
        fi
        test "$(tail -n 1 "$tmp/calls")" = "$fail_at"
    done
done

echo "benchmark recipe failure checks OK"
