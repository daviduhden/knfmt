---
name: knfmt-lexer
description: >-
  Analyze or modify knfmt's byte-oriented lexer, token representation,
  comments, literals, preprocessor directives, trigraphs and line splices.
  Use for tokenization bugs, lost or merged bytes, token ownership, and
  lexer-level fuzzing or regression tests.
---

# knfmt lexer

## When to use

Use this skill when a change touches:

- token recognition or token contents;
- comments, string/character literals, numeric literals or identifiers;
- preprocessor directive capture;
- translation phases 1 and 2 (trigraphs, digraphs, backslash-newline
  splicing);
- token ownership, reference counting or dangling prefix/suffix tokens;
- lexer diagnostics and recovery.

Do not use it for layout decisions. If the token stream is correct and only
the document tree is wrong, use `knfmt-document-rendering` instead.

## Files

- `lexer.c`, `lexer.h`, `lexer-callbacks.h` — input mapping, token list,
  peek/consume API, diagnostics.
- `clang.c`, `clang.h` — C vocabulary, `clang_read()`, literals, comments,
  directives, branch links.
- `token.c`, `token.h` — `struct token`, flags, lists, serialization.
- `comment.c` — block-comment reindentation and trimming.
- `cpp-format.c` — macro continuation alignment (consumes directive tokens).
- `tests/t.c` — in-process lexer/token tests.
- `tests/bytes.sh`, `tests/nul.sh`, `tests/semantic-*.c` — byte contracts.

## Contracts

- Input is byte-oriented. Bytes 0x80-0xFF are opaque and preserved, even when
  they are invalid UTF-8. Do not decode UTF-8.
- LF is the line boundary; CRLF is normalized to LF; a lone CR is rejected.
- A missing final newline is accepted and one is added.
- Token text is length-delimited (`tk_str`, `tk_len`). It is not necessarily
  NUL-terminated; embedded NUL is representable in comments and literals.
- `struct token` has stable identity. Tokens are reference counted
  (`token_ref`/`token_rele`) and valid until `lexer_free()`.
- Directives and comments are never ordinary parser tokens. They are
  dangling tokens in `tk_prefixes`/`tk_suffixes` of the following or previous
  token. `doc_token()` emits them.
- `TOKEN_FLAG_CPP`, `TOKEN_FLAG_COMMENT_C99`,
  `TOKEN_FLAG_COMMENT_CLANG_FORMAT_OFF/ON`, `TOKEN_FLAG_OPTLINE`,
  `TOKEN_FLAG_OPTSPACE` and `TOKEN_FLAG_DIFF` are consumed by later stages.
  Do not repurpose them.
- `lexer_if_*` consumes; `lexer_peek_*` does not. `lexer_peek_*` must not
  cross a CPP branch. `lexer_if_pair()` jumps directly to the closing token
  and does not run the normal `lexer_pop()` branch checks.
- Translation phase 2 is performed before tokenization: the logical buffer
  drops `\`+newline and `??/`+newline, while `map[]`/`pline[]` retain the
  physical offsets and line numbers. `lexer_apply_splices()` reattaches a
  newline to the preceding token unless the splice is inside a token or a
  directive.
- Trigraph/digraph substitution is an exact-spelling alias table plus the
  compound spellings `??'=`, `??!=`, `??!|`. It is not a general
  character-level rewrite pass. Keep the uniform behavior: knfmt has no
  language-version selector.
- A backslash-newline splice that joins the halves of a preprocessing token
  keeps the token joined. Macro realignment must not pad inside a literal or
  split a joined token.
- A stray non-whitespace byte, including the last byte of the input, is its
  own `TOKEN_NONE`. It must not be silently dropped.

## Analysis procedure

1. Identify the exact byte sequence and the expected token list. Add a
   minimal reproducer under `tests/` before changing recognition.
2. Decide which layer is wrong: physical-to-logical mapping, token scanning,
   token classification, or dangling-token attachment.
3. Read `clang_read()` and the helper for the token class. Check the
   `FOR_TOKEN_TYPES`/`FOR_TOKEN_ALIASES` tables in `token.h`.
4. Check whether the token text points into the input buffer or an arena copy.
   If you rewrite it, preserve the old bytes or take ownership explicitly.
5. For directives, check `lexer_token_physical()` and
   `lexer_apply_splices()` before changing the spelling.
6. For comments, check `comment_trim()`, `TOKEN_FLAG_OPTLINE` and the
   `clang-format off/on` detection in `sense_clang_format_comment()`.

## Modification procedure

1. Keep scanning iterative. Do not add recursion proportional to input length.
2. Preserve the `lexer_if_*`/`lexer_peek_*` split and the no-cross-branch
   rule.
3. When adding a token alias, update the token table and the serialization
   used by `tests/t.c` only if the test contract requires it.
4. When moving a token, move its prefixes/suffixes first
   (`lexer_remove()`, `lexer_move_before()`, `lexer_copy_after()`), or the
   comments/directives will be lost.
5. When changing a token's text, use `token_set_str()` or an arena copy;
   never point into a temporary buffer that dies with a scope.
6. Keep diagnostics suppressed while peeking; `lexer_error()` handles this
   through `lx_peek`.

## Tests and validation

- `make test` runs the fixture suite and `t`.
- `KNFMT=./knfmt sh tests/bytes.sh` — CRLF, lone CR, high-bit bytes,
  missing final newline, splices.
- `KNFMT=./knfmt sh tests/nul.sh` — embedded NUL and trailing-byte handling.
- `KNFMT=./knfmt sh tests/semantic.sh` — independent token preservation.
- `KNFMT=./knfmt sh tests/reparse.sh` — one-pass stability over the corpus.
- `make fuzz` and `docs/FUZZ.md` — harnesses and dictionaries.

Every tokenization change needs a fixture that fails before and passes after.
If the change is about raw bytes, include the exact byte sequence in the test
script rather than only in a `.c` fixture.

## Common failures

- Treating token text as NUL-terminated. Use `tk_len`.
- Dropping the final non-whitespace byte because it was mistaken for EOF.
- Joining two tokens during recovery (`voiint` + `i`). Consult
  `token_pair_needs_space()`.
- Moving a token without moving its dangling comments/directives.
- Splitting a token at a splice during macro realignment.
- Forgetting that a C99 comment ends the physical line; a following token
  must start a new line.
- Assuming a trigraph always forms a single token; it may combine with the
  following character.
- Using `lexer_if_pair()` and expecting branch/EOF checks.

## References

- `docs/ARCHITECTURE.md` sections "Input and translation", "Preprocessor
  handling", "Token separation", "NUL bytes and explicit lengths".
- `docs/ARCHITECTURE-REWRITE-PROGRESS.md` for historical trigraph/splice
  measurements.
- `docs/FUZZ.md` for corpus and harness operation.
