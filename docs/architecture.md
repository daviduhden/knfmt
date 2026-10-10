# knfmt architecture

This document describes the implementation as it exists. It intentionally
does not describe abstractions that are not present (for example there is no
`DOC_SEQ` node and no monolithic `knfmt_ctx`).

## Input and translation

- Input is **byte oriented**. High-bit bytes (0x80-0xFF) are opaque and
  preserved verbatim, including invalid UTF-8 sequences in comments and
  string literals. No UTF-8 decoding is performed.
- Newlines: LF is the line boundary; CRLF is accepted and normalized to LF;
  a lone CR is not a line boundary and is rejected cleanly (`tests/bytes.sh`).
- A missing terminal newline is accepted; one is added.
- Phase-2 backslash-newline splicing is recognised, including inside `//`
  comments and preprocessor directives.
- Trigraphs are recognised (`??/` behaves as a backslash for splicing; the
  punctuation mappings map to their tokens). knfmt has no language-version
  selector, so this translation behaviour is uniform.
- NUL bytes are tolerated where the lexer can carry them (e.g. comments) and
  otherwise produce a clean parse error (`tests/nul.sh`).

## Parser

- Invocation state is owned per file: `format_buffer()` creates a
  `struct clang`, `struct lexer` and `struct parser` per input; the shared
  `main_context` owns the arenas, style and simplifier state. Shared lookup
  tables (`clang_tokens`, `clang_identifiers`, `style` keywords, `expr`
  rules, `KS_str_match` statics) are initialised once and then read-only.
- Declaration classification uses a per-parser negative cache
  (`pr_decl_scan_beg`/`pr_decl_scan_end`/`pr_decl_scan_nolbrace`):
  `parser_type_decl_list_then_lbrace()` records that a scanned token range
  contains no declaration list followed by `{`, so a run of annotated
  declarations is not rescanned (near-linear).
- Expression parsing is recursive and guarded (`EXPR_MAX_DEPTH`); it is
  bounded by nesting, not by flat length.
- Parser outputs are initialised on every return path where a caller may
  observe them (`parser_expr()` sets `*expr = NULL` up front;
  `parser_attributes_expr()` falls back to the enclosing document).

## Document model

- `DOC_CONCAT` is an **n-ary list** (`LIST_INSERT_TAIL`, O(1) append); there
  is no left-nested binary concat tree.
- Flat binary expression chains are laid out **iteratively**
  (`expr_doc_binary_chain()`): an explicit frame array descends each level,
  builds the deepest operand, then unwinds emitting operators, groups, lines
  and right operands. Process-stack use is O(1) in the number of terms.
- Rendering is **iterative** (`doc_exec1()`): an explicit continuation-frame
  stack iterates `DOC_CONCAT` children and restores group mode/diff state
  after each `DOC_GROUP` child. Indent/scope/minimize helpers recurse only by
  nesting depth.
- `doc_summarize()` computes a cached, context-independent structural summary
  per node (saturated flat width and purity) in an iterative post-order;
  `doc_fits()` accepts or rejects a pure subtree whole (`DOC_WALK_SKIP`)
  instead of walking its left spine, making fitting near-linear.
- There is no document-height guard; `DOC_MAX_EXEC_DEPTH` was removed.

## Simplifier (`-s`)

- `simple_enter()`/`simple_leave()` track per-pass state; a pass is
  mutually exclusive with others and does not re-enter itself while enabled.
- `expr_doc_parens()` drops a redundant grouping pair and then calls
  `simple_leave()` **before** formatting the operand, so nested removable
  grouping and other simplifications are canonicalized in the same pass.
  Nesting terminates because each step descends into the expression. This
  yields one-pass `-s` idempotence without changing global re-entry rules.

## Verification

See `ARCHITECTURE-REWRITE-PROGRESS.md` for measured results. Commands:

```sh
LC_ALL=C bmake test                       # fixtures (C89-C23, GNU, error, diff)
KNFMT=./knfmt sh tests/reparse.sh         # one-pass stability, normal and -s
KNFMT=./knfmt sh tests/bytes.sh           # raw-byte domain
KNFMT=./knfmt sh tests/nul.sh             # NUL contract
KNFMT=./knfmt sh tests/bench.sh 10000 200000   # time + peak RSS
sh tests/cross-build.sh ./knfmt <clang-asan-knfmt>   # normal vs sanitized
```

Sanitizer builds (clean, compiler consistent): `CC=clang ./configure` then set
`DEBUG` to `-fsanitize=address` / `-fsanitize=undefined` /
`-fsanitize=unsigned-integer-overflow` with `-fno-sanitize-recover=all` and
rebuild with `rm -f *.o libks/*.o knfmt; bmake`.

Fuzzing: `CC=clang ./configure --fuzz llvm`, `bmake fuzz-parse`, then run
`./fuzz-parse <seed-dir>`. libFuzzer needs a C++ runtime at link time
(`LDFLAGS=-L<gcc-libstdc++-dir>` in this environment).

## Reentrancy

Sequential in-process reentrancy is verified: the CLI formats several files
per process, and `knfmt A B A`, `-s`, `-d`, and valid→malformed→valid
sequences produce output identical to isolated runs. Thread safety is not
established and is not claimed.

## Recovered input stability

