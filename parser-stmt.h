struct parser;

struct parser_stmt_block_arg {
	struct doc  *head;
	struct doc  *tail;
	struct doc  *rbrace;
	unsigned int flags;
/* Switch body: case labels control indentation. */
#define PARSER_STMT_BLOCK_SWITCH 0x00000001u
/* Remove a blank line directly after the opening brace. */
#define PARSER_STMT_BLOCK_TRIM 0x00000002u
/* GNU statement expression: braces are part of an expression, keep them tight. */
#define PARSER_STMT_BLOCK_EXPR_GNU 0x00000004u
};

int parser_stmt_block(struct parser *, struct parser_stmt_block_arg *);
int parser_stmt_peek(struct parser *);
