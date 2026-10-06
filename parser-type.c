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

/* Bound declarator-scanning recursion. */
#define SCAN_MAX_DEPTH	8000

static int		 peek_type_ident_after_type(struct parser *, const Parser_Type_Context *);
static int		 peek_type_declarator(struct parser *, struct token **,
    struct token **);
static int		 is_declarator_paren(struct parser *, struct lexer *);
static struct token	*matching_rparen(struct token *);
static int		 scan_declarator(struct parser *, struct lexer *,
    struct token **, struct token **, unsigned int *);
static int		 scan_declarator1(struct parser *, struct lexer *,
    struct token **, struct token **, unsigned int *);
static int		 scan_function_declarator1(struct parser *,
    struct lexer *, struct token **, struct token **);
static int		 peek_type_noident(struct lexer *, struct token **);
static int		 peek_type_unknown_array(struct lexer *, struct token **);
static int		 peek_type_unknown_bitfield(struct lexer *, struct token **);
static int		 peek_type_squares(struct lexer *, struct token **);
static int		 peek_type_paren_ident(struct lexer *, enum clang_token_type,
    struct token **);
static int		 peek_type_alignas(struct lexer *, struct token **);
static int		 peek_paren_pair_then_lparen(struct lexer *);
static int		 peek_param_list_valid(struct lexer *);
static int		 peek_param_content(struct lexer *);
static int		 peek_type_abstract_func(struct lexer *, struct token **,
    struct token **);

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
	int implicit_int = 0;
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
				if (parser_attributes_std_peek(pr, &rparen)) {
					if (!lexer_seek_after(lx, rparen))
						return 0;
					end = rparen;
				}
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
			if (lexer_peek_if(lx, TOKEN_IDENT, NULL)) {
				/*
				 * Also allow an implicit int pointer
				 * declaration, e.g. `static *p;'.
				 */
				if ((flags & (PARSER_TYPE_ARG |
				    PARSER_TYPE_CAST | PARSER_TYPE_EXPR)) == 0 &&
				    parser_type_implicit_int(pr))
					implicit_int = 1;
				break;
			}
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
			    peek_type_ident_after_type(pr, &c)) {
				/*
				 * Remember whether the trailing identifier is a
				 * plausible declarator. Together with a
				 * storage/qualifier-only specifier sequence this
				 * denotes an implicit int declaration as
				 * standardized by C89/C90, e.g. `static x;'.
				 * Only applicable to declarations; abstract
				 * parameter and cast type names must not be
				 * affected.
				 */
				if ((flags & (PARSER_TYPE_ARG |
				    PARSER_TYPE_CAST | PARSER_TYPE_EXPR)) == 0 &&
				    parser_type_implicit_int(pr))
					implicit_int = 1;
				break;
			}

			/* Identifier is part of the type, consume it. */
			if (!lexer_if(lx, TOKEN_IDENT, &end))
				return 0;
			/*
			 * Preceding storage/qualifier followed by identifier,
			 * treat it as a type.
			 */
			if (nkeywords > 0)
				peek = 1;
		} else if (ntokens > 0 && peek_type_declarator(pr, &args, &end)) {
			if (!lexer_back(lx, &align) ||
			    !lexer_seek_after(lx, end))
				return 0;
			peek = 1;
			break;
		} else if (ntokens > 0 && nkeywords > 0 &&
		    lexer_peek_if(lx, TOKEN_LPAREN, NULL) &&
		    parser_type_implicit_int(pr)) {
			/*
			 * Implicit int declaration with a parenthesized
			 * declarator, e.g. the C23 inferred declaration
			 * auto (*p) = init;. The declarator is rendered by
			 * parser_decl_init().
			 */
			implicit_int = 1;
			peek = 1;
			break;
		} else if (ntokens > 0 && peek_paren_pair_then_lparen(lx)) {
			/*
			 * A parenthesized declarator with a following parameter
			 * list ends the type: type ( ... ) ( args ).
			 */
			peek = 1;
			break;
		} else if (peek &&
		    (flags & (PARSER_TYPE_EXPR | PARSER_TYPE_CAST)) != 0 &&
		    peek_type_abstract_func(lx, &args, &end)) {
			/*
			 * Abstract function type in a type name, e.g.
			 * sizeof(int (int)) or typeof(int (void)).
			 */
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
		/*
		 * Only qualifier or storage token(s) cannot denote a type,
		 * unless this is a C89/C90 implicit int declaration where the
		 * trailing identifier is a non-function declarator.
		 */
		if (implicit_int && (flags &
		    (PARSER_TYPE_CAST | PARSER_TYPE_EXPR)) == 0)
			peek = 1;
		else
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
			struct token *rparen, *arg_rparen;
			unsigned int w;

			parser_doc_token(pr, lparen, dc);
			if (style(pr->pr_st, AlignAfterOpenBracket) == Align)
				w = parser_width(pr, dc);
			else
				w = style(pr->pr_st, ContinuationIndentWidth);
			indent = doc_indent(w, dc);
			/*
			 * The matching parenthesis may not be the end of the
			 * declarator, e.g. (*(*f)(void))[3]; use the parameter
			 * list's own closing parenthesis as the stop token.
			 */
			arg_rparen = matching_rparen(lparen);
			if (arg_rparen == NULL)
				return parser_fail(pr);
			while (parser_func_arg(pr, indent, NULL, arg_rparen) & GOOD)
				continue;
			if (lexer_expect(lx, TOKEN_RPAREN, &rparen))
				parser_doc_token(pr, rparen, dc);
			if (rparen == end)
				break;
			continue;
		}

		concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));
		parser_doc_token(pr, tk, concat);

		/*
		 * A parenthesis that is not the start of a function parameter
		 * list (type->args) and that encloses a declarator, e.g.
		 * (*const p) or (*(*f)(void))[3], is rendered by this loop so
		 * that the nested pointer qualifiers are preserved. Anything
		 * else, such as the argument list of a macro declaration, is
		 * rendered as an expression.
		 */
		if (tk->tk_type == TOKEN_LPAREN &&
		    !is_declarator_paren(pr, lx)) {
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

/*
 * Properties observed while scanning a declarator, used to decide whether a
 * parenthesized declarator must be part of the type range.
 */
#define SCAN_QUALIFIER	0x01	/* Pointer qualifier observed. */
#define SCAN_SUFFIX	0x02	/* Array or function suffix observed. */
#define SCAN_IDENT	0x04	/* Identifier observed. */

/*
 * Scan a (possibly abstract) C declarator, consuming tokens. On success *end
 * is set to the last token of the declarator and *args, if a function
 * parameter list is part of the declarator, to its opening parenthesis.
 * Properties observed are ORed into *flags. Returns non-zero on success. This
 * is a peek helper; the caller is responsible for restoring the lexer state.
 *
 * declarator       = pointer? direct-declarator
 * pointer          = ( * attribute-specifier-sequence? type-qualifier-list? )+
 * direct-declarator= identifier
 *                  | ( declarator )
 *                  | direct-declarator [ type-name? ]
 *                  | direct-declarator ( parameter-type-list? )
 */
static int
scan_declarator(struct parser *pr, struct lexer *lx, struct token **args,
    struct token **end, unsigned int *flags)
{
	int result;

	/* Bound recursion so that deeply nested declarators cannot overflow. */
	if (pr->pr_depth >= SCAN_MAX_DEPTH)
		return 0;
	pr->pr_depth++;
	result = scan_declarator1(pr, lx, args, end, flags);
	pr->pr_depth--;
	return result;
}

static int
scan_declarator1(struct parser *pr, struct lexer *lx, struct token **args,
    struct token **end, unsigned int *flags)
{
	struct token *tk;
	int have = 0;

	for (;;) {
		struct token *attr;
		int nqual = 0;

		while (parser_attributes_std_peek(pr, &attr) &&
		    lexer_seek_after(lx, attr))
			continue;
		while (lexer_if_flags(lx, TOKEN_FLAG_QUALIFIER, NULL))
			nqual++;
		if (nqual > 0)
			*flags |= SCAN_QUALIFIER;
		if (!lexer_if(lx, TOKEN_STAR, &tk))
			break;
		*end = tk;
		have = 1;
	}

	if (lexer_if(lx, TOKEN_IDENT, &tk)) {
		/*
		 * An identifier immediately followed by a parameter list is a
		 * function declarator, handled by parser_func(). Leave the
		 * declarator out of the type range so the return type is
		 * recognized, e.g. int (*f(void))(void).
		 */
		if (lexer_peek_if(lx, TOKEN_LPAREN, NULL))
			return 0;
		*flags |= SCAN_IDENT;
		*end = tk;
		have = 1;
	} else if (lexer_if(lx, TOKEN_LPAREN, &tk)) {
		if (!scan_declarator(pr, lx, args, end, flags))
			return 0;
		if (!lexer_if(lx, TOKEN_RPAREN, &tk))
			return 0;
		*end = tk;
		have = 1;
	}
	if (!have)
		return 0;

	for (;;) {
		struct token *lparen, *rparen;

		if (lexer_if_pair(lx, TOKEN_LSQUARE, TOKEN_RSQUARE, NULL,
		    &rparen)) {
			*flags |= SCAN_SUFFIX;
			*end = rparen;
			continue;
		}
		if (lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &lparen,
		    &rparen)) {
			*flags |= SCAN_SUFFIX;
			*args = lparen;
			*end = rparen;
			continue;
		}
		break;
	}

	return 1;
}

