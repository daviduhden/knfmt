#!/bin/sh
# Exercise diff mode: -d must report a difference and keep stderr empty.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_out="$(mktemp "${TMPDIR:-/tmp}/knfmt-diff.XXXXXX")"
_err="$(mktemp "${TMPDIR:-/tmp}/knfmt-diff.XXXXXX")"
trap 'rm -f "${_out}" "${_err}"' EXIT

if ${EXEC:-} "${KNFMT}" -d >"${_out}" 2>"${_err}" <<EOF
int main(void) { return 0; }
EOF
then
	echo "diff.sh: expected a non-zero exit status" >&2
	exit 1
fi

if [ ! -s "${_out}" ]; then
	echo "diff.sh: expected a diff on standard output" >&2
	exit 1
fi

if [ -s "${_err}" ]; then
	cat "${_err}" >&2
	exit 1
fi
