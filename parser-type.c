#include "parser-type.h"

#include "config.h"

#include <string.h>

#include "doc.h"
#include "clang.h"
#include "lexer.h"
#include "parser-attributes.h"
#include "parser-cpp.h"
#include "parser-expr.h"
#include "parser-func.h"
#include "parser-priv.h"
#include "ruler.h"
#include "simple-implicit-int.h"
#include "simple-storage.h"
#include "simple.h"
#include "style.h"
#include "token.h"

typedef struct Parser_Type_Context {
	/* Type of previously consumed token. */
	int previous_token_type;
	/* Type of consumed token. */
	int token_type;
} Parser_Type_Context;

static int	peek_type_func_ptr(struct parser *, struct lexer *,
    struct token **, struct token **);
static int	peek_type_ident_after_type(struct parser *, const Parser_Type_Context *);
static int	peek_type_ptr_array(struct lexer *, struct token **);
static int	peek_type_noident(struct lexer *, struct token **);
static int	peek_type_unknown_array(struct lexer *, struct token **);
static int	peek_type_unknown_bitfield(struct lexer *, struct token **);
static int	peek_type_squares(struct lexer *, struct token **);
static int	peek_type_paren_ident(struct lexer *, enum clang_token_type,
    struct token **);
static int	peek_type_alignas(struct lexer *, struct token **);

