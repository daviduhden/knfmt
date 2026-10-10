#include "parser-func.h"

#include "config.h"

#include <ctype.h>

#include "libks/arena.h"

#include "doc.h"
#include "lexer.h"
#include "parser-attributes.h"
#include "parser-decl.h"
#include "parser-expr.h"
#include "parser-priv.h"
#include "parser-stmt.h"
#include "parser-type.h"
#include "ruler.h"
#include "simple-decl-proto.h"
#include "simple.h"
#include "style.h"
#include "token.h"

struct parser_func_proto_arg {
	struct doc		*dc;
	struct ruler		*rl;
	struct parser_type	*type;
	unsigned int		 flags;
#define PARSER_FUNC_PROTO_IMPL		0x00000001u
};

static enum parser_func_peek	parser_func_peek1(struct parser *,
    struct parser_type *);

static int	parser_func_decl1(struct parser *, struct doc *,
    struct ruler *, struct parser_type *);
static int	parser_simple_decl_proto_enter(struct parser *,
    struct parser_type *);
static int	parser_func_impl1(struct parser *, struct doc *,
    struct ruler *, struct parser_type *);
static int	parser_func_proto(struct parser *, struct doc **,
    struct parser_func_proto_arg *);

static int	parser_func_arg_peek(struct parser *, struct parser_type *);

static int	peek_paren_ident(struct lexer *, struct token **);
static int	peek_func_ptr_pattern(struct parser *);
static int	parser_annotation_macro(struct parser *, struct doc *);
static int	parser_annotation_macros(struct parser *, struct doc *);

static int	want_line_after_func_impl(struct parser *);

enum parser_func_peek
parser_func_peek(struct parser *pr)
{
	struct parser_type type = {0};

	return parser_func_peek1(pr, &type);
}

/*
 * Consume a macro-like declaration annotation: an identifier optionally
 * followed by balanced parentheses, e.g. __THROW, __wur or __nonnull((1)).
 * The tokens are preserved opaquely; no semantics are attributed to them.
 * Returns non-zero if an annotation was consumed. When dc is non-nullptr the
 * tokens are appended to it.
 */
static int
parser_annotation_macro(struct parser *pr, struct doc *dc)
{
	struct lexer *lx = pr->pr_lx;
	struct token *lparen, *rparen, *tk;

	if (!lexer_if(lx, TOKEN_IDENT, &tk))
		return 0;
	if (dc != nullptr)
		parser_doc_token(pr, tk, dc);
	if (!lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &lparen,
	    &rparen))
		return 1;
	if (!lexer_if(lx, TOKEN_LPAREN, &lparen))
		return 1;
	if (dc != nullptr)
		parser_doc_token(pr, lparen, dc);
	for (;;) {
		if (!lexer_pop(lx, &tk))
			break;
		if (dc != nullptr)
			parser_doc_token(pr, tk, dc);
		if (tk == rparen)
			break;
	}
	return 1;
}

/*
 * Consume a run of annotation macros, separated by a single space when
 * rendered. Returns the number of annotations consumed.
 */
static int
parser_annotation_macros(struct parser *pr, struct doc *dc)
{
	int nattributes = 0;

	for (;;) {
		if (dc != nullptr)
			doc_alloc(DOC_LINE, dc);
		if (!parser_annotation_macro(pr, dc)) {
			if (dc != nullptr)
				(void)doc_remove_tail(dc);
			break;
		}
		nattributes++;
	}
	return nattributes;
}

/*
 * Returns non-zero if the next tokens form a parenthesized declarator, i.e.
 * ( identifier ). rparen is set to the closing parenthesis.
 */
static int
peek_paren_ident(struct lexer *lx, struct token **rparen)
{
	struct lexer_state s = {0};
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_LPAREN, nullptr) &&
	    lexer_if(lx, TOKEN_IDENT, nullptr) &&
	    lexer_if(lx, TOKEN_RPAREN, rparen))
		peek = 1;
	lexer_peek_leave(lx, &s);
	return peek;
}

