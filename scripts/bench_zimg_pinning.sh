#!/bin/sh
set -eu

bin=${1:?bench binary path required}
frames=${2:-200}

printf '%s\n' 'affinity,pin,threads,width,height,us_per_frame'
for affinity in all physical; do
    case $affinity in
        all) cpus=0-31 ;;
        physical) cpus=0-15 ;;
    esac
    for threads in 4 8 12 16; do
        for pin in 0 1; do
            row=$(taskset -c "$cpus" "$bin" "$threads" i420 \
                640 360 1280 720 "$frames" 1 "$pin")
            timing=${row##*,}
            printf '%s,%s,%s,%s,%s,%s\n' "$affinity" "$pin" \
                "$threads" 1280 720 "$timing"
        done
    done
done