/*
 * Scan a function declarator, i.e. a declarator whose identifier is directly
 * followed by a parameter list, e.g. f(void), (*const f(void))[10] or
 * (*(*f)(void))[3]. On success *func_lparen is set to the function's own
 * parameter list and *end to the last token of the declarator. Returns
 * non-zero on success. Peek helper; the caller restores the lexer state.
 */
static int
scan_function_declarator(struct parser *pr, struct lexer *lx,
    struct token **func_lparen, struct token **end)
{
	int result;

	if (pr->pr_depth >= SCAN_MAX_DEPTH)
		return 0;
	pr->pr_depth++;
	result = scan_function_declarator1(pr, lx, func_lparen, end);
	pr->pr_depth--;
	return result;
}

static int
scan_function_declarator1(struct parser *pr, struct lexer *lx,
    struct token **func_lparen, struct token **end)
{
	struct token *tk;

	for (;;) {
		struct token *attr;

		while (parser_attributes_std_peek(pr, &attr) &&
		    lexer_seek_after(lx, attr))
			continue;
		while (lexer_if_flags(lx, TOKEN_FLAG_QUALIFIER, NULL))
			continue;
		if (!lexer_if(lx, TOKEN_STAR, &tk))
			break;
		*end = tk;
	}

	if (lexer_if(lx, TOKEN_IDENT, &tk)) {
		*end = tk;
		if (!lexer_peek_if(lx, TOKEN_LPAREN, NULL))
			return 0;	/* Not a function. */
		if (!lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN,
		    func_lparen, &tk))
			return 0;
		*end = tk;
	} else if (lexer_if(lx, TOKEN_LPAREN, &tk)) {
		if (!scan_function_declarator(pr, lx, func_lparen, end))
			return 0;
		if (!lexer_if(lx, TOKEN_RPAREN, &tk))
			return 0;
		*end = tk;
	} else {
		return 0;
	}

	return 1;
}