/*
 * Returns non-zero if the current tokens form the pre-existing function
 * returning function pointer pattern, i.e. ( * identifier ( args ) ) ( args ).
 * Such declarators get dedicated alignment and line breaking; other
 * parenthesized function declarators are handled by
 * parser_type_func_declarator().
 */
static int
peek_func_ptr_pattern(struct parser *pr)
{
	struct lexer_state s = {0};
	struct lexer *lx = pr->pr_lx;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	peek = lexer_if(lx, TOKEN_LPAREN, nullptr) &&
	    lexer_if(lx, TOKEN_STAR, nullptr) &&
	    lexer_if(lx, TOKEN_IDENT, nullptr) &&
	    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, nullptr, nullptr) &&
	    lexer_if(lx, TOKEN_RPAREN, nullptr) &&
	    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, nullptr, nullptr);
	lexer_peek_leave(lx, &s);
	return peek;
}

static enum parser_func_peek
parser_func_peek1(struct parser *pr, struct parser_type *type)
{
	struct lexer_state s = {0};
	struct lexer *lx = pr->pr_lx;
	struct token *attr = nullptr;
	enum parser_func_peek peek = PARSER_FUNC_PEEK_NONE;

	lexer_peek_enter(lx, &s);
	if (parser_attributes_peek(pr, &attr, PARSER_ATTRIBUTES_FUNC) &&
	    !lexer_seek_after(lx, attr))
		goto out;

	if (parser_type_peek(pr, type, 0) &&
	    lexer_seek_after(lx, type->end)) {
		if (parser_attributes_peek(pr, &attr, PARSER_ATTRIBUTES_FUNC) &&
		    !lexer_seek_after(lx, attr))
			goto out;

		if (lexer_if(lx, TOKEN_IDENT, nullptr)) {
			/* nothing */
		} else if (peek_paren_ident(lx, &attr)) {
			/*
			 * Parenthesized declarator: type ( ident ) ( args ).
			 * Used by parser_func_proto().
			 */
			if (!lexer_seek_after(lx, attr))
				goto out;
			type->end->tk_flags |= TOKEN_FLAG_TYPE_PAREN;
		} else if (peek_func_ptr_pattern(pr)) {
			/*
			 * Function returning a function pointer, used by
			 * parser_func_proto().
			 */
			(void)lexer_if(lx, TOKEN_LPAREN, nullptr);
			(void)lexer_if(lx, TOKEN_STAR, nullptr);
			(void)lexer_if(lx, TOKEN_IDENT, nullptr);
			(void)lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN,
			    nullptr, nullptr);
			(void)lexer_if(lx, TOKEN_RPAREN, nullptr);
			type->end->tk_flags |= TOKEN_FLAG_TYPE_FUNC;
		} else if (lexer_peek_if(lx, TOKEN_LPAREN, nullptr) &&
		    parser_type_func_declarator(pr, type)) {
			/*
			 * Function whose return type is a parenthesized
			 * declarator, e.g. int (*const f(void))[10]. The
			 * declarator core is rendered by parser_type(); the
			 * remaining array or function suffixes describe the
			 * return type.
			 */
			if (!lexer_seek_after(lx, type->end))
				goto out;
			for (;;) {
				struct token *lparen, *rparen;

				if (lexer_if_pair(lx, TOKEN_LPAREN,
				    TOKEN_RPAREN, &lparen, &rparen))
					continue;
				if (lexer_if_pair(lx, TOKEN_LSQUARE,
				    TOKEN_RSQUARE, &lparen, &rparen))
					continue;
				break;
			}
		} else {
			goto out;
		}

		if (!type->func_decl &&
		    !lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, nullptr, nullptr))
			goto out;

		if (parser_attributes_peek(pr, &attr, 0) &&
		    !lexer_seek_after(lx, attr))
			goto out;
		if (parser_attributes_std_peek(pr, &attr) &&
		    !lexer_seek_after(lx, attr))
			goto out;

		if (lexer_if(lx, TOKEN_SEMI, nullptr))
			peek = PARSER_FUNC_PEEK_DECL;
		else if (lexer_if(lx, TOKEN_LBRACE, nullptr))
			peek = PARSER_FUNC_PEEK_IMPL;
		else if (parser_type_decl_list_then_lbrace(pr))
			peek = PARSER_FUNC_PEEK_IMPL;	/* K&R */
		else if (parser_annotation_macros(pr, nullptr) > 0) {
			/*
			 * Trailing annotation macros such as __THROW, __wur or
			 * __nonnull((1)).
			 */
			if (lexer_if(lx, TOKEN_SEMI, nullptr))
				peek = PARSER_FUNC_PEEK_DECL;
			else if (lexer_if(lx, TOKEN_LBRACE, nullptr))
				peek = PARSER_FUNC_PEEK_IMPL;
			else if (parser_type_decl_list_then_lbrace(pr))
				peek = PARSER_FUNC_PEEK_IMPL;
		}
	} else {
		/*
		 * Implicit int function declaration or definition as
		 * standardized by C89/C90, e.g. main() { } or f(a) int a; { }.
		 * The type is left empty; the source spelling is preserved.
		 */
		int kind = parser_type_implicit_int_func(pr);

		if (kind == PARSER_IMPLICIT_NONE)
			goto out;
		/*
		 * The type is left empty; clearing the whole struct also resets
		 * func_decl, which is otherwise read uninitialized by
		 * parser_func_proto().
		 */
		*type = (struct parser_type){0};
		peek = kind == PARSER_IMPLICIT_DECL ?
		    PARSER_FUNC_PEEK_DECL : PARSER_FUNC_PEEK_IMPL;
	}
