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

It can interoperate with clang-format if a `.clang-format` file is found by
searching upward from the current working directory, see [knfmt(1)][knfmt]
for further details.

The implementation is further described in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Documentation

All user-facing documentation lives under [`docs/`](docs/):

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — implementation, contracts and
  verification.
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — release history.
- [docs/FUZZ.md](docs/FUZZ.md) — fuzzing guide.
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

The installation prefix defaults to `/usr/local` and can be altered using the
`PREFIX` environment variable when invoking `configure`, or on the
`make install` command line:

```sh
$ ./configure
$ make
$ make install
```

## License

Copyright (c) 2021-2026 Anton Lindqvist <anton@basename.se>.
Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
Distributed under the ISC license.
