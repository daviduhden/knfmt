#!/bin/sh

# Semantic-preservation regression.
#
# Formatting changes layout, removes redundant semicolons and joins
# backslash-newline continuations, but it must not change the program's
# preprocessing tokens. Each accepted fixture is formatted in normal mode and
# the input and output are compared with tests/semanticoracle.pl, an
# independent preprocessing-token scanner (it does not use knfmt's lexer).
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

for _f in valid-*.c valid-*.h simple-*.c simple-*.h style-*.c style-*.h semantic-*.c; do
	[ -e "${_f}" ] || continue

	if ! ${EXEC:-} "${KNFMT}" "${_f}" >"${_wrkdir}/o1" 2>/dev/null; then
		continue
	fi

	_n=$((_n + 1))
	if ! perl semanticoracle.pl compare "${_f}" "${_wrkdir}/o1" \
		>"${_wrkdir}/why" 2>&1; then
		if [ "$(cat "${_wrkdir}/why")" = "ORACLE_UNSUPPORTED" ]; then
			continue
		fi
		echo "${_f}: $(cat "${_wrkdir}/why")" >&2
		_fail=$((_fail + 1))
	fi
done

echo "semantic: ${_n} files, ${_fail} content mismatch(es)"
[ "${_fail}" -eq 0 ]