int
parser_type_peek(struct parser *pr, struct parser_type *type,
    unsigned int flags)
{
	Parser_Type_Context c = {.previous_token_type = TOKEN_NONE};
	struct lexer *lx = pr->pr_lx;
	struct lexer_state s;
	struct token *align = NULL;
	struct token *args = NULL;
	struct token *beg, *end;
	int peek = 0;
	int nkeywords = 0;
	int ntokens = 0;
	int unknown = 0;
	int issizeof;

	if (!lexer_peek(lx, &beg))
		return 0;
	issizeof = lexer_back_if(lx, TOKEN_SIZEOF, NULL) ||
	    lexer_back_if(lx, TOKEN_ALIGNOF, NULL);

	/*
	 * Recognize function argument consisting of a single type and no
	 * variable name.
	 */
	if ((flags & (PARSER_TYPE_CAST | PARSER_TYPE_ARG)) &&
	    (peek_type_noident(lx, &end) || peek_type_unknown_array(lx, &end)))
		goto out;

	if (peek_type_unknown_bitfield(lx, &end))
		goto out;

	lexer_peek_enter(lx, &s);
	for (;;) {
		struct token *rparen, *rsquare;

		if (lexer_peek_if(lx, LEXER_EOF, NULL))
			break;

		c.previous_token_type = c.token_type;
		c.token_type = TOKEN_NONE;

		if (lexer_if(lx, TOKEN_ATOMIC, &end)) {
			/*
			 * _Atomic is either a type qualifier or, when
			 * followed by a parenthesized type name, a type
			 * specifier.
			 */
			if (lexer_peek_if(lx, TOKEN_LPAREN, NULL)) {
				if (!lexer_if_pair(lx, TOKEN_LPAREN,
				    TOKEN_RPAREN, NULL, &rparen))
					return 0;
				end = rparen;
			} else {
				nkeywords++;
			}
			peek = 1;
		} else if (lexer_if(lx, TOKEN_BITINT, &end)) {
			if (!lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN,
			    NULL, &rparen))
				return 0;
			end = rparen;
			peek = 1;
		} else if (peek_type_paren_ident(lx, CLANG_TOKEN_TYPEOF, &end) ||
		    peek_type_paren_ident(lx, CLANG_TOKEN_TYPEOF_UNQUAL, &end)) {
			if (!lexer_if(lx, TOKEN_IDENT, NULL) ||
			    !lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN,
			    NULL, NULL))
				return 0;
			peek = 1;
		} else if (peek_type_alignas(lx, &end)) {
			if (!lexer_seek_after(lx, end))
				return 0;
			peek = 1;
		} else if (parser_attributes_std_peek(pr, &rparen)) {
			if (!lexer_seek_after(lx, rparen))
				return 0;
			c.token_type = TOKEN_ATTRIBUTE;
			end = rparen;
		} else if (lexer_if_flags(lx,
		    TOKEN_FLAG_QUALIFIER | TOKEN_FLAG_STORAGE, &end)) {
			nkeywords++;
		} else if (lexer_if_flags(lx, TOKEN_FLAG_TYPE, &end)) {
			int type_token = end->tk_type;

			/* C23 attributes may follow struct/union/enum. */
			if (type_token == TOKEN_ENUM ||
			    type_token == TOKEN_STRUCT ||
			    type_token == TOKEN_UNION) {
				if (parser_attributes_std_peek(pr, &rparen) &&
				    !lexer_seek_after(lx, rparen))
					return 0;
				(void)lexer_if(lx, TOKEN_IDENT, &end);
			}
			/* C23: enum E : type-name { ... } */
			if (type_token == TOKEN_ENUM)
				(void)lexer_if(lx, TOKEN_COLON, &end);
			/* Recognize constructs like `struct s[]'. */
			if (peek_type_squares(lx, &rsquare) &&
			    lexer_seek_after(lx, rsquare))
				end = rsquare;
			peek = 1;
		} else if (ntokens > 0 && lexer_if(lx, TOKEN_STAR, &end)) {
			/*
			 * A pointer is expected to only be followed by another
			 * pointer or a known type.
			 */
			if (lexer_peek_if(lx, TOKEN_IDENT, NULL))
				break;
			peek = 1;
		} else if (parser_cpp_peek_type(pr, &rparen)) {
			if (!lexer_seek_after(lx, rparen))
				return 0;
			end = rparen;
			peek = 1;
		} else if (lexer_peek_if(lx, TOKEN_IDENT, NULL)) {
			/* Ensure this is not the identifier after the type. */
			if ((flags & PARSER_TYPE_CAST) == 0 &&
			    (flags & PARSER_TYPE_EXPR) == 0 &&
			    peek_type_ident_after_type(pr, &c))
				break;

			/* Identifier is part of the type, consume it. */
			if (!lexer_if(lx, TOKEN_IDENT, &end))
				return 0;
			/*
			 * Preceding storage/qualifier followed by identifier,
			 * treat it as a type.
			 */
			if (nkeywords > 0)
				peek = 1;
		} else if (ntokens > 0 && peek_type_func_ptr(pr, lx, &args, &end)) {
			if (!lexer_back(lx, &align))
				return 0;
			peek = 1;
			break;
		} else if (ntokens > 0 && peek_type_ptr_array(lx, &rsquare)) {
			if (!lexer_back(lx, &align) ||
			    !lexer_seek_after(lx, rsquare))
				return 0;
			end = rsquare;
			peek = 1;
			break;
		} else if (parser_attributes_peek(pr, &rparen, 0)) {
			if (!lexer_seek_after(lx, rparen))
				return 0;
			c.token_type = TOKEN_ATTRIBUTE;
			end = rparen;
		} else {
			unknown = 1;
			break;
		}

		ntokens++;
		if (issizeof)
			break;
	}
	lexer_peek_leave(lx, &s);

	if (ntokens > 0 && ntokens == nkeywords &&
	    (flags & PARSER_TYPE_ARG) == 0) {
		/* Only qualifier or storage token(s) cannot denote a type. */
		peek = 0;
	} else if (!peek && !unknown && ntokens > 0) {
		/*
		 * Nothing was found. However this is a sequence of identifiers
		 * (i.e. unknown types) therefore treat it as a type.
		 */
		peek = 1;
	}
	if (!peek)
		return 0;

	{
		simple_cookie(simple);
		if (simple_enter(pr->pr_si, SIMPLE_STORAGE, 0, &simple)) {
			end = simple_storage(lx, beg, end);
			/*
			 * Must be evaluated again as the simple static pass
			 * could reorder tokens.
			 */
			if (!lexer_peek(lx, &beg))
				return 0;
		}
	}

	{
		simple_cookie(simple);
		if (simple_enter(pr->pr_si, SIMPLE_IMPLICIT_INT, 0, &simple))
			end = simple_implicit_int(lx, beg, end);
	}

out:
	if (type != NULL) {
		*type = (struct parser_type){
		    .beg	= beg,
		    .end	= end,
		    .align	= align,
		    .args	= args,
		};
	}
	return 1;
}