out:
	lexer_peek_leave(lx, &s);
	return peek;
}

int
parser_func_decl(struct parser *pr, struct doc *dc, struct ruler *rl)
{
	struct parser_type type = {0};
	int error = 0;

	if (parser_func_peek1(pr, &type) != PARSER_FUNC_PEEK_DECL)
		return parser_none(pr);

	error = parser_simple_decl_proto_enter(pr, &type);
	if (error & HALT)
		return error;
	error = parser_func_decl1(pr, dc, rl, &type);
	return error;
}

static int
parser_func_decl1(struct parser *pr, struct doc *dc, struct ruler *rl,
    struct parser_type *type)
{
	struct lexer *lx = pr->pr_lx;
	struct doc *out = nullptr;
	struct token *tk = nullptr;
	int error = 0;

	error = parser_func_proto(pr, &out, &(struct parser_func_proto_arg){
	    .dc		= doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc)),
	    .rl		= rl,
	    .type	= type,
	});
	if (error & HALT)
		return parser_fail(pr);

	if (lexer_expect(lx, TOKEN_SEMI, &tk))
		parser_doc_token(pr, tk, out);

	return parser_good(pr);
}

static int
parser_simple_decl_proto_enter(struct parser *pr, struct parser_type *type)
{
	struct lexer_state s = {0};
	struct lexer *lx = pr->pr_lx;
	struct doc *dc = nullptr;
	int error = 0;

	simple_cookie(simple);
	if (!simple_enter(pr->pr_si, SIMPLE_DECL_PROTO, 0, &simple))
		return parser_good(pr);

	arena_scope(pr->pr_arena.scratch, scratch_scope);

	arena_scope(pr->pr_arena.doc, doc_scope);
	parser_arena_scope(&pr->pr_arena_scope.doc, &doc_scope, cookie);

	pr->pr_simple.decl_proto = simple_decl_proto_enter(pr->pr_lx,
	    &scratch_scope);
	dc = doc_root(&doc_scope);
	lexer_peek_enter(lx, &s);
	error = parser_func_decl1(pr, dc, nullptr, type);
	lexer_peek_leave(lx, &s);
	if (error & GOOD)
		simple_decl_proto_leave(pr->pr_simple.decl_proto);
	simple_decl_proto_free(pr->pr_simple.decl_proto);
	pr->pr_simple.decl_proto = nullptr;
	return parser_good(pr);
}

