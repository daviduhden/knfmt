# knfmt architecture

This document describes the implementation as it exists. It intentionally
does not describe abstractions that are not present (for example there is no
`DOC_SEQ` node and no monolithic `knfmt_ctx`).

It opens with a per-file walkthrough in order of execution and then documents
the cross-cutting contracts and the evidence that supports them.

## Source layout

- `lexer.c` turns source code into a list of tokens and owns the associated
  memory, so tokens have stable identity, are comparable by pointer, and are
  valid until `lexer_free()`. Token contents and flags may be updated in
  place. The API is split into `lexer_if_*` routines that consume one
  or many tokens and `lexer_peek_*` routines that peek at them; both return
  non-zero when the tokens satisfy the criterion. A token may carry dangling
  tokens (`tk_prefixes`/`tk_suffixes`) holding preprocessor directives and
  comments, see "Preprocessor handling".
- `clang.c` holds the C-specific routines used by the lexer: the keyword and
  identifier lookup tables and the tracking of preprocessor conditionals
  (`clang_stamp()`, `clang_branch()`, `clang_recover()`). Not to be confused
  with LLVM Clang.
- `parser.c` recognizes tokens by recursive descent and builds the document
  that represents the formatted output, so parsing is not a separate phase of
  execution.
- `simple-*.c` implements the `-s` transformations, each a pass tracked by
  `simple.c`.
- `expr.c` parses expressions with Pratt parsing and hands constructs it cannot
  recognize on its own (such as `sizeof(type)`) back to the parser through
  `expr_recover()`, see "Preprocessor handling".
- `doc.c` defines the document tree and renders it, see "Document model" and
  "Document types".
- `cpp-format.c` formats preprocessor constructs such as line-continuation
  alignment; `cpp-include.c` and `cpp-include-guard.c` implement the
  clang-format include-block and include-guard interop.
- `ruler.c` aligns prototypes and declarations: the parser calls
  `ruler_insert()` for each line and column it wants to align, and `ruler_exec()`
  computes the alignment once every line has been emitted.
- `error.c` buffers diagnostics so they can be discarded if a later cpp branch
  turns out to be valid; the lexer reports through `lexer_error()` and its
  `lexer_error_flush()`/`lexer_error_reset()` helpers.

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
  punctuation mappings map to their tokens). A replacement is a **character**,
  not a token: when it combines with the following character the compound
  punctuator is recognised too (`??'=` is `^=`, `??!=` is `|=`, `??!|` is
  `||`). knfmt has no language-version selector, so this translation behaviour
  is uniform (C23 removed the requirement, but the behaviour is kept).
- A backslash-newline splice that joins the two halves of a preprocessing
  token (for example an identifier split across physical lines) keeps the
  token joined. When a macro's continuations are aligned, a splice between two
  non-whitespace characters suppresses the alignment so the padding cannot
  split the token.
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
- Expression parsing is recursive and guarded (`EXPR_MAX_DEPTH`); document
  construction additionally enforces `EXPR_STACK_BUDGET`. It is bounded by
  nesting, not by flat length. Declarator scanning has its own
  `SCAN_MAX_DEPTH` guard, and statement/declaration nesting is bounded by
  `PARSER_MAX_DEPTH` (8000). New recursive descent must still account for
  process stack use.
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

## Document types

Kept in sync with `enum doc_type` in `doc.h`. A group is the only document
type that can trigger a refit between break mode (line breaks allowed) and
munge mode (no line breaks).

- `DOC_CONCAT`: zero or more nested documents; often the child of a group.
- `DOC_GROUP`: something intended to fit on a single line.
- `DOC_INDENT`: increases indentation; not emitted until the next new line.
- `DOC_NOINDENT`: trims the current line, assuming only indentation was
  emitted; used for goto labels, which are never indented.
- `DOC_ALIGN`: emits white space up to the column associated with the same
  document; used by the ruler.
- `DOC_LITERAL`: a string emitted as is; tokens become literal documents.
- `DOC_VERBATIM`: source taken as is, such as comments and preprocessor
  directives; often carries a trailing new line.
