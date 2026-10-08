#!/bin/sh
# Self-reparse invariant: if formatting succeeds, formatting the output
# again must succeed and reproduce the same bytes (one pass). Development
# tool; not part of `bmake test`.
#
#   KNFMT=./knfmt sh tests/reparse.sh [file ...]

K="${KNFMT:-./knfmt}"
cd "$(dirname "$0")/.." || exit 1
files="$*"
[ -n "$files" ] || files="tests/*.c tests/*.h"

fail=0; n=0; na=0
for f in $files; do
	for opts in "" "-s"; do
		out=$(mktemp); out2=$(mktemp)
		if $K $opts "$f" >"$out" 2>/dev/null; then
			n=$((n + 1))
			if ! $K $opts "$out" >"$out2" 2>/dev/null ||
			    ! cmp -s "$out" "$out2"; then
				echo "REPARSE FAIL: $f opts='$opts'"
				fail=$((fail + 1))
			fi
		else
			na=$((na + 1))
		fi
		rm -f "$out" "$out2"
	done
done
echo "reparse: accepted=$n rejected=$na fail=$fail"
[ "$fail" -eq 0 ]