int
parser_func_impl(struct parser *pr, struct doc *dc)
{
	struct ruler rl = {0};
	struct parser_type type;
	int error = 0;

	if (parser_func_peek1(pr, &type) != PARSER_FUNC_PEEK_IMPL)
		return parser_none(pr);

	arena_scope(pr->pr_arena.ruler, ruler_scope);

	ruler_init(&rl, 1, RULER_ALIGN_FIXED, &ruler_scope);
	error = parser_func_impl1(pr, dc, &rl, &type);
	if (error & GOOD)
		ruler_exec(&rl);
	return error;
}

int
parser_func_arg(struct parser *pr, struct doc *dc, struct doc **out,
    const struct token *rparen)
{
	struct parser_type type = {0};
	struct doc *attr, *concat;
	struct lexer *lx = pr->pr_lx;
	struct token *pv = nullptr;
	struct token *tk = nullptr;

	if (!parser_func_arg_peek(pr, &type))
		return parser_none(pr);

	if (is_simple_enabled(pr->pr_si, SIMPLE_DECL_PROTO))
		simple_decl_proto_arg(pr->pr_simple.decl_proto);

	/*
	 * Let each argument begin with a soft line, causing a line to be
	 * emitted immediately if the argument does not fit instead of breaking
	 * the argument.
	 */
	concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));
	doc_alloc(DOC_SOFTLINE, concat);
	concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_OPTIONAL, concat));

	if (parser_attributes(pr, concat, &attr, 0) & GOOD)
		doc_alloc(DOC_LINE, attr);
	if (parser_type(pr, concat, &type, nullptr) & HALT)
		return parser_fail(pr);

	/* Put the argument identifier in its own group to trigger a refit. */
	concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, concat));
	if (out != nullptr)
		*out = concat;

	/* Put a line between the type and identifier when wanted. */
	if (type.end->tk_type != TOKEN_STAR &&
	    !lexer_peek_if(lx, TOKEN_COMMA, nullptr) &&
	    !lexer_peek_if(lx, TOKEN_RPAREN, nullptr) &&
	    !lexer_peek_if(lx, TOKEN_ATTRIBUTE, nullptr))
		doc_alloc(DOC_LINE, concat);

	for (;;) {
		if (lexer_peek_if(lx, LEXER_EOF, nullptr))
			return parser_fail(pr);

		if (parser_attributes(pr, concat, nullptr,
		    PARSER_ATTRIBUTES_LINE) & FAIL)
			return parser_fail(pr);

		if (parser_attributes_std_peek(pr, nullptr)) {
			if (parser_attributes_std(pr, concat) & FAIL)
				return parser_fail(pr);
			doc_alloc(DOC_LINE, concat);
		}

		if (lexer_if(lx, TOKEN_COMMA, &tk)) {
			parser_doc_token(pr, tk, concat);
			doc_alloc(DOC_LINE, concat);
			break;
		}
		if (lexer_peek(lx, &tk) && tk == rparen)
			break;

		if (!lexer_pop(lx, &tk))
			return parser_fail(pr);
		/* Ensure tokens that would otherwise merge stay separated. */
		if (token_pair_needs_space(pv, tk))
			doc_alloc(DOC_LINE, concat);
		parser_doc_token(pr, tk, concat);
		pv = tk;
		if (tk->tk_type == TOKEN_IDENT &&
		    is_simple_enabled(pr->pr_si, SIMPLE_DECL_PROTO)) {
			simple_decl_proto_arg_ident(pr->pr_simple.decl_proto,
			    tk);
		}
	}

	return parser_good(pr);
}

static int
parser_func_impl1(struct parser *pr, struct doc *dc, struct ruler *rl,
    struct parser_type *type)
{
	struct lexer *lx = pr->pr_lx;
	struct doc *out = nullptr;
	int error = 0;

