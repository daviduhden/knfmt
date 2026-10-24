#!/bin/sh
# No such file or directory.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -d "${TMPDIR:-/tmp}/knfmt.XXXXXX")"
trap 'rm -rf "${_wrkdir}"' EXIT
_out="${_wrkdir}/out"

(cd "${_wrkdir}" && ${EXEC:-} "${KNFMT}" -c enoent) >"${_out}" 2>&1 && exit 1
diff -u "${_out}" - <<EOF
knfmt: enoent: No such file or directory
EOF
