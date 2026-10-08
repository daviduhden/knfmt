# knfmt architecture rewrite — progress checkpoint

Base/head: `c85c18f` on `main` (in sync with `origin/main`).
Working tree clean. All commits below are on `main`.

## Completed milestones (verified)

| Commit | Change | Evidence |
|---|---|---|
| `127c637` | `doc_fits()` caches an iterative structural width summary per document node (`dc_sum_flat`, `dc_sum_flags`, `VALID/PURE/TOOWIDE`, `doc_summarize()`, `DOC_WALK_SKIP`) and skips pure subtrees. | Flat `a+a+…+a`: 1k 88→23 ms, 2k 256→34 ms, 4k 980→59 ms, 8k 4.0 s→114 ms. `bmake test` green; `tests/cross-build.sh` OK (3700). |
| `c85c18f` | `parser_type_peek()` no longer calls `parser_type_implicit_int()` when a concrete type is already present (only when `ntokens == nkeywords`), breaking the `parser_type_peek ↔ classify_implicit_int ↔ parser_type_decl_list_then_lbrace` exponential cycle. | `pthread.h` (1353 lines): >300 s → 0.032 s. Synthetic `int f() A B;`: exponential → near-linear. `bmake test` green; cross-build OK. |
| `80ac297`, `1277edd`, `12fd011` | Parser output/scratch initialization and attribute-expression output fallback; `findpath` OOB fix. | Earlier milestones. |

## Correctness barriers (all currently green)

- `LC_ALL=C bmake test`
- `ASAN_OPTIONS=detect_leaks=0 sh tests/cross-build.sh ./knfmt <clang-asan-binary>` — currently `OK (3700 comparisons)`
- `sh tests/nul.sh`
- The `diff-014 -s` regression has not returned.

## Pending milestones (in dependency order)

1. **Iterative `expr_doc()` construction (PART II).** Recursion path:
   `expr_doc → expr_doc_binary → expr_doc(ex_lhs) → …`.
   Plan: in the *default* `expr_doc_binary` branch (non-assign, non-
   `BreakBeforeBinaryOperators`), walk the left spine iteratively:
   descend creating each level's `GROUP`/`CONCAT` into an explicit stack,
   build the innermost lhs once, then unwind appending `" op"`, the
   new `GROUP`/`CONCAT`, line/`SOFTLINE` and rhs. Reproduce
   `expr_doc_align`, `token_move_prev_line`, `expr_doc_has_spaces`,
   `token_has_suffix`/`token_has_line`, `expr_doc_soft` exactly.
   Fall back to recursion for assign / break-before chains. Verify with
   `-td` doc-tree comparison on small chains and the fixture corpus.
2. **Iterative `doc_exec1()` rendering (PART V).** Still recurses per
   sequential child (`GROUP[CONCAT[...]]` left spine). Requires an
   explicit render stack; large, must preserve all node semantics.
3. **Remove `EXPR_STACK_BUDGET`/`EXPR_MAX_DOC_DEPTH` guard** from `expr.c`
   only after (1) and (2) are done and 100k/200k terms succeed.
4. **`DOC_SEQ` n-ary sequence representation (PART III)** — optional if
   (1)+(2) already give O(1) stack and near-linear fitting; otherwise
   flatten sequential `CONCAT` spines.
5. **Per-run context (PART VI)** — `knfmt_ctx`; audit mutable globals.
6. **Reentrancy harness (PART VI)** — in-process A/B/A for normal/`-s`/`-D`/`-d`.
7. **Parser result contracts / checkpoints (PART VI).**
8. **Residual O(N²) in declaration scanning.** `parser_type_decl_list_then_lbrace()`
   still scans all following declarations per annotated prototype
   (`int f() A B;` N=640 ≈ 0.77 s; scaling ≈ N^1.7). Consider bounding the
   scan or memoizing the declaration-list result.
9. **183-byte fuzz finding, raw-byte domain, malformed self-reparse.**
10. **Benchmarks/regression tests for the above.**

## How to resume

```sh
cd /var/home/david/git/knfmt
bmake -j"$(nproc)" knfmt t          # normal build (gcc)
LC_ALL=C bmake test                 # full fixture suite

# ASan comparison build (separate worktree):
#   git worktree add -f /tmp/opencode/p1-san HEAD
#   cd /tmp/opencode/p1-san && ./configure --sanitize address && bmake knfmt
#   # apply the same source changes, rebuild, then:
cd /var/home/david/git/knfmt
ASAN_OPTIONS=detect_leaks=0 sh tests/cross-build.sh ./knfmt /tmp/opencode/p1-san/knfmt
```

