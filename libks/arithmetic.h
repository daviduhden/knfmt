/*
 * Copyright (c) 2023 Anton Lindqvist <anton@basename.se>
 * Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef LIBKS_ARITHMETIC_H
#define LIBKS_ARITHMETIC_H

#include <stddef.h> /* size_t */
#include <stdint.h>

#include "libks/stdckdint.h"

/*
 * Reference implementation of the checked arithmetic, kept only so that the
 * test suite can compare the compiler builtins against an independent
 * (division-based) algorithm.  Production code uses the KS_*_overflow()
 * wrappers below, which are a thin layer over ckd_add/ckd_sub/ckd_mul from
 * the vendored <stdckdint.h>.
 */
int KS_i32_add_overflow0(int32_t, int32_t, int32_t *);
int KS_i32_sub_overflow0(int32_t, int32_t, int32_t *);
int KS_i32_mul_overflow0(int32_t, int32_t, int32_t *);

int KS_i64_add_overflow0(int64_t, int64_t, int64_t *);
int KS_i64_sub_overflow0(int64_t, int64_t, int64_t *);
int KS_i64_mul_overflow0(int64_t, int64_t, int64_t *);

int KS_u32_add_overflow0(uint32_t, uint32_t, uint32_t *);
int KS_u32_sub_overflow0(uint32_t, uint32_t, uint32_t *);
int KS_u32_mul_overflow0(uint32_t, uint32_t, uint32_t *);

int KS_u64_add_overflow0(uint64_t, uint64_t, uint64_t *);
int KS_u64_sub_overflow0(uint64_t, uint64_t, uint64_t *);
int KS_u64_mul_overflow0(uint64_t, uint64_t, uint64_t *);

int KS_size_add_overflow0(size_t, size_t, size_t *);
int KS_size_sub_overflow0(size_t, size_t, size_t *);
int KS_size_mul_overflow0(size_t, size_t, size_t *);

/*
 * The C23 macros return bool; the wrappers preserve the historical int
 * interface used throughout knfmt.
 */
#define KS_OVERFLOW_FUNCS(tag, type)                                                               \
	[[nodiscard]] static inline int KS_##tag##_add_overflow(type a, type b, type *c)           \
	{                                                                                          \
		return ckd_add(c, a, b);                                                           \
	}                                                                                          \
	[[nodiscard]] static inline int KS_##tag##_sub_overflow(type a, type b, type *c)           \
	{                                                                                          \
		return ckd_sub(c, a, b);                                                           \
	}                                                                                          \
	[[nodiscard]] static inline int KS_##tag##_mul_overflow(type a, type b, type *c)           \
	{                                                                                          \
		return ckd_mul(c, a, b);                                                           \
	}

KS_OVERFLOW_FUNCS(i32, int32_t)
KS_OVERFLOW_FUNCS(i64, int64_t)
KS_OVERFLOW_FUNCS(u32, uint32_t)
KS_OVERFLOW_FUNCS(u64, uint64_t)
KS_OVERFLOW_FUNCS(size, size_t)

#undef KS_OVERFLOW_FUNCS

#endif /* !LIBKS_ARITHMETIC_H */
