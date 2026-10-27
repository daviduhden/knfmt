---
name: knfmt-simplification
description: >-
  Analyze or modify the -s simplifier: pass state, ordering, declaration
  grouping, brace and statement changes, expression rewrites, attributes,
  forward declarations and prototypes. Use for simplification output,
  idempotence, semantic-safety or pass-interaction bugs.
---

# knfmt simplification

## When to use

Use for changes to:

- `simple.c`, `simple.h` and any `simple-*.c`;
- the `-s` output of declarations, statements, expressions, attributes,
  prototypes, includes or storage classes;
- pass ordering, mutual exclusion or `-D` gating;
- idempotence of a simplification.

Do not apply the normal-mode semantic oracle to `-s`; `-s` legitimately
rewrites tokens. Its contract is the documented intent of each pass plus
idempotence.

## Files

- `simple.c`, `simple.h` — pass state machine and `simple_cookie`.
- `simple-decl.c`, `simple-decl-forward.c`, `simple-decl-proto.c` —
  declaration grouping/sorting, forward declarations, prototype names.
- `simple-stmt.c`, `simple-stmt-empty-loop.c`, `simple-stmt-switch.c` —
  braces, empty loops, `default`.
- `simple-expr-printf.c`, `simple-expr-sizeof` behavior in `expr.c` —
  format-string newline, `sizeof` parentheses, redundant parentheses.
- `simple-attributes.c`, `simple-implicit-int.c`, `simple-storage.c`.
- `simple.c` callers in `parser-decl.c`, `parser-func.c`, `parser-stmt.c`,
  `parser-type.c`, `parser-braces.c`, `cpp-include.c`, `expr.c`.
- `tests/simple-*.c`, `tests/diff-simple-*.c`, `tests/style-simple-*.c`.

## Pass model

`struct simple` has one state per `enum simple_pass`:
`NOP`, `DISABLE`, `ENABLE`, `IGNORE`. `SIMPLE_FORCE` enables a pass even
without `-s`; `SIMPLE_IGNORE` suppresses it, including nested use.

`simple_enter()`:

1. saves the previous state in a `simple_cookie`;
2. returns disabled if `-s` is off and the pass is not forced;
3. disables a pass that would re-enter another active pass, except for the
   explicit parent/nested exception table;
4. otherwise enables it.

`simple_leave()` restores the saved state and is installed as a cleanup
function, so it runs on every return path. Do not replace it with manual
restoration.

Passes are entered by parser code as it parses. There is no global pass loop.
The exception table is directional (`{parent, nested}`); adding an entry
changes which transformations can compose.

## Pass inventory and intent

- `SIMPLE_ATTRIBUTES`: strip one leading and trailing `__` from
  `__attribute__((...))` identifiers of length at least 4.
- `SIMPLE_BRACES`: add a trailing comma to a multi-line brace initializer or
  enum when the last token is not `,` or `)`.
- `SIMPLE_CPP_SORT_INCLUDES`: sort/regroup include blocks. Forced by
  `SortIncludes: CaseSensitive`; also active under `-s`.
- `SIMPLE_DECL`: group and sort uninitialized, movable block-scope
  declarations by type and pointer/array slot; drop empty declarations.
- `SIMPLE_DECL_FORWARD`: sort contiguous root-level `struct X;` and
  `typedef struct X X;`.
- `SIMPLE_DECL_PROTO`: drop parameter names from a prototype only when named
  and unnamed parameters are mixed.
- `SIMPLE_EXPR_PARENS`: omit redundant grouping parentheses when the operand
  does not require them.
- `SIMPLE_EXPR_PRINTF`: remove an exact trailing `\n` from the format string
  of the listed `err`/`warn`/`perror` family.
- `SIMPLE_EXPR_SIZEOF`: add parentheses to `sizeof` without a parenthesized
  operand; removal is handled by `EXPR_PARENS`.
- `SIMPLE_IMPLICIT_INT`: insert `int` for a bare `unsigned`/`signed`
  (optionally after `static`).
- `SIMPLE_STORAGE`: move a single `static`/`extern` to the front of the type
  range when movable.
- `SIMPLE_STMT`: add or remove braces around control-statement bodies based
  on line count and the existing brace state.
- `SIMPLE_STMT_EMPTY_LOOP`: turn an empty loop body into `continue;`.
- `SIMPLE_STMT_SWITCH`: turn `default: ;` into a `default:` with `break;`.

## Contracts and invariants

- Every pass must be idempotent: a second `-s` run produces identical bytes.
- `expr_doc_parens()` calls `simple_leave()` before formatting the operand so
  nested removable parentheses are canonicalized in the same pass. Keep this
  ordering; it is why one-pass `-s` is stable.
- Do not reorder initialized declarations or declarations whose
  initialization has side effects. `SIMPLE_DECL` only moves uninitialized,
  movable tokens.
- Brace removal must preserve dangling-`else` binding, declaration scope and
  lifetime. `SIMPLE_STMT` refuses to remove braces when the result needs them.
