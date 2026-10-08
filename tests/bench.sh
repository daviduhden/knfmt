#!/bin/sh
# Reproducible performance/RSS benchmark (development tool; not part of
# `bmake test`).  Usage: KNFMT=./knfmt sh tests/bench.sh [max-terms ...]

K="${KNFMT:-./knfmt}"
[ -x "$K" ] || K="./knfmt"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

terms="${*:-1000 2000 4000 8000 16000 32000 64000 100000 200000}"
for n in $terms; do
	python3 -c "open('$TMP/in.c','w').write('int x = ' + 'a+'*$n + 'a;\n')"
	in=$(wc -c <"$TMP/in.c")
	t0=$(date +%s.%N)
	if command -v /usr/bin/time >/dev/null 2>&1; then
		/usr/bin/time -f '%M' -o "$TMP/rss" \
		    "$K" "$TMP/in.c" >"$TMP/o1" 2>/dev/null || { echo "N=$n FAILED"; continue; }
		rss=$(cat "$TMP/rss")
	else
		"$K" "$TMP/in.c" >"$TMP/o1" 2>/dev/null || { echo "N=$n FAILED"; continue; }
		rss="-"
	fi
	t1=$(date +%s.%N)
	"$K" "$TMP/o1" >"$TMP/o2" 2>/dev/null
	cmp -s "$TMP/o1" "$TMP/o2" && idem=ok || idem=DIFF
	printf 'N=%-7s in=%-9s out=%-9s time=%-8s rssKB=%-8s idem=%s\n' \
	    "$n" "$in" "$(wc -c <"$TMP/o1")" "$(echo "$t1 - $t0" | bc)" "$rss" "$idem"
done
