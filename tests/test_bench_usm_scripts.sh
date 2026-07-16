#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
halo="$repo_root/scripts/bench_usm_halo.sh"
isa="$repo_root/scripts/bench_usm_isa.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/vlc-autoscaler-bench-usm.XXXXXX")
fake="$tmp/fake-benchmark"

cleanup() {
    rm -rf -- "$tmp"
}
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

fail() {
    echo "USM benchmark script test failed: $*" >&2
    exit 1
}

cat >"$fake" <<'EOF'
#!/bin/sh
set -eu
printf '%s,%s,%s,%s,%s,%s,%s,1.00\n' \
    "$1" "$1" "$2" "$3" "$4" "$5" "$6"
EOF
chmod 0755 "$fake"

"$halo" "$fake" 7 19 >"$tmp/halo.csv"
test "$(wc -l <"$tmp/halo.csv")" -eq 25 || fail "halo row count"
awk -F, '
    NR == 1 { if ($0 != "alias,threads,width,height,frames,amount,us_per_frame") exit 1; next }
    NF != 7 || ($1 != "out" && $1 != "in") || $2 !~ /^[0-9]+$/ ||
        $3 !~ /^[0-9]+$/ || $4 !~ /^[0-9]+$/ || $5 != 7 || $6 != 19 ||
        $7 != "1.00" { exit 1 }
    { rows++ }
    END { exit rows != 24 }
' "$tmp/halo.csv" || fail "halo CSV schema"

"$isa" "$fake" "$fake" "$fake" >"$tmp/isa.csv"
test "$(wc -l <"$tmp/isa.csv")" -eq 37 || fail "ISA row count"
awk -F, '
    NR == 1 { if ($0 != "isa,requested_threads,effective_threads,width,height,frames,amount,fill,us_per_frame") exit 1; next }
    NF != 9 || ($1 != "sse2" && $1 != "avx2" && $1 != "avx512") ||
        $2 !~ /^[0-9]+$/ || $3 !~ /^[0-9]+$/ || $4 !~ /^[0-9]+$/ ||
        $5 !~ /^[0-9]+$/ || $6 != 300 || $7 != 20 || $8 != "rand" ||
        $9 != "1.00" { exit 1 }
    { rows++ }
    END { exit rows != 36 }
' "$tmp/isa.csv" || fail "ISA CSV schema"

echo "USM benchmark script checks OK"