/*
 * Returns non-zero if the closing parenthesis matching the given opening
 * parenthesis is part of a declarator, i.e. the tokens between them form a
 * declarator and nothing else. The lexer must be positioned immediately after
 * the opening parenthesis. Used to decide between declarator and expression
 * rendering.
 */
static int
is_declarator_paren(struct parser *pr, struct lexer *lx)
{
	struct lexer_state s;
	struct token *args = NULL, *end = NULL;
	unsigned int flags = 0;
	int ok = 0;

	lexer_peek_enter(lx, &s);
	if (scan_declarator(pr, lx, &args, &end, &flags) &&
	    lexer_peek_if(lx, TOKEN_RPAREN, NULL))
		ok = 1;
	lexer_peek_leave(lx, &s);

	if (!ok) {
		lexer_peek_enter(lx, &s);
		if (scan_function_declarator(pr, lx, &args, &end) &&
		    lexer_peek_if(lx, TOKEN_RPAREN, NULL))
			ok = 1;
		lexer_peek_leave(lx, &s);
	}
	return ok;
}

/*
 * Returns the closing parenthesis matching the given opening parenthesis, or
 * NULL if there is none.
 */
static struct token *
matching_rparen(struct token *lparen)
{
	struct token *tk = token_next(lparen);
	int depth = 1;

	for (; tk != NULL; tk = token_next(tk)) {
		if (tk->tk_type == TOKEN_LPAREN)
			depth++;
		else if (tk->tk_type == TOKEN_RPAREN && --depth == 0)
			return tk;
	}
	return NULL;
}

