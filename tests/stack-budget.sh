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

_work=$(mktemp -d "${TMPDIR:-/tmp}/knfmt-stack.XXXXXX")
trap 'rm -rf "${_work}"' 0

repeat() {
	_repeat_left=$2
	while [ "${_repeat_left}" -gt 0 ]; do
		printf '%s' "$1"
		_repeat_left=$((_repeat_left - 1))
	done
}

for _kind in expr stmt decl; do
	case "${_kind}" in
	expr | decl)
		if [ "${_kind}" = expr ]; then
			printf 'int x = '
		else
			printf 'int '
		fi
		repeat '(' 2000
		if [ "${_kind}" = expr ]; then
			printf '1'
		else
			printf 'x'
		fi
		repeat ')' 2000
		printf ';\n'
		;;
	stmt)
		printf 'void f(void) {'
		repeat 'if (1) ' 2000
		printf ';}\n'
		;;
	esac >"${_work}/input.c"
	_status=0
	# BSD and GNU shells support these resource-limit options.
	# shellcheck disable=SC3045
	(ulimit -c 0; ulimit -s 512; "${KNFMT}" -c /dev/null "${_work}/input.c") \
	    >"${_work}/out" 2>"${_work}/err" || _status=$?
	[ "${_status}" -eq 1 ]
	[ ! -s "${_work}/out" ]
	[ -s "${_work}/err" ]
done

# Reject the deep file but continue processing later command-line inputs.
printf 'int valid;\n' >"${_work}/good.c"
"${KNFMT}" -c /dev/null "${_work}/good.c" >"${_work}/expected"
_status=0
# BSD and GNU shells support these resource-limit options.
# shellcheck disable=SC3045
(ulimit -c 0; ulimit -s 512; "${KNFMT}" -c /dev/null \
    "${_work}/input.c" "${_work}/good.c") \
    >"${_work}/out" 2>"${_work}/err" || _status=$?
[ "${_status}" -eq 1 ]
cmp "${_work}/expected" "${_work}/out"
