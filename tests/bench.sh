#!/bin/sh
# Reproducible performance/RSS benchmark (development tool; not part of
# `make test`). Usage: KNFMT=./knfmt sh tests/bench.sh [max-terms ...]

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

terms="${*:-1000 2000 4000 8000 16000 32000 64000 100000 200000}"
for n in ${terms}; do
	perl -e 'print "int x = " . ("a+" x $ARGV[0]) . "a;\n"' "${n}" >"${TMP}/in.c"
	in=$(wc -c <"${TMP}/in.c")
	t0=$(perl -MTime::HiRes=time -e 'printf "%.9f\n", time')
	rss=$(rss_of "${TMP}/o1" "${TMP}/rss" "${K}" "${TMP}/in.c") || {
		echo "N=${n} FAILED"
		continue
	}
	t1=$(perl -MTime::HiRes=time -e 'printf "%.9f\n", time')
	"${K}" "${TMP}/o1" >"${TMP}/o2" 2>/dev/null
	cmp -s "${TMP}/o1" "${TMP}/o2" && idem=ok || idem=DIFF
	_t=$(perl -e 'printf "%.3f", $ARGV[0] - $ARGV[1]' "${t1}" "${t0}")
	printf 'N=%-7s in=%-9s out=%-9s time=%-8s rssKB=%-8s idem=%s\n' \
		"${n}" "${in}" "$(wc -c <"${TMP}/o1")" "${_t}" "${rss}" "${idem}"
done