- `DOC_LINE`: a new line in break mode and a space in munge mode.
- `DOC_SOFTLINE`: a new line in break mode and nothing in munge mode.
- `DOC_HARDLINE`: a new line in both modes.
- `DOC_OPTLINE`: honored inside `DOC_OPTIONAL`, or unconditionally when the
  node carries a nonzero forced value (used after C99 comments).
- `DOC_MUTE`: suppresses emission while muted; hard lines update line state
  but are not printed. Used while traversing cpp branches.
- `DOC_UNMUTE`: the `DOC_MUTE` counterpart used for clang-format off/on
  comments; new lines missed while muted are not honored on unmute.
- `DOC_OPTIONAL`: allows `DOC_OPTLINE` to be emitted.
- `DOC_MINIMIZE`: selects, from a list of potential indentations, the one that
  minimizes the number of new line(s) and line(s) exceeding the column limit.
- `DOC_SCOPE`: enters a new scope in an arbitrary sense and resets document
  related state; used by `DOC_INDENT_NEWLINE`.
- `DOC_MAXLINES`: alters the maximum number of allowed consecutive new line(s).

The document representation and the decisions taken between the two modes can
be examined with `knfmt -td`.

## Preprocessor handling

knfmt does not run the source through the preprocessor: that would require
supplying include path(s), which in my opinion makes the utility less user
friendly. The downside is that preprocessor directives cannot be distinguished
from other well defined language constructs, and they can appear literally
anywhere.

Instead, directives are made completely transparent to the parser. A token
emitted by the lexer never directly represents a directive; it always
represents a well defined language construct and may carry dangling tokens
(`tk_prefixes`/`tk_suffixes`) that hold the directives and comments. The
dangling tokens are emitted by `doc_token()`.

Two macro shapes need special attention:

- Macros that expand to a loop construct, often with `foreach` in the
  identifier. Without knowing about the loop, the parser would halt on what
  looks like a function call followed by a brace or a statement; see
  `parser_stmt_cpp()`. An example is the set provided by
  [queue(3)](https://man.openbsd.org/queue.3).
- Macros that expand to a binary expression, often with a binary operator as a
  distinct argument. Encountering such an operator in a unary context makes the
  expr parser halt and hand control back to the parser through
  `expr_recover()`. An example is the set provided by
  [timercmp(3)](https://man.openbsd.org/timercmp.3).

Conditional constructs (branches) are tracked by `clang.c` and the lexer, see
"CPP branch emission model", "CPP branch content loss" and "Recovery rewinds
and crossed branches".

## Simplifier (`-s`)

- `simple_enter()`/`simple_leave()` track per-pass state; a pass is
  mutually exclusive with others and does not re-enter itself while enabled.
- `expr_doc_parens()` drops a redundant grouping pair and then calls
  `simple_leave()` **before** formatting the operand, so nested removable
  grouping and other simplifications are canonicalized in the same pass.
  Nesting terminates because each step descends into the expression. This
  yields one-pass `-s` idempotence without changing global re-entry rules.

## Verification

See [ARCHITECTURE-REWRITE-PROGRESS.md](ARCHITECTURE-REWRITE-PROGRESS.md) for measured results. Commands:

```sh
LC_ALL=C make test                        # GNU make; bmake also works
KNFMT=./knfmt sh tests/reparse.sh         # one-pass stability, normal and -s
KNFMT=./knfmt sh tests/bytes.sh           # raw-byte domain
KNFMT=./knfmt sh tests/nul.sh             # NUL contract
KNFMT=./knfmt sh tests/bench.sh 10000 200000   # time; RSS when available
sh tests/cross-build.sh ./knfmt <clang-asan-knfmt>   # normal vs sanitized
```

Sanitizer builds (clean, compiler consistent): `CC=clang ./configure
--sanitize address` (or `undefined`), then rebuild with
`rm -f *.o libks/*.o knfmt; make`. To add flags manually, pass them on the
`make` command line (`make DEBUG=...`); an environment `DEBUG` is overridden
by the generated `config.mk`.

Fuzzing: `CC=clang ./configure --fuzz llvm`, `make fuzz-parse`, then run
`./fuzz-parse <seed-dir>`. libFuzzer needs a C++ runtime at link time
(`LDFLAGS=-L<gcc-libstdc++-dir>` in this environment). The AFL-mode harness
and the mutation driver are described in [FUZZ.md](FUZZ.md).

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
  every `tests/repro-idempotence-*.c` twice, normal and `-s`).

## Brace initializers

- `lbrace_cache_lookup()` caches the next nested left brace. It must cache
  the *absence* of a nested brace too: a flat initializer otherwise
  rescanned the token stream to its end for every element (quadratic;
  a 640 KB initializer took 60 s). With negative caching the same input
  formats in <1 s and the large `tests/valid-292.c` fixture in ~0.25 s.

## Allocation fault injection

- `make fault` links a test-only `knfmt-fault` binary using
  `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc` and
  `tests/fault-wrap.c`, then `tests/fault.sh` fails each allocation in
  turn (`FAULT_AT=0,1,2,…`) and requires a controlled exit (status 1, no
  partial stdout) and an unchanged in-place target. This is a development
  target, not part of `make test`.

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
- The whole `tests/*.c`/`*.h` corpus runs Valgrind-clean, and
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
  soup). Both are now fixed and promoted to reproducers. The directory still
  contains `005.c`, a known include-guard recovery case.

