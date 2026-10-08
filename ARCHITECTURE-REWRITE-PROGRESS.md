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
