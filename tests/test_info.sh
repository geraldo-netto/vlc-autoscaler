#!/bin/sh
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

expect_line() {
    output=$1
    expected=$2
    if ! printf '%s\n' "$output" | grep -Fqx "$expected"; then
        echo "make info test failed: missing '$expected'" >&2
        exit 1
    fi
}

enabled=$(make -s -C "$repo_root" info HAVE_ZIMG=1 \
    MARCH=x86-64 MULTIVERSION=1)
expect_line "$enabled" "zimg backend   : ENABLED"
expect_line "$enabled" "MARCH          : x86-64"
expect_line "$enabled" "MULTIVERSION   : 1"

disabled=$(make -s -C "$repo_root" info HAVE_ZIMG= \
    MARCH=x86-64-v3 MULTIVERSION=0)
expect_line "$disabled" "zimg backend   : disabled"
expect_line "$disabled" "MARCH          : x86-64-v3"
expect_line "$disabled" "MULTIVERSION   : 0"

echo "make info configuration checks OK"