/*
 * Returns non-zero if the current token sequence is a parenthesized
 * declarator, e.g. (*const p), (*p)[10] or (*(*f)(void))[3]. On success *args
 * and *end are set to the last token of the declarator and, for a function
 * declarator, the opening parenthesis of its parameter list.
 *
 * A plain ( * identifier ) with no pointer qualifier and no array or function
 * suffix is deliberately excluded: it is indistinguishable from a function
 * call and is handled as a declarator by parser_decl_init() instead. This
 * keeps block scope expressions such as free(*buf) from being turned into
 * declarations.
 */
static int
peek_type_declarator(struct parser *pr, struct token **args, struct token **end)
{
	struct lexer *lx = pr->pr_lx;
	struct lexer_state s;
	struct token *a = NULL, *e = NULL;
	unsigned int flags = 0;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_peek_if(lx, TOKEN_LPAREN, NULL) &&
	    scan_declarator(pr, lx, &a, &e, &flags)) {
		if ((flags & (SCAN_QUALIFIER | SCAN_SUFFIX)) != 0 ||
		    (flags & SCAN_IDENT) == 0) {
			*args = a;
			*end = e;
			peek = 1;
		}
	}
	lexer_peek_leave(lx, &s);
	return peek;
}

/*
 * Classify the current declarator as a function declarator. On success the
 * type's end is set to the end of the declarator core (including enclosing
 * parentheses) and args to the function's own parameter list, and func_decl is
 * set. Return-type suffixes, e.g. [10] or (void), are left for the caller.
 */
