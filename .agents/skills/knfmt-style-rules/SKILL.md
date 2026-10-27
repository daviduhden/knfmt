---
name: knfmt-style-rules
description: >-
  Apply or change knfmt's KNF formatting rules and its clang-format option
  mapping: spacing, indentation, declarations, prototypes, control blocks,
  initializers, comments, directives and include ordering. Use when output
  differs from style(9), a clang-format option is added or interpreted, or a
  style fixture needs a new expected output.
---

# knfmt style rules

## When to use

Use for changes to:

- default KNF output (spacing, indentation, braces, declarations, operators,
  control statements, initializers, comments);
- the `.clang-format` option set or its mapping in `style.c`;
- include sorting, regrouping or include guards;
- style diagnostics and `-ts` tracing;
- `tests/style-*` fixtures.

Do not use it for tokenization, parser recognition or document rendering
internals. A style change is usually implemented as a parser building a
different document, not as a renderer special case.

## Files

- `style.c`, `style.h` — option table, defaults, YAML parsing, discovery,
  regex categories and include guards.
- `path.c` — upward search for `.clang-format`.
- `parser-decl.c`, `parser-func.c`, `parser-stmt.c`, `parser-type.c`,
  `expr.c` — where KNF decisions become documents.
- `cpp-format.c`, `cpp-include.c`, `cpp-include-guard.c` — directives,
  includes and guards.
- `knfmt.1` — user-visible option contract.
- `tests/style-*.c`, `tests/style-simple-*.c`, `tests/style-error-*.c`.

## What is objective KNF

These are requirements, not preferences:

- tabs for block indentation, tab stops at 8 columns;
- one statement per line; braces on their own line for function bodies and
  multi-line blocks;
- a space after keywords (`if`, `for`, `while`, `switch`, `return`);
- no space between a function name and `(`; a space after `,` and `;` in
  `for`;
- binary operators surrounded by spaces; unary operators attached;
- pointer `*` attached to the identifier in declarations;
- declarations at the start of a block;
- comments and preprocessor directives preserved byte-for-byte except for
  allowed reindentation;
- continuation lines aligned or indented consistently with the active style.

Subjective choices such as `AlignAfterOpenBracket`, `AlignOperands`,
`BreakBeforeBinaryOperators`, `BreakBeforeBraces` and `UseTab` are controlled
by the style options. Do not hard-code a preference that an option already
controls.

## Style model contracts

- `struct style` stores `options[Last]` and per-category/guard vectors. There
  are no individual fields per clang-format option.
- `style_defaults()` sets values but not the `isset` bit, so the dump only
  shows explicitly configured options.
- If no `.clang-format` is found, `BasedOnStyle` is set to `OpenBSD`. An empty
  or comment-only file counts as present and therefore does not set it.
- Discovery starts at the current working directory and walks upward. It does
  not search relative to each input file and does not merge files.
- `-c FILE` bypasses discovery. A missing explicit file is fatal; a malformed
  discovered file is non-fatal and normally silent unless `-ts` is used.
- Only YAML documents with no `Language` or `Language: Cpp` are applied.
- `BasedOnStyle: OpenBSD` is a no-op; `InheritParentConfig` is accepted but
  does not load a parent. Other named styles execute
  `clang-format -dump-config`.
- `BreakBeforeBraces: Linux` forces `AfterFunction`; only `AfterFunction`,
  `AfterStruct`, `AfterUnion` and `AfterEnum` are consulted.
- `AlignAfterOpenBracket`: only `Align` is special; other values behave as
  `DontAlign`.
- `AlignOperands`: only `Align` is special; `AlignAfterOperator` behaves as
  `DontAlign`.
- `BreakBeforeBinaryOperators: All` and `NonAssignment` both break before
  non-assignment operators; assignment operators always break after.
- `ColumnLimit: 0` means unlimited (`UINT_MAX`).
- `UseTab`: anything other than `Never` is treated as tabs, but only full
  8-column groups become tabs; a 4-column continuation uses spaces.
- `SortIncludes`: only `CaseSensitive` enables sorting by itself. Under `-s`
  the pass runs regardless and sorts bytewise/case-sensitively.
- `IncludeCategories` regexes are compiled with `REG_ICASE` by default;
  `CaseSensitive: true` recompiles them without it.
- `IncludeGuards` is a knfmt extension. It runs independently of `-s` and can
  insert or remove a canonical guard. Do not move it behind `-s` without an
  explicit contract change.

## Analysis procedure

1. Reproduce with the smallest input and the default style. Determine whether
   the difference is a KNF requirement, a style-option choice, or a bug.
2. Find the document builder for the construct. Search for the token or
   `style(st, Option)` in the relevant parser file.
3. Check the style default and the fixture's synthetic `.clang-format` (the
   leading comment block is extracted for `-c` tests).
4. Check whether the option is documented as supported, interpreted or
   unsupported in `knfmt.1`; if not, the documentation is part of the fix.
5. For include behavior, check `cpp_include_add()` collection boundaries,
   `include_cmp()`, `style_include_priority()` and `is_main_include()`.
6. For guards, check `sense_include_guards()`, `path_to_guard()` and the
   removal/insertion path in `cpp-include-guard.c`.

## Modification procedure

1. Add or extend a `tests/style-<Option>-NNN.c` fixture with a leading
   comment that is the exact `.clang-format` content, plus a `.ok` file when
   output equality is asserted.
2. Implement the decision in the document builder, using
   `style(st, Option)` rather than a new global.
3. Add the option to `style.c`'s keyword table only if it is a new option;
   add it to `style_defaults()` and `knfmt.1`.
4. If the option is accepted for compatibility but not implemented, interpret
   it explicitly and document the interpretation. Do not silently ignore it.
5. Keep KNF defaults unchanged unless the task explicitly changes them.
6. Run the style fixtures and inspect the whole corpus diff for cascades.

## Tests and validation

- `make test` — all `style-*` fixtures.
- `tests/style-error-*` assert diagnostics and exit 0.
- `tests/style-trace-*` use `-tss`; `tests/style-BasedOnStyle-*` use the
  `clang-format` stub.
- `tests/style-IncludeGuards-*` cover insertion and removal.
- `tests/include-categories.sh` covers regrouping and main-include handling.
- `tests/diff-style-*` cover style plus `-D`.
- Normal-mode output must still pass `tests/semantic.sh`.

A style change needs both input and expected output. A fixture without `.ok`
only asserts canonical input, which is not enough for a new rule.

## Common failures

- Adding a special case in `doc.c` for a construct that the parser should
  build differently.
- Changing a default when the fixture intended an explicit option.
- Assuming an option is implemented because it is parsed. Check where it is
  read.
- Forgetting that the test harness derives `.clang-format` from the leading
  comment block.
- Sorting includes without checking mixed `<>`/`""` forms and the main
  include rule.
- Removing an include guard for a header that the `IncludeGuards` regex does
  not match; the current behavior is intentional and tested.
- Documenting clang-format semantics instead of knfmt's actual interpretation.

## References

- `docs/ARCHITECTURE.md` sections "Preprocessor handling", "Simplifier",
  "Macro realignment and literals".
- `knfmt.1` for the option table.
- `.clang-format` for the repository's own formatting rules.
- `tests/style-*` for executable examples.

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
this skill, and run `tools/check-agent-sync.sh` after the final instruction
edit. That check verifies structural conditions; use judgment to ensure the
updated knowledge is pertinent and accurate. Report which agent and skill
instructions were reviewed or updated and whether the check passed. A
repository change is incomplete until this synchronization is done.
