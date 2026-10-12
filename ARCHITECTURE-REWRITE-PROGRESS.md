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
perl -e 'print "int x = " . ("a+" x 8000) . "a;\n"' >/tmp/ln.c
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

## Update — fuzz campaign, declaration benchmark, trigraph probe (HEAD next)

### Fuzz campaign (executed)
- Target: `fuzz-parse` (production path: clang_alloc -> lexer_tokenize ->
  parser_alloc -> parser_exec).
- Build: `CC=clang ./configure --fuzz llvm`; DEBUG =
  `-fsanitize=address,undefined,unsigned-integer-overflow,fuzzer
  -fno-sanitize-recover=all`; link needs
  `LDFLAGS=-L/home/linuxbrew/.linuxbrew/Cellar/gcc/16.2.0/lib/gcc/16`.
- Seeds: all `tests/*.c` and `tests/*.h` (925 files).
- Duration: ~120 s (killed by an outer timeout). Executions: 925,
  ~7 exec/s, peak RSS 425 MB, new units 0.
- Findings: no crashes, no sanitizer reports, no leaks, no hangs
  (slowest unit < 1 s). The low exec rate and high RSS are properties of
  per-iteration arena allocation under the sanitizer build, not crashes.

### Annotated-declaration scaling (near-linear, cache effective)
`int fN() A B;`

| declarations | time |
|---|---:|
| 320 | 0.030 s |
| 640 | 0.042 s |
| 1280 | 0.064 s |
| 2560 | 0.111 s |
| 5120 | 0.222 s |

### Trigraph probe
knfmt recognises trigraphs: `??/` acts as a backslash (phase-2 splice:
`int x??/\n= 1;` formats as `int x =\n    1;`), and `??>` is lexed as a
closing bracket token (a different token than the parser expected). C23
stops defining trigraphs; knfmt has no standard selector, so its actual
translation behaviour is as observed above.

### Still outstanding
UBSan/overflow re-run after `2a8d9bd`; OpenBSD/ANONERO corpus re-runs;
committed declaration benchmark mode; permanent architecture document.

## Update — UBSan/overflow re-run, corpus, architecture doc (HEAD next)

- UBSan (`-fsanitize=undefined -fno-sanitize-recover=all`, clean clang
  build) full suite: rc=0, 0 reports.
- Unsigned-overflow (`-fsanitize=unsigned-integer-overflow
  -fno-sanitize-recover=all`) full suite: rc=0, 0 reports.
- Available corpus (`/tmp/opencode/audit/corpus`, 73 files):
  normal accepted=72 rejected=1 idempotent=72 nonidem=0; the single
  rejection is `058-cpp_macro_frag.c` (deliberately malformed CPP macro
  fragment). `-s` accepted=72 nonidem=0.
  The historical 174-OpenBSD and 500+-ANONERO corpora are **not present**
  in this environment; only these 73 files plus `/tmp/opencode/audit`
  suites remain.
- `docs/architecture.md` added: permanent description of the implemented
  design and verification commands.

## Update — mutation-based fuzzing executed; non-idempotence finding

### Campaign (executed, mutations confirmed)
- Target `fuzz-parse` (production path), sanitizer build
  `-fsanitize=address,undefined,unsigned-integer-overflow,fuzzer
  -fno-sanitize-recover=all`.
- Curated corpus: 7 seed files (including `diff-014.c`,
  `simple-expr-parens-002.c`, an error fixture, high-bit bytes, flat chain).
- `-max_total_time=280` (outer `timeout 320`); **executions=2050**,
  **new_units_added=443**, coverage `cov: 2676 ft: 13879`, corpus grew from
  7 to 431 units, throughput ~36 exec/s, peak RSS 402 MB.
- This goes beyond initial seed replay: mutation-based exploration occurred.

### Finding (reproducible)
`fuzz-parse` trapped on `crash-cdf2344d...` (15058 bytes, mutated
`diff-014.c`). The trap is the harness's own assertion
`F(F(x)) == F(F(F(x)))`, i.e. a genuine non-idempotence:
`rm` on the input gives `o1 != o2 != o3` (sizes 15065/15065/15066); the
output contains corrupt tokens (`if (bparam->ifbrp_ctime 8` /
`| <|`). The production CLI accepts the input (rc=0) but its output is not
a fixed point.