## Recovered assignment with a brace initializer

- A brace initializer that is the right-hand side of an assignment may also
  be the field stop chosen by `peek_expr_stop()` (e.g. the malformed field
  `stat[0x0] = { ... }`). The expression parser used to stop at the brace,
  splitting `stat[0x0] =` and `{ ... }` into two fields. The formatter then
  moved the left brace onto its own line, and the next pass classified it
  differently and indented the continuation differently. `expr_exec_binary()`
  now attaches such a brace as the assignment operand. Regression:
  `tests/repro-idempotence-005.c`.

## CPP branch emission model

- A conditional group is emitted one branch per parse. When the parser meets
  the next directive (`#elif`, `#else`, `#endif`) it returns `BRCH`;
  `clang_branch()` crosses by removing the tokens of the branch it leaves and
  rewinding to the last stamp (or to the first token when no stamp survives).
  The enclosing `parser_exec()` loop then allocates a **new top-level
  document** and parses the same construct again. Each parse therefore
  contributes one branch's content.
- The repeated prefix (everything before the branch point) is not emitted
  twice: `parser_doc_token_impl()` emits `DOC_MUTE(+1)` when the token after
  it starts a branch and `DOC_MUTE(-1)` when the crossed-to token is emitted.
  The resulting mute spans the following document(s), so only the branch
  content of each parse reaches the output. `tests/valid-102.c` is the
  reference case: three parses, three documents, three branches, one shared
  prefix.
- `TOKEN_FLAG_EMITTED` (or any per-token marker set in
  `parser_doc_token_impl()`) is therefore **not** evidence of surviving
  output. A token is put into a document during speculative parsing, and the
  document may later be removed by `doc_remove_tail()` in the recovery path.
  The flag is also shared across the repeated parses of the same source token,
  so it cannot distinguish "emitted in the parse that is being rolled back"
  from "emitted in the parse that is committed".
- A recovery stamp is a token plus the number of top-level documents that
  existed when it was taken. It is only a valid rollback point if the tokens
  and documents up to that point are still intact. A crossing destroys the
  branch's tokens, so a stamp before the crossing no longer identifies a
  recoverable position.

## CPP branch content loss

- `clang_branch()` sets `cl->lossy` when it has to rewind to the first token
  because no stamp survived. At that point the tokens of the branch just
  crossed have been removed and re-parsing cannot reproduce their content:
  the input is recoverable only by reproducing it verbatim.
- The loss is not decided at the crossing. A later `clang_recover()` that also
  finds no surviving stamp *and* sees `cl->lossy` reports `deferred`; the
  caller records `pr->pr_lossy` and lets the normal rollback run. If parsing
  then **fails**, the failure is reported unchanged, so genuinely unparseable
  input is still rejected. Only when parsing **succeeds** does `parser_exec()`
  discard the (incomplete) speculative documents and emit the whole source
  verbatim through `parser_verbatim_all()`.
- Requiring `cl->lossy` as well as the missing stamp is what keeps valid
  inputs out of the fallback: `tests/valid-339.c` and `tests/diff-style-002.c`
  also rewind to the first token, but never after a crossing, so they are
  formatted normally. `tests/diff-031.c` crosses with a surviving stamp and is
  likewise unaffected.