- Prototype name removal must not create unnamed parameters in a prototype
  that had none. Check `simple_decl_proto`'s named-parameter detection.
- Attribute rewriting must not turn a valid attribute into an invalid or
  different one. Require the `__name__` shape.
- The `printf` pass must match the exact trailing `\n"` shape; do not strip
  newlines from arbitrary strings.
- Under `-D`, passes that move or delete tokens must check `TOKEN_FLAG_DIFF`
  for the whole construct. `SIMPLE_DECL`, `SIMPLE_DECL_FORWARD`,
  `SIMPLE_STMT` and include sorting do this; new passes must follow the same
  rule or document why the document mute is sufficient.
- `simple_disable()`/`simple_enable()` bracket speculative parses. Do not
  leave the simplifier enabled while peeking.

## Analysis procedure

1. Reproduce with `knfmt -s` and with `knfmt -s` twice. Confirm the failure is
   non-idempotence, a wrong transformation, or a missing transformation.
2. Identify the pass from the parser call site and the output shape. Use
   `-t`/`-ta` tracing if needed.
3. Check whether the pass is entered, forced, disabled by an active parent,
   or excluded by the exception table.
4. For declaration grouping, inspect `simple_decl_type()`,
   `classify()`, the type key and the slot computation. Verify that
   initializers, arrays and non-movable tokens are kept in place.
5. For braces, inspect `simple_stmt_need_braces()`, `add_braces()` and
   `remove_braces()`. Check the rendered line count rather than guessing.
6. For a new pass, prove semantic correctness and add a fixture before
   wiring it into `simple.c`.

## Modification procedure

1. Add `tests/simple-<area>-NNN.c` and `.ok`, and a
   `tests/diff-simple-*` case if `-D` behavior changes.
2. Use `simple_enter()`/`simple_leave()` with a cleanup cookie. Do not
   hand-roll state restoration.
3. Keep pass order and exclusivity explicit. If a pass must compose with
   another, add a directional exception and a test for both orders.
4. Preserve token ownership when moving or deleting tokens: move prefixes
   and suffixes first, and use `token_ref()` before inserting a token into a
   second list.
5. When the transformation rewrites text, update the token pointer/length or
   emit a literal document with an explicit length.
6. Re-run `-s` twice and inspect the diff. A pass that is correct once but
   drifts on the second run is not acceptable.
7. Update `knfmt.1` only if the user-visible `-s` contract changes; update
   `docs/ARCHITECTURE.md` when a pass ordering rule changes.

## Tests and validation

- `make test` — `simple-*`, `style-simple-*`, `diff-simple-*`.
- `tests/idempotence.sh` — every reproducer, normal and `-s`.
- `tests/reparse.sh` — corpus-wide one-pass stability, normal and `-s`.
- `tests/simple.sh` — checks that `-s` actually produces a diff for its
  inputs and that pass state does not leak between files.
- `tests/diff-simple-*` — `-Ds` partial formatting.
- `tests/known-nonidempotent.sh` — expected failures must not be hidden.

Do not claim the semantic oracle covers `-s`; it does not. If a pass could
change runtime behavior, justify it explicitly and add a targeted test.

## Common failures

- Adding a pass without a `simple_cookie`, leaking state into the next file.
- Re-enabling a pass after a speculative parse.
- Sorting declarations with initializers or side effects.
- Removing braces from an `if`/`else` chain and changing binding.
- Removing parentheses around a cast, dereference or binary/ternary
  expression that needs them.
- Stripping `__` from an identifier that is not an attribute argument.
- Inserting `continue;` into a loop whose body was not actually empty.
- Adding a `break` after a non-empty `default`.
- Assuming `-D` mutes token mutations; structural passes must check
  `TOKEN_FLAG_DIFF`.

## References

- `docs/ARCHITECTURE.md` sections "Simplifier (`-s`)", "C99 comments end the
  line", "Malformed declarations and punctuation".
- `simple.c`'s exception table and `simple.h` pass enum.
- `docs/CHANGELOG.md` for pass-specific bug history.

## Maintenance & Continuous Learning

When this skill is used to modify the repository, review it once after the
implementation. Update it when the supported procedures, tools,
configurations, assumptions, examples or validation commands changed, when a
reusable finding improves future work, or when an instruction proved wrong.
Keep the examples and validation steps aligned with the current repository;
remove obsolete or contradictory guidance. Do not add cosmetic edits,
timestamps or execution-specific notes that will not help future tasks.

Also inspect the applicable `AGENTS.md` and any related specialized agent
instructions, keep their responsibilities and references synchronized with
this skill, and run `python3 scripts/validate-agent-knowledge.py --scope worktree` after the final instruction
edit. That check verifies structural conditions; use judgment to ensure the
updated knowledge is pertinent and accurate. Report which agent and skill
instructions were reviewed or updated and whether the check passed. A
repository change is incomplete until this synchronization is done.
