# Raw-byte input domain: newline conventions, high-bit bytes, missing final
# newline, and backslash-newline splicing. knfmt is byte oriented: high-bit
# bytes are preserved verbatim, CRLF is accepted as a line boundary, a lone CR
# is not, and a missing final newline is added. Successful output is one-pass
# idempotent.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -dt knfmt.XXXXXX)"
trap 'rm -r $_wrkdir' EXIT

# run <file>: format in place, then format the result again and require the
# same bytes.
stable() {
	local _f="$1"

	(cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" "${_f}") >"${_wrkdir}/o1" 2>"${_wrkdir}/e1"
	(cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" "${_wrkdir}/o1") \
	    >"${_wrkdir}/o2" 2>"${_wrkdir}/e2"
	cmp -s "${_wrkdir}/o1" "${_wrkdir}/o2"
}

# CRLF is accepted and normalized to LF.
printf 'int x = 1;\r\nint y = 2;\r\n' >"${_wrkdir}/crlf.c"
stable crlf.c
printf 'int x = 1;\nint y = 2;\n' >"${_wrkdir}/crlf.ok"
cmp -s "${_wrkdir}/o1" "${_wrkdir}/crlf.ok"

# A lone CR is not a line boundary; formatting fails cleanly with no output.
printf 'int x = 1;\rint y = 2;\r' >"${_wrkdir}/cr.c"
if (cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" cr.c) \
    >"${_wrkdir}/o1" 2>"${_wrkdir}/e1"; then
	exit 1
fi
[ ! -s "${_wrkdir}/o1" ]

# High-bit bytes are opaque and preserved.
printf '/* \377\376 */\nchar s[] = "\377\376";\n' >"${_wrkdir}/hi.c"
stable hi.c

# Missing final newline is accepted and a newline is added.
printf 'int x = 1;' >"${_wrkdir}/nonl.c"
stable nonl.c
printf 'int x = 1;\n' >"${_wrkdir}/nonl.ok"
cmp -s "${_wrkdir}/o1" "${_wrkdir}/nonl.ok"

# Backslash-newline splicing is processed in translation phase 2.
printf 'int x = \\\n1;\n' >"${_wrkdir}/splice.c"
stable splice.c
printf '// c\\\nint y = 2;\n' >"${_wrkdir}/cslice.c"
stable cslice.c