	error = parser_func_proto(pr, &out, &(struct parser_func_proto_arg){
	    .dc		= dc,
	    .rl		= rl,
	    .type	= type,
	    .flags	= PARSER_FUNC_PROTO_IMPL,
	});
	if (error & (FAIL | NONE))
		return parser_fail(pr);
	if (!lexer_peek_if(lx, TOKEN_LBRACE, nullptr))
		return parser_fail(pr);

	if (style_brace_wrapping(pr->pr_st, AfterFunction))
		doc_alloc(DOC_HARDLINE, dc);
	else
		doc_literal(" ", dc);
	error = parser_stmt_block(pr, &(struct parser_stmt_block_arg){
	    .head	= dc,
	    .tail	= dc,
	});
	if (error & (FAIL | NONE))
		return parser_fail(pr);
	doc_alloc(DOC_HARDLINE, dc);
	if (want_line_after_func_impl(pr))
		doc_alloc(DOC_HARDLINE, dc);

	return parser_good(pr);
}

static int
has_many_args(struct parser *pr, struct token *rparen)
{
	struct lexer *lx = pr->pr_lx;
	return lexer_peek_until_comma(lx, rparen, nullptr);
}

/*
 * Parse a function prototype, i.e. return type, identifier, arguments and
 * optional attributes. The caller is expected to already have parsed the
 * return type.
 */
static int
parser_func_proto(struct parser *pr, struct doc **out,
    struct parser_func_proto_arg *arg)
{
	struct doc *dc = arg->dc;
	struct doc *attr, *concat, *group, *indent, *kr;
	struct lexer *lx = pr->pr_lx;
	struct parser_type *type = arg->type;
	struct token *lparen, *rparen, *tk;
	unsigned int s, w;
	int nkr = 0;
	int error = 0;

	error = parser_attributes(pr, dc, &attr, PARSER_ATTRIBUTES_FUNC);
	if (error & FAIL)
		return parser_fail(pr);
	if (error & GOOD)
		doc_alloc(DOC_LINE, attr);

	if (type->end != nullptr &&
	    parser_type(pr, dc, type,
	    type->func_decl ? nullptr : arg->rl) & (FAIL | NONE))
		return parser_fail(pr);

	error = parser_attributes(pr, dc, nullptr, PARSER_ATTRIBUTES_FUNC);
	if (error & FAIL)
		return parser_fail(pr);
	if ((error & GOOD) && (arg->flags & PARSER_FUNC_PROTO_IMPL) == 0)
		doc_alloc(DOC_LINE, dc);

