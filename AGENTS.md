# AGENTS.md

Permanent instructions for AI agents working on knfmt. Read this file before
changing code. Load the specialized skill under `.agents/skills/` that matches
the subsystem you touch.

## What knfmt is

knfmt is a best-effort formatter for C source code following the Kernel Normal
Form (KNF) from OpenBSD `style(9)`. It reads C from files or standard input,
builds a document tree while parsing, and renders that tree with a column
limit. It is not a compiler, a preprocessor, or a semantic refactoring tool.

Two modes must be kept distinct:

- **Formatting** (the default, including `-i`, `-d` and `-D`) changes layout
  only. It must preserve the program's preprocessing tokens and semantics.
- **Simplification** (`-s`) rewrites tokens: it removes redundant parentheses,
  adds braces, inserts `continue;`/`break`, reorders declarations, drops
  parameter names from mixed prototypes, rewrites `__attribute__` names and
  removes a trailing newline from `printf`-family format strings. Its contract
  is idempotence plus the intent of each individual pass, not the
  normal-mode semantic oracle.

The implementation is described in `docs/ARCHITECTURE.md`. That file is the
current contract; `docs/ARCHITECTURE-REWRITE-PROGRESS.md` is a historical
evidence log and must not be treated as a description of the current code.

## Execution flow

1. `knfmt.c` parses options, initializes shared tables, discovers and parses
   the style, applies `pledge()` where possible, then processes each file.
2. `fileformat()` creates a per-file `struct clang`, `struct lexer` and
   `struct parser`.
3. `lexer_input_init()` performs translation phases 1 and 2: trigraph
   spellings and backslash-newline splicing are mapped while the physical
   byte offsets and line numbers are retained.
4. `lexer_tokenize()` calls `clang_read()` for every token. Directives and
   comments become dangling prefix/suffix tokens (`tk_prefixes`,
   `tk_suffixes`) attached to the next or previous ordinary token.
5. `parser_exec()` recognizes declarations, functions and statements by
   recursive descent and emits documents as it parses. There is no separate
   AST. Expressions are parsed by `expr.c` with a Pratt/precedence table.
6. `ruler_insert()` records alignment requests while parsing;
   `ruler_exec()` fills the corresponding `DOC_ALIGN` documents.
7. `doc_exec()` summarizes the tree, then renders it iteratively. `-d` runs
   `diff(1)` over the original and formatted buffers; `-D` masks unchanged
   regions and copies them verbatim; `-i` writes a sibling temporary file and
   renames it over the target.

## Module responsibilities

| Area | Files | Contract |
|---|---|---|
| Lexing | `lexer.c`, `lexer.h`, `lexer-callbacks.h`, `token.c`, `token.h` | Owns tokens; byte-oriented; length-delimited strings; comments and directives are dangling tokens |
| C vocabulary and CPP state | `clang.c`, `clang.h` | Keyword/identifier tables and `#if`/`#elif`/`#else`/`#endif` branch tracking; not LLVM Clang |
| Parsing | `parser*.c`, `parser-priv.h` | Recursive descent; returns `GOOD`, `FAIL`, `NONE`, `BRCH` or `HALT` |
| Expressions | `expr.c`, `expr.h` | Pratt parser; `EXPR_MAX_DEPTH` and `EXPR_STACK_BUDGET`; iterative flat binary chains |
| Documents | `doc.c`, `doc.h` | Document tree and iterative renderer; `DOC_CONCAT` is n-ary |
| Alignment | `ruler.c`, `ruler.h` | Two-phase declaration/prototype alignment |
| Style | `style.c`, `style.h`, `path.c` | clang-format YAML subset and upward `.clang-format` discovery |
| CPP output | `cpp-format.c`, `cpp-include.c`, `cpp-include-guard.c` | Macro continuation alignment, include sorting/regrouping, include guards |
| Simplifier | `simple.c`, `simple-*.c` | Mutually exclusive `-s` passes with per-pass state |
| CLI | `knfmt.c`, `options.c`, `file.c`, `diff.c` | Modes, diagnostics, in-place writes, unified-diff parsing |
| Infrastructure | `arenas.c`, `libks/` | Arenas, vectors, maps, buffers, checked arithmetic, filesystem helpers |

