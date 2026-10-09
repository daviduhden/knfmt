#!/bin/sh

# One-pass idempotence for recovered/malformed input: a successful formatting
# pass must produce bytes that the next pass leaves unchanged. This guards the
# fuzz-discovered reproducers tests/repro-idempotence-001.c and
# tests/repro-idempotence-002.c, whose layout previously drifted by one
# operator per pass because a line break was repeatedly moved onto the token
# following an operator.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -dt knfmt.XXXXXX)"
trap 'rm -r $_wrkdir' EXIT

# stable <file> [flags]: format, then format the result again, and require the
# two outputs to be byte-identical.
stable() {
	local _f="$1" _flags="$2"

	${EXEC:-} "${KNFMT}" ${_flags:+-${_flags}} "${_f}" \
	    >"${_wrkdir}/o1" 2>"${_wrkdir}/e1"
	${EXEC:-} "${KNFMT}" ${_flags:+-${_flags}} "${_wrkdir}/o1" \
	    >"${_wrkdir}/o2" 2>"${_wrkdir}/e2"
	cmp -s "${_wrkdir}/o1" "${_wrkdir}/o2"
}

for _f in repro-idempotence-*.c; do
	stable "${_f}"
	stable "${_f}" "s"
done
