#!/bin/sh
# Annotated-declaration benchmark (development tool; not part of bmake test).
# Usage: KNFMT=./knfmt sh tests/bench-decl.sh [counts ...]

K="${KNFMT:-./knfmt}"
[ -x "$K" ] || K="./knfmt"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
prev=""
for n in ${*:-320 640 1280 2560 5120 10240}; do
	python3 -c "open('$TMP/d.c','w').write('\n'.join('int f%d () A B;'%i for i in range($n))+'\n')"
	t0=$(date +%s.%N)
	if command -v /usr/bin/time >/dev/null 2>&1; then
		/usr/bin/time -f '%M' -o "$TMP/rss" "$K" "$TMP/d.c" >"$TMP/o1" 2>/dev/null || { echo "N=$n FAILED"; continue; }
		rss=$(cat "$TMP/rss")
	else
		"$K" "$TMP/d.c" >"$TMP/o1" 2>/dev/null || { echo "N=$n FAILED"; continue; }
		rss="-"
	fi
	t1=$(date +%s.%N)
	"$K" "$TMP/o1" >"$TMP/o2" 2>/dev/null
	cmp -s "$TMP/o1" "$TMP/o2" && idem=ok || idem=DIFF
	t=$(echo "$t1 - $t0" | bc)
	ratio="-"
	[ -n "$prev" ] && ratio=$(echo "scale=2; $t / $prev" | bc)
	printf 'N=%-6s in=%-8s time=%-8s rssKB=%-7s ratio=%-5s idem=%s\n' \
	    "$n" "$(wc -c <"$TMP/d.c")" "$t" "$rss" "$ratio" "$idem"
	prev="$t"
done
