# Auxiliary tooling migration

## Scope and inventory

The maintained interpreter dependency was confined to two first-party files:

| Original component | Responsibility | Replacement |
| --- | --- | --- |
| `scripts/validate-agent-knowledge.py` | Read Git changes and instructions, parse optional TOML policy, emit text/JSON contract reports | `scripts/validate-agent-knowledge.pl` |
| `tests/agent_knowledge_validator/test_validator.py` | Black-box contract tests in isolated Git repositories | `tests/agent_knowledge_validator/test_validator.pl` and `test_migration.pl` |

The originals used only standard-library modules: argument parsing, paths and
filesystem access, subprocess capture, regular expressions, TOML, JSON,
dataclasses, temporary directories and unittest. No package manifest, virtual
environment, generated interpreter program or third-party interpreter source
was found in the repository. The C library's variable named `pip` is a pipe
descriptor array and is unrelated to package management.

The knowledge workflow, README, architecture, validation contract, root agent
instructions and nine skills referenced the previous implementation. The C
build had no interpreter dependency. All previous product/audit changes in the
working tree are preserved independently of this migration.

## Shell and build refactoring

The additional audit found embedded programs in the two benchmark drivers,
the stack/reparse resource tests, the knowledge workflow's runtime probe and
one historical command example. Small resource-test input generators now use
POSIX shell loops and `printf`; benchmark timing and arithmetic belong in a
standalone auxiliary program. The release recipe's archive/checksum logic is
delegated to a shell helper. Make retains dependency resolution and short
compiler, linker, installation and test commands.

| Component | Current organization | Dependencies |
| --- | --- | --- |
| `tests/bench.sh`, `tests/bench-decl.sh` | Small shell entry points delegate to `tests/benchmark.pl` | Core Time::HiRes, File::Temp and File::Compare; optional BSD time for RSS |
| `tests/stack-budget.sh`, `tests/reparse-budget.sh` | Input generation uses shell builtins only | Existing formatter and POSIX shell tools |
| `make dist` | One command delegates archive/checksum work to `scripts/dist.sh` | Git and sha256 or sha256sum |
| Knowledge CI | Executes the standalone validator and Test::More suite | Perl 5.20+, core modules and Git |
| Helper regressions | `tests/benchmark-tools.sh` runs `benchmark-tools.pl` | Core Test::More, Archive::Tar and Digest::SHA |

These are development tools distributed with the source. Installation of the
formatter and manual is unchanged. The Perl validator remains one file; no
separately installed project modules or CPAN packages were introduced.

## Compatibility and verification

The replacement retains contract `1.0.0`, JSON schema `1.0`, rule identifiers,
scope/revision/format arguments, Git merge-base semantics and exit codes
`0`, `1` and `2`. The command's source filename changes to `.pl`; no compatibility
wrapper requires the removed interpreter. Reports remain UTF-8. Process calls
use argument lists, and configuration remains data rather than executable code.

The implementation is standalone to preserve the documented single-file
replication/deployment interface. It uses core Perl modules and Git rather
than adding a compiled dependency. The original implementation and its test
suite were retained outside the repository for differential verification and
removed from maintained sources after those checks passed. They remain
recoverable from the pre-migration Git history. Legacy bytecode-cache exclusion
patterns and historical infrastructure filename patterns are retained as
compatibility metadata, including when checking revision ranges that remove
the retired entry point. They are not runtime dependencies of this project.

Configuration supports the forms accepted by the contract: multiline basic
and literal strings, escaped Unicode, quoted/dotted keys, arrays, nested/inline
tables, booleans and decimal/hexadecimal/octal/binary integers. Values outside
the configuration schema remain errors. Syntax-error details now come from
the native parser and may use different wording; the rule messages, JSON
schema and exit codes are preserved. The entry-point filename and help text
identify the new implementation. Benchmark counts must be nonnegative decimal
integers and release versions must be safe filename/ref components; malformed
arguments now return status 2 before generating input or an archive. Failure
to create the benchmark temporary directory now returns status 1 without
generating data: the old shell driver continued with an empty directory name
and attempted to write absolute paths such as `/in.c`. The unsafe continuation
is deliberately not preserved. Very short benchmark timings need a fresh
baseline because interpreter startup is
no longer included in timestamp measurement.

Compatibility checks exposed and corrected double decoding of Unicode roots,
percent-encoded UTF-8 split into individual bytes, and path resolution through
missing components followed by `..`. Regression tests cover those cases,
already-decoded arguments, symlink boundaries, process signals, simultaneous
stdout/stderr capture and literal shell metacharacters. Git test fixtures
disable hooks and signing without changing global configuration.

The original 20 contract tests passed before replacement. The new base suite
retains those scenarios and adds TOML cases; separate regression tests exercise
the migration and the no-inline-program policy. The policy scanner has positive
and negative fixtures and is supplemented by manual review of dynamic command
construction. Perl::Critic is not installed in the available environment;
syntax, security and manual quality reviews were performed instead.

## Verification status

- Inventory, replacements, removal of the originals and integration: complete.
- Differential comparison: 58 scenarios and 162 checks passed, including
  complete decoded JSON reports, exit codes, stderr, CLI abbreviations,
  worktree/range modes, diverged merge bases, UTF-8 paths and TOML forms.
- Contract/migration tests and benchmark/distribution checks pass: 98 checks
  using Test::More/prove. Interpreter-blocking checks confirm that maintained tools
  do not invoke the removed runtime.
- Benchmark output fields, input/output sizes and idempotence agree with the
  original drivers for zero and positive counts; timing/RSS values are measured
  rather than compared byte-for-byte. Temporary cleanup, failure markers,
  archive contents/checksums and reproducibility have regression coverage.
- Product `make test` passes. GNU make and bmake retain empty incremental
  builds; helper regression targets pass with both makes. Shell/Perl syntax,
  shellcheck, workflow validation and the knowledge gate pass.
- Final searches find no maintained `.py` files, removed-runtime invocations
  or inline interpreter programs.
  Historical infrastructure names and cache exclusions, regression inputs that
  detect forbidden commands, this inventory and the C pipe-descriptor variable
  are intentional search matches.
- No additional operating-system support or remote CI execution is claimed:
  verification ran locally on Linux; BSD/macOS-specific RSS reporting remains
  optional and needs native-platform verification.
