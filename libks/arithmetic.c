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

#include "libks/arithmetic.h"
#include <limits.h>

/*
 * Independent, division/sign-based implementation of the C23 7.20.1 checked
 * arithmetic.  It is not used by the KS_*_overflow() wrappers (those call the
 * compiler builtins through the vendored <stdckdint.h>); the test suite uses
 * it as a reference that does not share an implementation with the builtins.
 *
 * Like C23, each function stores the mathematical result wrapped to the width
 * of the result type and returns 1 exactly when that conversion is not value
 * preserving.  The wrapped value is computed in the corresponding unsigned
 * type, where the arithmetic is well defined, and converted back to the
 * signed type (two's complement, as required by C23).
 */

#define SIGNED_ADD_OVERFLOW(stype, utype, max, min, a, b, c) do {	\
	utype _r = (utype)(a) + (utype)(b);				\
	int _ovf = ((b) > 0 && (a) > ((max) - (b))) ||			\
	    ((b) < 0 && (a) < ((min) - (b)));				\
	*(c) = (stype)_r;						\
	return _ovf;							\
} while (0)

#define SIGNED_SUB_OVERFLOW(stype, utype, max, min, a, b, c) do {	\
	utype _r = (utype)(a) - (utype)(b);				\
	int _ovf = ((b) > 0 && (a) < ((min) + (b))) ||			\
	    ((b) < 0 && (a) > ((max) + (b)));				\
	*(c) = (stype)_r;						\
	return _ovf;							\
} while (0)

#define SIGNED_MUL_OVERFLOW(stype, utype, max, min, a, b, c) do {	\
	utype _r = (utype)(a) * (utype)(b);				\
	int _ovf = ((a) > 0 && (b) > 0 && (a) > (max) / (b)) ||		\
	    ((a) > 0 && (b) < 0 && (b) < (min) / (a)) ||		\
	    ((a) < 0 && (b) > 0 && (a) < (min) / (b)) ||		\
	    ((a) < 0 && (b) < 0 && (b) < (max) / (a));			\
	*(c) = (stype)_r;						\
	return _ovf;							\
} while (0)

#define UNSIGNED_ADD_OVERFLOW(max, a, b, c) do {			\
	int _ovf = (a) > (max) - (b);					\
	*(c) = (a) + (b);						\
	return _ovf;							\
} while (0)

#define UNSIGNED_SUB_OVERFLOW(max, a, b, c) do {			\
	int _ovf = (b) > (a);						\
	*(c) = (a) - (b);						\
	return _ovf;							\
} while (0)

#define UNSIGNED_MUL_OVERFLOW(max, a, b, c) do {			\
	int _ovf = (b) != 0 && (a) > (max) / (b);			\
	*(c) = (a) * (b);						\
	return _ovf;							\
} while (0)

int
KS_i32_add_overflow0(int32_t a, int32_t b, int32_t *c)
{
	SIGNED_ADD_OVERFLOW(int32_t, uint32_t, INT32_MAX, INT32_MIN, a, b, c);
}

int
KS_i32_sub_overflow0(int32_t a, int32_t b, int32_t *c)
{
	SIGNED_SUB_OVERFLOW(int32_t, uint32_t, INT32_MAX, INT32_MIN, a, b, c);
}

int
KS_i32_mul_overflow0(int32_t a, int32_t b, int32_t *c)
{
	SIGNED_MUL_OVERFLOW(int32_t, uint32_t, INT32_MAX, INT32_MIN, a, b, c);
}

int
KS_i64_add_overflow0(int64_t a, int64_t b, int64_t *c)
{
	SIGNED_ADD_OVERFLOW(int64_t, uint64_t, INT64_MAX, INT64_MIN, a, b, c);
}

int
KS_i64_sub_overflow0(int64_t a, int64_t b, int64_t *c)
{
	SIGNED_SUB_OVERFLOW(int64_t, uint64_t, INT64_MAX, INT64_MIN, a, b, c);
}

int
KS_i64_mul_overflow0(int64_t a, int64_t b, int64_t *c)
{
	SIGNED_MUL_OVERFLOW(int64_t, uint64_t, INT64_MAX, INT64_MIN, a, b, c);
}

int
KS_u32_add_overflow0(uint32_t a, uint32_t b, uint32_t *c)
{
	UNSIGNED_ADD_OVERFLOW(UINT32_MAX, a, b, c);
}

int
KS_u32_sub_overflow0(uint32_t a, uint32_t b, uint32_t *c)
{
	UNSIGNED_SUB_OVERFLOW(UINT32_MAX, a, b, c);
}

int
KS_u32_mul_overflow0(uint32_t a, uint32_t b, uint32_t *c)
{
	UNSIGNED_MUL_OVERFLOW(UINT32_MAX, a, b, c);
}

int
KS_u64_add_overflow0(uint64_t a, uint64_t b, uint64_t *c)
{
	UNSIGNED_ADD_OVERFLOW(UINT64_MAX, a, b, c);
}

int
KS_u64_sub_overflow0(uint64_t a, uint64_t b, uint64_t *c)
{
	UNSIGNED_SUB_OVERFLOW(UINT64_MAX, a, b, c);
}

int
KS_u64_mul_overflow0(uint64_t a, uint64_t b, uint64_t *c)
{
	UNSIGNED_MUL_OVERFLOW(UINT64_MAX, a, b, c);
}

int
KS_size_add_overflow0(size_t a, size_t b, size_t *c)
{
	UNSIGNED_ADD_OVERFLOW(SIZE_MAX, a, b, c);
}

int
KS_size_sub_overflow0(size_t a, size_t b, size_t *c)
{
	UNSIGNED_SUB_OVERFLOW(SIZE_MAX, a, b, c);
}

int
KS_size_mul_overflow0(size_t a, size_t b, size_t *c)
{
	UNSIGNED_MUL_OVERFLOW(SIZE_MAX, a, b, c);
}