static const struct token *
find_align_token(const struct parser_type *type, unsigned int *nspaces)
{
	const struct token *align, *nx;
	unsigned int nstars = 0;

	/*
	 * Find the first non pointer token starting from the end, this is where
	 * the ruler alignment must be performed.
	 */
	align = type->align != NULL ? type->align : type->end;
	while (align->tk_type == TOKEN_STAR) {
		nstars++;
		if (align == type->beg)
			break;
		align = token_prev(align);
	}

	nx = token_next(align);
	if (nx != NULL) {
		/* No alignment wanted if the first non-pointer token is
		 * followed by a semi. */
		if (nx->tk_type == TOKEN_SEMI)
			return NULL;
		/* No alignment wanted for function pointer types. */
		if (align->tk_type == TOKEN_RPAREN && nx->tk_type == TOKEN_LPAREN)
			return NULL;
	}

	*nspaces = nstars;
	return align;
}

int
parser_type(struct parser *pr, struct doc *dc, struct parser_type *type,
    struct ruler *rl)
{
	struct lexer *lx = pr->pr_lx;
	const struct token *align = NULL;
	const struct token *end = type->end;
	unsigned int nspaces = 0;

	if (rl != NULL)
		align = find_align_token(type, &nspaces);

	for (;;) {
		struct doc *concat;
		struct token *pending;
		struct token *tk;
		int didalign = 0;

		if (parser_attributes_peek(pr, &pending, 0)) {
			struct token *before = NULL;

			(void)lexer_back(lx, &before);
			concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));
			if ((parser_attributes(pr, concat, NULL, 0) & FAIL) || !lexer_back(lx, &tk))
				return parser_fail(pr);
			if (tk == before) {
				/* No progress, bail out to avoid looping forever. */
				return parser_fail(pr);
			}
			doc_alloc(DOC_LINE, concat);
			if (tk == end)
				break;
			continue;
		}

		if (parser_attributes_std_peek(pr, NULL)) {
			struct token *before = NULL;
			struct token *first;

			(void)lexer_back(lx, &before);
			if (lexer_peek(lx, &first) && first != type->beg)
				doc_alloc(DOC_LINE, dc);
			concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));
			if ((parser_attributes_std(pr, concat) & FAIL) ||
			    !lexer_back(lx, &tk))
				return parser_fail(pr);
			if (tk == before || tk->tk_type == LEXER_EOF)
				return parser_fail(pr);
			if (!lexer_peek_if(lx, TOKEN_RPAREN, NULL) &&
			    !lexer_peek_if(lx, TOKEN_RSQUARE, NULL) &&
			    !lexer_peek_if(lx, TOKEN_COMMA, NULL) &&
			    !lexer_peek_if(lx, TOKEN_SEMI, NULL) &&
			    !lexer_peek_if(lx, TOKEN_LSQUARE, NULL))
				doc_alloc(DOC_LINE, concat);
			if (tk == end)
				break;
			continue;
		}

		if (!lexer_pop(lx, &tk))
			return parser_fail(pr);
		parser_token_trim_after(pr, tk);

		/* The end token is expected to be reachable. */
		if (tk->tk_type == LEXER_EOF)
			return parser_fail(pr);

		if (tk == type->args) {
			struct doc *indent;
			struct token *lparen = tk;
			struct token *rparen;
			unsigned int w;

			parser_doc_token(pr, lparen, dc);
			if (style(pr->pr_st, AlignAfterOpenBracket) == Align)
				w = parser_width(pr, dc);
			else
				w = style(pr->pr_st, ContinuationIndentWidth);
			indent = doc_indent(w, dc);
			while (parser_func_arg(pr, indent, NULL, end) & GOOD)
				continue;
			if (lexer_expect(lx, TOKEN_RPAREN, &rparen))
				parser_doc_token(pr, rparen, dc);
			break;
		}

		concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));
		parser_doc_token(pr, tk, concat);

		if (tk->tk_type == TOKEN_LPAREN) {
			unsigned int indent;
			int error;

			indent = style(pr->pr_st, ContinuationIndentWidth);
			error = parser_expr(pr, &concat,
			    &(struct parser_expr_arg){
				.dc	= concat,
				.indent	= indent,
			});
			if (error & HALT)
				return error;
			if (!lexer_back(lx, &tk))
				return parser_fail(pr);
		}

		if (tk == align) {
			if (token_is_decl(tk, TOKEN_ENUM) ||
			    token_is_decl(tk, TOKEN_STRUCT) ||
			    token_is_decl(tk, TOKEN_UNION)) {
				doc_alloc(DOC_LINE, concat);
			} else {
				ruler_insert(rl, tk, concat, 1,
				    parser_width(pr, dc), nspaces);
			}
			didalign = 1;
		}

		if (tk == end)
			break;

		if (!didalign) {
			struct lexer_state s;
			struct token *nx;

			lexer_peek_enter(lx, &s);
			if (tk->tk_type != TOKEN_STAR &&
			    tk->tk_type != TOKEN_LPAREN &&
			    tk->tk_type != TOKEN_LSQUARE &&
			    lexer_pop(lx, &nx) &&
			    (nx->tk_type != TOKEN_LPAREN ||
			     lexer_if(lx, TOKEN_STAR, NULL)) &&
			    nx->tk_type != TOKEN_LSQUARE &&
			    nx->tk_type != TOKEN_RSQUARE &&
			    nx->tk_type != TOKEN_RPAREN &&
			    nx->tk_type != TOKEN_COMMA)
				doc_alloc(DOC_LINE, concat);
			lexer_peek_leave(lx, &s);
		}
	}

	return parser_good(pr);
}

