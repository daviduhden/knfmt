---
name: knfmt-memory-performance
description: >-
  Analyze or modify knfmt's arenas, libks ownership contracts, checked
  arithmetic, recursion limits, caches and performance. Use for memory-safety
  review, allocation faults, overflow/index bugs, quadratic behavior,
  benchmarking and sanitizer/Valgrind interpretation.
---

# knfmt memory and performance

## When to use

Use for changes involving:

- `arenas.c`, `libks/arena.c`, `libks/arena-buffer.c`,
  `libks/arena-vector.c`;
- `libks/vector.c`, `libks/map.c`, `libks/buffer.c`, `libks/string.c`,
  `libks/fs.c`, `libks/exec.c`;
- allocation, reallocation, ownership, reference counting or cleanup;
- integer overflow, index/length validation, use-after-free or double-free;
- recursion depth, stack budgets or iterative frame arrays;
- performance caches, benchmarking or sanitizer evidence.

Do not use it to justify a behavior change. A faster or smaller change is
only acceptable if it preserves output and the documented invariants.

## Files

- `arenas.c`, `arenas.h` — the five process-wide arenas.
- `util.c`, `util.h` — shared per-operation process-stack guard.
- `libks/arena.c`, `libks/arena.h` — bump allocator, scopes, cleanup lists.
- `libks/arena-buffer.c`, `libks/arena-vector.c` — arena-backed containers.
- `libks/vector.c`, `libks/vector.h` — hidden-header vectors.
- `libks/map.c`, `libks/map.h` — uthash-derived maps.
- `libks/buffer.c`, `libks/buffer.h` — callback-based buffers.
- `libks/arithmetic.h`, `libks/stdckdint.h` — checked arithmetic.
- `libks/fs.c` — atomic in-place replacement.
- `doc.c`, `expr.c` — explicit frame arrays.
- `tests/fault-wrap.c`, `tests/fault.sh`, `tests/bench.sh`,
  `tests/bench-decl.sh`, `tests/benchmark.pl`, `tests/cross-build.sh`.
- `.valgrindrc`, `.clang-tidy`, `cppcheck-suppressions.xml`.

## Arena contracts

- There are five arenas: `eternal`, `scratch`, `doc`, `buffer`, `ruler`.
  They are created once by `arenas_init()` and destroyed once by
  `arenas_free()`.
- Allocation is a bump allocator. A failed frame allocation or an arithmetic
  overflow is fatal (`err`/`errx`); arena allocation is not nullable.
- A scope records the current frame and length. Leaving it frees newer frames
  and truncates the entry frame. Registered cleanup callbacks run in reverse
  registration order. Use `arena_scope(arena, name)` and let the cleanup
  attribute run on every return path. Releasing the arena owner closes its
  trace descriptor; outstanding scopes keep the arena storage alive until they
  leave and run their cleanup callbacks.
- `arena_scope_validate()` traps when an allocation is made from an outer
  scope while an inner scope is live. This catches pointers escaping a
  speculative parse scope.
- `arena_realloc()` requires the exact previous allocation size. The fast
  path only works for the most recent allocation; otherwise it copies and
  poisons the old region.
- Arena alignment is `sizeof(void *)`, not a general `max_align_t`
  guarantee. Do not allocate over-aligned objects from an arena without
  checking.
- `arena_free()` is not idempotent. Do not call it twice. With no outstanding
  scopes it closes the `ARENA_TRACE` descriptor before teardown. It also closes
  that descriptor when outstanding scopes keep the arena storage alive after
  the owner reference is released.

## libks contracts

- `VECTOR` uses a hidden header immediately before the user pointer.
  `VECTOR_LENGTH`/`VECTOR_STRIDE` read `vp[-1]`; passing an uninitialized or
  null vector is undefined. `VECTOR_ALLOC`/`VECTOR_CALLOC` map `ULONG_MAX` to
  `nullptr`.
- `ARENA_VECTOR_ALLOC`/`CALLOC` also map a failed allocation to `nullptr`.
  Check the result if the caller can continue.
- `MAP_INSERT` does not reject duplicate keys; lookup finds the most recent
  insertion. `MAP_FREE` nulls the caller's pointer; `map_free()` itself does
  not. `MAP_ITERATOR` must be zero-initialized. Removing the current element
  is safe; retaining
  `nx` across removals is not.