	if (type->func_decl) {
		/*
		 * The whole declarator was rendered by parser_type(). Render
		 * the return type suffixes, i.e. array or function, which
		 * follow it.
		 */
		for (;;) {
			struct token *l, *r;

			if (lexer_peek_if_pair(lx, TOKEN_LSQUARE, TOKEN_RSQUARE,
			    &l, &r)) {
				struct doc *expr = nullptr;

				if (lexer_expect(lx, TOKEN_LSQUARE, &l))
					parser_doc_token(pr, l, dc);
				if (!lexer_peek_if(lx, TOKEN_RSQUARE, nullptr))
					parser_expr(pr, &expr,
					    &(struct parser_expr_arg){
						.dc = dc,
					});
				if (lexer_expect(lx, TOKEN_RSQUARE, &r))
					parser_doc_token(pr, r, dc);
				continue;
			}
			if (lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN,
			    &l, &r)) {
				if (lexer_expect(lx, TOKEN_LPAREN, &l))
					parser_doc_token(pr, l, dc);
				while (parser_func_arg(pr, dc, nullptr, r) & GOOD)
					continue;
				if (lexer_expect(lx, TOKEN_RPAREN, &r))
					parser_doc_token(pr, r, dc);
				continue;
			}
			break;
		}
		error = parser_attributes(pr, dc, nullptr, 0);
		if (error & FAIL)
			return parser_fail(pr);
		if (error & GOOD)
			doc_alloc(DOC_LINE, dc);
		*out = dc;
		return parser_good(pr);
	}

	s = style(pr->pr_st, AlwaysBreakAfterReturnType);
	if (type->end != nullptr &&
	    ((s == All || s == TopLevel) ||
	     ((arg->flags & PARSER_FUNC_PROTO_IMPL) &&
	      (s == AllDefinitions || s == TopLevelDefinitions))))
		doc_alloc(DOC_HARDLINE, dc);

	/*
	 * The function identifier and arguments are intended to fit on a single
	 * line.
	 */
	group = doc_alloc(DOC_GROUP, dc);
	concat = doc_alloc(DOC_CONCAT, group);

	if (type->end != nullptr && (type->end->tk_flags & TOKEN_FLAG_TYPE_FUNC)) {
		/* Function returning function pointer. */
		if (lexer_expect(lx, TOKEN_LPAREN, &lparen))
			parser_doc_token(pr, lparen, concat);
		if (lexer_expect(lx, TOKEN_STAR, &tk))
			parser_doc_token(pr, tk, concat);
		if (lexer_expect(lx, TOKEN_IDENT, &tk))
			parser_doc_token(pr, tk, concat);
		if (!lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN,
		    &lparen, &rparen))
			return parser_fail(pr);
		if (lexer_expect(lx, TOKEN_LPAREN, nullptr))
			parser_doc_token(pr, lparen, concat);
		while (parser_func_arg(pr, concat, nullptr, rparen) & GOOD)
			continue;
		if (lexer_expect(lx, TOKEN_RPAREN, &rparen))
			parser_doc_token(pr, rparen, concat);
		if (lexer_expect(lx, TOKEN_RPAREN, &rparen))
			parser_doc_token(pr, rparen, concat);
	} else if (type->end != nullptr &&
	    (type->end->tk_flags & TOKEN_FLAG_TYPE_PAREN)) {
		/* Parenthesized declarator: ( ident ). */
		if (lexer_expect(lx, TOKEN_LPAREN, &lparen))
			parser_doc_token(pr, lparen, concat);
		if (lexer_expect(lx, TOKEN_IDENT, &tk))
			parser_doc_token(pr, tk, concat);
		if (lexer_expect(lx, TOKEN_RPAREN, &rparen))
			parser_doc_token(pr, rparen, concat);
	} else if (lexer_expect(lx, TOKEN_IDENT, &tk)) {
		parser_token_trim_after(pr, tk);
		parser_doc_token(pr, tk, concat);
	} else {
		doc_remove(group, dc);
		return parser_fail(pr);
	}

	if (!lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &lparen,
	    &rparen))
		return parser_fail(pr);
	int lparen_has_line = token_has_line(lparen, 1);
	if (lexer_expect(lx, TOKEN_LPAREN, nullptr)) {
		parser_token_trim_after(pr, lparen);
		parser_token_trim_before(pr, rparen);
		parser_token_trim_after(pr, rparen);
		parser_doc_token(pr, lparen, concat);
	}
	w = style(pr->pr_st, ContinuationIndentWidth);
	if (style(pr->pr_st, AlignAfterOpenBracket) == Align) {
		const struct doc_minimize minimizers[2] = {
			{
				.type	= DOC_MINIMIZE_INDENT,
				.indent	= DOC_INDENT_WIDTH,
			},
			{
				.type	= DOC_MINIMIZE_INDENT,
				.indent	= w,
			},
		};

		indent = doc_minimize(minimizers, dc);
	} else {
		indent = doc_indent(w, concat);
	}
	if (lparen_has_line && has_many_args(pr, rparen)) {
		/* Must be emitted here to get indentation right. */
		doc_alloc(DOC_HARDLINE, indent);
	}
	while (parser_func_arg(pr, indent, out, rparen) & GOOD)
		continue;
	/* Can be empty if arguments are absent. */
	if (*out == nullptr)
		*out = concat;
	if (lexer_expect(lx, TOKEN_RPAREN, &rparen))
		parser_doc_token(pr, rparen, *out);

	/* C23 standard attributes trailing the declarator. Must be handled
	 * before the K&R declaration list as [[...]] ; would otherwise be
	 * parsed as an attribute declaration. */
	if (parser_attributes_std_peek(pr, nullptr)) {
		doc_alloc(DOC_LINE, *out);
		if (parser_attributes_std(pr, *out) & HALT)
			return parser_fail(pr);
	}

	/*
	 * Recognize K&R argument declarations. Only definitions can carry
	 * them; a declaration ends with attributes and annotations instead.
	 */
	if (arg->flags & PARSER_FUNC_PROTO_IMPL) {
		kr = doc_alloc(DOC_GROUP, dc);
		indent = doc_indent(style(pr->pr_st, IndentWidth), kr);
		doc_alloc(DOC_HARDLINE, indent);
		if (parser_decl(pr, indent, 0) & GOOD)
			nkr++;
		if (nkr == 0)
			doc_remove(kr, dc);
	}
	if (nkr == 0) {
		/*
		 * Trailing annotation macros (e.g. __THROW, __wur,
		 * __nonnull((1))) and attributes, possibly interleaved.
		 * Annotations are macro-like declaration modifiers and are
		 * preserved opaquely without attributing semantics to them.
		 */
		for (;;) {
			int progressed = 0;

			if (parser_annotation_macros(pr, *out) > 0)
				progressed = 1;
			attr = doc_alloc(DOC_GROUP, dc);
			indent = doc_indent(style(pr->pr_st, IndentWidth),
			    attr);
			if (parser_attributes(pr, indent, out,
			    PARSER_ATTRIBUTES_LINE) & GOOD) {
				progressed = 1;
			} else {
				doc_remove(attr, dc);
			}
			if (parser_attributes_std_peek(pr, nullptr)) {
				doc_alloc(DOC_LINE, *out);
				if (parser_attributes_std(pr, *out) & HALT)
					return parser_fail(pr);
				progressed = 1;
			}
			if (!progressed)
				break;
		}
	}

	return parser_good(pr);
}