- `token_move_prev_line()` moves a line break that precedes a binary
  operator to just after it (break-after-operator layout). For recovered
  malformed input the token before an operator may itself be an operator;
  moving the break then places it before the *next* operator, which the
  next pass moves again, so the layout drifted one operator per pass.
  Operators that immediately follow another binary operator are left
  untouched, which makes `F(F(x)) == F(x)` hold in one pass. Valid
  expressions are unaffected because a binary operator's left operand
  never ends in an operator. Regression: `tests/idempotence.sh` (runs
  `tests/repro-idempotence-00{1,2}.c` twice, normal and `-s`).

## Brace initializers

- `lbrace_cache_lookup()` caches the next nested left brace. It must cache
  the *absence* of a nested brace too: a flat initializer otherwise
  rescanned the token stream to its end for every element (quadratic;
  a 640 KB initializer took 60 s). With negative caching the same input
  formats in <1 s and `tests/valid-292.c` (210 KB) in ~0.25 s.

## Allocation fault injection

- `bmake fault` links a test-only `knfmt-fault` binary using
  `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc` and
  `tests/fault-wrap.c`, then `tests/fault.sh` fails each allocation in
  turn (`FAULT_AT=0,1,2,…`) and requires a controlled exit (status 1, no
  partial stdout) and an unchanged in-place target. This is a development
  target, not part of `bmake test`.

## Sanitizer instrumentation

- Sanitizers are only verified with Clang; the system GCC 16 cannot link
  its sanitizer runtimes in this environment, and `./configure --sanitize`
  then silently produces an uninstrumented build. Always confirm the
  resulting `CFLAGS` contain `-fsanitize=...` and that the binary is
  actually instrumented. LeakSanitizer cannot run here (`ptrace` is
  restricted); leak-freedom is therefore not established by LSan and the
  `tests/fault.sh` and reference counting checks are used instead.

## Valgrind memory checking

- The system preloads `libhardened_malloc.so` (Secureblue), whose custom
  allocator aborts under Valgrind. Run tools under
  `with-standard-malloc …` (a `bwrap` wrapper that masks
  `/etc/ld.so.preload`) to use the standard allocator.
- `.valgrindrc` enables `--leak-check=full` with
  `--errors-for-leak-kinds=all` and `--error-exitcode=1`, so a clean exit
  already implies no memory errors and no leaks.
- The whole `tests/*.c`/`*.h` corpus (928 files) runs Valgrind-clean, and
  so does a `-s` subset. This compensates for the unavailable
  LeakSanitizer.

## Parser type initialization

- `parser_func_peek1()`'s implicit-int branch must clear the whole
  `struct parser_type`, not only the token pointers: `func_decl` is read
  by `parser_func_proto()` and was otherwise uninitialized (found by
  Valgrind on `tests/valid-533.c`).

## C99 line comments in declarations

- A `//` comment runs to the end of the line, so any following token must
  start a new line. The type-specifier loop in `parser_type()` used a soft
  line before the next type token; when the preceding token carried a C99
  comment the soft line could be dropped and the following declaration was
  appended to the comment, commenting it out and swallowing one more
  declaration per pass. A hard line is emitted after a C99 comment there.
  This line is internal to a declaration, so it never duplicates the
  statement separator (normal `int x; // c` output is unchanged).
- Regression: `tests/repro-idempotence-003.c`.

## C89–C23 feature audit (summary)

The build is `-std=c23` with `-D_DEFAULT_SOURCE`; libks is C23 too. The
following were evaluated against real code (not adopted merely because
they exist).

| Standard | Feature | Status | Reason |
|---|---|---|---|
| C90 | prototypes, `const`, `void *`, enums, storage classes | used | already idiomatic; no K&R/undefined declarations |
| C90 | wide characters (C95) | n/a | byte/UTF-8 oriented formatter |
| C99 | designated initializers | used | e.g. `buffer_callbacks`, style tables |
| C99 | `stdint.h`, `inline`, variadic macros | used | fixed widths; header inlines; trace macros |
| C99 | `restrict` | not used | no no-alias contract established for public entry points |
| C99 | flexible array members | n/a | no layout benefit over current representations |
| C11 | `static_assert` | adopted | `libks/map.c` bucket/log2 invariant |
| C11 | `_Generic`, atomics, TLS | n/a | no type-dispatch or shared-state use |
| C11 | alignment | used | `__attribute__((aligned))` where ABI requires it |
| C23 | `nullptr` | adopted | whole tree (project + libks) |
| C23 | `bool`, `true`, `false` | used | already native |
| C23 | `[[noreturn]]` | adopted | `usage()`; `format`/`cleanup` retained (no standard equivalent) |
| C23 | `[[nodiscard]]` | adopted | checked-arithmetic helpers |
| C23 | `<stdckdint.h>` | adopted | `KS_*_overflow` prefer `ckd_*`, fall back to builtins |
| C23 | `static_assert` | adopted | see C11 |
| C23 | `alignas`/`alignof`, `typeof`, `auto`, `<stdbit.h>`, `#embed`, `_BitInt`, `#elifdef`/`#warning`/`__VA_OPT__` | n/a | no applicable site that would not add complexity or change layout/ABI |

The C++ side (`benchmark.cpp`, Google Benchmark harness) is C++23
(`-std=c++23`); it cannot be built here and is reviewed at source level
only. Standard C++ ownership abstractions are not introduced into the C
codebase.