static int
peek_type_func_ptr(struct parser *pr, struct lexer *lx, struct token **lhs,
    struct token **rhs)
{
	struct lexer_state s;
	struct token *lparen, *rparen;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_LPAREN, NULL) &&
	    lexer_if(lx, TOKEN_STAR, NULL)) {
		struct token *ident = NULL;

		lexer_if_flags(lx, TOKEN_FLAG_QUALIFIER, NULL);
		while (lexer_if(lx, TOKEN_STAR, NULL))
			continue;
		lexer_if_flags(lx, TOKEN_FLAG_QUALIFIER, NULL);

		lexer_if(lx, TOKEN_IDENT, &ident);
		/* C23 attributes may trail the declarator identifier. */
		for (;;) {
			struct token *attr;

			if (!parser_attributes_std_peek(pr, &attr) ||
			    !lexer_seek_after(lx, attr))
				break;
		}
		if (lexer_if(lx, TOKEN_LSQUARE, NULL)) {
			(void)lexer_if(lx, TOKEN_LITERAL, NULL);
			(void)lexer_if(lx, TOKEN_RSQUARE, NULL);
		}
		if (lexer_if(lx, TOKEN_RPAREN, &rparen)) {
			if (lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN,
			    &lparen, rhs)) {
				*lhs = lparen;
				peek = 1;
			} else if (ident == NULL &&
			    (lexer_if(lx, TOKEN_RPAREN, NULL) ||
			     lexer_if(lx, LEXER_EOF, NULL))) {
				/*
				 * Function pointer lacking arguments wrapped in
				 * parenthesis. Be careful here in order to not
				 * confuse a function call.
				 */
				peek = 1;
				*rhs = rparen;
			}
		}
	}
	lexer_peek_leave(lx, &s);

	return peek;
}

static int
peek_type_ptr_array(struct lexer *lx, struct token **rsquare)
{
	struct lexer_state s;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_LPAREN, NULL) &&
	    lexer_if(lx, TOKEN_STAR, NULL)) {
		(void)lexer_if(lx, TOKEN_IDENT, NULL);
		if (lexer_if(lx, TOKEN_RPAREN, NULL) &&
		    lexer_if(lx, TOKEN_LSQUARE, NULL) &&
		    (lexer_if(lx, TOKEN_LITERAL, NULL) ||
		     lexer_if(lx, TOKEN_IDENT, NULL)) &&
		    lexer_if(lx, TOKEN_RSQUARE, rsquare))
			peek = 1;
	}
	lexer_peek_leave(lx, &s);
	return peek;
}