Reproducer saved (not wired into `bmake test`, since it currently fails):
`tests/repro-idempotence-001.c` (sha256 16 hex above). Minimal mechanism
not yet isolated: small synthetic variants (`return a 8\n| b;` etc.) are
stable, so the trigger involves additional malformed structure.

### Remaining
Fix the malformed-input non-idempotence; run `-s` and full-formatter fuzz
targets; unavailable historical corpora remain an evidence limitation.

## Update — minimized non-idempotence reproducer (mechanism)

Reduced `tests/repro-idempotence-001.c` (15058 bytes) to a 7-line
reproducer, saved as `tests/repro-idempotence-002.c`:

```c
int f(struct ifbrparam *bparam)
{
	if (bparam->ifbrp_ctime 8
| <| 	    bparam->ifbrp_ctime > 3600)
		return (EINVAL);
	return (0);
}
```

Pass 1 emits `... 8 |` / `    <  | ...`; pass 2 emits `... 8 |  <` /
`    | ...`. The break position moves one token per pass.

Mechanism: the malformed `8 <newline> | <|` recovers into a binary
expression, and the line-break decision honours the source line break
before `|`. Reformatting the already-broken output (where the break is now
after `|`) moves the break again, so the layout is not a fixed point.
This is the "layout depends on incidental source line breaks" class.

Not yet fixed: the break-placement rule must be made independent of the
incidental source break for this recovered expression, or the malformed
input must be rejected cleanly. Both the original (15058-byte) and the
minimized reproducer are preserved; neither is wired into `bmake test`
while the defect remains.

## Update — idempotence resolved, fault injection, quadratic brace fix

Head: `main`, working tree clean. Both reproducers below are fixed.

### `tests/repro-idempotence-00{1,2}.c` — resolved (Option A)

Root cause: `token_move_prev_line()` moved a line break before a binary
operator onto the operator. When the token before an operator is itself an
operator (as happens when recovering malformed input), the moved break
then precedes the next operator, which the following pass moved again —
one operator per pass. Fix: leave an operator that immediately follows
another binary operator untouched. `F(F(x)) == F(x)` now holds in one
pass; the output is unchanged for valid input. `001` shares the root cause
and is also stable. Regression: `tests/idempotence.sh`, wired into
`bmake test`, covering both reproducers in normal and `-s` mode.

### Allocation fault injection (`bmake fault`, dev target)

`knfmt-fault` links `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc` with
`tests/fault-wrap.c`; `tests/fault.sh` fails each allocation in turn.
Result: every failure point for `valid-001.c` (0..291), `diff-014.c`
(0..1869, `-s` 0..3553) and `repro-idempotence-002.c` (0..310) exits
cleanly (status 1, no partial stdout), and `-i` leaves the file unchanged.

### Quadratic flat brace initializers — fixed

Found by mutation fuzzing as a timeout (210 KB in ~18 s). The cache in
`lbrace_cache_lookup()` did not record the absence of a nested brace, so
every field rescanned to end of input. Now near-linear: 640 KB in 0.74 s
(was 60 s); `tests/valid-292.c` 0.25 s (was 6.1 s). No output change.

### Lexer line table — latent fix

`lexer_get_lines()` now rejects `beg == 0` (would index `lx_lines[-1]`);
all callers pass `beg >= 1`, but the function now enforces its own
1-based contract.

### Verification performed

- `LC_ALL=C bmake test` green; `tests/reparse.sh`
  accepted=1692 rejected=164 fail=0.
- `bmake fault` (GCC build) green.
- Mutation fuzzing: `tests/*.c`/`*.h` seeds (928), 5000 mutated executions
  via the standalone `fuzz-parse` oracle (normal and trap-on-non-idempotence);
  1 timeout (the quadratic case above, now fixed), 0 crashes.

### Remaining / limitations

