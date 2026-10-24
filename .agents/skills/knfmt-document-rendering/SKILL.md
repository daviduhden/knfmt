---
name: knfmt-document-rendering
description: >-
  Analyze or modify knfmt's document model and renderer: DOC_CONCAT, groups,
  lines, indentation, alignment, mute/optional/minimize/scope nodes, width
  summaries, fitting, ruler interaction and output buffering. Use for layout
  bugs, line-breaking regressions, renderer performance and document
  invariants.
---

# knfmt document rendering

## When to use

Use for changes in:

- `doc.c`, `doc.h` — document allocation, summarization, fitting, walking and
  rendering;
- `ruler.c`, `ruler.h` — alignment documents and two-phase execution;
- line breaking, indentation, tab alignment, column limits, trailing
  whitespace or maximum consecutive newlines;
- `DOC_MINIMIZE` candidate selection;
- renderer performance or stack use.

Do not use it to decide KNF policy. Style decisions belong in
`knfmt-style-rules`; this skill is about representing and executing them.

## Files

- `doc.c`, `doc.h` — the complete document implementation.
- `ruler.c`, `ruler.h` — `ruler_insert()`, `ruler_exec()`, indent cookies.
- `expr.c` — builds expression documents, including the iterative binary
  chain.
- `parser*.c` — builds declaration/statement documents.
- `cpp-format.c` — uses a ruler for macro continuations.
- `tests/valid-*.c`, `tests/style-*.c`, `tests/diff-*.c` — output contracts.
- `tests/bench.sh`, `tests/bench-decl.sh` — time and peak RSS.

## Document model

`enum doc_type` currently contains exactly:

`DOC_CONCAT`, `DOC_GROUP`, `DOC_INDENT`, `DOC_NOINDENT`, `DOC_ALIGN`,
`DOC_LITERAL`, `DOC_VERBATIM`, `DOC_LINE`, `DOC_SOFTLINE`, `DOC_HARDLINE`,
`DOC_OPTLINE`, `DOC_MUTE`, `DOC_UNMUTE`, `DOC_OPTIONAL`, `DOC_MINIMIZE`,
`DOC_SCOPE`, `DOC_MAXLINES`.

There is no `DOC_SEQ`, no binary concat tree and no monolithic context
object.

- `DOC_CONCAT` is an n-ary list with O(1) tail append.
- `DOC_GROUP` is the only node that can refit between break mode (newlines
  allowed) and munge mode (no newlines).
- `DOC_INDENT` variants encode parens alignment, force, newline-only and
  width alignment. Underflow is clamped.
- `DOC_ALIGN` is filled by the ruler; `tabalign` uses 8-column tab stops.
- `DOC_LITERAL`/`DOC_VERBATIM` are length-delimited. Verbatim text may
  contain newlines and is not reindented except by `doc_exec_verbatim()`.
- `DOC_LINE` is a space in munge mode and a newline in break mode;
  `DOC_SOFTLINE` is empty in munge mode; `DOC_HARDLINE` always breaks.
- `DOC_OPTLINE` is honored inside `DOC_OPTIONAL` or when its value is forced
  (used after C99 comments).
- `DOC_MUTE`/`DOC_UNMUTE` suppress output while muted. Hard lines update line
  state but are not printed. The diff renderer has a separate mute mode.
- `DOC_MINIMIZE` currently supports indent minimization only. It re-renders
  candidate indents and chooses the lowest penalty for newlines and
  over-limit columns.
- `DOC_SCOPE` resets per-scope line statistics; `DOC_MAXLINES` bounds
  consecutive newlines (default 2).

## Rendering contracts

- `doc_exec1()` is iterative for `DOC_CONCAT` and `DOC_GROUP`, using an
  explicit frame stack. The stack starts at 32 frames and doubles.
- Indent/scope/maxlines/optional/minimize helpers still recurse by document
  nesting depth. Process-stack use is O(nesting), not O(document length).
- `doc_summarize()` computes `dc_sum_flat`, `dc_sum_flags` and
  `dc_sum_depth` in an iterative post-order. `DOC_SUM_PURE` subtrees may be
  accepted or rejected whole by `doc_fits1()` with `DOC_WALK_SKIP`.
- The summary is valid only while the tree is immutable during layout. Do not
  mutate a document after summarizing it without recomputing.
