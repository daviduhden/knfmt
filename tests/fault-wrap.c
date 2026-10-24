/*
 * Test-only allocation fault injector.
 *
 * Link an otherwise normal knfmt with
 *     -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
 * so that every allocation performed by the program (all of them go through
 * libks/arena.c or the buffer/vector callbacks) can be made to return NULL
 * deterministically: FAULT_AT=N fails the N-th intercepted allocation.
 *
 * This file is never part of the production binary. Its purpose is to verify
 * that an allocation failure terminates cleanly instead of leaving a partially
 * initialised object, a dangling pointer or a partially written output file.
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

void	*__real_malloc(size_t);
void	*__real_calloc(size_t, size_t);
void	*__real_realloc(void *, size_t);

static long	fault_at = -1;
static long	ncalls;

static int
should_fail(void)
{
	if (fault_at < 0)
		return 0;
	return ncalls++ == fault_at;
}

void *
__wrap_malloc(size_t size)
{
	if (should_fail()) {
		errno = ENOMEM;
		return NULL;
	}
	return __real_malloc(size);
}

void *
__wrap_calloc(size_t nmemb, size_t size)
{
	if (should_fail()) {
		errno = ENOMEM;
		return NULL;
	}
	return __real_calloc(nmemb, size);
}

void *
__wrap_realloc(void *ptr, size_t size)
{
	if (should_fail()) {
		errno = ENOMEM;
		return NULL;
	}
	return __real_realloc(ptr, size);
}

__attribute__((constructor))
static void
fault_init(void)
{
	const char *s = getenv("FAULT_AT");

	if (s != NULL)
		fault_at = strtol(s, NULL, 10);
}
