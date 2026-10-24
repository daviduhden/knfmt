#!/bin/sh
# Inplace edit of stdin does not make sense.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -d "${TMPDIR:-/tmp}/knfmt.XXXXXX")"
trap 'rm -rf "${_wrkdir}"' EXIT
cd "${_wrkdir}"

printf 'int\na;\n' | ${EXEC:-} "${KNFMT}" -i >"${_wrkdir}/out" 2>&1 && exit 1
if ! grep -q 'usage' "${_wrkdir}/out"; then
	cat "${_wrkdir}/out"
	exit 1
fi

# Inplace edit in diff mode is considered valid.
${EXEC:-} "${KNFMT}" -Di </dev/null
