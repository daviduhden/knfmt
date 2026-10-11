/*
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

/*
 * libks/stdckdint.h -- vendored ISO C23 <stdckdint.h>.
 *
 * knfmt is compiled as C23 and performs its checked arithmetic through the
 * wrappers in libks/arithmetic.h.  <stdckdint.h> belongs to the C library,
 * but knfmt must build with the base compiler of an unmodified OpenBSD
 * installation even when neither the libc nor the compiler installation ships
 * the header.  It must therefore not depend on a system <stdckdint.h> and in
 * particular must not pick up a local GCC or clang resource header.
 *
 * This header is included by the explicit path "libks/stdckdint.h" and never
 * with #include_next, so it cannot be preempted by a system header and cannot
 * recurse into itself (the include guard is also deliberately not the
 * __STDC_VERSION_STDCKDINT_H__ macro, which a compiler header may define).
 *
 * The macros expand to the compiler overflow builtins, whose contract is
 * exactly C23 7.20.1: each operand is taken at its mathematical value, the
 * exact sum/difference/product is converted to the type of *result (wrapping
 * on overflow), stored, and the macro evaluates to true exactly when that
 * conversion is not value preserving.  Every argument is evaluated exactly
 * once.  The same formulation is used by the GCC and clang resource headers
 * and by the musl <stdckdint.h> proposal (Jens Gustedt, musl@, 2023-05-31,
 * message-id <323b1aaf4c7e31010ead4eae127d0d580f319ce6.1685535458.git.
 * Jens.Gustedt@inria.fr>); no code is copied from them, only the builtin
 * contract they all express.
 *
 * The #ifndef ckd_* guards keep a definition that a translation unit obtained
 * from a system header from being redefined.
 */

#ifndef LIBKS_STDCKDINT_H
#define LIBKS_STDCKDINT_H

#if !defined(__STDC_VERSION_STDCKDINT_H__)
#define __STDC_VERSION_STDCKDINT_H__ 202311L
#endif

#if defined(__has_builtin)
#if !__has_builtin(__builtin_add_overflow) || \
    !__has_builtin(__builtin_sub_overflow) || \
    !__has_builtin(__builtin_mul_overflow)
#error "knfmt requires the compiler overflow builtins"
#endif
#elif !defined(__GNUC__) && !defined(__clang__)
#error "knfmt requires __builtin_*_overflow"
#endif

#ifndef ckd_add
#define ckd_add(result, a, b) \
	((_Bool)__builtin_add_overflow((a), (b), (result)))
#endif
#ifndef ckd_sub
#define ckd_sub(result, a, b) \
	((_Bool)__builtin_sub_overflow((a), (b), (result)))
#endif
#ifndef ckd_mul
#define ckd_mul(result, a, b) \
	((_Bool)__builtin_mul_overflow((a), (b), (result)))
#endif

#endif /* !LIBKS_STDCKDINT_H */
