#!/bin/sh
set -eu

if test "$#" -ne 3; then
    echo "usage: $0 <sse2-bench> <avx2-bench> <avx512-bench>" >&2
    exit 2
fi
b_sse2=$1
b_avx2=$2
b_avx512=$3

frames=${BENCH_FRAMES:-300}
amount=${BENCH_AMOUNT:-20}

printf '%s\n' 'isa,threads,width,height,frames,amount,fill,us_per_frame'
for size in '1280 720' '1920 1080' '2560 1440'; do
    set -- $size
    width=$1
    height=$2
    for threads in 1 4 8 12; do
        isa=sse2
        for bin in "$b_sse2" "$b_avx2" "$b_avx512"; do
            row=$($bin "$threads" "$width" "$height" \
                "$frames" "$amount" rand out)
            printf '%s,%s\n' "$isa" "$row"
            case $isa in
                sse2) isa=avx2 ;;
                avx2) isa=avx512 ;;
            esac
        done
    done
done
