# NUL bytes are ordinary bytes: preserved where the lexer can carry them
# (e.g. inside a comment), otherwise a clean parse error with no output.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -dt knfmt.XXXXXX)"
trap 'rm -r $_wrkdir' EXIT
_out="${_wrkdir}/out"
_err="${_wrkdir}/err"

# A NUL inside a comment is preserved verbatim.
printf '/*\000*/\n' >"${_wrkdir}/nul-comment.c"
printf '/*\000*/\n' >"${_wrkdir}/nul-comment.ok"
(cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" nul-comment.c) >"${_out}" 2>"${_err}"
cmp -s "${_out}" "${_wrkdir}/nul-comment.ok"

# A NUL that forms an unexpected token is rejected without partial output.
printf 'int x = 1;\000int y = 2;\n' >"${_wrkdir}/nul-mid.c"
if (cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" nul-mid.c) >"${_out}" 2>"${_err}"; then
	exit 1
fi
[ ! -s "${_out}" ]
