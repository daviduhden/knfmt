#!/bin/sh
# Ensure main include detection works with relative include(s).

set -e

[ -z "${VALGRINDRC:-}" ] || export "VALGRIND_OPTS=$(xargs <"${VALGRINDRC}")"

_wrkdir="$(mktemp -d "${TMPDIR:-/tmp}/knfmt.XXXXXX")"
trap 'rm -rf "${_wrkdir}"' EXIT
cd "${_wrkdir}"

cat <<'EOF' >.clang-format
---
IncludeBlocks: Regroup
IncludeCategories:
  - Regex: '^"libks/'
    Priority: 1
...
EOF

mkdir libks
cat <<'EOF' >libks/arena.c
#include "libks/arena.h"

#include "libks/arithmetic.h"
EOF

${EXEC:-} "${KNFMT}" -ds libks/arena.c
