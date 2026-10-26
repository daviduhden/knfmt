#include "config.h"
#include "lexer.h"
#include "parser-expr.h"
#include "parser-priv.h"
#include "parser-stmt-expr.h"
#include "parser-stmt.h"
#include "parser-type.h"
#include "style.h"
#include "token.h"

/*
 * Expression statements and GNU statement expressions.
 *
 * An expression statement is only recognized when the whole expression is
 * followed by a semicolon. The lookahead rejects a `NAME(...)` macro that is
 * followed by another statement, since that is a loop-like construct from a
 * preprocessor macro rather than an expression. A GNU statement expression is
 * simply a braced block in expression position.
 */
static int
is_loop_stmt(struct parser *pr, const struct token *semi)
{
	struct lexer_state s;
	struct lexer	  *lx = pr->pr_lx;
	struct token	  *ident, *lparen, *nx, *rparen;
	int		   peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_IDENT, &ident) &&
	    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &lparen, &rparen) &&
	    lexer_peek(lx, &nx) && nx != semi && token_cmp(rparen, nx) < 0 &&
	    parser_stmt_peek(pr))
		peek = 1;
	lexer_peek_leave(lx, &s);

	return peek;
}

/*
 * Parse an expression statement. A type-like prefix is rejected because the
 * declaration parser owns that shape. The expression must be immediately
 * followed by a semicolon; otherwise NONE is returned so another statement
 * alternative can try.
 */
int
parser_stmt_expr(struct parser *pr, struct doc *dc)
{
	struct doc   *expr = nullptr;
	struct token *nx, *semi;
	int	      error;

	if (parser_type_peek(pr, nullptr, 0) || !parser_expr_peek(pr, &nx))
		return parser_none(pr);
	nx = token_next(nx);
	if (nx->tk_type != TOKEN_SEMI)
		return parser_none(pr);
	semi = nx;

	/*
	 * Do not confuse a loop construct hidden behind cpp followed by a sole
	 * statement:
	 *
	 * 	foreach()
	 * 		func();
	 */
	if (is_loop_stmt(pr, semi))
		return parser_none(pr);

	error = parser_expr(pr, &expr,
	    &(struct parser_expr_arg){
		.dc = dc,
		.indent = style(pr->pr_st, ContinuationIndentWidth),
	    });
	if (error & HALT)
		return parser_fail(pr);
	return parser_semi(pr, expr);
}

int
parser_stmt_expr_gnu(struct parser *pr, struct doc *dc)
{
	return parser_stmt_block(pr,
	    &(struct parser_stmt_block_arg){
		.head = dc,
		.tail = dc,
		.flags = PARSER_STMT_BLOCK_EXPR_GNU,
	    });
}
