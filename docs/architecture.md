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