Flat-expression reproducer:

```sh
python3 -c "open('/tmp/ln.c','w').write('int x = ' + 'a+'*8000 + 'a;\n')"
knfmt /tmp/ln.c
```

`pthread.h` reproducer: copy `/usr/include/pthread.h` and run `knfmt` on it.

## Update — iterative expression construction (head `a412587`)

- `f983e23` `expr: construct flat binary expression documents iteratively`.
  `expr_doc_binary_chain()` handles a homogeneous default-policy binary
  chain with an explicit reallocatable frame array (descend creating each
  level's group/concat, build the deepest left operand once, unwind
  emitting operator/group/line/rhs). Produced document is identical:
  `bmake test` green, cross-build `OK (3700)`.
- `a412587` `doc: bound document height while rendering stays recursive`.
  `DOC_MAX_EXEC_DEPTH = 9000` (in `doc.c`) rejects documents taller than
  the rendering stack in both gcc and Clang+ASan. This is a **temporary**
  guard.

Effect: flat `a+a+…+a` construction no longer recurses per term, but
**rendering (`doc_exec1`, `doc.c:783`) still recurses per term**, so:

| terms | gcc | clang ASan |
|---|---|---|
| 2000 | ok | ok |
| 4000 | ok | ok |
| 5000 | reject | reject |
| 50000 | reject (guard) | — |

The gcc-only stack allowed ~50000 before the guard; the guard is set to
the ASan-safe height. Removing it requires iterative `doc_exec1`.

### Next exact action

Convert `doc_exec1()` (and the recursive helpers at `doc.c:956`,
`doc.c:1184`, `doc.c:1193`) to an explicit work/continuation stack. The
recursive sites are `doc.c:792` (CONCAT children), `806/814` (GROUP with
mode save/restore + diff enter/leave), `833` (NOINDENT indent
save/restore), `906` (OPTIONAL optline save/restore), and the
MINIMIZE/SCOPE/MAXLINES helpers. Preserve `doc_fits()`/summary use. Then
remove `DOC_MAX_EXEC_DEPTH` and verify 100k/200k terms.

## Update — iterative rendering, guard removed (head `5e58398`)

- `5e58398` `doc: render documents iteratively`. `doc_exec1()` now uses an
  explicit continuation-frame stack for `DOC_CONCAT` (resumable child
  iteration) and `DOC_GROUP` (mode/diff restore). `INDENT`/`NOINDENT`/
  `OPTIONAL`/`MINIMIZE`/`SCOPE`/`MAXLINES` still delegate to helpers but
  their child is rendered by the same iterative traversal (bounded
  nesting).
- Removed `DOC_MAX_EXEC_DEPTH` and its rejection logic.

Results (flat `a+a+…+a`), guard removed:

| terms | gcc | gcc time | clang ASan |
|---|---|---|---|
| 10,000 | ok | 0.16 s | ok |
| 50,000 | ok | — | ok |
| 100,000 | ok | 2.2 s | ok |
| 200,000 | ok | 5.5 s | ok |

One-pass idempotent at 200k. `bmake test` green; cross-build `OK (3700)`.

### Remaining (in order)

1. Residual O(N²) in `parser_type_decl_list_then_lbrace()`: repeated
   `int f() A B;` decls scale ≈3.8× per doubling (N=320 0.21 s, 640 0.78 s,
   1280 2.9 s). `pthread.h` itself is fast (0.03 s).
2. `DOC_SEQ` n-ary sequence representation (current CONCAT is a list node,
   not left-nested, so this is now lower priority).
3. Per-run `knfmt_ctx`; mutable-global audit.
4. In-process A/B/A reentrancy harness (normal/`-s`/`-D`/`-d`).
5. Parser result/checkpoint contracts.
6. 183-byte fuzz finding; raw-byte policy; malformed self-reparse.
7. Benchmarks/tests committed; RSS scaling.

## Update — declaration scan cached, O(N²) removed (head next)

Cached the `parser_type_decl_list_then_lbrace()` scan range in
`struct parser` (`pr_decl_scan_beg/end/nolbrace`). A scan that proves no
declaration-list-then-`{` from a position proves it for every later start
in the scanned range, so repeated annotated declarations no longer rescan
the suffix.

