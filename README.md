# knfmt

The [knfmt(1)][knfmt] utility formats source code files according to
[style(9)][style] from OpenBSD in a best effort fashion. By default, source
code is read from standard input and the formatted source code is written to
standard output:

```sh
$ knfmt <knfmt.c
```

It can perform an in place edit by writing back the formatted source code. The
replacement synchronizes the temporary file and its parent directory; a parent
directory sync error can be reported after the replacement is visible:

```sh
$ knfmt -i knfmt.c
```

It can produce a diff between the original and formatted source code:

```sh
$ knfmt -d knfmt.c
```

It can subjectively try to simplify the source code:

```sh
$ knfmt -s knfmt.c
```

Given a unified diff, it can format only the changed lines. The diff is read
from standard input and names the files to format in its `+++` headers; file
operands are not accepted with `-D`. It is compatible with `-d`, `-i` and
`-s`:

```sh
$ git diff | knfmt -Dds
```

It can interoperate with clang-format when a standard `.clang-format` file is
found by searching upward from the current working directory. `knfmt` first
looks for its `.knfmt` configuration, which supports the OpenBSD base style and
knfmt-specific options, then falls back to `.clang-format`. Only the nearest
configuration is used; files are not merged. See [knfmt(1)][knfmt] for details.

The implementation is further described in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Documentation

The command-line reference is [knfmt.1](knfmt.1); development guides live
under [`docs/`](docs/):

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — implementation, contracts and
  verification.
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — release history.
- [docs/FUZZ.md](docs/FUZZ.md) — fuzzing guide.
- [docs/python-migration.md](docs/python-migration.md) — auxiliary tooling
  migration inventory and verification.
- [docs/ARCHITECTURE-REWRITE-PROGRESS.md](docs/ARCHITECTURE-REWRITE-PROGRESS.md)
  — chronological evidence log of the rewrite.

## Agent instructions

[AGENTS.md](AGENTS.md) contains the permanent project instructions for AI
agents. Specialized procedures live in [.agents/skills/](.agents/skills/).

[knfmt]: https://www.basename.se/knfmt/
[style]: https://man.openbsd.org/style.9

## Installation

### OpenBSD

```sh
# pkg_add knfmt
```

The OpenBSD package tracks the upstream release and may be older than this
fork.

### From source

Building requires a C23 compiler (including `nullptr`, `bool` and standard
attributes), POSIX shell, make and a C library with the BSD interfaces used by
`libks`. CI targets Clang 19 with GNU make and GCC 14 with bmake on Linux;
other platforms still require local verification. `configure` rejects a
compiler without the required C23 features. Normal builds need no external
library. Development checks use Perl 5.20+, core modules and Git, including
`make test` and the agent knowledge checker's Test::More suite. Benchmark builds
additionally require a C++23 compiler, `pkg-config` and Google Benchmark. `ksh` and `mkls` are needed only to
regenerate the committed source/test lists.

The installation prefix defaults to `/usr/local` and can be altered using the
`PREFIX` environment variable when invoking `configure`, or on the
`make install` command line:

```sh
$ ./configure
$ make
$ make install
```

## Development and releases

Run `make test` for fixture, semantic and filesystem-failure checks, and
`make str-match` for SIMD/fallback equivalence. `make fault` uses ELF linker
wrapping and is not portable to Apple's linker. See
[docs/FUZZ.md](docs/FUZZ.md) for fuzzing and instrumentation checks.

Run the agent knowledge checks independently of the C build:

```sh
$ prove -v tests/agent_knowledge_validator/*.pl
$ perl scripts/validate-agent-knowledge.pl --scope worktree
```

See [the validation contract](docs/agent-knowledge-validation.md) for range
checks, JSON output and repository configuration.

GitHub Actions runs product checks separately from the agent knowledge gate.
The product workflow builds with two compilers/makes and compares normal and
sanitized output. External action revisions are pinned to reviewed commit
SHAs; updating them requires reviewing the associated release and changing
the pin deliberately. These checks do not establish support for untested
platforms.

`make dist` delegates to the POSIX shell helper `scripts/dist.sh` and archives
the existing `v<VERSION>` Git tag, not the working tree, and writes a SHA-256
checksum using `sha256` or `sha256sum`. Before publishing,
verify the tag names the reviewed release commit, build/test its archive in a
fresh directory, and verify the checksum. No tag or publication is created
by the build. The helper accepts a nonempty version consisting of ASCII
letters, digits, `.`, `_`, `+` and `-`; other values fail with status 2 before
Git or output creation. This avoids interpreting release metadata as a path.
The distribution helper and benchmark tools are source-only
development tools; `make install` continues to install only the formatter and
manual. Release archives include the vendored `libks` sources and ISC
notices; there is no separately resolved `libks` package version. Its history
and local modifications are tracked with the enclosing repository's Git
revision. The fork started from `mptre/knfmt` revision `8d14715` (5.3.1);
compare vendored changes against that provenance rather than replacing them
with an unrelated library release.

## License

Copyright (c) 2021-2026 Anton Lindqvist <anton@basename.se>.
Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
Distributed under the ISC license.