static int
peek_type_ident_after_type(struct parser *pr, const Parser_Type_Context *c)
{
	struct lexer_state s;
	struct lexer *lx = pr->pr_lx;
	struct token *rparen;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_IDENT, NULL) &&
	    (lexer_if_flags(lx, TOKEN_FLAG_ASSIGN, NULL) ||
	     lexer_if(lx, TOKEN_LSQUARE, NULL) ||
	     (lexer_if(lx, TOKEN_LPAREN, NULL) &&
	      !lexer_peek_if(lx, TOKEN_STAR, NULL)) ||
	     lexer_if(lx, TOKEN_RPAREN, NULL) ||
	     lexer_if(lx, TOKEN_SEMI, NULL) ||
	     lexer_if(lx, TOKEN_COMMA, NULL) ||
	     lexer_if(lx, TOKEN_COLON, NULL) ||
	     lexer_if(lx, TOKEN_ASSEMBLY, NULL) ||
	     (parser_attributes_peek(pr, &rparen, 0) &&
	      lexer_seek_after(lx, rparen) &&
	      !lexer_if(lx, TOKEN_IDENT, NULL)) ||
	     c->previous_token_type == TOKEN_ATTRIBUTE))
		peek = 1;
	lexer_peek_leave(lx, &s);

	return peek;
}

static int
peek_type_noident(struct lexer *lx, struct token **tk)
{
	struct lexer_state s;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_IDENT, tk) &&
	    (lexer_if(lx, TOKEN_RPAREN, NULL) ||
	     lexer_if(lx, TOKEN_COMMA, NULL)))
		peek = 1;
	lexer_peek_leave(lx, &s);
	return peek;
}

static int
peek_type_unknown_array(struct lexer *lx, struct token **tk)
{
	struct lexer_state s;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_IDENT, NULL) &&
	    lexer_if(lx, TOKEN_LSQUARE, NULL)) {
		(void)lexer_if(lx, TOKEN_LITERAL, NULL);
		if (lexer_if(lx, TOKEN_RSQUARE, tk))
			peek = 1;
	}
	lexer_peek_leave(lx, &s);
	return peek;
}

static int
peek_type_unknown_bitfield(struct lexer *lx, struct token **tk)
{
	struct lexer_state s;
	int peek;

	lexer_peek_enter(lx, &s);
	peek = lexer_if(lx, TOKEN_IDENT, tk) &&
	    lexer_if(lx, TOKEN_COLON, NULL) &&
	    lexer_if(lx, TOKEN_LITERAL, NULL);
	lexer_peek_leave(lx, &s);
	return peek;
}

static int
peek_type_squares(struct lexer *lx, struct token **rsquare)
{
	struct lexer_state ls;
	int peek = 0;

	lexer_peek_enter(lx, &ls);
	if (lexer_if(lx, TOKEN_LSQUARE, NULL)) {
		(void)lexer_if(lx, TOKEN_LITERAL, NULL);
		if (lexer_if(lx, TOKEN_RSQUARE, rsquare))
			peek = 1;
	}
	lexer_peek_leave(lx, &ls);
	return peek;
}

/*
 * Recognize a type specifier of the form <identifier> ( type-name ) where the
 * identifier is a C23 contextual keyword such as typeof. Returns non-zero and
 * sets rparen to the closing parenthesis if recognized. The lexer is left
 * untouched.
 */
static int
peek_type_paren_ident(struct lexer *lx, enum clang_token_type clang_type,
    struct token **rparen)
{
	struct lexer_state s;
	struct token *tk, *end;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_IDENT, &tk) &&
	    clang_token_type(tk) == clang_type &&
	    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, NULL, &end)) {
		*rparen = end;
		peek = 1;
	}
	lexer_peek_leave(lx, &s);
	return peek;
}

/*
 * Recognize an alignment specifier, i.e. _Alignas or the C23 contextual
 * keyword alignas, followed by a parenthesized constant expression or type
 * name. Returns non-zero and sets rparen to the closing parenthesis if
 * recognized. The lexer is left untouched.
 */
static int
peek_type_alignas(struct lexer *lx, struct token **rparen)
{
	struct lexer_state s;
	struct token *tk, *end;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_ALIGNAS, &tk) ||
	    lexer_if(lx, TOKEN_IDENT, &tk)) {
		if ((tk->tk_type == TOKEN_ALIGNAS ||
		     clang_token_type(tk) == CLANG_TOKEN_ALIGNAS) &&
		    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, NULL,
		    &end)) {
			*rparen = end;
			peek = 1;
		}
	}
	lexer_peek_leave(lx, &s);
	return peek;
}
