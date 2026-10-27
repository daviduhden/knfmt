---
name: knfmt-build-portability
description: >-
  Work on knfmt's configure script, Makefile/GNUmakefile, C23 build flags,
  OpenBSD-first portability, x86 assembly and fallbacks, and static analysis.
  Use for build failures, compiler warnings, platform conditionals,
  generated config files and toolchain diagnostics.
---

# knfmt build and portability

## When to use

Use for:

- `configure`, `config.h`, `config.mk`, `Makefile`, `GNUmakefile`;
- compiler/linker flags, C23/C++23 modes, `--pedantic`, `--sanitize`,
  `--fuzz`;
- OpenBSD, Linux, macOS, musl and i386 differences;
- `libks` x86-64 assembly, SIMD dispatch and C fallbacks;
- static-analysis targets;
- diagnosing a build failure or warning.

Do not use it to change formatter behavior. Build changes must not alter
output; verify with the test and cross-build layers when possible.

## Build system map

- `configure` is POSIX `sh`. It probes the compiler by compiling and linking
  a small program with `-Werror`, then writes `config.h` and `config.mk` into
  the current directory. It accepts `--benchmark`, `--fuzz`, `--pedantic`,
  `--sanitize` and `--help`; unknown options and missing option values are
  rejected.
- `config.h` contains `HAVE_PLEDGE`, `FUZZER_*` and a fallback `pledge`
  prototype.
- `config.mk` contains `CC`, `CFLAGS`, `CXX`, `CXXFLAGS`, `CPPFLAGS`,
  `DEBUG`, `AFLAGS`, `ASFLAGS`, `LDFLAGS`, install paths and the suffix
  rules. The `.S.o` rule passes both assembly flag variables. It is generated
  and untracked.
- `Makefile` is written in the common subset of bmake and GNU make.
  `GNUmakefile` is a four-line shim that defines `.CURDIR` and `.OBJDIR` and
  includes `Makefile`; GNU make reads it automatically, bmake reads
  `Makefile` directly. Do not delete either.
- `tests/GNUmakefile` is the same shim for the fixture suite.
- `mkls.sh` regenerates source and test lists with `mkls` and `ksh`. The
  committed `tests/Makefile` must stay consistent with it.

## Flags and modes

- C is compiled as `-std=c23` with `-D_DEFAULT_SOURCE`; `libks` is C23 too.
- The C++ benchmark harness is `-std=c++23`. It needs Google Benchmark and a
  working `g++`/`c++`; do not make it a prerequisite for normal builds.
- `--pedantic` adds `-g -O2 -Wall -Werror -Wextra -Wcast-qual
  -Wmissing-prototypes -Wpedantic -Wshadow -Wsign-conversion
  -Wwrite-strings` plus compiler-specific probes.
- `--sanitize X` adds the sanitizer subset the compiler can link and
  `-fno-sanitize-recover=all`. Always inspect the resulting `CFLAGS` and the
  binary; a configure option can silently produce an uninstrumented build if
  the runtime cannot link.
- `--fuzz llvm` implies `--sanitize address`, links the fuzzer runtime and
  defines `FUZZER_LLVM`. Without it, harnesses use the AFL-mode `main`.
- `DEBUG` is folded into `CFLAGS` and also passed separately. Because
  `config.mk` assigns it with `=`, an environment `DEBUG` is ignored; pass
  `make DEBUG=...` on the command line or configure with the desired flags.
- Changing compiler flags requires `make clean`; there is no dependency on
  `config.mk`.

## Targets

- `all`, `knfmt`, `t`, `knfmt-fault`, `strmatch`, `benchmark`.
- `test` builds `knfmt` and `t` and recurses into `tests/`.
- `fault` links with `--wrap=malloc,--wrap=calloc,--wrap=realloc` and runs
  `tests/fault.sh`. This is ELF/GNU-ld/lld-specific.
- `str-match` was historically bmake-only because it used the `:N` modifier.
  It is now built from `SRCS` plus `tests/str-match.c`, which works with both
  makes.
- `fuzz`, `fuzz-parse`, `fuzz-style`, `fuzz-dict`, `style.dict`.
- `lint`, `lint-clang-tidy`, `lint-cppcheck`, `lint-include-what-you-use`,
  `lint-shellcheck`.
- `install` uses `${DESTDIR}${BINDIR}` and `${DESTDIR}${MANDIR}`. `PREFIX`
  is recorded by `configure`, and `BINDIR`/`MANDIR` default to
  `$(PREFIX)/bin` and `$(PREFIX)/man`, so `make PREFIX=... install` also
  works. Explicit `BINDIR`/`MANDIR` values still win.

## Portability contracts

- Prioritize OpenBSD, but preserve the existing Linux, macOS and musl
  support. Do not remove `GNUmakefile`, fallbacks or compatibility shims
  because BSD make is preferred.
- `pledge()` is applied after style parsing because `BasedOnStyle` may execute
  `clang-format`. Diff mode skips pledge because it executes `diff(1)`.
