#!/bin/sh
# Every top-level command target must be declared .PHONY, or a same-named
# file in the repo root silently satisfies it and skips the recipe
# (BUILD-10: the bench-* trio regressed exactly this way).
set -eu

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
makefile="$repo_root/Makefile"

phony=$(grep -E '^\.PHONY:' "$makefile" | sed 's/^\.PHONY://' | tr '\n' ' ')
missing=0
while IFS= read -r target; do
    case " $phony " in
        *" $target "*) ;;
        *)
            echo "Makefile phony test failed: '$target' not in .PHONY" >&2
            missing=1
            ;;
    esac
done <<EOF
$(grep -E '^[a-z][a-z0-9-]*:' "$makefile" | cut -d: -f1 | sort -u)
EOF
[ "$missing" -eq 0 ]

echo "Makefile .PHONY coverage OK"
