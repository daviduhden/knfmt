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
root="$(CDPATH='' cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/knfmt-tooling.XXXXXX")"
trap 'rm -rf "${tmp}"' EXIT HUP INT TERM

# Explicit unsupported instrumentation must fail during configure.
mkdir "${tmp}/configure"
# bmake's jobserver flags are not valid GNU make options. This isolated
# configure probe must not inherit the outer test runner's jobserver.
if (unset MAKEFLAGS MFLAGS MAKELEVEL
    cd "${tmp}/configure" && sh "${root}/configure" --sanitize knfmt-invalid-sanitizer) \
    >"${tmp}/log" 2>&1; then
    echo 'configure accepted an unsupported sanitizer' >&2
    exit 1
fi
grep -q 'requested sanitizer is unavailable' "${tmp}/log"
if sh "${root}/configure" --fuzz unknown >"${tmp}/log" 2>&1; then
    echo 'configure accepted an unknown fuzzer' >&2
    exit 1
fi

# A missing executable must fail before creating campaign output.
if (cd "${root}" && perl tests/mutate.pl "${tmp}/missing" 0 "${tmp}/unexpected") \
    >"${tmp}/log" 2>&1; then
    echo 'mutation driver accepted a missing harness' >&2
    exit 1
fi
[ ! -e "${tmp}/unexpected" ]
grep -q 'not an executable file' "${tmp}/log"

cat >"${tmp}/harness" <<'HARNESS'
#!/bin/sh
if [ -e "${TEST_COUNT_FILE}" ]; then
    case "${TEST_FAULT}" in
    exit) exit 7;;
    signal) kill -TERM "$$";;
    esac
fi
: >"${TEST_COUNT_FILE}"
cat >/dev/null
HARNESS
chmod u+x "${tmp}/harness"
export TEST_COUNT_FILE="${tmp}/count"
export TEST_FAULT=exit
if (cd "${root}" && perl tests/mutate.pl "${tmp}/harness" 1 "${tmp}/findings") \
    >"${tmp}/log" 2>&1; then
    echo 'mutation driver ignored a harness execution failure' >&2
    exit 1
fi
grep -q 'harness failed with exit 7' "${tmp}/log"
rm "${TEST_COUNT_FILE}"
TEST_FAULT=signal
export TEST_FAULT
if (cd "${root}" && perl tests/mutate.pl "${tmp}/harness" 1 "${tmp}/findings") \
    >"${tmp}/log" 2>&1; then
    echo 'mutation driver ignored a harness signal' >&2
    exit 1
fi
grep -q 'CRASH sig=' "${tmp}/log"
# The separate semantic formatter must not hide a crash either.
rm "${TEST_COUNT_FILE}"
if (cd "${root}" && KNFMT="${tmp}/harness" perl tests/mutate.pl /bin/cat 1 "${tmp}/findings") \
    >"${tmp}/log" 2>&1; then
    echo 'mutation driver ignored a formatter signal' >&2
    exit 1
fi
grep -q 'CRASH formatter sig=' "${tmp}/log"
echo 'tooling: OK'