- Rejection is preserved because the decision is deferred to the parse result:
  a failed parse returns 1 before the verbatim block is reached. This is the
  property the previously rejected blanket conditions lacked — they
  substituted output at the recovery point itself and so turned a failure into
  a success.
- The rollback that removes documents uses `while (doc_remove_tail(dc))`
  rather than a document count: the recovery may have adjusted the counter, so
  only clearing the list guarantees that no stale speculative document (and no
  unmatched `DOC_MUTE`) survives into the verbatim output.

### Corrections tried and rejected

| Candidate | `001`/`002`/`004` | `error-014` | Other |
|---|---|---|---|
| Skip removal when no stamp survives | fixed | **accepted** | — |
| Synthetic prefix with the removed text | fixed | rejected | `valid-102`, `diff-style-002` duplicated |
| Prefix gated on "no committed construct inside the range" | fixed | rejected | `valid-102` duplicated |
| Resume at the alternative branch | fixed | **accepted** | empty diffs |
| Blanket verbatim for `seek_index == 0 && brch_docs > 0` | fixed | **accepted** | — |
| Keep documents down to the committed count | partial | rejected | duplicated prefix |
| Deferred verbatim keyed only on the missing stamp | fixed | rejected | `valid-339`, `diff-style-002` verbatim |

Each of these substitutes output at the recovery point, so none can tell a
failure from a success; the accepted designs failed exactly when the input
should have been rejected.

### What made it correct

The commit point is the pair *(a crossing that had to rewind to the first
token)* and *(a later recovery with no surviving stamp)*, and the commit
decision is taken only after the whole parse has been attempted. A crossing
alone is harmless, and a missing stamp alone is harmless; together they mark
content that only verbatim reproduction can preserve, while the success test
leaves rejection intact. The three reproducers are now
`tests/repro-idempotence-014.c`, `-015.c` and `-016.c`; the valid CPP forms are
covered by `tests/semantic-cpp-branches.c`.

- Every reproducer (`tests/repro-idempotence-001..018.c`) is idempotent in
  normal and `-s` mode.

## CPP branch-processing cost

Sequential conditional *initializers* (`int v = #ifdef A 1 #else 2 #endif ;`)
scale quadratically; top-level conditionals (`#ifdef A int v = 1; #endif`) do
not. Measured with the semantic build (`int v_i =` construct):

| constructs | time | peak RSS | documents |
|---:|---:|---:|---:|
| 100 | 0.27 s | 31 MB | 190 353 |
| 200 | 0.69 s | 105 MB | 750 703 |
| 400 | 2.39 s | 402 MB | 2 981 403 |
| 800 | 9.11 s | 1588 MB | 11 882 803 |

Cause: `clang_stamp()` is only taken on `GOOD`, but a conditional inside an
initializer makes every top-level pass return `BRCH`. With `cl->stamps` empty,
`clang_last_stamped()` returns `nullptr`, so `clang_branch()` rewinds to the
**first token** on every crossing (measured: `laststamp_iter == 0` for all
800 passes). Pass *i* therefore re-parses and re-emits the whole preceding
file (muted by `DOC_MUTE`), producing O(i) documents per pass and O(N²)
overall. The parse and render phases are each quadratic.

Operations that are *not* the cause: the number of top-level passes is N+1 and
the number of crossings is N (both linear); `lexer_seek()` is O(1); with a
surviving stamp the rewind is O(1) and the same input is linear.

A safe correction needs a rewind floor that is nearer than the first token —
the start of the construct that contains the crossing — but the parser has no
such position recorded when no stamp survives. This is not implemented; the
behaviour is pre-existing and unchanged by the recovery corrections.

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
- `tests/semanticoracle.pl --selftest` exercises its own case set covering
  identifier and operator concatenation, token splitting, deleted/duplicate
  punctuation,
  changed and truncated literals, swallowed comments, macro continuation and
  escaped-newline changes, and CPP branch deletion/duplication/reordering.
- `tests/mutate.pl` runs the same check on every mutant, so the fuzzer
  reports `SEMANTIC` findings separately from `CRASH`/`TIMEOUT`.