- Clang became unavailable mid-session (Homebrew LLVM directory perms
  changed to owner-only), so the Clang ASan/UBSan/unsigned-overflow suite
  and libFuzzer campaign could not be re-run after the latest fixes; the
  last instrumented run was on the pre-`idempotence`-fix tree.
- LeakSanitizer cannot run here (`ptrace` restricted): leak-freedom is not
  established by LSan.
- Full interprocedural ownership reconstruction and the C89-C23 feature
  matrix remain partially complete; brace/expression/declaration paths are
  audited, but this is not an exhaustive file-by-file claim.

## Update — Valgrind memory-checking pass (current head)

Valgrind became usable via `with-standard-malloc` (bypasses the Secureblue
`libhardened_malloc` preload that aborts under Valgrind). Using the repo
`.valgrindrc` (full leak checking, `errors-for-leak-kinds=all`):

- Scanned all 928 `tests/*.c`/`*.h` files plus a `-s` subset: **0 memory
  errors, 0 leaks** after the fix below.
- Found and fixed: `parser_func_peek1()`'s implicit-int branch left
  `func_decl` uninitialized; `parser_func_proto()` then branched on it
  ("Conditional jump or move depends on uninitialised value(s)",
  `tests/valid-533.c`). Fixed by clearing the whole `struct parser_type`.
  Commit `f3776f0`.
- Fixed a latent unsigned underflow: `lexer_get_lines()` now rejects
  `beg == 0`. Commit `d33e642`.
- Fixed the quadratic flat brace initializer scan found by the mutation
  fuzzer. Commit `2dbef70`.

Mutation fuzzing (standalone `fuzz-parse`, GCC): 4000 executions on the
fixed tree, 0 crashes, 0 timeouts; a 25000-execution campaign is recorded
separately.

Remaining limitations: Clang (ASan/UBSan/unsigned-overflow and libFuzzer)
is unavailable because the Homebrew LLVM tree is currently owner-only;
the last instrumented Clang run predates today's fixes. The
interprocedural ownership review and the C89-C23 feature matrix are
partial; the claim of exhaustiveness is not made.

## Update — C99 comment in a recovered declaration (fuzzing)

Mutation fuzzing (standalone `fuzz-parse`, idempotence oracle) found a
SIGILL at execution 7184 of a 25000-run campaign. Minimized to 84 bytes:

```c
main(void)
{
	struct //vattr vattr;
	char *name;
	const char *dir = "/var/crash";
}
```

Pass 1 emitted `struct //vattr vattr; char *name;`, turning `char *name;`
into part of the C99 comment and losing the declaration; each pass
swallowed one more declaration. Root cause: `parser_type()` used a soft
line between type tokens; after a C99 comment that line must be hard.
Fixed in `parser-type.c` (commit `6a196a5`), regression
`tests/repro-idempotence-003.c`. Follow-up campaigns: 8000 executions,
0 crashes, 0 timeouts.

## Update — final verification (head `a16c27b`)

- `fuzz-parse` harness: fixed a leak (teardown freed the arenas without
  leaving the eternal scope `init()` entered, so `style_free()` never
  `regfree()`d the compiled patterns) and two `-Wdiscarded-qualifiers`
  warnings. The harness now builds warning-free and is Valgrind-clean.
- Mutation fuzzing after all fixes: 23000 executions (8000 + 15000) over
  929 seeds, **0 crashes, 0 timeouts**; the earlier campaign's single
  finding (C99 comment swallowing a declaration) is fixed and regressed.
- Valgrind (via `with-standard-malloc`), full leak checking:
  - 928 `tests/*.c`/`*.h` normal mode: 0 errors, 0 leaks.
  - 542 `tests/valid-*.c` in `-s` mode: 0 errors, 0 leaks.
  - 80 mutated recovery inputs through the `fuzz-parse` pipeline: 0 errors.
- Ordinary suite, `tests/reparse.sh` (accepted=1694 rejected=164 fail=0),
  `lint-shellcheck`, and `bmake fault` (every allocation point 0..N for
  three inputs, normal/`-s`/in-place) all pass.
- 100k/200k flat terms: 2.0 s / 5.2 s, idempotent (unchanged).