- `buffer` callbacks are `alloc`, `realloc(ptr, oldsize, newsize)` and
  `free(ptr, size)`. Buffers hold length-delimited bytes unless `buffer_str()`
  adds a NUL; `buffer_getline()` returns scratch storage invalidated by its next
  call. `buffer_release()` transfers ownership and nulls the internal pointer;
  a later `buffer_free()` is safe. After a null line result, the iterator's
  `error` field distinguishes allocation failure from EOF; retry clears it and
  EOF/free clears the entire iterator. `buffer_getline_impl()` propagates both line
  append and terminating-NUL allocation failures without advancing the cursor;
  the caller may retry or free the iterator. `t.c` tests both failures.
- `KS_str` match tables are initialized once from linker sections. The x86-64
  dispatch selects SSE/AVX2/AVX-512; Valgrind disables PCMPISTRI.
- `KS_fs_replace_checked()` also receives the original file descriptor and
  snapshot, validates identity and bytes, and checks metadata again before
  rename. A detected concurrent edit reports `EBUSY`; a residual race remains
  between the final check and rename, so the protection is best effort.
- `KS_fs_replace()` writes a hidden sibling temporary file, copies mode and
  ownership, fsyncs the file, renames it, then fsyncs the parent directory.
  Failures before rename leave the target untouched; a directory-sync error
  can be returned after publication, with crash durability uncertain.
- Checked arithmetic is mandatory for sizes and indices:
  `KS_size_add_overflow`, `KS_size_mul_overflow`, `ckd_add`, etc. Do not
  compute a byte count in `size_t` and hope.
- `libks/arithmetic.c` contains independent test-reference functions whose
  contract includes returning the wrapped result on overflow. Only those
  functions disable `unsigned-integer-overflow` instrumentation, since their
  unsigned wrap is intentional; signed overflow and other sanitizers remain
  active. Keep production checked wrappers instrumented, and do not suppress
  unsigned-overflow checking globally to run the reference boundary tests.

## Recursion and stack

- Expression parsing, declarator scanning, and statement/declaration nesting
  retain their 8000-level depth guards.
- `util.c` implements a per-operation `stack_guard`: the budget is the lesser
  of 2 MiB and one quarter of the current `RLIMIT_STACK`, queried once when
  the operation initializes its guard. The parser shares it across declarator
  scans, expression parsing/peeking/document construction, width queries and
  final rendering. Exceeding the budget is sticky and rejects the file.
- Standalone expression and renderer callers initialize their own guard;
  do not reset the parser guard at a mutually recursive entry.
- `expr_doc_binary_chain()` and `doc_exec1()` use explicit growable frame
  arrays with checked capacity and byte-count multiplication. Keep them iterative.
- Renderer helpers for indent/scope/maxlines/optional/minimize still recurse
  by document nesting depth; recursive renderer entries check the shared guard.
- `tests/stack-budget.sh` exercises expressions, statements and declarators
  with a 512 KiB stack. Small-stack acceptance can depend on compiled frame
  sizes; controlled rejection and no partial output are required in every build.

## Performance invariants

Preserve these existing optimizations unless you can show equivalent or
better behavior with measurements:

- n-ary `DOC_CONCAT` with O(1) tail append;
- iterative `doc_exec1()` and `expr_doc_binary_chain()`;
- `doc_summarize()` cached flat width/purity and `doc_fits()` pure-subtree
  skip;
- `lbrace_cache_lookup()` positive *and negative* caching;
- declaration-scan negative cache;
- O(1) `lexer_seek()` via intrusive list `LIST_PREV`;
- allocation-free common token paths and length-delimited strings;
- SIMD `KS_str_match` with a C fallback.

Sequential conditional initializers (`int v = #ifdef A 1 #else 2 #endif;`)
still have quadratic reparse work below the resource limit. After reparsing
starts, `128 * initial_token_count + 65536` token visits (including peeks)
bound that work; the parser checks between top-level attempts and rejects
excessive input without partial output. `tests/reparse-budget.sh` verifies
small acceptance, large rejection, in-place safety and linear top-level CPP.
A full algorithmic correction still needs a safe rewind floor and measurements.

## Analysis procedure

1. Identify the allocation lifetime: process, per-file, document, scratch or
   ruler. Draw the scope boundaries before changing ownership.
