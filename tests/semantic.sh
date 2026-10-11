#!/bin/sh

# Semantic-preservation regression.
#
# In normal mode the formatter changes layout, removes redundant semicolons and
# backslash-newline continuations, and may relocate a comment within a line.
# It must not lose or duplicate code tokens. This test formats each accepted
# fixture and compares the non-comment, non-whitespace, non-semicolon,
# non-backslash byte sequence before and after. It catches declaration/statement
# loss and duplication that the idempotence oracle cannot see.
#
# The comparison is independent of the formatter's lexer. It deliberately
# ignores comments (their placement is covered by the .ok fixtures) and is not
# applied to -s mode, where the simplifier legitimately rewrites tokens.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -dt knfmt.XXXXXX)"
trap 'rm -r "${_wrkdir}"' EXIT

# strip file: remove comments, whitespace, semicolons and backslashes.
strip() {
	python3 - "$1" <<-'PY'
	import re, sys
	d = open(sys.argv[1], "rb").read()
	d = re.sub(rb"\\\n", b"", d)            # translation phase 2 splicing
	d = re.sub(rb"/\*.*?\*/", b"", d, flags=re.S)
	d = re.sub(rb"//[^\n]*", b"", d)
	d = re.sub(rb"\s", b"", d)              # all whitespace
	d = re.sub(rb"[;\\]", b"", d)
	sys.stdout.buffer.write(d)
	PY
}

_fail=0
_n=0

for _f in valid-*.c valid-*.h simple-*.c simple-*.h style-*.c style-*.h semantic-*.c; do
	[ -e "${_f}" ] || continue

	if ! ${EXEC:-} "${KNFMT}" "${_f}" >"${_wrkdir}/o1" 2>/dev/null; then
		continue
	fi

	_n=$((_n + 1))
	strip "${_f}" >"${_wrkdir}/c1"
	strip "${_wrkdir}/o1" >"${_wrkdir}/c2"
	if ! cmp -s "${_wrkdir}/c1" "${_wrkdir}/c2"; then
		echo "${_f}: code content changed" >&2
		_fail=$((_fail + 1))
	fi
done

echo "semantic: ${_n} files, ${_fail} content mismatch(es)"
[ "${_fail}" -eq 0 ]