Clang ASan/UBSan/unsigned-overflow and libFuzzer are unavailable in the
current environment (Homebrew LLVM is owner-only), so those are not
re-run on this head; the last such run predates the C99-comment and
brace/lexer fixes. Leak-freedom is established by Valgrind here, not by
LSan.

## File-level memory-safety coverage ledger

Status categories: Complete = implementation read and all applicable
categories examined; N/A = no applicable memory-management operations.

### libks implementation (16 .c, 2 .S)

Complete: arena.c, arena-buffer.c, arena-vector.c, arithmetic.c, bit.c,
buffer.c, capabilities-x86.c, consistency.c, exec.c, fs.c, init.c,
init-x86_64.c, map.c, string.c, valgrind.c, vector.c, bit-x86_64.S,
string-x86_64.S. string-x86_64.S additionally receives differential
verification against the C reference via `bmake str-match`.

### Project implementation (50 .c)

Complete: arenas.c, clang.c, comment.c, cpp-format.c, cpp-include.c,
cpp-include-guard.c, diff.c, doc.c, error.c, expr.c, file.c, knfmt.c,
lexer.c, options.c, parser.c, parser-attributes.c, parser-braces.c,
parser-cpp.c, parser-decl.c, parser-expr.c, parser-extern.c, parser-func.c,
parser-stmt.c, parser-stmt-asm.c, parser-stmt-expr.c, parser-type.c,
path.c, ruler.c, simple.c, simple-attributes.c, simple-decl.c,
simple-decl-forward.c, simple-decl-proto.c, simple-expr-printf.c,
simple-implicit-int.c, simple-stmt.c, simple-stmt-empty-loop.c,
simple-stmt-switch.c, simple-storage.c, style.c, t.c, token.c, trace.c,
util.c, fuzz-dict.c, fuzz-parse.c, fuzz-style.c.
N/A: compat-pledge.c.

### Headers

Reviewed: all project headers (declarations) and libks headers; the
macro-heavy libks/compiler.h, libks/section.h, libks/list.h,
libks/fuzzer.h and libks/asm.h were read in full.

### Reviewed invariants without a concrete counterexample

- clang.c clang_recover(): remove = cur_docs - stamp_docs[...]; stamps are
  recorded at increasing document counts along the committed path, so the
  difference is non-negative in practice; no reproducer found.
- simple-decl-proto.c is_qualifier(), simple-stmt.c add_braces(): rely on
  a guaranteed preceding "(" / "{" token; no reproducer found.
- diff.c matchchunk(), cpp-format.c max_width(), doc.c doc_column():
  bounded logic/statistics deviations, not memory access.

## C++23 toolchain note

`/home/linuxbrew/.linuxbrew/bin/g++-16` is available and accepts
`-std=c++23`; the shared boundary header (util.h included from C++) also
compiles cleanly as C++23 with it. `benchmark.cpp` still cannot be
compiled because the Google Benchmark headers/library are absent, so no
C++23 runtime validation is claimed. The Makefile default CXX (g++) is
absent, but that only affects the optional benchmark target.

## Update — 001 resolved, 002 class documented

- Fixed `tests/known-nonidempotent/001.c` (moved to
  `tests/repro-idempotence-005.c`): an assignment whose brace-initializer RHS
  was also the field stop was split into two fields, so the layout depended on
  the incidental brace position. `expr_exec_binary()` now attaches the brace
  as the operand. Normal and `-s` are idempotent.
- `tests/known-nonidempotent/00{2,3}.c` (CPP branch crossing an `extern "C"`
  block close) remain unresolved and are exercised by the expected-failure
  test `tests/known-nonidempotent.sh`.
- GCC `-fanalyzer` re-run: same 11 findings, all false positives (six
  `VECTOR_POP` guarded by `!VECTOR_EMPTY`, the arena never-NULL contract,
  `newbuckets` ownership transfer, the deliberate null-out in `style.c`, and
  the child-process fds before `execlp`).
- Mutation fuzzing (20000 executions, 933 seeds): 1 non-idempotence finding
  (the 002 class, now `known-nonidempotent/003.c`), 0 timeouts; the 001 class
  no longer reproduces.
