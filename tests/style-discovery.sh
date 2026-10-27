#!/bin/sh

set -eu

: "${KNFMT:=knfmt}"
_tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/knfmt-style.XXXXXX")
trap 'rm -rf "$_tmpdir"' 0 HUP INT TERM

mkdir "$_tmpdir/work"
cat >"$_tmpdir/.knfmt" <<'EOF'
---
BasedOnStyle: OpenBSD
ColumnLimit: 100
IncludeGuards:
  - Regex: 'fixture\.h'
...
EOF
cat >"$_tmpdir/.clang-format" <<'EOF'
---
BasedOnStyle: LLVM
ColumnLimit: 1
...
EOF

if ! (cd "$_tmpdir/work" &&
	printf 'int main(void) { return 0; }\n' | "$KNFMT" -tss >/dev/null) \
	2>"$_tmpdir/trace"; then
	cat "$_tmpdir/trace" 1>&2
	exit 1
fi

if ! grep -q '^\[s\] BasedOnStyle: OpenBSD$' "$_tmpdir/trace"; then
	cat "$_tmpdir/trace" 1>&2
	exit 1
fi
if ! grep -Fq "[s] - Regex: 'fixture\.h'" "$_tmpdir/trace"; then
	cat "$_tmpdir/trace" 1>&2
	exit 1
fi

cat >"$_tmpdir/work/.clang-format" <<'EOF'
---
BasedOnStyle: LLVM
ColumnLimit: 80
...
EOF
if ! (cd "$_tmpdir/work" &&
	printf 'int main(void) { return 0; }\n' | "$KNFMT" -tss >/dev/null) \
	2>"$_tmpdir/trace"; then
	cat "$_tmpdir/trace" 1>&2
	exit 1
fi
if ! grep -q '^\[s\] BasedOnStyle: LLVM$' "$_tmpdir/trace"; then
	cat "$_tmpdir/trace" 1>&2
	exit 1
fi