Repeated `int f() A B;`: 320 0.212s→0.028s, 640 0.775s→0.040s,
1280 2.94s→0.063s, 2560 →0.112s (near-linear). pthread.h 0.024s.

### Remaining

Per-run context; parser result/checkpoint contracts (largely addressed by
`1277edd`/`80ac297`); in-process A/B/A reentrancy harness; 183-byte fuzz
finding; raw-byte policy; malformed self-reparse; DOC_CONCAT audit
(already n-ary, no DOC_SEQ needed); benchmarks; UBSan runs.

## Update — in-process reentrancy verified

The CLI formats several files in one process, so `knfmt A B A` is an
in-process A/B/A test (not subprocesses).

- normal: `knfmt A.c B.c A.c` == `A.out B.out A.out` ✓
- `-s`: `knfmt -s A.c B.c A.c` == concatenated single runs ✓
- `-d`: `knfmt -d A.c B.c A.c` == concatenated single runs ✓
- `-D`: reads a single patch from stdin (presentation over the same
  formatter); rc=0 sanity ✓

State does not leak across invocations for these modes.

## Update — mutable-state audit and failure-transition reentrancy

Audit of all file-scope and function-local mutable state:

- `clang.c`: `clang_tokens`, `clang_identifiers` (MAPs), `token_types[]`,
  `keywords[]`/`aliases[]` — populated by `clang_init()` once, read-only
  thereafter.
- `style.c`: `keywords[256]` — populated by `style_init()` once.
- `expr.c`: `table_rules[][2]` — populated by `expr_init()` once.
- `lexer.c`/`parser-cpp.c`/`util.c`/`cpp-include.c`: `static struct
  KS_str_match match` with `_init_once` — initialised once, read-only.
- `simple-decl-forward.c`: `static struct token fallback` — never written
  (permanent zero sentinel), returned by address.
- `libks/capabilities-x86.c`, `libks/valgrind.c`: init-once detection
  statics.

Conclusion: **no per-invocation mutable global state exists**. All
invocation state already lives in `struct main_context` and the per-file
`struct lexer`/`struct parser`/`struct clang`/`struct doc_state`
objects, arena/heap owned per run. `pr_decl_scan_*` is per-`struct
parser` (per run). A monolithic `knfmt_ctx` would rename existing explicit
ownership, not add safety, so it was not introduced. Configuration
(`style`, `simple`, `options`) is per-`main_context`.

Failure-transition reentrancy (single process, `knfmt A M A`):

- normal: `A M A` output == `A A` when M is malformed; M reports a clean
  error, rc=1, and does not contaminate the following A ✓
- `-s`: same ✓

## Update — raw-byte tests, UBSan/overflow, fuzz artifact

- `dc2975e` `tests: add the raw-byte input-domain contract`
  (`tests/bytes.sh`, registered in `tests/Makefile`).
- Sanitizer matrix (clean clang builds, `LC_ALL=C`):
  - `-fsanitize=undefined -fno-sanitize-recover=all`: full suite green,
    0 reports.
  - `-fsanitize=unsigned-integer-overflow -fno-sanitize-recover=all`:
    full suite green, 0 reports.
  - ASan cross-build: `OK (3700)`.
- 183-byte fuzz artifact: not found in repo, history, or `/tmp`; the
  original artifact directory no longer exists → classification **(E)**
  irretrievable, documented. The raw-parser harness `fuzz-parse.c` uses
  the production path (`clang_alloc` → `lexer_tokenize` → `parser_alloc`
  → `parser_exec`), i.e. it follows the real initialization contract, so
  prior failures were not caused by an artificially uninitialized API use
  in the current harness.

## Update — self-reparse harness and `-s` non-idempotence finding

- Added `tests/reparse.sh` (development tool): for every `tests/*.c`/`*.h`
  and options `""`/`-s`, if formatting succeeds it formats the output again
  and requires byte-identical output.

Result over the fixture corpus: `accepted=1686 rejected=164 fail=2`.

Two `-s` self-reparse failures (first pass != second pass):

- `tests/simple-expr-parens-002.c`: `while (((0) != 0))` → pass1
  `while ((0) != 0)` → pass2 `while (0 != 0)`.
- `tests/simple-expr-sizeof-002.c`: `return ((sizeof int));` → pass1
  `return (sizeof(int));` → pass2 `return sizeof(int);`.