- Valgrind: clean on the changed reproducers. `str-match` 2520/0 (AVX-512
  unavailable on this CPU). Performance unchanged: 100k 2.03 s, 200k 5.17 s.

## Update — 003 resolved (CPP branch over a block terminator)

- Fixed `tests/known-nonidempotent/003.c` (moved to
  `tests/repro-idempotence-007.c`): a CPP branch crossing the closing brace of
  an `extern "C" { ... }` block. `parser_extern()` declines such a conditional
  terminator, but the top-level recovery then had no committed position to
  rewind to. It folded the branch, which collected the leading directives and
  comments as prefixes of the first folded token, and `remove_token()` dropped
  those prefixes with the token; the rewind started at the first remaining
  token, so the content was never re-emitted (output began at `#else`).
- `parser_extern()` now sets `pr->pr_verbatim` when it declines the block. On
  the resulting failure `parser_exec()` removes every document built so far and
  emits the whole source verbatim (`parser_verbatim_all()`), using the existing
  `DOC_VERBATIM` path. The emission is the raw physical lines, so it is
  byte-stable, idempotent in normal and `-s` mode, and content-preserving. A
  conditional brace without an alternative is still parsed normally.
- Verification: full suite green; `tests/reparse.sh`
  accepted=1708 rejected=164 fail=0; semantic oracle 791 files, 0 mismatches;
  oracle selftest 14/0; Valgrind clean on the reproducer. Performance
  unchanged (100k 1.92 s, 200k 5.37 s).

## Update — mutation-fuzzing semantic corruption

Triaged every `SEMANTIC` finding from the saved campaigns (c10/c11/c12, 55
inputs) and grouped them by root cause. Before: 16 mismatches plus 18
rejections. After the fixes below: 36 oracle-clean, 18 cleanly rejected, 1
unresolved (`tests/known-nonidempotent/001.c`). A fresh 6000-execution
campaign with the phase-1 binary reported 0 semantic findings.

Root causes corrected:

- `comment.c`/`cpp-format.c`/`doc.c`: rebuilding a comment or macro used a
  NUL-terminated length, so an embedded NUL truncated the comment and
  everything after it. `comment_trim()` now returns the length, `cpp_format()`
  takes one, and `doc_literaln()` carries explicit lengths.
- `clang.c` `clang_read()`: a non-white-space byte that was the last byte of
  the input was classified as `LEXER_EOF` and dropped. It is now an ordinary
  `TOKEN_NONE` and is rejected consistently.
- `cpp-format.c`: realigning a macro whose line continuation lies inside a
  string/character literal inserted padding into the literal. Such macros are
  left untouched (`splice_inside_literal()`).
- `expr.c` `expr_doc_concat()`: a malformed recovery could emit two adjacent
  identifiers with no separator (`voiint` + `i` -> `voiinti`). The separator is
  now mandatory when `token_pair_needs_space()` says the tokens would merge.
- `parser.c`: a C99 comment followed by a discarded optional line let the next
  token be moved onto the comment line and swallowed. The line break is forced
  (a new forced `DOC_OPTLINE`, `dc_int`), keeping the deferred indentation.
- `clang.c` `clang_recover()`: folding a branch whose rewind stamp precedes the
  most recent branch crossing loses the crossed branch. It now requests the
  full-source verbatim fallback. Likewise, an unbalanced `clang-format off`
  mute is cancelled by preserving the source instead of silently truncating.
- `tests/semanticoracle.pl`: added translation phase 1 (trigraphs) and 15 more
  self-tests (29 total).

Unresolved: `tests/known-nonidempotent/001.c` (CPP-branched parameter list
plus a stray byte in the body; recovery rewinds to the first token with no
stamp and drops the parameter branch). `known-nonidempotent.sh` also treats a
first pass whose output the formatter rejects as a known failure.

## Update — bracket fields and remaining CPP class

- `parser_braces_field1()`: a malformed field sequence such as `.ar g` was
  recovered as two adjacent fields with no separator, emitting `.arg`. A
  separator is now kept when the boundary tokens would merge.
  Regression: covered by the fuzzer, no dedicated fixture.
