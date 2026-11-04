#!/bin/sh
# Annotated-declaration benchmark (development tool; not part of make test).
# Usage: KNFMT=./knfmt sh tests/bench-decl.sh [counts ...]

_dir=${0%/*}
[ "${_dir}" != "$0" ] || _dir=.
exec perl "${_dir}/benchmark.pl" declaration "$@"