## Invariants

Unless a change is explicitly about one of these properties and is justified
with tests and measurements, preserve all of them:

1. **Semantics in normal mode.** Formatting must not add, remove, reorder or
   merge preprocessing tokens. `tests/semantic.sh` and
   `tests/semanticoracle.pl` are the independent oracle.
2. **Idempotence.** `F(F(x)) == F(x)` in normal and `-s` mode for every
   accepted input. `tests/idempotence.sh` checks the reproducers;
   `tests/reparse.sh` checks the whole corpus.
3. **Comments, literals and directives.** Preserve bytes, including C99
   comments, block-comment content, string/character literals, encoding
   prefixes, trigraphs, splices, embedded NULs and all preprocessor
   directives.
4. **Malformed input.** Reject it cleanly or recover to byte-stable output.
   Never emit partial output and never turn a rejection into a success.
5. **Memory and resources.** Arena and token ownership rules must hold.
   Every allocation failure must exit in a controlled way.
6. **In-place safety.** Failures before replacement leave the target
   byte-identical. A failure syncing the parent directory can be reported after
   the atomic rename has published the replacement, with crash durability
   uncertain. `make fault` checks allocation faults and `tests/fault.sh`
   checks pre-replacement failures.
7. **Documented options.** Existing option names, combinations and exit
   statuses remain compatible. New behavior needs `knfmt.1` and tests.
8. **Diff and partial formatting.** `-D` trusts the new-file line numbers in
   the diff and formats at document-group granularity; unchanged regions are
   copied from the source. Do not silently change that contract.
9. **Reproducibility.** The same input, options and style produce the same
   bytes regardless of build, and normal versus instrumented builds agree
   (`tests/cross-build.sh`).
10. **Performance.** Do not reintroduce quadratic scans, deep recursion or
    per-token rescans. Preserve the existing caches and iterative walks.

### `-s` is a separate contract

A simplification may change text, but only in the way its pass documents.
Do not assume that any text change is allowed under `-s`. In particular:

- reordering uninitialized declarations must not reorder initializers or
  side effects;
- removing braces must not change dangling-`else`, declaration scope or
  lifetime;
- dropping prototype parameter names must only happen when the prototype
  mixes named and unnamed parameters;
- stripping `__` from attribute names must keep the attribute meaningful;
- the `printf` pass only removes an exact trailing `\n` from the format
  string.

Every pass must remain idempotent and must have an input/expected-output
fixture.

## Preprocessor model

knfmt does not run the C preprocessor. Directives are transparent to the
parser: a token always represents an ordinary C construct and carries
directives/comments as dangling tokens. A conditional group is emitted one
branch per parse. `clang_branch()` crosses a branch by removing its tokens and
rewinding to the last surviving stamp; `DOC_MUTE`/`DOC_UNMUTE` prevent the
repeated prefix from being emitted more than once.

When no stamp survives after a crossing, content cannot be reconstructed by
reparsing. `clang_recover()` reports `deferred`, `parser_exec()` records
`pr_lossy`, and a successful parse emits the whole source verbatim. A failed
parse still fails. Do not replace this with output substitution at the
recovery point; the rejected alternatives are recorded in
`docs/ARCHITECTURE.md`.

## Memory and ownership

- Five process-wide arenas: `eternal`, `scratch`, `doc`, `buffer`, `ruler`.
  `arenas_init()` creates them once; `arenas_free()` destroys them once.
- `arenas.c` stores the arena pointers; the implementation is
  `libks/arena.c`. Allocation is a bump allocator with scope cleanup lists.
  Arena allocation failure is fatal (`err`/`errx`), by design.
- Enter a scope with `arena_scope(arena, name)`; the cleanup attribute leaves
  it on every return path. Never return a pointer allocated in a nested scope
  that has already been left; `arena_scope_validate()` traps on that misuse.
- Tokens are reference counted (`token_ref`/`token_rele`). The lexer owns the
  primary list; documents and rulers take references. Removing a token must
  move its prefixes/suffixes first.
