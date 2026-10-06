#include "parser-expr.h"

#include "config.h"

#include "libks/compiler.h"

#include "doc.h"
#include "expr.h"
#include "lexer.h"
#include "parser-braces.h"
#include "parser-priv.h"
#include "parser-stmt-expr.h"
#include "parser-type.h"
#include "simple.h"
#include "style.h"
#include "token.h"

static struct doc	*expr_recover(const struct expr_exec_arg *, void *);
static struct doc	*expr_recover_cast(const struct expr_exec_arg *,
    void *);
static struct doc	*expr_recover_generic(const struct expr_exec_arg *,
    void *);
static struct doc	*expr_doc_token(struct token *, struct doc *,
    const char *, int, void *);

/*
 * A preprocessor branch can interrupt a recovery construct before it is
 * complete. Return the document emitted so far so that syntax committed
 * before the branch is preserved; the branch retry's duplicate is muted.
 */
static struct doc *
recover_fail(struct parser *pr, struct doc *dc)
{
	if ((parser_good(pr) & BRCH) != 0)
		return dc;
	return NULL;
}

int
parser_expr_peek(struct parser *pr, struct token **tk)
{
	const struct expr_exec_arg ea = {
		.st		= pr->pr_st,
		.lx		= pr->pr_lx,
		.arena		= {
			.scratch	= pr->pr_arena.scratch,
			.buffer		= pr->pr_arena.buffer,
		},
		.callbacks	= {
			.recover	= expr_recover,
			.recover_cast	= expr_recover_cast,
			.recover_generic	= expr_recover_generic,
			.doc_token	= expr_doc_token,
			.arg		= pr,
		},
	};
	int peek, simple;

	simple = simple_disable(pr->pr_si);
	peek = expr_peek(&ea, tk);
	simple_enable(pr->pr_si, simple);
	return peek;
}

int
parser_expr(struct parser *pr, struct doc **expr, struct parser_expr_arg *arg)
{
	const struct expr_exec_arg ea = {
		.st		= pr->pr_st,
		.si		= pr->pr_si,
		.lx		= pr->pr_lx,
		.dc		= arg->dc,
		.rl		= arg->rl,
		.stop		= arg->stop,
		.indent		= arg->indent,
		.align		= arg->align,
		.flags		= arg->flags,
		.arena		= {
			.scratch	= pr->pr_arena.scratch,
			.buffer		= pr->pr_arena.buffer,
		},
		.callbacks	= {
			.recover	= expr_recover,
			.recover_cast	= expr_recover_cast,
			.recover_generic	= expr_recover_generic,
			.doc_token	= expr_doc_token,
			.arg		= pr,
		},
	};
	struct doc *ex;

	ex = expr_exec(&ea);
	if (ex == NULL)
		return parser_none(pr);
	if (expr != NULL)
		*expr = ex;
	return parser_good(pr);
}

static struct doc *
expr_recover(const struct expr_exec_arg *ea, void *arg)
{
	struct parser_type type;
	struct doc *dc = NULL;
	struct parser *pr = arg;
	struct lexer *lx = pr->pr_lx;
	struct token *lbrace, *tk;

	if (parser_type_peek(pr, &type, PARSER_TYPE_EXPR)) {
		struct token *nx;

		if (lexer_back_if(lx, TOKEN_SIZEOF, NULL) ||
		    lexer_back_if(lx, TOKEN_ALIGNOF, NULL) ||
		    ((lexer_back_if(lx, TOKEN_LPAREN, NULL) ||
		      lexer_back_if(lx, TOKEN_COMMA, NULL)) &&
		     ((nx = token_next(type.end)) != NULL &&
		      (nx->tk_type == TOKEN_RPAREN ||
		       nx->tk_type == TOKEN_COMMA ||
		       nx->tk_type == LEXER_EOF)))) {
			dc = doc_root(pr->pr_arena_scope.doc);
			if (parser_type(pr, dc, &type, NULL) & GOOD)
				return dc;
		}
	} else if (lexer_peek_if_flags(lx, TOKEN_FLAG_BINARY, &tk)) {
		struct token *pv;

		pv = token_prev(tk);
		if (pv != NULL &&
		    (pv->tk_type == TOKEN_LPAREN ||
		     pv->tk_type == TOKEN_COMMA)) {
			(void)lexer_pop(lx, &tk);
			dc = doc_root(pr->pr_arena_scope.doc);
			parser_doc_token(pr, tk, dc);
			return dc;
		}
	} else if (lexer_peek_if(lx, TOKEN_LBRACE, &lbrace)) {
		int error;

		dc = doc_root(pr->pr_arena_scope.doc);
		error = parser_braces(pr, dc, dc, ea->indent,
		    PARSER_BRACES_DEDENT | PARSER_BRACES_INDENT_MAYBE);
		if (error & GOOD)
			return dc;
		if (error & BRCH)
			return dc;
		if (error & FAIL) {
			/* Try again, could be a GNU statement expression. */
			dc = doc_root(pr->pr_arena_scope.doc);
			parser_reset(pr);
			lexer_seek(lx, lbrace);
			error = parser_stmt_expr_gnu(pr, dc);
			if (error & GOOD)
				return dc;
			if (error & BRCH)
				return dc;
		}
	} else if (lexer_if(lx, TOKEN_COMMA, &tk)) {
		/* Some macros allow empty arguments such as queue(3). */
		dc = doc_root(pr->pr_arena_scope.doc);
		parser_doc_token(pr, tk, dc);
		return dc;
	} else if (lexer_if(lx, TOKEN_STAR, &tk)) {
		/*
		 * Some macros like MAP() from libks accepts a sole star as an
		 * argument. Prevent the expression parser from interpreting it
		 * as a unary operator.
		 */
		dc = doc_root(pr->pr_arena_scope.doc);
		parser_doc_token(pr, tk, dc);
		return dc;
	}

	return NULL;
}