- `doc_walk()` is iterative with a queue. Callbacks may return
  `DOC_WALK_BREAK`, `DOC_WALK_CONTINUE`, `DOC_WALK_SKIP` or
  `DOC_WALK_RESTORE`.
- There is no document-height guard. Do not reintroduce
  `DOC_MAX_EXEC_DEPTH` unless the renderer is made recursive again and the
  change is measured.
- `doc_exec()` uses `DOC_EXEC_TRIM` for normal output and `DOC_EXEC_DIFF` for
  `-D`. Diff masking is part of the CLI contract, not a renderer detail to
  optimize away.
- `ruler_insert()` appends a `DOC_ALIGN` and records a token reference.
  `ruler_exec()` computes the columns and releases the references. A ruler
  must be executed on every path that emitted alignment documents.

## Analysis procedure

1. Reproduce with `knfmt -td` (trace documents) or `-td` plus the narrow
   fixture. Identify whether the wrong choice is a group fit, a mode, an
   indent, an align, a minimize candidate or a mute.
2. Check the document type's `doc_descriptions[]` entry and the `doc_exec1()`
   branch. Verify the value/child union is the one the builder intended.
3. Check the summary flags for the subtree. An impure node disables the
   pure-subtree shortcut; a too-wide summary rejects the group.
4. Check whether the node is inside `DOC_MUTE`, `DOC_OPTIONAL`,
   `DOC_MAXLINES` or `DOC_MINIMIZE`; these alter state around the child.
5. For alignment, check `ruler_insert()` columns, `ruler_exec()` flags and
   whether `ruler_exec()` ran after the last insert.
6. For performance, inspect `doc_summarize()`, `doc_fits()` and
   `doc_walk()` before adding a cache. Do not add a per-node memo table
   without proving the key is stable.

## Modification procedure

1. Add a minimal fixture with input and expected output before changing the
   renderer. Use `tests/style-*` for option-specific output and
   `tests/valid-*` for default KNF.
2. Preserve iterative rendering of `DOC_CONCAT` and `DOC_GROUP`. If a new node
   needs post-child work, add an explicit frame kind; do not recurse over a
   list.
3. Preserve the n-ary concat and O(1) append. Never introduce a left-nested
   binary concat tree.
4. If you add a document type, update `enum doc_type`, `doc_descriptions[]`,
   every switch in `doc.c`, `doc_summarize()` and any serializer/trace code.
5. Keep `doc_state` snapshots self-contained. Minimize and diff depend on
   restoring mode, diff state, mute, column, indent and buffer length
   together.
6. Measure with `tests/bench.sh` and `tests/bench-decl.sh` for changes that
   affect fitting, summaries or ordering. Report time and peak RSS.
7. Check `-D` and `-d` after any renderer change; they exercise different
   execution modes.

## Tests and validation

- `make test` — default, style, diff and trace fixtures.
- `tests/diff.sh`, `tests/idempotence.sh`, `tests/reparse.sh`.
- `tests/semantic.sh` after any change that could drop a token or comment.
- `tests/cross-build.sh` to compare normal and instrumented builds.
- `tests/bench.sh 10000 200000` for flat expression chains.
- `tests/bench-decl.sh` for large declaration/alignment inputs.

A layout change is not validated by a single fixture. Inspect the corpus diff
and confirm that only the intended groups changed.

## Common failures

- Emitting a newline while muted and expecting it to reappear on unmute.
  `DOC_UNMUTE` does not restore missed newlines; `DOC_MUTE` with a negative
  value can.
- Using `DOC_OPTLINE` outside `DOC_OPTIONAL` without setting the force value.
- Forgetting to recompute summaries after mutating a tree.
- Replacing `doc_fits1()`'s pure-subtree shortcut with a full walk and
  turning fitting quadratic.
- Recursing over `DOC_CONCAT` children and reintroducing stack growth.
- Computing a ruler column after `ruler_exec()`.
- Assuming `DOC_MINIMIZE` is free; it re-renders and snapshots the output
  buffer per candidate.
- Changing `DOC_EXEC_DIFF` masking while fixing a `-D` fixture.

## References

- `docs/ARCHITECTURE.md` sections "Document model", "Document types",
  "Recovered input stability", "Brace initializers".
- `docs/ARCHITECTURE-REWRITE-PROGRESS.md` for iterative-renderer and
  summary-cache measurements.