- A second, larger CPP class remains: a branch crossed by `clang_branch()`
  whose tokens are removed and a later no-stamp recovery rewinding to the
  first token. Reproducers: `tests/known-nonidempotent/001.c` (parameter list)
  and `002.c` (include guard in a large file). The candidate fix
  (`seek_index == 0 && brch_docs > 0` -> full-source verbatim) fixes every
  observed case but also turns `tests/error-014.c` into a success, so it is
  deferred pending a stamp recorded at the last branch crossing.
- Fresh 12000-execution campaign (c14) on the fixed binary: the only findings
  are the two known CPP classes above plus the brace-field case addressed
  here.

## Update — further fuzzing classes

- `expr_doc_field()`: a member designator and its operand could also merge
  (`.` + `1` -> `.1`); the separator check is now applied there too.
- `parser_braces_field1()`: see the previous update.
- The semantic oracle now reports `ORACLE_UNSUPPORTED` for unterminated
  literals (a raw newline inside a string/character literal is a constraint
  violation and the bytes have no defined preprocessing-token meaning).
  `tests/mutate.pl` counts these separately and does not treat them as
  defects; `tests/semantic.sh` skips them. Two c14 findings (asm operand
  reindentation misread as literal content) fall into this class.
- Added the remaining reproducers to `tests/known-nonidempotent/`
  (`003.c` malformed declaration comma, `004.c` crossed-branch include guard).

## Update — punctuation preservation and the CPP crossing class

- `parser_decl_init()`: a malformed declarator list ending in a dangling
  separator (`void (*cb)(void),;aux;`) consumed the comma, failed the next
  declarator and then removed the whole initializer document because the
  declarator count was zero. The document is now kept when a separator was
  consumed, so the comma survives. `tests/known-nonidempotent/003.c` is
  promoted to `tests/repro-idempotence-012.c`; `013.c` adds neighbouring
  cases (`int a,;`, `int (*fp)(void),;`, `int b, c,;`).
- The CPP branch-crossing class (001/002/004) was investigated at length.
  `clang_branch()` removes the branch it crosses; when no stamp survives it
  rewinds to the first token and the removed content cannot be re-emitted.
  Five candidate corrections were implemented and rejected because each
  regressed valid input or the expected rejection of `tests/error-014.c`
  (details in `docs/architecture.md`). The discriminator the fix needs is
  whether the crossed branch's content was represented in a surviving
  document; a token-level "emitted" marker is set before the containing
  document is discarded. The three reproducers stay under
  `tests/known-nonidempotent/` and the root cause is documented for a
  document-level ownership fix.

## Update — CPP branch emission model and rollback boundary

- Reconstructed the intended branch emission contract: each branch is emitted
  by one parse into its own top-level document, and the repeated prefix is
  suppressed by the `DOC_MUTE(+1)`/`DOC_MUTE(-1)` pair emitted in
  `parser_doc_token_impl()`. `tests/valid-102.c` is the reference case. This
  explains why a synthetic-prefix fix duplicated `valid-102` and
  `diff-style-002`: the content was already emitted, correctly.
- Established that a token-level "emitted" flag cannot express surviving
  ownership: the flag is set during speculative parsing and the document may
  be removed afterwards, and the same token is emitted again by each repeated
  branch parse.
- Traced the remaining defect to the recovery's rollback boundary:
  `clang_recover()` removes documents back to `brch_docs`, which any
  intervening `GOOD` resets to zero, so it removes the documents holding the
  crossed branch's content and mute. The re-parse cannot restore them (tokens
  removed) and the mute balance is lost. Keeping one, two or all three
  documents was measured; each yields a lost branch, a duplicated prefix or
  duplication.
- Confirmed with the existing instrumentation that `error-014.c` is rejected
  precisely because the crossed branch's tokens are removed; every candidate
  that keeps them changes rejection into success.
- Added `tests/semantic-cpp-branches.c` covering `#if/#else`,
  `#ifdef/#elif/#else`, `#ifndef/#elifdef/#elifndef` and a conditional
  parameter list.
