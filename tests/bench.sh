#!/bin/sh
# Reproducible performance/RSS benchmark (development tool; not part of
# make test). Usage: KNFMT=./knfmt sh tests/bench.sh [max-terms ...]

_dir=${0%/*}
[ "${_dir}" != "$0" ] || _dir=.
exec perl "${_dir}/benchmark.pl" expression "$@"
