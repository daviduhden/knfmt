#!/bin/sh
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
# SPDX-License-Identifier: ISC
# Benchmark and distribution helper regressions, independent of knfmt builds.

_dir=${0%/*}
[ "${_dir}" != "$0" ] || _dir=.
exec prove -v "${_dir}/benchmark-tools.pl"
