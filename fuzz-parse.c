#include "config.h"

#include <string.h>

#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "libks/compiler.h"
#include "libks/fuzzer.h"

#include "arenas.h"
#include "clang.h"
#include "expr.h"
#include "lexer.h"
#include "options.h"
#include "parser.h"
#include "simple.h"
#include "style.h"

struct test_context {
	struct options		 op;
	struct style		*st;
	struct simple		*si;
	struct arenas		 arena;
	struct arena_scope	 eternal_scope;
};

static void *
init(int UNUSED(argc), char **UNUSED(argv))
{
	static struct test_context c;

	clang_init();
	expr_init();
	style_init();
	arenas_init(&c.arena);
	options_init(&c.op);

	c.eternal_scope = arena_scope_enter(c.arena.eternal);
	c.st = style_parse(nullptr, &c.eternal_scope, c.arena.scratch, &c.op);
	c.si = simple_alloc(&c.eternal_scope, &c.op);
	return &c;
}
FUZZER_INIT(init);

static void
teardown(void *userdata)
{
	struct test_context *c = userdata;

	arenas_free(&c->arena);
	style_shutdown();
	expr_shutdown();
	clang_shutdown();
}
FUZZER_TEARDOWN(teardown);

static int
format_once(const struct test_context *c, const struct buffer *bf,
    struct arena_scope *scope, struct buffer *dst)
{
	struct clang *clang;
	struct lexer *lx;
	struct parser *pr;

	clang = clang_alloc(c->st, c->si, &c->arena, nullptr, &c->op, scope);
	lx = lexer_tokenize(&(const struct lexer_arg){
	    .path		= "test.c",
	    .bf			= bf,
	    .op			= &c->op,
	    .arena		= {
		.eternal_scope	= scope,
		.scratch	= c->arena.scratch,
	    },
	    .callbacks		= clang_lexer_callbacks(clang),
	});
	if (lx == nullptr)
		return 0;

	pr = parser_alloc(&(struct parser_arg){
	    .lexer	= lx,
	    .options	= &c->op,
	    .style	= c->st,
	    .simple	= c->si,
	    .clang	= clang,
	    .arena	= &c->arena,
	}, scope);
	return parser_exec(pr, nullptr, dst) == 0;
}

static void
target(const struct buffer *bf, void *userdata)
{
	struct test_context *c = userdata;
	struct buffer *dst, *dst2, *dst3;

	arena_scope(c->arena.eternal, eternal_scope);
	arena_scope(c->arena.buffer, buffer_scope);

	dst = arena_buffer_alloc(&buffer_scope, 1 << 12);
	if (!format_once(c, bf, &eternal_scope, dst))
		return;

	/*
	 * Format three times. Malformed input may need a first pass to
	 * normalize, but from the second pass on the output must be a fixed
	 * point.
	 */
	dst2 = arena_buffer_alloc(&buffer_scope, 1 << 12);
	if (!format_once(c, dst, &eternal_scope, dst2))
		return;

	dst3 = arena_buffer_alloc(&buffer_scope, 1 << 12);
	if (!format_once(c, dst2, &eternal_scope, dst3))
		return;

	if (buffer_get_len(dst2) != buffer_get_len(dst3) ||
	    memcmp(buffer_str(dst2), buffer_str(dst3),
	     buffer_get_len(dst2)) != 0)
		__builtin_trap();
}
FUZZER_TARGET_BUFFER(target);
