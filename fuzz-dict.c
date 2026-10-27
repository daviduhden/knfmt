#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "style.h"

[[noreturn]] static void usage(void);

/*
 * Emit a fuzzing dictionary for one language.  Only the style language is
 * currently defined; the output is written to stdout so it can be redirected to
 * a dictionary file by the build or fuzzing script.
 */
int
main(int argc, const char *argv[])
{
	struct arena *scratch;
	struct buffer *bf;

	if (argc != 2)
		usage();

	scratch = arena_alloc("scratch");

	arena_scope(scratch, s);

	bf = arena_buffer_alloc(&s, 1 << 10);
	if (strcmp(argv[1], "style") == 0) {
		style_init();
		style_dump_keywords(bf);
		style_shutdown();
	} else {
		usage();
	}
	printf("%s", buffer_str(bf));

	arena_free(scratch);
	return 0;
}

/* Print the required argument form and exit with a failure status. */
static void
usage(void)
{
	fprintf(stderr, "usage: fuzz-dict language\n");
	exit(1);
}