Root cause: nested redundant parentheses are removed one level per pass.
`expr_doc_parens()` removes a paren pair only when
`simple_enter(SIMPLE_EXPR_PARENS, ...)` succeeds, and `simple_enter()`
suppresses re-entry while the same pass is already `ENABLE` (the
`state != DISABLE` check, in addition to `is_pass_mutually_exclusive`).
Allowing self-nesting in the exception table alone did not change the
output. Fixing this requires permitting nested same-pass simplification
in `simple.c`, which was not completed.

Normal-mode (non `-s`) self-reparse: no failures.

## Update — `-s` nested-paren fix attempted; reverted (unsafe)

Attempted to make `-s` canonicalize nested redundant parentheses in one
pass by allowing same-pass nesting in `simple.c`:

- `is_pass_mutually_exclusive()`: skip `i == pass` (self is never exclusive).
- `simple_enter()`: treat an already `ENABLE` pass as re-enterable (only
  `IGNORE` suppresses), and only assign `flags` on the `DISABLE -> ENABLE`
  transition.

Result:

- Both known fixtures became idempotent: `while (((0) != 0))` -> pass1
  `while (0 != 0)`; `return ((sizeof int));` -> pass1 `return sizeof(int);`.
- **But it caused a SIGSEGV** in `-Ds` (diffparse + simplify) on
  `tests/diff-simple-002` (`simple.c` interaction with diff-mode document
  emission). That is an unacceptable regression.

The change was reverted (`git checkout -- simple.c`). `-Ds` works again and
`LC_ALL=C bmake test` is green. The two `-s` self-reparse failures therefore
**remain**:
`tests/simple-expr-parens-002.c`, `tests/simple-expr-sizeof-002.c`.

### Next exact action for this defect

Canonicalize a whole chain of redundant parentheses in a single traversal
inside `expr_doc_parens()` (Approach C), rather than relaxing the pass state
machine: when the current paren pair is removable, descend into the operand
and, while it is another removable `EXPR_PARENS`, skip every layer without
re-entering `simple_enter()` for the same pass. Verify with `-Ds`,
`tests/diff-simple-002.c`, and `tests/reparse.sh`.

## Update — `-s` nested-paren idempotence FIXED (HEAD next)

`2a8d9bd` `expr: canonicalize nested redundant parentheses in one pass`.

Fix: in `expr_doc_parens()`, after deciding to drop a redundant paren pair,
call `simple_leave()` *before* formatting the operand, so nested removable
grouping and other simplifications run in the same pass. The general
`simple_enter()`/`simple_leave()` mutual-exclusion/re-entry semantics are
unchanged (the reverted global re-entry relaxation is not used).

Results:

- `tests/reparse.sh`: `accepted=1686 rejected=164 fail=0`.
- `while (((0) != 0))` -> pass1 `while (0 != 0)` (idempotent).
- `return ((sizeof int));` -> pass1 `return sizeof(int);` (idempotent).
- `tests/diff-simple-002` with `-Ds`: crash-free (rc=0).
- `LC_ALL=C bmake test`: green. Only two `.ok` files changed, to the
  canonical form.
- ASan cross-build: `OK (3700)`.

Remaining: byte-domain extensions (invalid UTF-8, mixed newlines,
trigraphs), committed benchmarks/RSS, executed fuzz campaigns,
OpenBSD/ANONERO re-runs, permanent documentation.

## Update — raw-byte extensions, benchmarks and RSS (HEAD next)

- `137cf2f` `tests: extend the raw-byte contract`: invalid UTF-8 in
  comments/strings (opaque, preserved), mixed LF/CRLF, lone-CR rejection
  within mixed input, EOF after backslash/blank/whitespace.
- `tests/bench.sh`: reproducible flat-expression benchmark (time + peak
  RSS via `/usr/bin/time`).

Measured (gcc, this environment, `int x = a+a+...+a;`):

| terms | in bytes | out bytes | time | peak RSS | idempotent |
|---|---:|---:|---:|---:|---|
| 10,000 | 20,011 | 41,675 | 0.154 s | 28 MB | ok |
| 50,000 | 100,011 | 208,343 | 0.861 s | 114 MB | ok |
| 100,000 | 200,011 | 416,675 | 2.05 s | 222 MB | ok |
| 200,000 | 400,011 | 833,343 | 5.56 s | 437 MB | ok |

Time and RSS scale approximately linearly (20x terms -> ~16x RSS, ~36x
time). Memory ~2.2 KB/term (documents + AST + summaries).