- Buffers, vectors and maps in `libks/` have their own contracts. Read the
  header before using them. `arena_realloc()` requires the exact previous
  size. `MAP_FREE` nulls the caller's pointer; `map_free()` itself does not.
- Prefer arena allocation for parse-lifetime objects. Direct `malloc` is
  reserved for the iterative frame stacks in `doc_exec1()` and
  `expr_doc_binary_chain()`, where growth must be freed explicitly.

## Style and clang-format

- `.knfmt` in this repository is the source formatting rule
  (`BasedOnStyle: OpenBSD`, `ColumnLimit: 100`). Follow the surrounding KNF.
- `.clang-format` and `.clang-tidy` use standard LLVM-compatible configuration
  for the upstream tools. Keep knfmt-specific style options in `.knfmt`.
- Runtime style discovery is separate: knfmt searches upward from the current
  working directory for the nearest `.knfmt` or `.clang-format` (or uses
  `-c FILE`). `.knfmt` wins when both are in the same directory. It does not
  look next to each input file, and it does not merge configuration files.
- `style.c` maps a subset of clang-format options. Options that are accepted
  but not implemented are interpreted (for example
  `AlignAfterOpenBracket: AlwaysBreak` behaves as `DontAlign`). `-s` enables
  include sorting even when `SortIncludes` is not `CaseSensitive`.
- `IncludeGuards` is a `.knfmt` extension. It can insert or remove guards and
  is not gated on `-s`.
- A malformed discovered style file is non-fatal and is normally silent. Use
  `-ts` to see style diagnostics.

## Build, test and fuzzing

The build is ISO C23 (`-std=c23`, `-D_DEFAULT_SOURCE`); `libks` is C23 too.
`configure` writes `config.h` and `config.mk`. Both are generated and
untracked; edit `configure`, not the generated files.

```sh
./configure                 # or: CC=clang ./configure --pedantic
make                        # GNU make; bmake also works
make test                   # fixtures + shell tests
make fault                  # allocation-fault sweep (development)
make str-match              # SIMD vs C string matching
make fuzz                   # fuzz-parse and fuzz-style
make benchmark              # needs Google Benchmark and a C++23 compiler
```

Test conventions:

- `tests/<name>.c` fixtures are driven by `tests/knfmt.sh` through
  `tests/Makefile`.
- Expected output is `<name>.ok`; patch-driven input is `<name>.patch`.
- A fixture without `.ok` asserts that the input is already canonical under
  `-d` (empty output, exit 0).
- Prefixes select options: `valid-*`, `error-*`, `diff-*`, `diff-simple-*`,
  `diff-style-*`, `simple-*`, `style-*`, `style-simple-*`, `inplace-*`,
  `trace-*`, `bug-*`.
- The first line of most fixtures must be `/*`, because the leading comment
  is stripped and, for `-c`, used as the synthetic `.clang-format`.
- `tests/Makefile` is generated by `mkls.sh`; add new fixtures to both the
  generated list and the generator globs, or explain why not.
- Test fixtures are raw inputs and may intentionally contain trailing
  whitespace, blank lines at EOF or unusual indentation. `.gitattributes`
  disables whitespace checks for `tests/*.c` and `tests/*.h`; do not "clean"
  a fixture to satisfy `git diff --check`.
- Development-only scripts (`reparse.sh`, `bench*.sh`, `cross-build.sh`,
  `fault.sh`) are not part of `make test`.
- Test scripts must run with both BSD and GNU userlands. Use `mktemp` with an
  explicit `${TMPDIR:-/tmp}/...XXXXXX` template, Perl for high-resolution
  timing/arithmetic. Do not require `readlink -f`, `date +%s.%N`, `bc`,
  `timeout` or GNU-only `/usr/bin/time` flags. RSS reporting is optional and
  may use the BSD `time -l` format when available.

Fuzzing details are in `docs/FUZZ.md`. Sanitizer and Valgrind caveats are in
`docs/ARCHITECTURE.md`; confirm that the binary is actually instrumented
before trusting a clean run.

## Program conventions

