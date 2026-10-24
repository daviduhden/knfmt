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
- `libks/arena.c`, `libks/arena.h` — bump allocator, scopes, cleanup lists.
- `libks/arena-buffer.c`, `libks/arena-vector.c` — arena-backed containers.
- `libks/vector.c`, `libks/vector.h` — hidden-header vectors.
- `libks/map.c`, `libks/map.h` — uthash-derived maps.
- `libks/buffer.c`, `libks/buffer.h` — callback-based buffers.
- `libks/arithmetic.h`, `libks/stdckdint.h` — checked arithmetic.
- `libks/fs.c` — atomic in-place replacement.
- `doc.c`, `expr.c` — explicit frame arrays.
- `tests/fault-wrap.c`, `tests/fault.sh`, `tests/bench.sh`,
  `tests/bench-decl.sh`, `tests/cross-build.sh`.
- `.valgrindrc`, `.clang-tidy`, `cppcheck-suppressions.xml`.

## Arena contracts

- There are five arenas: `eternal`, `scratch`, `doc`, `buffer`, `ruler`.
  They are created once by `arenas_init()` and destroyed once by
  `arenas_free()`.
- Allocation is a bump allocator. A failed frame allocation or an arithmetic
  overflow is fatal (`err`/`errx`); arena allocation is not nullable.
- A scope records the current frame and length. Leaving it frees newer frames
  and truncates the entry frame. Use `arena_scope(arena, name)` and let the
  cleanup attribute run on every return path.
- `arena_scope_validate()` traps when an allocation is made from an outer
  scope while an inner scope is live. This catches pointers escaping a
  speculative parse scope.
- `arena_realloc()` requires the exact previous allocation size. The fast
  path only works for the most recent allocation; otherwise it copies and
  poisons the old region.
- Arena alignment is `sizeof(void *)`, not a general `max_align_t`
  guarantee. Do not allocate over-aligned objects from an arena without
  checking.
- `arena_free()` is not idempotent. Do not call it twice. It closes the
  `ARENA_TRACE` descriptor when it actually frees the arena.

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
  `free(ptr, size)`. `buffer_release()` transfers ownership and nulls the
  internal pointer; a later `buffer_free()` is safe.
- `KS_str` match tables are initialized once from linker sections. The x86-64
  dispatch selects SSE/AVX2/AVX-512; Valgrind disables PCMPISTRI.
- `KS_fs_replace()` writes a hidden sibling temporary file, copies mode and
  ownership, then renames. It must unlink the temporary on every failure and
  leave the target untouched.
- Checked arithmetic is mandatory for sizes and indices:
  `KS_size_add_overflow`, `KS_size_mul_overflow`, `ckd_add`, etc. Do not
  compute a byte count in `size_t` and hope.

## Recursion and stack

- `expr_exec1()` is recursive, bounded by `EXPR_MAX_DEPTH` (8000).
- `expr_doc()` checks `EXPR_STACK_BUDGET` (2 MiB) using the process stack
  distance from `es_stack_base`; exceeding it makes the expression fail
  cleanly.
- Declarator scanning is bounded by `SCAN_MAX_DEPTH` (8000).
- `expr_doc_binary_chain()` and `doc_exec1()` use explicit growable frame
  arrays for flat chains and concat/group rendering. Keep them iterative.
- Renderer helpers for indent/scope/maxlines/optional/minimize still recurse
  by document nesting depth. Do not add recursion proportional to document
  length.
- Statement/declaration nesting is bounded by `PARSER_MAX_DEPTH` (8000).
  If you add recursive descent with input-controlled depth, add a limit or
  make it iterative.

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

Known remaining quadratic behavior: sequential conditional initializers
(`int v = #ifdef A 1 #else 2 #endif;`). Do not "fix" it by changing recovery
semantics unless you have a safe rewind floor and measurements.

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
   input and build flags.
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

- `make test` — functional fixtures and the in-process `t` binary.
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