- Mutation campaign c15: 8000 executions, corpus 943, 0 crashes, 0 timeouts,
  5 semantic findings, 31 unsupported. Findings: 3 TOKEN_LOSS and 2
  TOKEN_DUPLICATION; all reproduce the same crossing/recovery class.

## Update — CPP branch-crossing content loss corrected

- `clang_branch()` now records `cl->lossy` when it must rewind to the first
  token because no stamp survived: the crossed branch's tokens are gone and
  re-parsing cannot reproduce their content.
- `clang_recover()` reports `deferred` when a recovery has no surviving stamp
  *and* `cl->lossy` is set. `parser_exec()` records `pr_lossy` and lets the
  normal rollback run. If parsing fails, the failure is reported unchanged, so
  `tests/error-014.c` is still rejected; only on success does it clear the
  document list (`while (doc_remove_tail(dc))`) and emit the whole source
  verbatim.
- Requiring the crossing marker as well as the missing stamp keeps valid
  inputs out of the fallback: `valid-339`, `diff-style-002` (rewind without a
  crossing) and `diff-031` (crossing with a stamp) are unaffected.
- The three reproducers are promoted to
  `tests/repro-idempotence-014.c` (`001.c`), `-015.c` (`002.c`) and `-016.c`
  (`004.c`). All three are idempotent in normal and `-s` mode and pass the
  semantic oracle.
- Fuzzing found two pre-existing token-merge defects, both of which the
  formatter now avoids:
  - A bitfield whose following comma is missing merged `1` and `b` into
    `1b`. `parser_decl_init()` keeps a separator when the previous field token
    is a number and `token_pair_needs_space()` reports a merge. Reproducer:
    `tests/repro-idempotence-017.c`.
  - A bare `__attribute__` with no argument list merged with the parameter
    identifier (`x __attribute__` became `x__attribute__`). The line between
    type and identifier is suppressed in front of an attribute and the bare
    keyword is not consumed by `parser_attributes()`, so `parser_func_arg()`
    now seeds its merge check with the type's last token. Reproducer:
    `tests/repro-idempotence-018.c`.

### Remaining findings (pre-existing, unrelated subsystems)

Two further findings from the same campaign reproduce unchanged at the
previous HEAD, so they are not caused by the branch-crossing correction. They
are kept minimized rather than fixed, since they live in different
subsystems:

- A bare `_BitInt` with no declarator after a declaration is dropped:
  `int c;` then `_BitInt` formats to `int c;` (TOKEN_DUPLICATION). This is the
  root cause behind the larger `_BitInt` stress input. Minimized as
  `/tmp/opencode/fuzz/c17-minimized/bitint-dropped.c`.
- A `clang-format` directive hidden by a trigraph line splice
  (`// clang-??/` + `format on`) is rewritten to a literal
  `// clang-format on` and `// clang-format off` is duplicated
  (COMMENT_CHANGE). Minimized as
  `/tmp/opencode/fuzz/c17-minimized/trigraph-clang-format.c`.

- Validation: `bmake test` rc=0 (semantic 793/0); reparse 1734 accepted / 164
  rejected / 0 failed; `bmake fault` rc=0; `lint-shellcheck` rc=0; Valgrind
  clean on the promoted reproducers; `-fanalyzer` reports only the
  pre-existing `VECTOR_POP` false positive. ASan/UBSan runtimes are still
  unavailable, so no sanitizer run is claimed.

### Corpus re-evaluation after the correction

Re-running the saved c10–c17 corpora (111 inputs) against the corrected
formatter: 18 rejected, 44 unsupported, and 4 semantic mismatches, all in c15
and all reproduced unchanged at the pre-correction HEAD. Eight of the twelve
mismatches previously attributed to the branch-crossing family are now clean.
The four that remain are outside that family:

- `semantic-5390` (CPP branch inside an initializer, `int x = #ifdef FOO ...`)
  and `semantic-3046` (`_BitInt` interleaving) take a recovery path with a
  surviving stamp, so the crossing/missing-stamp condition does not apply.