static int
peek_binary_operator(struct lexer *lx, const struct token *rparen)
{
	struct token *op;

	return lexer_peek_if_flags(lx, TOKEN_FLAG_BINARY, &op) &&
	    token_has_spaces(rparen) &&
	    (token_has_spaces(op) || token_has_line(op, 1));
}

static struct doc *
expr_recover_cast(const struct expr_exec_arg *UNUSED(ea), void *arg)
{
	struct parser_type type;
	struct doc *dc = NULL;
	struct lexer_state s;
	struct parser *pr = arg;
	struct lexer *lx = pr->pr_lx;
	struct token *rparen;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (parser_type_peek(pr, &type, PARSER_TYPE_CAST) &&
	    lexer_seek_after(lx, type.end) &&
	    lexer_if(lx, TOKEN_RPAREN, &rparen) &&
	    !lexer_if(lx, TOKEN_RPAREN, NULL) &&
	    !lexer_if(lx, TOKEN_COMMA, NULL) &&
	    !peek_binary_operator(lx, rparen) &&
	    !(lexer_if(lx, TOKEN_AMP, NULL) &&
	    lexer_if(lx, TOKEN_TILDE, NULL)) &&
	    !lexer_if(lx, LEXER_EOF, NULL))
		peek = 1;
	lexer_peek_leave(lx, &s);
	if (!peek)
		return NULL;

	dc = doc_root(pr->pr_arena_scope.doc);
	if (parser_type(pr, dc, &type, NULL) & GOOD)
		return dc;
	return NULL;
}

/*
 * Parse a C11/C23 generic selection, i.e.
 * _Generic ( assignment-expression , generic-association-list ).
 */
static struct doc *
expr_recover_generic(const struct expr_exec_arg *UNUSED(ea), void *arg)
{
	struct parser *pr = arg;
	struct lexer *lx = pr->pr_lx;
	struct doc *dc, *expr;
	struct token *tk, *lparen, *rparen, *comma, *stop;
	int error, nassoc = 0;

	if (!lexer_back(lx, &tk) || tk->tk_type != TOKEN_GENERIC)
		return NULL;

	dc = doc_root(pr->pr_arena_scope.doc);
	parser_doc_token(pr, tk, dc);

	/* Find the matching right parenthesis to bound the construct. */
	if (!lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &lparen,
	    &rparen))
		return NULL;
	if (!lexer_if(lx, TOKEN_LPAREN, &lparen))
		return NULL;
	parser_doc_token(pr, lparen, dc);

	/* Controlling expression, a single assignment-expression. */
	if (!lexer_peek_until_comma(lx, rparen, &stop) || stop == rparen)
		return recover_fail(pr, dc);
	error = parser_expr(pr, &expr, &(struct parser_expr_arg){
	    .dc		= dc,
	    .stop	= stop,
	    .indent	= style(pr->pr_st, ContinuationIndentWidth),
	});
	/*
	 * A branch retry re-parses the whole construct. Return what has been
	 * emitted so far so the committed prefix is not lost, and let the
	 * retry's copy be muted.
	 */
	if (error & BRCH)
		return dc;
	if (error & (FAIL | NONE))
		return NULL;
	if (!lexer_expect(lx, TOKEN_COMMA, &comma))
		return recover_fail(pr, dc);
	parser_doc_token(pr, comma, dc);
	doc_literal(" ", dc);

	for (;;) {
		struct parser_type type;

		if (lexer_if(lx, TOKEN_DEFAULT, &tk)) {
			parser_doc_token(pr, tk, dc);
		} else if (parser_type_peek(pr, &type, 0)) {
			error = parser_type(pr, dc, &type, NULL);
			if (error & BRCH)
				return dc;
			if (error & HALT)
				return recover_fail(pr, dc);
		} else {
			return recover_fail(pr, dc);
		}

		if (!lexer_expect(lx, TOKEN_COLON, &tk))
			return recover_fail(pr, dc);
		parser_doc_token(pr, tk, dc);
		doc_literal(" ", dc);

		(void)lexer_peek_until_comma(lx, rparen, &stop);
		error = parser_expr(pr, &expr, &(struct parser_expr_arg){
		    .dc		= dc,
		    .stop	= stop,
		    .indent	= style(pr->pr_st, ContinuationIndentWidth),
		});
		if (error & BRCH)
			return dc;
		if (error & (FAIL | NONE))
			return NULL;
		nassoc++;

		if (stop == rparen)
			break;
		if (!lexer_expect(lx, TOKEN_COMMA, &comma))
			return recover_fail(pr, dc);
		parser_doc_token(pr, comma, dc);
		doc_literal(" ", dc);
	}
	if (nassoc == 0)
		return recover_fail(pr, dc);

	if (!lexer_expect(lx, TOKEN_RPAREN, &tk))
		return recover_fail(pr, dc);
	parser_doc_token(pr, tk, dc);

	return dc;
}

static struct doc *
expr_doc_token(struct token *tk, struct doc *dc, const char *fun, int lno,
    void *arg)
{
	struct parser *pr = arg;

	return parser_doc_token_impl(pr, tk, dc, fun, lno);
}