- `compat-pledge.c` provides a no-op when `HAVE_PLEDGE` is absent.
- `libks/asm.h` supplies the ELF `.note.GNU-stack` prologue for Linux
  x86-64. Its spelling must remain
  `.section .note.GNU-stack,"",%progbits`. A source reformat previously
  corrupted it and broke Linux assembler input; do not let a formatter touch
  assembly macro bodies blindly.
- `libks/bit-x86_64.S`, `libks/string-x86_64.S`, `libks/init-x86_64.c` and
  `libks/capabilities-x86.c` are x86-64 fast paths. Other architectures use
  the C implementations in `libks/bit.c`, `libks/string.c` and `libks/init.c`.
- The Valgrind client requests are x86-64-only and compiled out under
  `NDEBUG`.
- `--wrap` is not supported by Apple ld64; `make fault` is a Linux/OpenBSD
  development target.
- `make dist` uses `git archive` and `sha256` and assumes a release tag; the
  current clone has no tags.
- The repository has no CI. GNU make is still supported through
  `GNUmakefile`, and bmake reads `Makefile` directly; both should keep
  working.

## Analysis procedure

1. Reproduce the failure with the exact `CC`, `CFLAGS`, `CPPFLAGS`,
   `LDFLAGS` and target. Record them.
2. Check whether `config.h`/`config.mk` are stale. Re-run `./configure` and
   `make clean` if flags changed.
3. For a missing symbol or header, check the configure probe and the
   fallback in `libks/compiler.h`, `compat-pledge.c` or the relevant
   `#if defined(...)` branch.
4. For an assembler error, preprocess the `.S` file and inspect the expanded
   macro before changing C.
5. For a warning, fix the code or the narrow suppression. Do not add
   `-Wno-*` globally unless the warning is demonstrably inapplicable.
6. For a compiler flag, confirm that the local toolchain supports it.
   `-std=c23` requires a recent compiler; older systems may need a newer
   toolchain or an explicit standard. Verify each target locally.
7. For static analysis, read the tool's config (`.clang-tidy`,
   `cppcheck-suppressions.xml`, the `lint-*` recipe) before changing code.

## Modification procedure

1. Edit `configure` for generated variables and suffix rules. Do not hand-edit
   `config.mk`.
2. Keep `Makefile` in the bmake/GNU make common subset. Avoid `:N`, `:S`
   beyond the existing substitutions, and GNU-only functions.
3. Add flags through the existing `CFLAGS`/`CPPFLAGS`/`LDFLAGS` variables so
   dependencies and `--pedantic`/`--sanitize` continue to work.
4. Preserve `DESTDIR`, `BINDIR`, `MANDIR`, `INSTALL` and `INSTALL_MAN`.
5. Do not make `benchmark` or a sanitizer runtime a hard dependency of
   `make`/`make test`.
6. If you add a source file, add it to `SRCS` and to the relevant lint lists;
   update `mkls.sh` if the lists are generated.

## Tests and validation

- `./configure && make` on the target platform.
- `make test` under the same flags.
- `make fault` on ELF platforms.
- `make str-match` under both GNU make and bmake if available.
- `tests/cross-build.sh` for normal vs sanitizer behavior.
- `make lint-*` where the tools are installed.
- `sh -n`/`shellcheck` for shell changes; `mandoc -Tlint -Wstyle knfmt.1` for
  the manual.

Do not claim a platform build succeeded unless it was actually performed.

## Common failures

- Editing `config.mk` instead of `configure`; the next configure overwrites
  it.
- Adding a GNU make function to `Makefile` and breaking bmake.
- Adding a bmake modifier and breaking GNU make (the old `str-match` case).
- Assuming `--sanitize` instrumented the binary without checking `CFLAGS`.
- Forgetting `-D_DEFAULT_SOURCE`, `-I${.CURDIR}` or the generated `config.h`.
- Treating an x86-64 assembly/SIMD path as portable; always provide or keep
  the C fallback.
- Applying a code formatter to `.S` macro bodies without checking the
  expansion.
- Assuming the OpenBSD `pkg_add` package matches the fork's version.

## References

- `docs/ARCHITECTURE.md` sections "Verification", "Sanitizer
  instrumentation", "Valgrind memory checking".
- `docs/FUZZ.md` for fuzzing builds.
- `.clang-tidy`, `cppcheck-suppressions.xml`, `.clang-format`.

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

The repository's CI is defined in `.github/workflows/agent-sync.yml`; it runs
`python3 scripts/validate-agent-knowledge.py --scope range --base <pull-request-base> --head HEAD` on every pull request.
The checker is a POSIX shell script using Git and standard utilities, so keep
it independent of the C toolchain and invoke it directly from CI. Run
`python3 scripts/validate-agent-knowledge.py --scope worktree` locally after making the required instruction
updates. If the workflow or checker changes, update this skill and the root
`AGENTS.md` procedure together, then validate the checker itself.