- Use C23 and the existing idioms: `nullptr`, `bool`, designated
  initializers, `[[nodiscard]]`, `static_assert`, checked arithmetic via
  `KS_*_overflow`/`ckd_*`.
- Follow KNF and the file's existing style: tabs for indentation, braces on
  their own line for functions, no unnecessary abstraction layers.
- Keep module boundaries: do not move lexer responsibilities into the parser,
  parser decisions into the renderer, or style policy into ad-hoc literals.
- Prefer length-delimited strings (`tk_str`/`tk_len`, `doc_literaln`) where
  embedded NUL or non-UTF-8 bytes are possible.
- Check every size/length addition and multiplication. Validate indices
  before subtracting. Do not assume a NUL terminator on token text.
- Avoid new external dependencies. `libks` is vendored for a reason.
- Preserve copyright notices and the ISC license text. New files need the
  project's copyright header.
- Do not edit generated `config.h`/`config.mk`, build artifacts, or vendored
  `libks` files merely to silence a local warning.

## Working with agents

Use subagents for independent investigation when the task spans more than one
subsystem. Give each subagent a concrete file list and require file:line
evidence. Do not let a subagent's summary replace reading the code.

## Continuous Agent & Skills Maintenance

Agent instructions and skills are living repository artifacts.

Every repository modification MUST include an agent/skills synchronization
step.

Before completing any task, inspect the applicable AGENTS.md and SKILL.md
files, update the affected instructions, and validate their consistency with
the implementation.

A code change without the corresponding agent knowledge maintenance is
considered incomplete.

Never claim completion when the synchronization step has been skipped or has
failed.

This applies to every change, including code, tests, configuration,
dependencies, documentation, build files, workflows, and agent or skill
files. At the end of the task, make one complete review; repeat it only when
that review finds an error that is then corrected.

### Required review procedure

1. Inventory the changed paths with `git diff` and identify the subsystem or
   repository practice they change.
2. Inspect the root and nearest applicable `AGENTS.md` files, all relevant
   `.agents/skills/**/SKILL.md` files, and any tool-specific agent or skill
   instructions present under `.github/`, `.claude/` or `.cursor/`.
3. Select skills by their stated scope and the files or behavior changed.
   Review every matching skill; review the build/portability skill for build,
   tooling or CI changes and the testing/fuzzing skill for validation changes.
4. Update an existing skill when its supported files, procedure, tools,
   assumptions, examples, commands, validation or reusable knowledge changed.
   Create a skill only when a durable responsibility has no suitable existing
   skill; do not split or duplicate a responsibility merely to satisfy this
   policy.
5. Keep specialized agent instructions synchronized with the root policy and
   their matching skills. If a new specialized agent is added, give it the
   mandatory post-change procedure and final synchronization report below.
6. Remove stale or contradictory operational knowledge, add reusable findings,
   and check referenced paths and cross-links. Do not make cosmetic or empty
   edits, add timestamps, or record one-off execution details.
7. Run `python3 scripts/validate-agent-knowledge.py --scope worktree` after the final documentation edit. It
   checks Git change coverage, non-empty instruction edits, required section
   structure, skill frontmatter and locally resolvable Markdown links. The
   check is structural; a reviewer must still judge whether the update is
   pertinent and accurate.

The task is incomplete if applicable instructions were not inspected, an
affected instruction was not updated, the synchronization check fails, or a
limitation prevents the required maintenance. Fix the issue or state the
limitation and report FAIL; never silently omit the step.

### Post-change workflow and definition of done

Every specialized agent must include **Post-Change Knowledge Synchronization**
in its workflow: identify and review related skills after changes, update new
or changed knowledge and its own instructions, run or delegate the relevant
validation, and report the result. Every skill must include
**Maintenance & Continuous Learning** and direct its user to review and update
the skill after repository modifications, keeping reusable procedures current
without adding execution-specific notes.

Before finishing, verify each item:

- [ ] Repository changes implemented.
- [ ] Relevant agent instructions reviewed.
- [ ] Relevant skills reviewed.
- [ ] Affected AGENTS.md / SKILL.md files updated.
- [ ] Obsolete knowledge removed.
- [ ] New reusable knowledge incorporated.
- [ ] Agent and skill references validated.
- [ ] Automated maintenance checks passed.
- [ ] Final synchronization status reported.