int
parser_type_func_declarator(struct parser *pr, struct parser_type *type)
{
	struct lexer *lx = pr->pr_lx;
	struct lexer_state s;
	struct token *lparen = NULL, *end = NULL;
	int ok = 0;

	lexer_peek_enter(lx, &s);
	if (scan_function_declarator(pr, lx, &lparen, &end)) {
		type->end = end;
		type->args = lparen;
		type->align = NULL;
		type->func_decl = 1;
		ok = 1;
	}
	lexer_peek_leave(lx, &s);
	return ok;
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
	     (!peek_paren_pair_then_lparen(lx) &&
	      lexer_if(lx, TOKEN_LPAREN, NULL) &&
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

/*
 * Returns non-zero if the tokens starting at the first parameter content
 * denote a plausible K&R identifier list or a single parameter declaration.
 */
static int
peek_param_content(struct lexer *lx)
{
	if (!lexer_if(lx, TOKEN_IDENT, NULL))
		return 0;
	if (lexer_if(lx, TOKEN_RPAREN, NULL))
		return 1;	/* single K&R identifier */
	if (lexer_if(lx, TOKEN_COMMA, NULL)) {
		/* Must be a pure comma-separated identifier list. */
		while (lexer_if(lx, TOKEN_IDENT, NULL)) {
			if (lexer_if(lx, TOKEN_RPAREN, NULL))
				return 1;
			if (!lexer_if(lx, TOKEN_COMMA, NULL))
				return 0;
		}
		return 0;
	}
	/*
	 * A typedef-based parameter declaration, e.g. MyType a or MyType *a.
	 * Require a plausible declarator continuation to reject expressions
	 * such as `a + b' that could otherwise be mistaken for a declaration.
	 */
	if (lexer_peek_if(lx, TOKEN_IDENT, NULL) ||
	    lexer_peek_if(lx, TOKEN_STAR, NULL) ||
	    lexer_peek_if(lx, TOKEN_LSQUARE, NULL) ||
	    lexer_peek_if(lx, TOKEN_LPAREN, NULL) ||
	    lexer_peek_if(lx, TOKEN_COLON, NULL))
		return 1;
	return 0;
}

/*
 * Returns non-zero if the next tokens form a plausible function parameter list
 * or K&R identifier list, i.e. ( ... ). The lexer is left untouched.
 */
static int
peek_param_list_valid(struct lexer *lx)
{
	struct lexer_state s, s2;
	struct token *rparen;
	int valid = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, NULL,
	    &rparen)) {
		lexer_peek_enter(lx, &s2);
		(void)lexer_if(lx, TOKEN_LPAREN, NULL);
		valid = lexer_if(lx, TOKEN_RPAREN, NULL) ||
		    (lexer_if(lx, TOKEN_VOID, NULL) &&
		     lexer_if(lx, TOKEN_RPAREN, NULL)) ||
		    lexer_if(lx, TOKEN_ELLIPSIS, NULL) ||
		    lexer_peek_if_flags(lx,
		    TOKEN_FLAG_TYPE | TOKEN_FLAG_QUALIFIER |
		    TOKEN_FLAG_STORAGE, NULL) ||
		    peek_param_content(lx);
		lexer_peek_leave(lx, &s2);
	}
	lexer_peek_leave(lx, &s);
	return valid;
}

/*
 * Returns non-zero if, starting at the current position, a declaration list
 * follows that is terminated by a compound statement. Used to distinguish an
 * old-style function definition from a macro attribute followed by an ordinary
 * declaration, e.g. `f(a) int a; { }' versus `__aligned(x) int y;'. The lexer
 * is left untouched.
 */
int
parser_type_decl_list_then_lbrace(struct parser *pr)
{
	struct lexer *lx = pr->pr_lx;
	struct lexer_state s;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	for (;;) {
		struct token *tk;
		int nest = 0;

		if (!parser_type_peek(pr, NULL, 0))
			break;

		/* Skip to the terminating ';' of this declaration. */
		for (;;) {
			if (!lexer_pop(lx, &tk) ||
			    tk->tk_type == LEXER_EOF)
				goto out;
			if (tk->tk_type == TOKEN_SEMI && nest == 0)
				break;
			if (tk->tk_type == TOKEN_LPAREN ||
			    tk->tk_type == TOKEN_LBRACE ||
			    tk->tk_type == TOKEN_LSQUARE)
				nest++;
			else if (tk->tk_type == TOKEN_RPAREN ||
			    tk->tk_type == TOKEN_RBRACE ||
			    tk->tk_type == TOKEN_RSQUARE)
				nest--;
			if (nest < 0)
				goto out;
		}

		if (lexer_peek_if(lx, TOKEN_LBRACE, NULL)) {
			peek = 1;
			break;
		}
	}
out:
	lexer_peek_leave(lx, &s);
	return peek;
}

/*
 * Classify the current token sequence as an implicit int declaration or
 * definition as standardized by C89/C90. is_func is set when the declarator is
 * a function declarator. Returns one of PARSER_IMPLICIT_*.
 */
