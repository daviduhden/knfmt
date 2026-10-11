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

# A NUL inside a multi-line comment is preserved. Reindentation now carries
# the explicit length instead of relying on NUL termination.
printf '/*\n * a\000b\n */\n' >"${_wrkdir}/nul-comment-ml.c"
printf '/*\n * a\000b\n */\n' >"${_wrkdir}/nul-comment-ml.ok"
(cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" nul-comment-ml.c) >"${_out}" 2>"${_err}"
cmp -s "${_out}" "${_wrkdir}/nul-comment-ml.ok"

# A NUL inside a multi-line macro is preserved. Realignment now carries the
# explicit length instead of relying on NUL termination.
printf '#define X a\000b \\\n\tc \\\n\td\nint y;\n' >"${_wrkdir}/nul-define.c"
printf '#define X a\000b \\\n\tc \\\n\td\nint y;\n' >"${_wrkdir}/nul-define.ok"
(cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" nul-define.c) >"${_out}" 2>"${_err}"
cmp -s "${_out}" "${_wrkdir}/nul-define.ok"

# A NUL that forms an unexpected token is rejected without partial output.
printf 'int x = 1;\000int y = 2;\n' >"${_wrkdir}/nul-mid.c"
if (cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" nul-mid.c) >"${_out}" 2>"${_err}"; then
	exit 1
fi
[ ! -s "${_out}" ]

# A stray non-white-space byte at end of file is rejected, not silently
# dropped when it is the last byte of the input.
printf 'int x;\032' >"${_wrkdir}/nul-trailing.c"
if (cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" nul-trailing.c) >"${_out}" 2>"${_err}"; then
	exit 1
fi
[ ! -s "${_out}" ]