static int
parser_func_arg_peek(struct parser *pr, struct parser_type *type)
{
	struct lexer_state s = {0};
	struct lexer *lx = pr->pr_lx;
	struct token *attr = nullptr;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	peek = (!parser_attributes_peek(pr, &attr, 0) ||
	    lexer_seek_after(lx, attr)) &&
	    parser_type_peek(pr, type, PARSER_TYPE_ARG);
	lexer_peek_leave(lx, &s);
	return peek;
}

/*
 * Returns non-zero if the right brace of a function implementation can be
 * followed by a hard line.
 */
static int
want_line_after_func_impl(struct parser *pr)
{
	struct lexer *lx = pr->pr_lx;
	struct token *cpp, *ident, *rbrace, *rparen;
	struct lexer_state s = {0};
	int annotated = 0;

	if (lexer_peek_if(lx, LEXER_EOF, nullptr) ||
	    !lexer_back_if(lx, TOKEN_RBRACE, &rbrace))
		return 0;

	if (lexer_peek_if_prefix_flags(lx, TOKEN_FLAG_CPP, &cpp))
		return cpp->tk_lno - rbrace->tk_lno > 1;

	lexer_peek_enter(lx, &s);
	if ((lexer_if(lx, TOKEN_IDENT, &ident) ||
	    lexer_if(lx, TOKEN_ASSEMBLY, &ident)) &&
	    lexer_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, nullptr, &rparen)) {
		struct token *nx = nullptr;

		if (lexer_if(lx, TOKEN_SEMI, nullptr) &&
		    ident->tk_lno - rbrace->tk_lno == 1)
			annotated = 1;
		else if (lexer_pop(lx, &nx) && nx->tk_lno - rparen->tk_lno > 1)
			annotated = 1;
		else if (lexer_if(lx, LEXER_EOF, nullptr))
			annotated = 1;
	}
	lexer_peek_leave(lx, &s);
	return !annotated;
}