The final response must contain **Agent & Skills Synchronization** and list
agents and skills reviewed, instruction files updated, knowledge incorporated
or corrected, validations actually run, and a final **PASS** or **FAIL**.
Do not claim a validation passed unless it was run.

Before changing code:

1. Identify the modules and skills involved.
2. Read the relevant header and the implementation, not only the `.md` files.
3. Locate the existing fixtures for the behavior.
4. State the invariant(s) at risk and the expected test evidence.
5. Check whether the behavior is fork-specific before comparing with upstream.

After changing code:

1. Re-read the diff and check ownership, overflow, error paths and
   idempotence.
2. Run the narrowest relevant test, then the corpus-level checks if the
   change is cross-cutting. Do not claim a test ran if it did not.
3. Inspect unexpected formatting differences; do not accept a large diff
   merely because tests pass.
4. Update `knfmt.1`, `README.md`, `docs/ARCHITECTURE.md` or the changelog when
   the observable contract changes.
5. Record known limitations rather than hiding them.

## Upstream and fork

The fork is based on `mptre/knfmt` at `8d14715` (version 5.3.1). There is no
upstream remote in the clone; do not assume branches are interchangeable.
Fork-specific work includes the C89-C23 coverage, C23 migration, iterative
document rendering, the expression-chain framing, the width-summary cache,
the declaration-scan and brace caches, the CPP recovery model, the semantic
oracle, fault injection, and the extended test/fuzz corpus.

When considering an upstream change:

- inspect its commit message and tests, not only the final diff;
- identify whether it touches a local invariant or correction;
- port the intent, then re-run the local reproducers;
- never drop a local regression test to make an upstream patch apply;
- if the architectures conflict, resolve in favor of the documented local
  contract and record the decision.

## Known limitations

These are real and must not be "fixed" by guesswork:

- Sequential conditional initializers (`int v = #ifdef A 1 #else 2 #endif;`)
  are quadratic because no stamp survives and each pass rewinds to the first
  token. The behavior is documented and measured.
- `tests/known-nonidempotent/005.c` is still mishandled; the expected-failure
  script fails when it starts passing, so promote it deliberately.
- Statement/declaration nesting is guarded by `PARSER_MAX_DEPTH` (8000);
  expression and declarator recursion have their own limits. New recursive
  descent must still account for stack use.
- `-D` formats at document-group granularity, not individual lines.

## Keeping instructions current

- Update `AGENTS.md` when a general convention, invariant, build command or
  repository map changes.
- Update a `SKILL.md` when its procedure, file list or validation steps
  change; delete instructions that no longer match the code.
- Do not turn skills into progress logs or duplicate `docs/ARCHITECTURE.md`.
  Link to the document instead.
- If code and documentation conflict, verify the code first, then fix the
  documentation or the code as part of the same change.

### Shared Agent Knowledge Validation Contract

Use `python3 scripts/validate-agent-knowledge.py --scope worktree` for local
changes and `python3 -m unittest discover -s tests/agent_knowledge_validator -v`
for its contract tests. The validator requires Python 3.11+ and Git, uses only
the Python standard library, and publishes contract version `1.0.0`. All
adopting repositories share the same CLI, AK001-AK006 identifiers, output
schema, and exit codes (`0` pass, `1` policy failure, `2` execution/configuration
error). For a pull request, pass both revisions with
`--scope range --base BASE_REF --head HEAD_REF`; compare from their merge base.

Read [Agent Knowledge Validation Contract](docs/agent-knowledge-validation.md)
for the complete rules, JSON schema, optional repository configuration,
warning semantics, and extension process. AK004 and AK006 can require human
review; their warnings do not prove or disprove semantic correctness. Keep the
canonical script and black-box unittest suite byte-identical across adopting
repositories. Repository-specific requirements belong in the documented TOML
configuration or a stable `AKX...` extension, never in a private fork of the
shared rules. The GitHub PR workflow runs the same tests and range command;
it does not edit files or require secrets.
