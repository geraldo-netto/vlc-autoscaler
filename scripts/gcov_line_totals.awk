BEGIN {
    FS = ":"
    malformed = 0
}

NF >= 3 {
    count = $1
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", count)
    line_number = $2
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", line_number)

    if (count == "-")
        next

    if (count == "#####" || count == "=====") {
        runnable[line_number] = 1
        next
    }

    if (count !~ /^[0-9]+[*]?$/) {
        printf "ERROR: malformed gcov count at %s:%d: %s\n", \
               FILENAME, FNR, count > "/dev/stderr"
        malformed = 1
        next
    }

    runnable[line_number] = 1
    sub(/[*]$/, "", count)
    if (count ~ /[1-9]/)
        covered[line_number] = 1
}

END {
    if (malformed)
        exit 2

    total = 0
    hit = 0
    for (line_number in runnable) {
        total++
        if (line_number in covered)
            hit++
    }
    print total, hit
}
