---
name: knfmt-testing-fuzzing
description: >-
  Work with knfmt's test corpus and fuzzing infrastructure: fixture naming and
  .ok/.patch conventions, tests/knfmt.sh, idempotence and semantic oracles,
  allocation-fault tests, libFuzzer/AFL harnesses and crash reduction. Use
  when adding regression tests, reproducing a crash or validating a change
  across the corpus.
---

# knfmt testing and fuzzing

## When to use

Use for:

- adding a fixture or expected output;
- reproducing a parser/formatter crash or mismatch;
- deciding which test layer proves an invariant;
- extending the corpus, fuzzing harnesses or dictionaries;
- interpreting a failing test or fuzz finding.

Do not use it to decide whether a behavior change is correct; the relevant
subsystem skill owns that decision.

## Files

- `tests/knfmt.sh` — fixture driver.
- `tests/Makefile` — generated test list and suffix rules.
- `mkls.sh` — generator for `tests/Makefile` and lint source lists.
- `tests/*.c`, `tests/*.h` — fixtures and their `.ok`/`.patch` companions.
- `tests/idempotence.sh`, `tests/reparse.sh` — stability.
- `tests/semantic.sh`, `tests/semanticoracle.pl`, `tests/mutate.pl` — token
  preservation and mutation fuzzing.
- `tests/fault.sh`, `tests/fault-wrap.c` — allocation fault injection.
- `tests/cross-build.sh` — normal vs instrumented comparison.
- `tests/bench.sh`, `tests/bench-decl.sh` — performance without GNU-only
  `date`/`bc`/`timeout` dependencies.
- `fuzz-parse.c`, `fuzz-style.c`, `fuzz-dict.c`, `parse.dict` — harnesses.
- `docs/FUZZ.md` — operational fuzzing guide.

## Fixture conventions

- Expected output is `<basename>.ok`.
- Patch-driven input is `<basename>.patch`; it is fed on standard input and
  the fixture itself is copied into a temporary directory.
- A fixture with no `.ok` must already be canonical under `-d`: the test
  requires empty output and exit 0.
- The first line is normally `/*`; the leading comment block is stripped for
  comparison. For `-c` tests, that block is also written as the synthetic
  `.clang-format`.
- Fixtures are raw inputs: trailing whitespace, blank lines at EOF and odd
  indentation may be deliberate. `.gitattributes` disables whitespace checks
  for `tests/*.c` and `tests/*.h`; never reformat a fixture to satisfy a
  style tool.
- `valid-*` — accepted C; default formatting.
- `error-*` — rejected input; `-s`, exit 1, quiet.
- `diff-*`, `diff-simple-*`, `diff-style-*` — `-D`, `-Ds`, `-Ds` with style.
- `simple-*` — `-s`.
- `style-*` — `-c` with the extracted style, `-ts`.
- `style-simple-*` — `-s -ts`; `style-error-*` expects diagnostics but exit 0.
- `inplace-*` — `-i` clean run; the harness also compares the resulting file
  with stdout formatting of the same input.
- `trace-*` — `-s -ta`.
- `bug-*` — expected-failure regression under `-s`.
- `repro-idempotence-*` — stability reproducers, driven by
  `idempotence.sh`, not listed in `TESTS`.
- `semantic-*` — token-preservation cases, driven by `semantic.sh`.
- `known-nonidempotent/*` — known failures; the harness fails when one starts
  passing so it must be promoted deliberately.

Test scripts do not require GNU utilities: `mktemp` always receives a
template under `TMPDIR`, timing and arithmetic use Perl, and `mutate.pl` uses
an in-process alarm instead of `timeout`. Benchmark RSS uses the BSD
`time -l` format when available and reports `-` otherwise; no GNU-specific
`time` flags are used. Keep new scripts compatible with the BSD userland as
well as GNU.

## Test layers

1. **Fixture output** (`make test`): exact bytes for the selected options.
2. **Acceptance/rejection**: `valid-*` and `error-*`.
3. **Idempotence**: `F(F(x)) == F(x)` for reproducers and the whole corpus.
4. **Semantic oracle**: independent preprocessing-token comparison in normal
   mode. It is not applied to `-s`.
5. **Diff mode**: `-D` masking and partial output.
6. **Fault injection**: every allocation failure exits cleanly and leaves
   in-place targets untouched.