static int
classify_implicit_int(struct parser *pr, int *is_func)
{
	struct lexer *lx = pr->pr_lx;
	struct lexer_state s;
	struct token *rparen;
	int kind = PARSER_IMPLICIT_NONE;

	*is_func = 0;
	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_IDENT, NULL)) {
		if (!lexer_peek_if(lx, TOKEN_LPAREN, NULL)) {
			/* Plain variable declarator. */
			kind = PARSER_IMPLICIT_DECL;
		} else if (peek_param_list_valid(lx) &&
		    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, NULL,
		    &rparen)) {
			*is_func = 1;
			if (lexer_if(lx, TOKEN_SEMI, NULL))
				kind = PARSER_IMPLICIT_DECL;
			else if (lexer_if(lx, TOKEN_LBRACE, NULL))
				kind = PARSER_IMPLICIT_IMPL;
			else if (parser_type_decl_list_then_lbrace(pr))
				kind = PARSER_IMPLICIT_IMPL;
		}
	} else if (lexer_peek_if(lx, TOKEN_LPAREN, NULL)) {
		/*
		 * Parenthesized declarator, e.g. the C23 inferred declaration
		 * `auto (*p) = init;' or `static (*p);'.
		 */
		struct token *args = NULL, *end = NULL;
		unsigned int flags = 0;

		if (scan_declarator(pr, lx, &args, &end, &flags))
			kind = PARSER_IMPLICIT_DECL;
	}
	lexer_peek_leave(lx, &s);
	return kind;
}

/*
 * Returns non-zero if the current specifier sequence is followed by an
 * implicit int declarator, either a variable or a function.
 */
int
parser_type_implicit_int(struct parser *pr)
{
	int is_func;

	return classify_implicit_int(pr, &is_func) != PARSER_IMPLICIT_NONE;
}

/*
 * Returns PARSER_IMPLICIT_DECL or PARSER_IMPLICIT_IMPL if the current token
 * sequence is an implicit int function declarator, otherwise
 * PARSER_IMPLICIT_NONE.
 */
int
parser_type_implicit_int_func(struct parser *pr)
{
	int is_func;
	int kind = classify_implicit_int(pr, &is_func);

	if (!is_func)
		return PARSER_IMPLICIT_NONE;
	return kind;
}

/*
 * Returns non-zero if the next tokens form ( ... ) ( , i.e. a parenthesized
 * declarator followed by a parameter list. Used to keep a preceding identifier
 * from being mistaken for the declarator instead of a typedef name, e.g.
 * T (g)(T) or T (*(*g)(T))(T).
 */
static int
peek_paren_pair_then_lparen(struct lexer *lx)
{
	struct lexer_state s;
	struct token *rparen;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, NULL, &rparen) &&
	    lexer_peek_if(lx, TOKEN_LPAREN, NULL))
		peek = 1;
	lexer_peek_leave(lx, &s);
	return peek;
}

/*
 * Returns non-zero if the next tokens form an abstract function declarator,
 * i.e. ( parameter-type-list ), and sets args/end to the parentheses. Only
 * used while parsing type names such as sizeof(int (int)).
 */
static int
peek_type_abstract_func(struct lexer *lx, struct token **args,
    struct token **end)
{
	struct lexer_state s, s2;
	struct token *lparen = NULL, *rparen = NULL;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &lparen,
	    &rparen)) {
		int content = 0;

		lexer_peek_enter(lx, &s2);
		(void)lexer_if(lx, TOKEN_LPAREN, NULL);
		content = lexer_if(lx, TOKEN_RPAREN, NULL) ||
		    lexer_if(lx, TOKEN_VOID, NULL) ||
		    lexer_if(lx, TOKEN_ELLIPSIS, NULL) ||
		    lexer_peek_if_flags(lx,
		    TOKEN_FLAG_TYPE | TOKEN_FLAG_QUALIFIER |
		    TOKEN_FLAG_STORAGE, NULL);
		lexer_peek_leave(lx, &s2);

		if (content &&
		    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &lparen,
		    &rparen)) {
			*args = lparen;
			*end = rparen;
			peek = 1;
		}
	}
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
