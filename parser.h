/*
 * Parser entry point. One parser is created per input and lives in the same
 * scope as its lexer. parser_exec() consumes the token list, builds the document
 * tree and renders it into the caller's buffer; it returns non-zero only for a
 * rejected input, never for a merely reformatted one.
 */
struct arena_scope;
struct buffer;
struct diffchunk;

struct parser_arg {
	struct lexer *lexer;
	const struct options *options;
	const struct style *style;
	struct simple *simple;
	struct clang *clang;
	struct arenas *arena;
};

struct parser *parser_alloc(const struct parser_arg *, struct arena_scope *);
int parser_exec(struct parser *, const struct diffchunk *, struct buffer *);