7. **Cross-build**: normal and instrumented builds agree on stdout, stderr
   and exit status.
8. **Performance**: time and peak RSS on generated large inputs.

## Adding a regression test

1. Reduce the failure to the smallest input that still fails.
2. Choose the owning prefix by the behavior, not by the file name you like.
3. Add the fixture and its `.ok` (or `.patch`). If the input is rejected, add
   an `error-*` case.
4. Add the fixture to `tests/Makefile`. If `mkls.sh` globs cover it, update
   the committed list to match the generator output; do not hand-edit only
   one side.
5. For idempotence, add `repro-idempotence-NNN.c` and let
   `idempotence.sh` discover it.
6. For semantic preservation, add `semantic-*.c`; `semantic.sh` discovers it.
7. Run the narrow fixture, then the relevant script. If you cannot run it,
   say so explicitly; do not mark it as executed.
8. Inspect the diff for unrelated output changes before accepting it.

## Fuzzing workflow

- Build AFL mode: `./configure --pedantic`, then
  `AFL_USE_ASAN=1 AFL_USE_UBSAN=1 CC=afl-cc make`.
- Build libFuzzer mode: `CC=clang ./configure --fuzz llvm`,
  `make fuzz-parse fuzz-style`.
- `fuzz-parse` formats three times and traps unless the last two results are
  identical. The first pass may normalize.
- `parse.dict` is the C token dictionary; pass it with
  `-dict=parse.dict`. `style.dict` is generated by `make style.dict`.
- `tests/mutate.pl <harness> [iterations] [outdir]` classifies `CRASH`,
  `TIMEOUT` and `SEMANTIC_*` findings using the independent oracle. It uses
  a temporary directory and forks the harness directly, so it does not
  depend on a system `timeout` command.
- Minimize a finding before promoting it. Keep the original bytes if the bug
  depends on a raw byte, NUL, CRLF or splice.

## Analysis and diagnosis

- A fixture mismatch with a correct parser usually means the document builder
  or style decision is wrong; use the subsystem skill.
- A fixture that passes alone but fails in the suite suggests state leakage
  (simplifier, style, static tables) or cwd-dependent discovery.
- A crash only under a sanitizer may be undefined behavior that the normal
  build tolerates; reproduce under the same compiler and flags.
- A semantic finding is stronger than an idempotence finding: it means tokens
  were lost, duplicated or reordered. Treat it as a correctness bug.
- A timeout in mutate.pl may be the known CPP quadratic case; confirm the
  input shape before treating it as new.
- `tests/diff.sh` must fail when `-d` returns success or writes to stderr.
  Do not weaken it to `|| exit 0`.
- `tests/cross-build.sh` compares `-D` by feeding a synthetic patch that marks
  every line as added; `-D` rejects file operands, so do not add it to the
  ordinary file-option loop again.

## Common failures

- Adding a fixture but not registering it in `tests/Makefile`.
- Writing `.out`/`.exp` instead of `.ok`.
- Forgetting that a fixture without `.ok` is an "already canonical" test.
- Assuming `tests/semantic.sh` covers `-s`.
- Reporting a test as run when only the command was prepared.
- Hiding a known failure by deleting it instead of keeping the
  expected-failure entry.
- Treating the first fuzz pass as the idempotence baseline; `fuzz-parse`
  deliberately compares the second and third passes.
- Using a corpus file with a missing final newline as if it had one.

## References

- `docs/FUZZ.md` for commands and dictionaries.
- `docs/ARCHITECTURE.md` sections "Verification", "Allocation fault
  injection", "Semantic-preservation oracle", "CPP branch content loss".
- `tests/knfmt.sh` for the exact flag dispatch.
- `mkls.sh` for generated lists.

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

The maintenance gate is `python3 scripts/validate-agent-knowledge.py --scope worktree`. It checks Git's changed
paths for at least one substantive instruction-file edit, required section
structure, skill frontmatter and resolvable local Markdown links. Run it after
the final instruction edit; CI runs it for pull requests. This structural
check cannot decide whether knowledge is relevant, so review that manually.
Changes to the checker, its workflow, or its validation contract must also be
reflected in the root `AGENTS.md` and this skill where relevant.
