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
#
# Exercise path decoding, guard identifiers and incomplete regex entries.

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"
_wrkdir="$(mktemp -d "${TMPDIR:-/tmp}/knfmt-inputs.XXXXXX")"
trap 'rm -rf "${_wrkdir}"' EXIT
cd "${_wrkdir}"
printf 'BasedOnStyle: OpenBSD\n' >.knfmt
printf 'int\nf(void)\n{\n\treturn 0;\n}\n' >expected

# A tab separates the optional unified diff timestamp; spaces belong to paths.
for _name in 'space name.c' 'quote"name.c' 'back\slash.c' "$(printf 'tab\tname.c')"; do
	printf 'int f( void ){return 0;}\n' >"${_name}"
	case "${_name}" in
	'space name.c') _header='b/space name.c' ;;
	'quote"name.c') _header='"b/quote\"name.c"' ;;
	'back\slash.c') _header='"b/back\\slash.c"' ;;
	*) _header='"b/tab\011name.c"' ;;
	esac
	{
		printf '+++ %s\t2026-01-01\n' "${_header}"
		printf '@@ -1 +1 @@\n+int f( void ){return 0;}\n'
	} | ${EXEC:-} "${KNFMT}" -Di
	cmp expected "${_name}"
done

# Reject malformed quoting and embedded NUL rather than opening a partial name.
for _header in '"b/space name.c' '"b/bad\000name.c"' '"b/bad\qname.c"' '"b/bad\0"'; do
	if printf '+++ %s\n' "${_header}" | ${EXEC:-} "${KNFMT}" -Di >out 2>err; then
		echo 'malformed diff path accepted' >&2
		exit 1
	fi
done

cat >.knfmt <<'STYLE'
BasedOnStyle: OpenBSD
IncludeGuards:
  - Regex: '\.h$'
    PathComponents: 1
STYLE
for _name in 'a b.h' '1.h' 'a+b.h'; do
	printf 'extern int x;\n' >"${_name}"
	${EXEC:-} "${KNFMT}" -i "${_name}"
	case "${_name}" in
	'a b.h') _guard=A_B_H ;;
	'1.h') _guard=_1_H ;;
	*) _guard=A_B_H ;;
	esac
	grep -qx "#ifndef ${_guard}" "${_name}"
	cp "${_name}" before
	${EXEC:-} "${KNFMT}" -i "${_name}"
	cmp before "${_name}"
done

# Missing, invalid and redefined patterns must never reach regexec uncompiled.
for _section in IncludeCategories IncludeGuards; do
	for _regex in missing invalid redefine; do
		{
			printf 'BasedOnStyle: OpenBSD\n%s:\n' "${_section}"
			case "${_regex}" in
			missing) printf '  - Priority: 1\n' ;;
			invalid) printf "  - Regex: '('\n" ;;
			redefine) printf "  - Regex: '.*'\n    Regex: '('\n" ;;
			esac
		} >.knfmt
		printf '#include "z.h"\n#include "a.h"\n' >regex.h
		${EXEC:-} "${KNFMT}" -s -tss regex.h >out 2>err
	done
done

# A failed category replacement retains the usable earlier pattern and priority.
cat >.knfmt <<'STYLE'
BasedOnStyle: OpenBSD
IncludeBlocks: Regroup
IncludeCategories:
  - Priority: -1
    Regex: '^"z'
    Regex: '('
STYLE
${EXEC:-} "${KNFMT}" -s regex.h >out 2>err
head -n 1 out | grep -qx '#include "z.h"'

# Enable guards before the invalid section, then exercise its retained pattern.
cat >.knfmt <<'STYLE'
BasedOnStyle: OpenBSD
IncludeGuards:
  - Regex: '^never$'
IncludeGuards:
  - Regex: '.*'
    Regex: '('
STYLE
${EXEC:-} "${KNFMT}" -s regex.h >out 2>err
grep -qx '#ifndef REGEX_H' out
