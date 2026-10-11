#!/bin/sh

# Expected-failure test for known-broken malformed reproducers.
#
# Every file under known-nonidempotent/ is an input the formatter still
# mishandles: it is not idempotent, or it loses/duplicates source content, or
# both. This test FAILS when a case becomes both idempotent and
# content-preserving, so a fix cannot silently leave the case forgotten: move
# the file to tests/repro-idempotence-*.c when that happens.
#
# Content preservation is checked on the whitespace-stripped text, which
# formatting must keep unchanged in the normal mode exercised here.

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
		# The first pass produced input the formatter itself rejects:
		# recovery corrupted it into something unparseable. That is still
		# a known failure, not proof of correctness.
		echo "${_f}: known failure (expected)"
		continue
	fi

	tr -d '[:space:]' <"${_f}" >"${_wrkdir}/c1"
	tr -d '[:space:]' <"${_wrkdir}/o1" >"${_wrkdir}/c2"

	if cmp -s "${_wrkdir}/o1" "${_wrkdir}/o2" &&
	    cmp -s "${_wrkdir}/c1" "${_wrkdir}/c2"; then
		echo "${_f}: now correct; move it to tests/repro-idempotence-*.c" >&2
		_status=1
	else
		echo "${_f}: known failure (expected)"
	fi
done

exit "${_status}"
