#!/bin/sh
# Annotated-declaration benchmark (development tool; not part of make test).
# Usage: KNFMT=./knfmt sh tests/bench-decl.sh [counts ...]

K="${KNFMT:-./knfmt}"
[ -x "${K}" ] || K="./knfmt"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/knfmt.XXXXXX")
trap 'rm -rf "${TMP}"' EXIT

# Run a command and print peak RSS in KiB when the system time(1) supports
# the BSD -l format (OpenBSD, FreeBSD, macOS). Otherwise run the command
# without an RSS measurement; no GNU-specific time flags are required.
rss_of() {
	_rss_out="$1"
	_rss_file="$2"
	shift 2

	if command -v /usr/bin/time >/dev/null 2>&1; then
		if /usr/bin/time -l "$@" >"${_rss_out}" 2>"${_rss_file}"; then
			_rss_kb=$(awk '/maximum resident set size/ {
				printf "%d", $1 / 1024
				exit
			}' "${_rss_file}")
			if [ -n "${_rss_kb}" ]; then
				echo "${_rss_kb}"
			else
				echo "-"
			fi
			return 0
		fi
	fi

	if "$@" >"${_rss_out}" 2>/dev/null; then
		echo "-"
		return 0
	fi
	return 1
}

prev=""
if [ "$#" -eq 0 ]; then
	set -- 320 640 1280 2560 5120 10240
fi
for n in "$@"; do
	perl -e 'for my $i (0 .. $ARGV[0] - 1) { print "int f$i () A B;\n"; }' "${n}" >"${TMP}/d.c"
	t0=$(perl -MTime::HiRes=time -e 'printf "%.9f\n", time')
	rss=$(rss_of "${TMP}/o1" "${TMP}/rss" "${K}" "${TMP}/d.c") || {
		echo "N=${n} FAILED"
		continue
	}
	t1=$(perl -MTime::HiRes=time -e 'printf "%.9f\n", time')
	"${K}" "${TMP}/o1" >"${TMP}/o2" 2>/dev/null
	cmp -s "${TMP}/o1" "${TMP}/o2" && idem=ok || idem=DIFF
	_t=$(perl -e 'printf "%.3f", $ARGV[0] - $ARGV[1]' "${t1}" "${t0}")
	ratio="-"
	if [ -n "${prev}" ]; then
		ratio=$(perl -e 'print $ARGV[1] ? sprintf("%.2f", $ARGV[0] / $ARGV[1]) : "-"' "${_t}" "${prev}")
	fi
	printf 'N=%-6s in=%-8s time=%-8s rssKB=%-7s ratio=%-5s idem=%s\n' \
		"${n}" "$(wc -c <"${TMP}/d.c")" "${_t}" "${rss}" "${ratio}" "${idem}"
	prev="${_t}"
done