2. Check every return path for scope leave, cleanup, `regfree`, `close`,
   `free` or token release.
3. For overflow/index bugs, trace the value from input to the allocation or
   array access. Add a checked operation at the boundary.
4. For use-after-free, look for pointers returned from a nested scope, tokens
   removed without moving dangling tokens, or map/vector elements invalidated
   by growth.
5. For performance, measure first. Use `tests/bench.sh` and
   `tests/bench-decl.sh`, and report time and peak RSS. Compare the same
   input and build flags. The benchmark wrappers share `tests/benchmark.pl`:
   keep high-resolution timing and arithmetic in that driver, preserve output
   fields and optional BSD RSS, and avoid interpreter startup in measured
   timestamp calculations.
6. Confirm sanitizer instrumentation by inspecting `CFLAGS`/the binary, not
   by trusting the configure option. GCC sanitizer linking may be unavailable;
   Clang is the supported sanitizer compiler in the documented environment.
7. Valgrind may need the standard allocator wrapper on systems that preload a
   hardened malloc. Read `docs/ARCHITECTURE.md` before interpreting a failure.

## Modification procedure

1. Prefer arena allocation for parse-lifetime data; use explicit `malloc`
   only for a bounded frame array whose lifetime is the call.
2. Use `KS_*_overflow` for every size/length computation that can be reached
   from input.
3. When reallocating, pass the exact old size. When shrinking, update any
   statistics that track bytes.
4. When adding a cache, define its key, invalidation rule and negative case.
   A cache that only stores positive results usually reintroduces the
   quadratic scan.
5. When adding recursion, add a depth check or an explicit frame array.
6. Do not add a suppressions file entry to hide a real finding. Fix the code
   or document why the checker is wrong.
7. Update the memory/performance notes in `docs/ARCHITECTURE.md` when a
   measured fact or contract changes.

## Tests and validation

- `make test` — functional fixtures, the in-process `t` binary, and `fs-test`.
  `tests/fs-replace.c` uses portable syscall wrappers to cover short writes,
  EINTR, zero/error writes, chmod/fsync/rename failures, symlinks, modes and
  identity/content conflicts. Directory-fsync failures occur after publication.
- `make fault` — allocation-failure sweep; checks controlled exit, no partial
  stdout and an unchanged in-place target.
- `make str-match` — SIMD vs C string matching (bmake-only historically;
  fixed to work with GNU make).
- `tests/cross-build.sh` — normal vs instrumented output/exit equality.
- `tests/bench.sh 10000 200000` — flat expression time; peak RSS when the
  BSD `time -l` format is available, otherwise `-`.
- `tests/bench-decl.sh` — declaration/alignment scaling.
- `tests/bytes.sh`, `tests/nul.sh` — raw-byte edge cases that often expose
  length bugs.
- `make lint-clang-tidy`, `make lint-cppcheck` where the tools are installed.

Do not report a sanitizer/Valgrind run as clean unless you actually ran it and
confirmed the binary was instrumented.

## Common failures

- Using `strlen` on token text that may contain NUL. Use `tk_len`.
- Adding a `size_t` length without overflow checks.
- Returning a pointer from a scratch scope that has been left.
- Forgetting `regfree()` for every compiled regex on style teardown.
- Double-freeing a map through `map_free()` directly (the `MAP_FREE` macro
  nulls the pointer) or using a stale map iterator.
- Replacing a cached negative result with a rescan.
- Making `doc_fits()` walk a pure subtree it could skip.
- Adding recursion over a flat list.
- Assuming `arena_free()` is idempotent or that arena memory is zeroed.
- Trusting `./configure --sanitize` without checking the resulting flags.

## References

- `docs/ARCHITECTURE.md` sections "Allocation fault injection", "Sanitizer
  instrumentation", "Valgrind memory checking", "Brace initializers",
  "CPP branch-processing cost".
- `docs/ARCHITECTURE-REWRITE-PROGRESS.md` for measured scalability results
  and the memory-safety ledger.
- `libks/*.h` for exact API contracts.

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
this skill, and run `perl scripts/validate-agent-knowledge.pl --scope worktree` after the final instruction
edit. That check verifies structural conditions; use judgment to ensure the
updated knowledge is pertinent and accurate. Report which agent and skill
instructions were reviewed or updated and whether the check passed. A
repository change is incomplete until this synchronization is done.
