#!/bin/sh

# Compare the formatter output of two builds over the fixture corpus. Any
# build-dependent output or exit status is a failure. This guards against the
# class of defect where sanitizer instrumentation changes parser behavior.

set -e

bin1="${1:-}"
bin2="${2:-}"
if [ -z "${bin1}" ] || [ -z "${bin2}" ]; then
	echo "usage: $0 <knfmt-1> <knfmt-2>" >&2
	exit 2
fi

src="$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

fail=0
count=0

for f in "${src}"/tests/*.c "${src}"/tests/*.h; do
	[ -f "${f}" ] || continue
	for opts in "" "-s" "-D" "-d"; do
		# shellcheck disable=SC2086
		"${bin1}" ${opts} "${f}" >"${tmp}/o1" 2>"${tmp}/e1" || r1=$?
		r1=${r1:-0}
		# shellcheck disable=SC2086
		"${bin2}" ${opts} "${f}" >"${tmp}/o2" 2>"${tmp}/e2" || r2=$?
		r2=${r2:-0}
		count=$((count + 1))
		if [ "${r1}" != "${r2}" ] ||
		    ! cmp -s "${tmp}/o1" "${tmp}/o2" ||
		    ! cmp -s "${tmp}/e1" "${tmp}/e2"; then
			echo "MISMATCH: ${f} opts='${opts}' rc ${r1}/${r2}" >&2
			fail=1
		fi
		unset r1 r2
	done
done

if [ "${fail}" -ne 0 ]; then
	exit 1
fi
echo "cross-build: OK (${count} comparisons)"
