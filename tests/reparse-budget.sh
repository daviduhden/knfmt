#!/bin/sh
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

set -eu

_work=$(mktemp -d "${TMPDIR:-/tmp}/knfmt-reparse.XXXXXX")
trap 'rm -rf "${_work}"' 0

generate() {
	_generate_index=0
	while [ "${_generate_index}" -lt "$1" ]; do
		printf 'int v%s =\n#ifdef A\n1\n#else\n2\n#endif\n;\n' "${_generate_index}"
		_generate_index=$((_generate_index + 1))
	done
}

# A bounded number of initializer branches still formats normally.
generate 25 >"${_work}/small.c"
"${KNFMT}" -c /dev/null "${_work}/small.c" >"${_work}/one"
"${KNFMT}" -c /dev/null "${_work}/one" >"${_work}/two"
cmp "${_work}/one" "${_work}/two"

for _n in 100 1600; do
	generate "${_n}" >"${_work}/large.c"
	cp "${_work}/large.c" "${_work}/original"
	for _flags in '' s i; do
		_status=0
		"${KNFMT}" ${_flags:+-${_flags}} -c /dev/null "${_work}/large.c" \
		    >"${_work}/out" 2>"${_work}/err" || _status=$?
		[ "${_status}" -eq 1 ]
		[ ! -s "${_work}/out" ]
		grep -q 'preprocessor reparse limit exceeded' "${_work}/err"
		cmp "${_work}/original" "${_work}/large.c"
	done
done

# Many ordinary top-level branches do not repeatedly reparse the prefix.
{
	_linear_index=0
	while [ "${_linear_index}" -lt 2000 ]; do
		printf '#ifdef A\nint v%s;\n#endif\n' "${_linear_index}"
		_linear_index=$((_linear_index + 1))
	done
} >"${_work}/linear.c"
"${KNFMT}" -c /dev/null "${_work}/linear.c" >"${_work}/out"
