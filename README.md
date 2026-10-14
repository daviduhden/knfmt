# knfmt

The [knfmt(1)][knfmt] utility formats source code files according to
[style(9)][style] from OpenBSD in a best effort fashion. By default, source
code is read from standard input and the formatted source code is written to
standard output:

```sh
$ knfmt <knfmt.c
```

It can perform an in place edit by writing back the formatted source code:

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

It can given a unified diff only format the changed lines. This option is
compatible with the options mentioned above:

```sh
$ git diff | knfmt -Dds
```

It can interoperate with clang-format if a `.clang-format` file is found in
any directory along the current working path, see [knfmt(1)][knfmt] for
further details.

The implementation is further described in
[docs/architecture.md](docs/architecture.md).

## Documentation

All documentation lives under [`docs/`](docs/):

- [docs/architecture.md](docs/architecture.md) — implementation, contracts and
  verification.
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — release history.
- [docs/FUZZ.md](docs/FUZZ.md) — fuzzing guide.
- [docs/ARCHITECTURE-REWRITE-PROGRESS.md](docs/ARCHITECTURE-REWRITE-PROGRESS.md)
  — chronological evidence log of the rewrite.

[knfmt]: https://www.basename.se/knfmt/
[style]: https://man.openbsd.org/style.9

## Installation

### OpenBSD

```sh
# pkg_add knfmt
```

### From source

The installation prefix defaults to `/usr/local` and can be altered using the
`PREFIX` environment variable when invoking `configure`:

```sh
$ ./configure
$ make
$ make install
```

## License

Copyright (c) 2021-2026 Anton Lindqvist.
Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
Distributed under the ISC license.
