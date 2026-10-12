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
| C23 | `<stdckdint.h>` | adopted | vendored in `libks/stdckdint.h`; `KS_*_overflow` call `ckd_*` |
| C23 | `static_assert` | adopted | see C11 |
| C23 | `alignas`/`alignof`, `typeof`, `auto`, `<stdbit.h>`, `#embed`, `_BitInt`, `#elifdef`/`#warning`/`__VA_OPT__` | n/a | no applicable site that would not add complexity or change layout/ABI |

The C++ side (`benchmark.cpp`, Google Benchmark harness) is C++23
(`-std=c++23`); it cannot be built here and is reviewed at source level
only. Standard C++ ownership abstractions are not introduced into the C
codebase.

## C23 keyword macros

- `libks/compiler.h`: under C23 `STATIC_ASSERT` now maps to the `static_assert`
  keyword and `FALLTHROUGH` to the `[[fallthrough]]` attribute, keeping the
  older fallbacks for other language modes.
- `libks/arena.c`: `#elif defined(__i386__)` uses the C23 `#elifdef`.
- These are purely the standardized spelling; no behavior change.

## Recovered brace initializers

- When a brace initializer is recovered as an expression (e.g. an
  identifier before a designator: `L.callbacks = { ... }`), the assignment
  operator's trailing space was re-emitted by the brace parser after the
  expression formatter had already emitted one, giving `=  {` on the first
  pass and `= {` on the next. Assignment operators are now excluded from
  that re-emission. Regression: tests/repro-idempotence-004.c.
- Two malformed-recovery idempotence cases were formerly preserved under
  `tests/known-nonidempotent/00{1,2}.c` (an aligned continuation after an
  unexpected identifier before a subscript designator, and a CPP branch
  soup). Both are now fixed and promoted to reproducers; the directory is
  empty.

## Recovered assignment with a brace initializer

- A brace initializer that is the right-hand side of an assignment may also
  be the field stop chosen by `peek_expr_stop()` (e.g. the malformed field
  `stat[0x0] = { ... }`). The expression parser used to stop at the brace,
  splitting `stat[0x0] =` and `{ ... }` into two fields. The formatter then
  moved the left brace onto its own line, and the next pass classified it
  differently and indented the continuation differently. `expr_exec_binary()`
  now attaches such a brace as the assignment operand. Regression:
  `tests/repro-idempotence-005.c`.

## Known non-idempotent reproducers

- `tests/known-nonidempotent/001.c` (function whose parameter list contains a
  CPP conditional branch and whose body then fails on a stray byte), `002.c`
  (large include guard) and `004.c` (another instance) share one root cause:
  `clang_branch()` removes the tokens of the branch it crosses and rewinds to
  the last stamp. When no stamp survives the removal it rewinds to the first
  token, so the removed branch can no longer be emitted. `001.c` also
  produces output the formatter rejects on a second pass.
- The candidate correction `seek_index == 0 && brch_docs > 0` -> full-source
  verbatim preserves every observed case, but also turns `tests/error-014.c`
  into a success; the token removal is what makes that input fail, so the
  fallback cannot be used there. Replacing the rewind target or preserving
  the removed text conditionally was tried too:
  - skipping the removal when no stamp survives fixes `001.c` but accepts
    `error-014.c`;
  - adding a synthetic prefix holding the removed text fixes `001.c` but
    duplicates `tests/valid-102.c` and `tests/diff-style-002.c`, whose first
    branch *is* emitted;
  - gating that prefix on "no committed construct inside the removed range"
    fixes `001.c` but duplicates `valid-102.c`;
  - resuming at the alternative branch instead of the first token breaks
    `diff-031.c` and `diff-style-002.c`.
  The missing discriminator is whether the first branch's content was
  represented in a document that survives the recovery; a
  `TOKEN_FLAG_EMITTED` marker is set too early (the containing document may
  still be discarded). A correct fix needs document-level ownership
  tracking, or a stamp recorded at each crossing, without weakening the
  rejection of `error-014.c`.
- Every other reproducer (`tests/repro-idempotence-001..013.c`) is idempotent
  in normal and `-s` mode.

## Malformed declarations and punctuation

- `parser_decl_init()` removes the ruler indent (and the whole initializer
  document) when it parsed nothing. A declarator list that ends in a dangling
  separator, e.g. `int a,;`, consumes the comma and then fails the following
  declarator, leaving the initializer count at zero; the comma's document was
  removed with it. A consumed separator now keeps the document, so the comma
  survives (`int a, ;`). Regression: `tests/repro-idempotence-012.c`,
  `tests/repro-idempotence-013.c`.

