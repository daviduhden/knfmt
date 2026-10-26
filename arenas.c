#include "arenas.h"
#include "config.h"
#include "libks/arena.h"

/*
 * Create the process-wide arenas. Their lifetimes differ by design: eternal
 * holds state that must survive all files, scratch is reset per operation, doc
 * holds the document tree, buffer backs temporary output buffers, and ruler
 * holds alignment state. Each arena is destroyed exactly once in arenas_free().
 */
void
arenas_init(struct arenas *a)
{
	a->eternal = arena_alloc("eternal");
	a->scratch = arena_alloc("scratch");
	a->doc = arena_alloc("doc");
	a->buffer = arena_alloc("buffer");
	a->ruler = arena_alloc("ruler");
}

/*
 * Destroy all arenas in reverse creation order, releasing every allocation and
 * running registered cleanup callbacks.
 */
void
arenas_free(struct arenas *a)
{
	arena_free(a->ruler);
	arena_free(a->buffer);
	arena_free(a->doc);
	arena_free(a->scratch);
	arena_free(a->eternal);
}
