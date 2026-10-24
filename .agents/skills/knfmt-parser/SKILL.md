---
name: knfmt-parser
description: >-
  Analyze or modify knfmt's recursive-descent C parser and Pratt expression
  parser, including declarations, functions, statements, initializers,
  attributes, inline assembly, GNU extensions and preprocessor branch
  recovery. Use for parse failures, misclassified constructs, recovery bugs
  and expression-document construction.
---

# knfmt parser

## When to use

Use for changes in:

- `parser.c` and `parser-*.c` (declarations, functions, statements, types,
  braces, attributes, inline assembly, CPP constructs);
- `expr.c` and `expr.h` (precedence, unary/binary/ternary expressions, calls,
  casts, `sizeof`, initializer braces);
- parser state, return codes, recovery, or the declaration-scan cache;
- recognition of C89-C23 and GNU constructs.

Do not use it for pure rendering changes. If the parse succeeds but the
output is wrong, use `knfmt-document-rendering` or `knfmt-style-rules`.

## Files

- `parser.c`, `parser.h`, `parser-priv.h` — entry points, dispatch, recovery.
- `parser-decl.c` — declaration groups, bitfields, initializers, simplifier
  hooks.
- `parser-func.c` — prototypes, definitions, K&R forms, function pointers.
- `parser-stmt.c`, `parser-stmt-expr.c`, `parser-stmt-asm.c` — statements,
  loops, switches, labels, `asm`.
- `parser-type.c` — type specifiers, declarators, implicit int, scan cache.
- `parser-braces.c` — brace initializers/enums and the nested-brace cache.
- `parser-attributes.c` — GNU `__attribute__` and C23 `[[...]]`.
- `parser-cpp.c`, `parser-extern.c` — directives and `extern "C"`.
- `expr.c`, `expr.h` — expression parsing and document construction.
- `t.c` — parser expression/type/attribute unit tests.

## Contracts

- Parsing is recursive descent and builds documents directly. There is no AST
  to inspect after the fact.
- Return values are `GOOD=1`, `FAIL=2`, `NONE=4`, `BRCH=8`, `HALT=0xE`.
  They may be combined. `BRCH` means the parser reached the next CPP branch.
- `struct parser` owns per-file state: lexer, options, style, simplifier,
  arenas, brace/statement depth, `pr_verbatim`, `pr_lossy`, `pr_mute`, and
  the declaration-scan cache.
- `parser_exec()` allocates one top-level `DOC_CONCAT` per parse attempt. On
  `BRCH` it calls `clang_branch()` and reparses. On failure it calls
  `clang_recover()`; a deferred lossy recovery is only committed after the
  whole parse succeeds, and then the source is emitted verbatim.
- The declaration-scan cache records only negative results for ranges of
  token offsets. It assumes scans are made at increasing positions. Its
  offsets are `size_t`.
- `EXPR_MAX_DEPTH` is 8000 and `EXPR_STACK_BUDGET` is 2 MiB. Both must be
  respected by new recursive expression code. Statement and declaration
  recursion is bounded by `PARSER_MAX_DEPTH` (8000).
- Flat left-associative binary chains use
  `expr_doc_binary_chain()` with an explicit frame array. Do not replace it
  with recursive construction.
- A parse failure must never be converted into success by substituting
  output at the recovery point. The rejection must survive.
- Parser outputs must be initialized on every return path where the caller
  can observe them.

## Analysis procedure

1. Reduce the input to the smallest construct that misparses. Check whether
   the failure is at the token, declaration-classification, statement or
   expression layer.
2. Run the parser mentally against the dispatch order:
   `parser_root()` -> declaration/function/extern/asm; `parser_stmt1()` ->
   block/expression/if/return/declaration/case/goto/for/while/label/switch/
   continue/asm/do/empty/CPP.
3. Check `parser_type_peek()` and `parser_func_peek()` before assuming a
   declaration is a function or vice versa. K&R and implicit-int forms are
   deliberately recognized.
4. For expressions, locate the rule in `expr.c`'s `rules[]` and the
   unary/binary special cases. Check whether `expr_recover()` is expected to
   hand a construct back to the parser.
5. For a recovery bug, trace `clang_stamp()`, `clang_branch()`,
   `clang_recover()`, `pr_lossy`, `pr_verbatim` and `doc_remove_tail()`.
6. For a performance issue, check the declaration-scan cache and the
   `lbrace_cache` before adding a new cache.

## Modification procedure

1. Add the failing input as a fixture first: `tests/valid-*` for accepted
   input, `tests/error-*` for rejected input, or
   `tests/repro-idempotence-*` for stability.
2. Change the narrowest parser function. Do not broaden a heuristic to fix a
   single malformed case if it changes valid classification.
3. Keep `parser_decl_peek()`, `parser_stmt_peek()` and `parser_expr_peek()`
   speculative: disable the simplifier while peeking and restore it on every
   path.
4. Preserve branch semantics. A token marked `TOKEN_FLAG_EMITTED` is not
   proof that its document survives; documents from speculative parses may
   be removed.
5. Before adding recursion, check the depth and stack budgets. Prefer an
   iterative frame array for flat chains.
6. Update `docs/ARCHITECTURE.md` when a recovery rule or parser contract
   changes, and add a `.ok` fixture when output changes.

## Tests and validation

- `make test` — `valid-*`, `error-*`, `diff-*`, `simple-*`, `style-*`,
  `inplace-*`, `trace-*`.
- `tests/repro-idempotence-*.c` via `tests/idempotence.sh`.
- `tests/semantic.sh` for normal-mode token preservation.
- `tests/reparse.sh` for corpus-wide one-pass stability.
- `t` includes `test_parser_expr`, `test_parser_type`,
  `test_parser_attributes_peek`, `test_lexer_read` and branch unlinking.
- `tests/known-nonidempotent.sh` must keep failing for `005.c` until that
  case is deliberately promoted.

If a parser fix changes output, inspect the full diff of the fixture corpus;
a cascade usually means a classification heuristic became too broad.

## Common failures

- Treating a macro use as a declaration or vice versa. Check
  `parser_cpp_*()` and the `PARSER_DECL_*` flags.
- Dropping the comma or semicolon of a malformed declaration during
  recovery. Check `parser_decl_init()` and `ruler_indent_remove()`.
- Losing content when crossing a CPP branch. Check whether a stamp survives
  and whether `cl->lossy` is set.
- Swallowing the token after a C99 comment.
- Reintroducing a rescan of an annotated declaration suffix; keep the
  negative cache valid.
- Building a deep recursive document for a flat expression chain.
- Assuming `pr_depth` guards all parser recursion; today it guards
  declarator scans only.
- Returning a document allocated in a speculative scope that has been left.

## References

- `docs/ARCHITECTURE.md` sections "Parser", "Preprocessor handling", "CPP
  branch emission model", "CPP branch content loss", "Recovery rewinds and
  crossed branches", "Brace initializers".
- `docs/ARCHITECTURE-REWRITE-PROGRESS.md` for recovery evidence and rejected
  alternatives.
- `docs/CHANGELOG.md` for the C89-C23 feature history.