- `semantic-2216` (backslash line-continuation inside an identifier) and
  `semantic-7808` (trigraph `??'`) are lexer splice/trigraph cases.

### Performance

No regression on the changed paths. A 4000-line flat input formats in 0.57 s
(HEAD 0.54 s); a 10500-line input of 1500 sequential conditional initializers
takes 31.8 s and peaks at ~5.6 GB (HEAD 32.1 s, same peak). That input
reproduces a pre-existing roughly quadratic cost in the number of sequential
branch crossings (200/400/800 branches: 0.57 s / 2.36 s / 9.27 s), which the
correction neither introduces nor removes. The correction adds no document
scans: it checks two flags and, on the rare fallback, clears the document
list once.

## Update — four semantic findings triaged; three corrected; CPP cost measured

The four outstanding semantic findings were recovered from
`/tmp/opencode/fuzz/c15/` and investigated independently; two are parser
recovery and two are lexical.

### Corrected

- **`_BitInt` with no width (semantic-3046, bitint-dropped).**
  `parser_type_peek()` returned "not a type" for a bare `_BitInt`, so the
  caller fell back to a preprocessor construct and dropped the token
  (`int c;` + `_BitInt` formatted to `int c;`). A bare `_BitInt` is now treated
  as a type specifier; the declaration then fails on the missing declarator and
  is rejected, like every other bare type keyword. Fixture: `error-077.c`.

- **Trigraph replacement combining with the next character (semantic-7808).**
  knfmt maps each trigraph spelling to a token, which is not faithful to
  phase 1: `??'=` should be the single `^=` punctuator, but was lexed as
  `=` `??'` `=`, so the formatting inserted spaces and the oracle (which
  replaces at the character level) reported a mismatch. Added the compound
  spellings `??'=`, `??!=` and `??!|`. Fixture:
  `semantic-trigraph-compound.c`. knfmt has no language selector and keeps
  trigraph support uniformly, although C23 removed the requirement.

- **Splice-joined token split by continuation alignment (semantic-2216).**
  Aligning a macro's line continuations inserts padding before the backslash.
  When the splice joins the two halves of an identifier (`consumer_descrip\`
  + newline + `tor`) the padding split the token. `cpp_format()` now detects a
  splice between two non-whitespace characters and leaves the macro unaligned,
  as already done for a splice inside a literal. Fixture:
  `semantic-splice-identifier.c`.

### Remaining (precisely characterized)

- **semantic-5390** (minimized to 8 lines): a branch crossing rewinds past a
  `// clang-format off` comment. `parser_reset()` clears the recorded
  off-comment (so the re-parse can re-emit it), but the `DOC_MUTE` it emitted
  stays, and the re-parse skips re-emitting it (and `clang_format_on()` skips
  its unmute plus verbatim region) because `pr_token.unmute != nullptr` during
  branch re-parsing. The off-region is therefore suppressed. Not corrected;
  the mute lifecycle across a rewind needs a consistent owner before it can be
  changed safely.

### CPP branch-processing cost

Measured on sequential conditional initializers: 100/200/400/800 constructs
cost 0.27/0.69/2.39/9.11 s and 31/105/402/1588 MB. Documents are O(N²)
(190 k / 751 k / 2.98 M / 11.9 M). Cause: every pass returns `BRCH`, so no
stamp is taken, `clang_last_stamped()` is always null, and `clang_branch()`
rewinds to the first token, re-parsing the whole preceding file each pass.
Top-level conditionals (which reach `GOOD`) are linear. Operations that are
not the cause: passes, crossings, `lexer_seek()` are all O(1) or O(N).
No safe correction was implemented; see `docs/architecture.md`.

### Validation

`bmake test` rc=0, semantic 795 files / 0 mismatches; reparse 1738 accepted /
166 rejected / 0 failed; `diff-031.c` 7717 bytes and `diff-002.c` 160 bytes
unchanged; `error-014.c` still rejected. Oracle self-tests extended to 46
cases (trigraph compounds, identifier splices, backslash-CRLF, consecutive
splices, a splice inside a line comment, non-trigraph question marks).
