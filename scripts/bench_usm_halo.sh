#!/bin/sh
set -eu

bin=${1:?bench binary path required}
frames=${2:-300}
amount=${3:-20}

printf '%s\n' 'alias,threads,width,height,frames,amount,us_per_frame'
for size in '1280 720' '1920 1080' '2560 1440'; do
    width=${size% *}
    height=${size#* }
    for threads in 1 4 8 12; do
        for alias in out in; do
            row=$("$bin" "$threads" "$width" "$height" \
                "$frames" "$amount" rand "$alias")
            timing=${row##*,}
            printf '%s,%s,%s,%s,%s,%s,%s\n' "$alias" "$threads" \
                "$width" "$height" "$frames" "$amount" "$timing"
        done
    done
done