## Token separation

- `token_pair_needs_space()` defines when two adjacent tokens would merge into
  a different preprocessing token (identifier/number characters, comment
  openers, operator combinations, `.`/digit). `expr_doc_concat()` consults it
  at every operand boundary, so a malformed recovery can no longer join e.g.
  `voiint` and `i` into `voiinti`. Regression: `tests/repro-idempotence-008.c`.

## C99 comments end the line

- A `//` comment runs to the end of the physical line, so the line break after
  it is mandatory. `parser_doc_token_impl()` now forces the following optional
  line (a discarded optional line is emitted as well) when a C99 comment
  precedes it. Without this the parser could move the next token onto the
  comment line, silently swallowing it. Regression:
  `tests/repro-idempotence-010.c`.

## NUL bytes and explicit lengths

- Rebuilding a comment with a NUL byte must not rely on NUL termination:
  `comment_trim()` returns the explicit length and `cpp_format()` plus the new
  `doc_literaln()` carry one as well. `tests/nul.sh` covers a multi-line
  comment and a multi-line macro containing NUL.
- A non-white-space byte that is the last byte of the input is its own
  preprocessing token; `clang_read()` no longer classifies it as `LEXER_EOF`
  and drops it. A lone stray byte is rejected without partial output, matching
  the non-final case.

## Macro realignment and literals

- `cpp_format()` refuses to realign a macro when a line continuation occurs
  inside a string or character literal: the alignment padding would become
  part of the literal. Regression: `tests/semantic-macro-literal.c`.

## Recovery rewinds and crossed branches

- `clang_recover()` folds a branch to recover. If the stamp it would rewind to
  precedes the most recent branch crossing (`stamp_docs < brch_docs`), the
  crossed branch's tokens are already gone and re-parsing cannot restore their
  content, so recovery instead asks the caller to preserve the whole source
  verbatim. Regression: `tests/repro-idempotence-009.c`.
- A `clang-format off` region emits a mute document. If recovery discards the
  recorded off comment while the mute is still active, `parser_exec()` cancels
  it by preserving the whole source, so the tail is never silently dropped.
  Regression: `tests/repro-idempotence-011.c`.

## CPP branches and block terminators

- `parser_extern()` accepts `extern "C" { ... }` only when its matching `}`
  is not the body of a conditional branch that has an alternative
  (`#else`/`#elif`). Such a conditional terminator cannot be taken as an
  unconditional block close: crossing the alternative removes the brace and
  the branch recovery re-emits or drops the block. When the block is
  declined, `parser_extern()` sets `pr_verbatim`; on the resulting parse
  failure `parser_exec()` discards every document built so far and emits the
  whole source verbatim (`parser_verbatim_all()`). The same full-source
  fallback is used when `clang_recover()` would otherwise rewind across a
  crossed branch (see "Recovery rewinds and crossed branches"). No committed
  position can precede the branch reliably, and folding it would collect the
  leading directives and comments onto tokens that are then removed without
  being re-emitted. The verbatim emission is byte-stable, so it is idempotent
  and content-preserving; `tests/repro-idempotence-007.c` guards it. A
  conditional brace without an alternative (the usual C++ linkage guard,
  `#ifdef __cplusplus ... #endif`) is still parsed.
- `parser_func_arg()` keeps a hard line after a parameter type that ends in a
  line comment, so the next parameter is not joined onto the comment.

## Semantic-preservation oracle

- `tests/semantic.sh` formats each accepted fixture in normal mode and
  compares the non-comment, non-whitespace, non-semicolon, non-backslash byte
  sequence before and after. Translation phase 1 (trigraph replacement) and
  phase 2 (backslash-newline splicing) are applied first. It is independent of
  the formatter's lexer and catches token loss, duplication and reordering
  that idempotence cannot see. Not applied to `-s`, which legitimately
  rewrites tokens.
- `tests/semanticoracle.pl --selftest` exercises 29 cases covering identifier
  and operator concatenation, token splitting, deleted/duplicate punctuation,
  changed and truncated literals, swallowed comments, macro continuation and
  escaped-newline changes, and CPP branch deletion/duplication/reordering.
- `tests/mutate.pl` runs the same check on every mutant, so the fuzzer
  reports `SEMANTIC` findings separately from `CRASH`/`TIMEOUT`.
