#!/bin/sh

# Semantic-preservation regression.
#
# Formatting must preserve preprocessing tokens, including semicolons and
# stray backslashes. Historical violations are tracked by exact input/output
# SHA-256 pairs in semantic-known.tsv; the general oracle remains strict.
#
# The comparison detects token loss, duplication, reordering, operator and
# literal changes, directive changes and branch swaps. Comments may be
# relocated, so only their multiset of spellings is compared. It is not applied
# to -s mode, where the simplifier legitimately rewrites tokens.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

perl semanticoracle.pl --selftest >/dev/null

_wrkdir="$(mktemp -d "${TMPDIR:-/tmp}/knfmt.XXXXXX")"
trap 'rm -r "${_wrkdir}"' EXIT

_fail=0
_n=0
_known=0
_unsupported=0
_rejected=0
_checked=0

for _f in valid-*.c valid-*.h simple-*.c simple-*.h style-*.c style-*.h semantic-*.c; do
	[ -e "${_f}" ] || continue

	if ${EXEC:-} "${KNFMT}" "${_f}" >"${_wrkdir}/o1" 2>/dev/null; then
		:
	else
		_rc=$?
		if [ "${_rc}" -eq 1 ]; then
			_rejected=$((_rejected + 1))
		else
			echo "${_f}: formatter failed with exit ${_rc}" >&2
			_fail=$((_fail + 1))
		fi
		continue
	fi

	_n=$((_n + 1))
	if ! perl semanticoracle.pl compare-known "${_f}" "${_wrkdir}/o1" \
		>"${_wrkdir}/why" 2>&1; then
		if [ "$(cat "${_wrkdir}/why")" = "ORACLE_UNSUPPORTED" ]; then
			_unsupported=$((_unsupported + 1))
			continue
		fi
		echo "${_f}: $(cat "${_wrkdir}/why")" >&2
		_fail=$((_fail + 1))
	elif [ -s "${_wrkdir}/why" ]; then
		cat "${_wrkdir}/why"
		_known=$((_known + 1))
	else
		_checked=$((_checked + 1))
	fi
done

_expected_known=$(awk '!/^#/ && NF { n++ } END { print n + 0 }' semantic-known.tsv)
if [ "${_known}" -ne "${_expected_known}" ]; then
	echo "semantic: known exception missing, rejected or changed; review manifest" >&2
	_fail=$((_fail + 1))
fi

echo "semantic: accepted=${_n} checked=${_checked} rejected=${_rejected} failures=${_fail} known=${_known} unsupported=${_unsupported}"
[ "${_fail}" -eq 0 ]
