#include <stdio.h>

#include "config.h"
#include "error.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"

struct error {
	struct buffer *er_bf;
	int er_flush;
};

/*
 * Allocate a diagnostic buffer. Diagnostics are accumulated in the buffer and
 * only written to standard error by error_flush() when flush is set or when a
 * forced flush is requested. This lets the lexer discard errors produced while
 * speculatively parsing a preprocessor branch that later turns out to be valid.
 */
struct error *
error_alloc(int flush, struct arena_scope *s)
{
	struct error *er;

	er = arena_calloc(s, 1, sizeof(*er));
	er->er_bf = arena_buffer_alloc(s, 1 << 10);
	er->er_flush = flush;
	return er;
}

/*
 * Return the buffer to append a diagnostic to. A subsequent error_end() makes
 * the text visible according to the flush policy.
 */
struct buffer *
error_begin(struct error *er)
{
	return er->er_bf;
}

void
error_end(struct error *er)
{
	error_flush(er, 0);
}

/*
 * Discard all buffered diagnostics without printing them.
 */
void
error_reset(struct error *er)
{
	buffer_reset(er->er_bf);
}

/*
 * Emit the buffered diagnostics to standard error and reset the buffer. When
 * force is zero, nothing happens unless the error object was created with
 * flushing enabled.
 */
void
error_flush(struct error *er, int force)
{
	size_t buflen;

	if (!force && !er->er_flush)
		return;

	buflen = buffer_get_len(er->er_bf);
	if (buflen > 0)
		fprintf(stderr, "%.*s", (int)buflen, buffer_get_ptr(er->er_bf));
	error_reset(er);
}
