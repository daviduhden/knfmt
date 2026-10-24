---
name: knfmt-cli-documentation
description: >-
  Work on knfmt's command-line interface, diagnostics, exit status, in-place
  and diff modes, and its documentation: knfmt.c, options.c, knfmt.1,
  README.md, docs/ and the changelog. Use when adding or changing an option,
  mode combination, error message or user-visible contract.
---

# knfmt CLI and documentation

## When to use

Use for changes to:

- `knfmt.c`, `options.c`, `file.c`, `diff.c`;
- option parsing, mode combinations, standard input/output, in-place writes,
  diff output or exit status;
- diagnostics and their formatting;
- `knfmt.1`, `README.md`, `docs/*.md`, `docs/CHANGELOG.md`;
- the user-visible `-s`, `-D`, `-d`, `-i`, `-c` or `-t` contract.

Do not use it to change formatting rules; document the change and let the
owning subsystem skill implement it.

## CLI contract

Options accepted by `getopt`: `c:Ddist:`.

- `-c FILE` — explicit clang-format configuration. The last `-c` wins. A
  missing explicit file is fatal.
- `-D` — read a unified diff from standard input and format the touched
  lines. File operands are rejected. May be combined with `-d`, `-i`, `-s`.
- `-d` — produce a unified diff using external `diff(1)`. Exits 1 when
  differences are found, not only on error.
- `-i` — in-place edit. Standard input is rejected unless `-D` is also
  given.
- `-s` — enable the simplifier.
- `-t FLAGS` — implementation tracing; development interface.
- `-` is not a synonym for standard input; it is opened as a literal path.

Validation is intentionally small:

```
-D with file operands        -> usage error
-i without -D and no files   -> usage error
```

`-d` wins over `-i` in dispatch; both may be parsed, but `-i` is not reached.
Multiple files are processed sequentially; a failure sets exit 1 but does not
stop later files.

## Output and error behavior

- Default and `-D` write formatted bytes to stdout. `-D` writes the whole
  source: unchanged regions are copied verbatim and groups intersecting the
  diff are reformatted.
- `-d` writes a real unified diff with `<path>.orig` and `<path>` labels.
- `-i` writes no stdout. `KS_fs_replace()` writes a hidden sibling temporary
  file, copies mode/ownership, then renames. It is atomic against partial
  writes but does not fsync; symlinks are replaced, hard links are broken.
- Exit status is 0 on success and 1 on any read/parse/write/style failure.
  `-d` also returns 1 when a difference exists. `diff(1)` exit 2 or a signal
  is collapsed to 1.
- Diagnostics are `<path>:<line>: <message>` followed by the source line and
  a caret marker. Style diagnostics are silent unless `-ts` is used.
- A diff hunk before any `+++` path, or a truncated hunk, exits 1 without a
  message. Do not add noise unless the contract is deliberately changed.

## Diff parsing details

- Only `+++` headers name files. Git `b/` prefixes are stripped; if a relative
  path is missing and `.git` exists above the cwd, leading components are
  trimmed up to the repository depth.
- Hunk headers use the new-file range. `+` lines create or extend a chunk;
  `-` lines do not advance the new-file counter; any other line closes the
  open chunk.
- A deletion-only hunk produces no chunk and is effectively ignored.
- `-D` does not apply the patch and does not verify the on-disk file against
  the patch. It trusts line numbers.
- Changed tokens get `TOKEN_FLAG_DIFF`; multi-line `#define` checks every
  line it spans.
- Partial formatting is at document-group granularity: a one-line group is
  reformatted if any token in it is in a chunk; a multi-line group is not
  itself forced, but nested single-line groups are considered.

## Documentation rules

- `knfmt.1` is mdoc. Keep the synopsis accurate for both file and `-D`
  forms, list every accepted option, and use `.Sh EXIT STATUS` for the
  difference-found status.
- Document actual behavior, including interpreted options. If an option is
  accepted but not implemented, say so.
- `README.md` is a short user overview. Link to `docs/ARCHITECTURE.md` and
  `docs/FUZZ.md` instead of duplicating them.
- `docs/ARCHITECTURE.md` describes the implementation as it exists. Update it
  when a contract, invariant or measured fact changes.
- `docs/ARCHITECTURE-REWRITE-PROGRESS.md` is a historical evidence log. Do not
  add current-contract claims there without a date and context.
- `docs/CHANGELOG.md` is reverse chronological, with `News` and
  `Bug fixes` sections, commit hashes where available and the author.
- Preserve the ISC license and copyright headers.

## Analysis procedure

1. Reproduce the behavior with the exact invocation. Check the option
   validation at the top of `main()` and the dispatch in `fileformat()`.
2. Check `options.c` for trace parsing; unknown trace flags warn and return 1.
3. For `-D`, trace `diff_parse()`, `matchchunk()`, `matchline()` and the
   `TOKEN_FLAG_DIFF` assignment in `clang.c`.
4. For `-i`, trace `filewrite()` and `KS_fs_replace()`. Confirm that a
   failure before `filewrite()` leaves the target untouched.
5. For `-d`, trace `filediff()` and `KS_exec_diff()`. Remember that exit 1 is
   a normal "differences found" result.
6. Compare `knfmt.1` and README against the code, not against upstream
   documentation. Upstream `knfmt.1` predates this fork's options.

## Modification procedure

1. Add the option to `getopt`, `usage()`, `knfmt.1` and the tests in the same
   change.
2. Define the allowed combinations explicitly. Do not let a new mode silently
   fall through to stdout or in-place.
3. Add a shell test under `tests/` for stdin, files, exit status and
   diagnostics. Use `tests/stdin.sh`, `tests/enoent.sh`, `tests/diff.sh`,
   `tests/inplace-*` as models.
4. Keep stdout byte-exact and diagnostics on stderr. Do not mix them.
5. If an in-place operation can fail after creating a temporary file, ensure
   the temporary is removed and the target is unchanged.
6. Update the changelog for user-visible changes.
7. Run `mandoc -Tlint -Wstyle knfmt.1` if `mandoc` is available.

## Tests and validation

- `make test`.
- `tests/stdin.sh` — `-i` on stdin is a usage error; `-Di` is accepted.
- `tests/enoent.sh`, `tests/style-enoent.sh` — exact error strings.
- `tests/diff.sh` — `-d` must produce output, exit 1 and keep stderr empty.
- `tests/git.sh` — nested `.git` path handling.
- `tests/include-categories.sh` — include regrouping in `-ds`.
- `tests/inplace-001.c` — clean in-place run.
- `tests/fd.sh` — in-place editing with a low descriptor limit.
- `tests/cross-build.sh` — output/exit equality including `-D`.

## Common failures

- Documenting `-D` as "only changed lines" without the file-operand and
  group-granularity caveats.
- Forgetting that `-d` returns 1 for differences.
- Using stdout for diagnostics or stderr for formatted source.
- Letting `-i` proceed after a parse failure.
- Replacing the target non-atomically or leaving a temporary file behind.
- Adding an option to `getopt` but not to `usage()` or `knfmt.1`.
- Treating `-c`/`-t` as public stable interfaces; `-t` is not.
- Assuming `.clang-format` is discovered next to the input file.
- Copying upstream manual text that no longer matches the fork.

## References

- `knfmt.1` for the user contract.
- `README.md` for the overview.
- `docs/ARCHITECTURE.md` sections "Input and translation", "Verification",
  "CPP branch emission model".
- `docs/CHANGELOG.md` for release history.
- `tests/stdin.sh`, `tests/diff.sh`, `tests/enoent.sh`, `tests/git.sh`.
