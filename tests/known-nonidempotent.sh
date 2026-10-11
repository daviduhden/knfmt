#!/bin/sh

# Expected-failure test for known non-idempotent reproducers.
#
# Every file under known-nonidempotent/ is a malformed input that still
# violates F(F(x)) == F(x). This test FAILS if one of them starts passing, so
# a fix cannot silently leave the case forgotten: when it passes, move the file
# to tests/repro-idempotence-*.c and remove it here.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -dt knfmt.XXXXXX)"
trap 'rm -r "${_wrkdir}"' EXIT

_status=0

for _f in known-nonidempotent/*.c; do
	[ -e "${_f}" ] || continue

	if ! ${EXEC:-} "${KNFMT}" "${_f}" >"${_wrkdir}/o1" 2>/dev/null; then
		echo "${_f}: unexpectedly rejected" >&2
		_status=1
		continue
	fi
	if ! ${EXEC:-} "${KNFMT}" "${_wrkdir}/o1" >"${_wrkdir}/o2" 2>/dev/null; then
		echo "${_f}: unexpectedly rejected on the second pass" >&2
		_status=1
		continue
	fi
	if cmp -s "${_wrkdir}/o1" "${_wrkdir}/o2"; then
		echo "${_f}: now idempotent; move it to tests/repro-idempotence-*.c" >&2
		_status=1
	else
		echo "${_f}: known non-idempotent (expected)"
	fi
done

exit "${_status}"
