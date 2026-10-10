#!/bin/sh

# Allocation fault injection (development tool; not part of `bmake test`).
#
# knfmt is linked with the wrapper in tests/fault-wrap.c so the FAULT_AT-th
# intercepted allocation returns NULL. For every failure point we require a
# clean, controlled outcome: no signal, no partial output on stdout, and an
# in-place target left byte-for-byte untouched on failure. Usage:
#
#   KFAULT=./knfmt-fault sh tests/fault.sh

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

KF="${KFAULT:-./knfmt-fault}"
[ -x "${KF}" ] || { echo "no fault binary: ${KF}" >&2; exit 2; }

srcdir="$(dirname "$0")/.."
wrkdir="$(mktemp -d)"
trap 'rm -rf "${wrkdir}"' EXIT

limit=200000

# run <file> [flags ...]: fail each allocation in turn until the program
# succeeds, requiring a controlled exit and no partial stdout in between.
run() {
	local _in="$1"; shift
	local _n=0 _rc

	while [ "${_n}" -lt "${limit}" ]; do
		_rc=0
		FAULT_AT="${_n}" "${KF}" "$@" "${_in}" \
		    >"${wrkdir}/out" 2>"${wrkdir}/err" || _rc=$?
		_rc=${_rc:-0}
		if [ "${_rc}" -eq 0 ]; then
			echo "fault: ${_in} ${*} 0..${_n} controlled"
			return 0
		fi
		if [ "${_rc}" -ne 1 ]; then
			echo "FAIL: ${_in} ${*} FAULT_AT=${_n} rc=${_rc}" >&2
			return 1
		fi
		if [ -s "${wrkdir}/out" ]; then
			echo "FAIL: ${_in} ${*} FAULT_AT=${_n} partial output" >&2
			return 1
		fi
		_n=$((_n + 1))
	done
	echo "FAIL: ${_in} did not converge" >&2
	return 1
}

# run_inplace <file>: on failure the target must be unchanged.
run_inplace() {
	local _src="$1"
	local _n=0 _rc
	local _f="${wrkdir}/in.c" _orig="${wrkdir}/orig.c"

	cp "${_src}" "${_orig}"
	while [ "${_n}" -lt "${limit}" ]; do
		cp "${_orig}" "${_f}"
		_rc=0
		FAULT_AT="${_n}" "${KF}" -i "${_f}" \
		    >"${wrkdir}/out" 2>"${wrkdir}/err" || _rc=$?
		_rc=${_rc:-0}
		if [ "${_rc}" -eq 0 ]; then
			echo "fault: ${_src} -i 0..${_n} controlled"
			return 0
		fi
		if [ "${_rc}" -ne 1 ]; then
			echo "FAIL: ${_src} -i FAULT_AT=${_n} rc=${_rc}" >&2
			return 1
		fi
		if ! cmp -s "${_f}" "${_orig}"; then
			echo "FAIL: ${_src} -i FAULT_AT=${_n} modified file" >&2
			return 1
		fi
		_n=$((_n + 1))
	done
	return 1
}

for _f in \
    "${srcdir}/tests/valid-001.c" \
    "${srcdir}/tests/diff-014.c" \
    "${srcdir}/tests/repro-idempotence-002.c" \
    "${srcdir}/tests/repro-idempotence-003.c" \
    "${srcdir}/tests/simple-attributes-003.c"; do
	run "${_f}"
	run "${_f}" -s
done
run "${srcdir}/tests/valid-001.c" -d
run_inplace "${srcdir}/tests/valid-001.c"
run_inplace "${srcdir}/tests/repro-idempotence-003.c"
