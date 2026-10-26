#include "clang.h"
#include "config.h"
#include "doc.h"
#include "expr.h"
#include "lexer.h"
#include "libks/arena.h"
#include "parser-attributes.h"
#include "parser-braces.h"
#include "parser-cpp.h"
#include "parser-decl.h"
#include "parser-expr.h"
#include "parser-func.h"
#include "parser-priv.h"
#include "parser-stmt-asm.h"
#include "parser-type.h"
#include "ruler.h"
#include "simple-decl-forward.h"
#include "simple-decl.h"
#include "simple.h"
#include "style.h"
#include "token.h"

/*
 * Declaration parsing. A declaration is classified by parser_type_peek() before
 * any token is consumed, then emitted by parser_decl2(). The scanner handles
 * multiple declarators, initializers, bitfields, struct/union/enum bodies and
 * C11/C23 static assertions. Declaration alignment is two-phase: a ruler is
 * filled while the declarator list is emitted and executed once the whole block
 * has been recognized, so every line in the block can be aligned together.
 *
 * Function declarations and definitions are delegated to parser-func.c; only
 * the ambiguous decisions (implicit int, K&R forms, preprocessor constructs)
 * live here.
 */
struct parser_decl_init_arg {
	struct doc   *dc;
	struct ruler *rl;
	struct token *semi;
	unsigned int  indent;
	unsigned int  flags;
/* Insert space before equal operator. */
#define PARSER_DECL_INIT_SPACE_BEFORE_EQUAL 0x00000001u
};

static int parser_decl1(struct parser *, struct doc *, unsigned int);
static int parser_decl2(
    struct parser *, struct doc *, struct ruler *, unsigned int);
static int parser_decl_init(
    struct parser *, struct doc **, struct parser_decl_init_arg *);
static int parser_decl_init1(struct parser *, struct doc *, struct doc **);
static int parser_decl_init_assign(struct parser *, struct doc *, struct doc **,
    struct parser_decl_init_arg *);
static int parser_decl_bitfield(struct parser *, struct doc *);
static int parser_decl_braces(struct parser *, struct doc *, int);
static int parser_static_assert(struct parser *, struct doc *);

static int parser_simple_decl_enter(
    struct parser *, unsigned int, struct simple_cookie *);

static int parser_simple_decl_forward_enter(
    struct parser *, unsigned int, struct simple_cookie *);

/*
 * Check whether a declaration starts at the cursor without consuming input or
 * committing documents. Simplification is disabled and all documents are built
 * in a scratch scope, which is discarded when the peek ends.
 */
int
parser_decl_peek(struct parser *pr)
{
	struct lexer_state s;
	struct lexer	  *lx = pr->pr_lx;
	struct doc	  *dc;
	int		   error, simple;

	arena_scope(pr->pr_arena.doc, doc_scope);
	parser_arena_scope(&pr->pr_arena_scope.doc, &doc_scope, cookie);

	dc = doc_root(&doc_scope);
	lexer_peek_enter(lx, &s);
	simple = simple_disable(pr->pr_si);
	error = parser_decl(pr, dc, 0);
	simple_enable(pr->pr_si, simple);
	lexer_peek_leave(lx, &s);
	return error & GOOD;
}

/*
 * Parse a declaration, invoking the -s declaration passes around a speculative
 * parse when they are enabled. The pass-specific wrapper reruns parser_decl1()
 * with simplification on, after which the pass has recorded its token edits and
 * the normal parse is committed.
 */
int
parser_decl(struct parser *pr, struct doc *dc, unsigned int flags)
{
	int error;

	simple_cookie(simple_decl_cookie);
	error = parser_simple_decl_enter(pr, flags, &simple_decl_cookie);
	if (error & HALT)
		return error;

	simple_cookie(simple_decl_forward_cookie);
	error = parser_simple_decl_forward_enter(
	    pr, flags, &simple_decl_forward_cookie);
	if (error & HALT)
		return error;

	return parser_decl1(pr, dc, flags);
}

/*
 * Parse a run of declarations at the current nesting level. Each declaration
 * after the first is preceded by a hard line; with PARSER_DECL_BREAK, a blank
 * line or a following preprocessor directive ends the run. A failed declaration removes the line
 * that was just emitted. Alignment is applied only if at least one declaration
 * was recognized and no failure occurred.
 */
static int
parser_decl1(struct parser *pr, struct doc *dc, unsigned int flags)
{
	struct ruler  rl;
	struct doc   *line = nullptr;
	struct doc   *decl;
	struct lexer *lx = pr->pr_lx;
	int	      ndecl = 0;
	int	      error;

	if (!parser_depth_enter(pr))
		return parser_fail(pr);
	parser_depth_guard(pr);

	arena_scope(pr->pr_arena.ruler, ruler_scope);

	ruler_init(&rl, 0, RULER_ALIGN_SENSE, &ruler_scope);
	decl = doc_alloc(DOC_CONCAT, dc);

	for (;;) {
		struct token *tk;

		if (ndecl > 0)
			line = doc_alloc(DOC_HARDLINE, decl);
		error = parser_decl2(pr, decl, &rl, flags);
		if (error & (FAIL | NONE)) {
			if (line != nullptr)
				doc_remove(line, decl);
			break;
		}
		ndecl++;
		if (error & BRCH)
			break;

		if (flags & PARSER_DECL_BREAK) {
			/*
			 * Honor empty line(s) which denotes the end of this
			 * block of declarations.
			 */
			if (lexer_back(lx, &tk) && token_has_line(tk, 2))
				break;

			/*
			 * Any cpp directive also denotes the end of this block
			 * of declarations.
			 */
			if (lexer_peek_if_prefix_flags(
				lx, TOKEN_FLAG_CPP, nullptr))
				break;
		}
	}
	if (ndecl == 0)
		doc_remove(decl, dc);
	else if ((error & FAIL) == 0)
		ruler_exec(&rl);

	if (ndecl == 0)
		return parser_none(pr);

	if (flags & PARSER_DECL_LINE)
		doc_alloc(DOC_HARDLINE, decl);
	return parser_good(pr);
}

/*
 * Recognize and emit one declaration. The alternatives are tried in order:
 * cdefs macros, root-level cpp declarations, static assertions, standalone
 * attributes, then a type specifier followed by declarators. A trailing
 * identifier without a type is treated as an implicit-int function declaration
 * at file scope, or left to a cpp construct otherwise. The terminating
 * semicolon is located before the initializer is parsed so the expression parser
 * can stop there.
 */
static int
parser_decl2(
    struct parser *pr, struct doc *dc, struct ruler *rl, unsigned int flags)
{
	struct parser_type type = {0};
	struct lexer	  *lx = pr->pr_lx;
	struct doc	  *out = nullptr;
	struct doc	  *concat;
	struct token	  *beg, *end, *semi, *attr_end;
	int		   iscpp = 0;
	int		   error;

	if (parser_cpp_cdefs(pr, dc) & GOOD)
		return parser_good(pr);
	if ((flags & PARSER_DECL_ROOT) && (parser_cpp_decl_root(pr, dc) & GOOD))
		return parser_good(pr);
	if (parser_static_assert(pr, dc) & GOOD)
		return parser_good(pr);
	/* C23 attribute declaration, i.e. [[ ... ]] ; */
	if (parser_attributes_std_peek(pr, &attr_end) &&
	    token_next(attr_end) != nullptr &&
	    token_next(attr_end)->tk_type == TOKEN_SEMI) {
		concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));
		if (parser_attributes_std(pr, concat) & FAIL)
			return parser_fail(pr);
		return parser_semi(pr, concat);
	}
	if (!parser_type_peek(pr, &type, 0)) {
		/*
		 * No declaration specifiers. At file scope, try an implicit
		 * int function declaration or definition as standardized by
		 * C89/C90 before falling back to preprocessor constructs.
		 * Block scope is intentionally excluded to avoid mistaking a
		 * function call for a K&R identifier list.
		 */
		if (flags & PARSER_DECL_ROOT) {
			switch (parser_func_peek(pr)) {
			case PARSER_FUNC_PEEK_DECL:
				return parser_func_decl(pr, dc, rl);
			case PARSER_FUNC_PEEK_IMPL:
				return parser_none(pr);
			case PARSER_FUNC_PEEK_NONE:
				break;
			}
		}
		iscpp = parser_cpp_peek_decl(pr, &type,
		    (flags & PARSER_DECL_ROOT) ? PARSER_CPP_DECL_ROOT : 0);
		if (!iscpp)
			return parser_cpp_x(pr, dc);
	}

	/*
	 * Presence of a type does not necessarily imply that this a declaration
	 * since it could be a function declaration or implementation.
	 */
	switch (parser_func_peek(pr)) {
	case PARSER_FUNC_PEEK_NONE:
		break;
	case PARSER_FUNC_PEEK_DECL:
		return parser_func_decl(pr, dc, rl);
	case PARSER_FUNC_PEEK_IMPL:
		return parser_none(pr);
	}

	concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));

	if (!lexer_peek(lx, &beg))
		return parser_fail(pr);
	end = type.end;

	if (is_simple_enabled(pr->pr_si, SIMPLE_DECL))
		simple_decl_type(pr->pr_simple.decl, beg, end);
	if (parser_type(pr, concat, &type, rl) & (FAIL | NONE))
		return parser_fail(pr);

	/* Presence of semicolon implies that this declaration is done. */
	if (lexer_peek_if(lx, TOKEN_SEMI, nullptr))
		goto out;

	if (token_is_decl(end, TOKEN_STRUCT)) {
		error = parser_decl_braces(
		    pr, concat, style_brace_wrapping(pr->pr_st, AfterStruct));
		if (error & FAIL)
			return parser_fail(pr);
	} else if (token_is_decl(end, TOKEN_UNION)) {
		error = parser_decl_braces(
		    pr, concat, style_brace_wrapping(pr->pr_st, AfterUnion));
		if (error & FAIL)
			return parser_fail(pr);
	} else if (token_is_decl(end, TOKEN_ENUM)) {
		struct token *lbrace, *rbrace;
		unsigned int  w;

		if (!lexer_peek_if_pair(
			lx, TOKEN_LBRACE, TOKEN_RBRACE, &lbrace, &rbrace))
			return parser_fail(pr);
		parser_token_trim_before(pr, rbrace);
		if (style_brace_wrapping(pr->pr_st, AfterEnum))
			doc_alloc(DOC_HARDLINE, concat);

		w = style(pr->pr_st, IndentWidth);
		error = parser_braces(pr, concat, concat, w,
		    PARSER_BRACES_ENUM | PARSER_BRACES_TRIM);
		if (error & HALT)
			return parser_fail(pr);
	}

	if (!lexer_peek_until(lx, TOKEN_SEMI, &semi))
		return parser_fail(pr);

	struct parser_decl_init_arg arg = {
	    .dc = concat,
	    .rl = rl,
	    .semi = semi,
	    .indent = style(pr->pr_st, IndentWidth),
	    .flags = iscpp && lexer_peek_if(lx, TOKEN_EQUAL, nullptr) ?
		0 :
		PARSER_DECL_INIT_SPACE_BEFORE_EQUAL,
	};
	error = parser_decl_init(pr, &out, &arg);
	if (error & (FAIL | NONE))
		return parser_fail(pr);
	if (out != nullptr)
		concat = out;

out:
	if (lexer_expect(lx, TOKEN_SEMI, &semi)) {
		if (flags & PARSER_DECL_TRIM_SEMI) {
			struct token *nx;

			while (lexer_if(lx, TOKEN_SEMI, &nx))
				lexer_remove(lx, nx);
		}

		parser_doc_token(pr, semi, concat);

		if (is_simple_enabled(pr->pr_si, SIMPLE_DECL))
			simple_decl_semi(pr->pr_simple.decl, semi);

		if (is_simple_enabled(pr->pr_si, SIMPLE_DECL_FORWARD)) {
			simple_decl_forward(
			    pr->pr_simple.decl_forward, beg, semi);
		}
	}

	return parser_good(pr);
}

/*
 * Parse any initialization as part of a declaration, i.e. the comma-separated
 * declarators before the terminating semicolon. A ruler indent makes wrapped
 * declarators line up under the first one. Commas are emitted with a break
 * opportunity, and malformed adjacent declarators keep a separating space when
 * dropping it would merge their boundary tokens.
 *
 * If nothing was parsed, the ruler indent and the whole initializer document are
 * removed. A consumed separator, however, is kept so a dangling comma in
 * `int a,;` survives.
 */
static int
parser_decl_init(
    struct parser *pr, struct doc **out, struct parser_decl_init_arg *arg)
{
	struct doc	    *concat, *dc, *indent;
	struct lexer	    *lx = pr->pr_lx;
	struct ruler_indent *cookie = nullptr;
	int		     ninit = 0;
	int		     nconsumed = 0;

	indent = ruler_indent(arg->rl, arg->dc, &cookie);
	dc = doc_alloc(DOC_CONCAT, indent);
	concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));

	for (;;) {
		struct token *comma, *semi;
		int	      error;

		if (lexer_peek_if(lx, TOKEN_SEMI, &semi) && semi == arg->semi)
			break;

		if (lexer_if(lx, TOKEN_COMMA, &comma)) {
			int hasline = token_has_line(comma, 1);

			parser_doc_token(pr, comma, concat);
			nconsumed++;
			doc_alloc(hasline ? DOC_HARDLINE : DOC_LINE, concat);

			if (is_simple_enabled(pr->pr_si, SIMPLE_DECL))
				simple_decl_comma(pr->pr_simple.decl, comma);
			/*
			 * Break before the argument. A hard line already broke
			 * the line, so no optional break is needed in that
			 * case; emitting both would produce a blank line on the
			 * next pass.
			 */
			concat =
			    doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));
			if (!hasline)
				doc_alloc(DOC_SOFTLINE, concat);
			concat =
			    doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, concat));
		}

		/*
		 * A malformed declarator list, e.g. a bitfield missing its
		 * following comma, can leave a field width directly before the
		 * next declarator. Keep a separator when dropping it would
		 * merge the boundary tokens.
		 */
		if (ninit > 0) {
			struct token *pv, *nx;

			if (lexer_back(lx, &pv) && lexer_peek(lx, &nx) &&
			    pv->tk_type == TOKEN_LITERAL &&
			    token_pair_needs_space(pv, nx))
				doc_literal(" ", concat);
		}

		error = parser_decl_init1(pr, concat, out);
		if (error & NONE)
			error = parser_decl_init_assign(pr, concat, out, arg);
		if (error & HALT)
			break;
		ninit++;
	}
	if (ninit == 0 && nconsumed == 0) {
		ruler_indent_remove(arg->rl, cookie);
		doc_remove(indent, arg->dc);
	}
	return parser_good(pr);
}

static int
parser_decl_peek_parens_or_squares(struct parser *pr, struct token **lhs)
{
	struct lexer *lx = pr->pr_lx;

	if (lexer_peek_if_pair(lx, TOKEN_LSQUARE, TOKEN_RSQUARE, lhs, nullptr))
		return 1;
	if (lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, lhs, nullptr))
		return 1;
	return 0;
}

/*
 * Parse one declarator component: trailing attributes, an identifier,
 * parenthesized or array declarator, bitfield, qualifier/storage keyword, a
 * pointer star, or an asm label. Each component hangs off the same concat so
 * later components (for example `*` then `name`) share a group. NONE means no
 * component matched.
 */
static int
parser_decl_init1(struct parser *pr, struct doc *dc, struct doc **out)
{
	struct lexer *lx = pr->pr_lx;
	struct token *lhs, *tk;

	/*
	 * C23 standard attributes may trail a declarator. Must be checked
	 * before parenthesized or square declarators as [[...]] would
	 * otherwise be mistaken for a pair of square brackets.
	 */
	if (parser_attributes_std_peek(pr, nullptr)) {
		doc_literal(" ", dc);
		if (parser_attributes_std(pr, dc) & FAIL)
			return parser_fail(pr);
		return parser_good(pr);
	}

	if (lexer_if(lx, TOKEN_IDENT, &tk)) {
		parser_doc_token(pr, tk, dc);
		if (lexer_peek_if(lx, TOKEN_IDENT, nullptr))
			doc_literal(" ", dc);
		*out = dc;
		return parser_good(pr);
	} else if (parser_decl_peek_parens_or_squares(pr, &lhs)) {
		struct doc *expr = nullptr;
		int rhs = lhs->tk_type == TOKEN_LSQUARE ? TOKEN_RSQUARE :
							  TOKEN_RPAREN;
		int error;

		if (lexer_expect(lx, lhs->tk_type, &lhs))
			parser_doc_token(pr, lhs, dc);

		/* Let the remaning tokens hang of the expression. */
		error = parser_expr(pr, &expr,
		    &(struct parser_expr_arg){
			.dc = dc,
		    });
		if (error & HALT)
			expr = dc;
		if (lexer_expect(lx, rhs, &tk))
			parser_doc_token(pr, tk, expr);
		if (lexer_peek_if(lx, TOKEN_IDENT, nullptr))
			doc_literal(" ", dc);
		return parser_good(pr);
	} else if (parser_decl_bitfield(pr, dc) & GOOD) {
		return parser_good(pr);
	} else if (lexer_if_flags(
		       lx, TOKEN_FLAG_QUALIFIER | TOKEN_FLAG_STORAGE, &tk)) {
		parser_doc_token(pr, tk, dc);
		doc_literal(" ", dc);
		return parser_good(pr);
	} else if (lexer_if(lx, TOKEN_STAR, &tk)) {
		parser_doc_token(pr, tk, dc);
		return parser_good(pr);
	} else if (parser_attributes(pr, dc, out, PARSER_ATTRIBUTES_LINE) &
	    GOOD) {
		if (!lexer_peek_if(lx, TOKEN_SEMI, nullptr) &&
		    !lexer_peek_if(lx, TOKEN_EQUAL, nullptr))
			doc_literal(" ", dc);
		return parser_good(pr);
	} else if (parser_asm_peek(pr)) {
		doc_alloc(DOC_LINE, dc);
		return parser_decl_asm(pr, dc);
	}

	return parser_none(pr);
}

/*
 * Parse an initializer introduced by '='. A left brace is delegated to the brace
 * initializer parser with a dedented continuation; any other initializer is an
 * expression that stops at the next comma or at the declaration's semicolon. A
 * line break before '=' is moved after it, and a hard line after '=' is preserved
 * inside the expression document so indentation is correct.
 */
static int
parser_decl_init_assign(struct parser *pr, struct doc *dc, struct doc **out,
    struct parser_decl_init_arg *arg)
{
	struct doc   *dedent;
	struct lexer *lx = pr->pr_lx;
	struct token *equal;
	int	      error;

	if (!lexer_if(lx, TOKEN_EQUAL, &equal))
		return parser_none(pr);

	if (arg->flags & PARSER_DECL_INIT_SPACE_BEFORE_EQUAL)
		doc_literal(" ", dc);
	parser_doc_token(pr, equal, dc);
	doc_literal(" ", dc);

	dedent = doc_alloc(DOC_CONCAT, ruler_dedent(arg->rl, dc, nullptr));
	if (lexer_peek_if(lx, TOKEN_LBRACE, nullptr)) {
		error = parser_braces(pr, arg->dc, dedent, arg->indent, 0);
		if (error & (FAIL | NONE))
			return parser_fail(pr);
		*out = dedent;
	} else {
		struct token *pv, *stop;
		unsigned int  expr_flags = 0;

		/* Never break before the assignment operator. */
		if ((pv = token_prev(equal)) != nullptr &&
		    token_has_line(pv, 1))
			token_move_suffixes(pv, equal);

		/*
		 * Honor hard line after assignment operator, must be emitted
		 * inside the expression document to get indentation right.
		 */
		if (token_has_line(equal, 1))
			expr_flags |= EXPR_EXEC_HARDLINE;
		lexer_peek_until_comma(lx, arg->semi, &stop);
		error = parser_expr(pr, out,
		    &(struct parser_expr_arg){
			.dc = dedent,
			.stop = stop,
			.indent = style(pr->pr_st, ContinuationIndentWidth),
			.flags = expr_flags,
		    });
		if (error & HALT)
			return parser_fail(pr);
	}

	return parser_good(pr);
}

/*
 * Returns non-zero if the next token(s) denote a C11/C23 static assertion,
 * i.e. _Static_assert or static_assert followed by a parenthesized expression
 * and a semicolon.
 */
static int
parser_static_assert_peek(struct parser *pr)
{
	struct lexer	  *lx = pr->pr_lx;
	struct lexer_state s;
	struct token	  *tk, *rparen, *semi, *attr;
	int		   peek = 0;

	lexer_peek_enter(lx, &s);
	while (parser_attributes_std_peek(pr, &attr)) {
		if (!lexer_seek_after(lx, attr))
			break;
	}
	if (lexer_if(lx, TOKEN_STATIC_ASSERT, &tk) ||
	    (lexer_if(lx, TOKEN_IDENT, &tk) &&
		clang_token_type(tk) == CLANG_TOKEN_STATIC_ASSERT)) {
		if (lexer_if_pair(
			lx, TOKEN_LPAREN, TOKEN_RPAREN, nullptr, &rparen) &&
		    lexer_if(lx, TOKEN_SEMI, &semi))
			peek = 1;
	}
	lexer_peek_leave(lx, &s);
	return peek;
}

/*
 * Parse a C11/C23 static assertion declaration, i.e.
 * _Static_assert ( constant-expression [, string-literal] ) ;
 * or the C23 spelling static_assert.
 */
static int
parser_static_assert(struct parser *pr, struct doc *dc)
{
	struct lexer *lx = pr->pr_lx;
	struct doc   *concat;
	struct token *tk, *rparen, *stop;
	int	      error;

	if (!parser_static_assert_peek(pr))
		return parser_none(pr);

	concat = doc_alloc(DOC_CONCAT, doc_alloc(DOC_GROUP, dc));

	/* Emit any leading C23 standard attributes. */
	while (parser_attributes_std_peek(pr, nullptr)) {
		if (parser_attributes_std(pr, concat) & FAIL)
			return parser_fail(pr);
		doc_alloc(DOC_LINE, concat);
	}

	if (!lexer_pop(lx, &tk))
		return parser_fail(pr);
	parser_doc_token(pr, tk, concat);

	if (!lexer_peek_if_pair(lx, TOKEN_LPAREN, TOKEN_RPAREN, &tk, &rparen))
		return parser_fail(pr);
	if (!lexer_if(lx, TOKEN_LPAREN, &tk))
		return parser_fail(pr);
	parser_doc_token(pr, tk, concat);

	(void)lexer_peek_until_comma(lx, rparen, &stop);
	error = parser_expr(pr, nullptr,
	    &(struct parser_expr_arg){
		.dc = concat,
		.stop = stop,
		.indent = style(pr->pr_st, ContinuationIndentWidth),
	    });
	if (error & HALT)
		return parser_fail(pr);

	if (stop != rparen) {
		if (!lexer_expect(lx, TOKEN_COMMA, &tk))
			return parser_fail(pr);
		parser_doc_token(pr, tk, concat);
		doc_literal(" ", concat);
		if (!lexer_expect(lx, TOKEN_STRING, &tk))
			return parser_fail(pr);
		parser_doc_token(pr, tk, concat);
	}

	if (!lexer_expect(lx, TOKEN_RPAREN, &tk))
		return parser_fail(pr);
	parser_doc_token(pr, tk, concat);

	return parser_semi(pr, concat);
}

static int
parser_decl_bitfield(struct parser *pr, struct doc *dc)
{
	struct lexer *lx = pr->pr_lx;
	struct token *colon, *size;
	unsigned int  dospace = style(pr->pr_st, BitFieldColonSpacing);

	if (!lexer_if(lx, TOKEN_COLON, &colon))
		return parser_none(pr);

	if (dospace == Both || dospace == Before)
		doc_literal(" ", dc);
	parser_doc_token(pr, colon, dc);
	if (dospace == Both || dospace == After)
		doc_literal(" ", dc);
	if (lexer_expect(lx, TOKEN_LITERAL, &size))
		parser_doc_token(pr, size, dc);
	return parser_good(pr);
}

/*
 * Parse the body of a struct, union, or enum declaration. Members are parsed as
 * nested declarations; the closing brace is followed by a space unless a
 * semicolon or attribute follows. break_before_braces controls whether the
 * opening brace starts a new line.
 */
static int
parser_decl_braces(struct parser *pr, struct doc *dc, int break_before_braces)
{
	struct lexer *lx = pr->pr_lx;
	struct doc   *indent;
	struct token *lbrace, *rbrace;

	if (!lexer_peek_if_pair(
		lx, TOKEN_LBRACE, TOKEN_RBRACE, &lbrace, &rbrace))
		return parser_fail(pr);
	parser_token_trim_before(pr, rbrace);
	if (lexer_expect(lx, TOKEN_LBRACE, nullptr)) {
		parser_token_trim_after(pr, lbrace);
		if (break_before_braces)
			doc_alloc(DOC_HARDLINE, dc);
		parser_doc_token(pr, lbrace, dc);
	}

	indent = doc_indent(style(pr->pr_st, IndentWidth), dc);
	doc_alloc(DOC_HARDLINE, indent);
	if (parser_decl(pr, indent, 0) & FAIL)
		return parser_fail(pr);
	doc_alloc(DOC_HARDLINE, dc);

	if (lexer_expect(lx, TOKEN_RBRACE, &rbrace))
		parser_doc_token(pr, rbrace, dc);

	if (!lexer_peek_if(lx, TOKEN_SEMI, nullptr) &&
	    !lexer_peek_if(lx, TOKEN_ATTRIBUTE, nullptr) &&
	    !parser_attributes_std_peek(pr, nullptr))
		doc_literal(" ", dc);

	return parser_good(pr);
}

/*
 * Run the declaration simplification pass over a speculative parse. The pass is
 * ignored unless PARSER_DECL_SIMPLE is set; on success it has rearranged the
 * token stream, and the caller then parses the declaration normally in
 * parser_decl1(). The temporary documents are discarded.
 */
static int
parser_simple_decl_enter(
    struct parser *pr, unsigned int flags, struct simple_cookie *simple)
{
	struct lexer_state s;
	struct lexer	  *lx = pr->pr_lx;
	struct doc	  *dc;
	unsigned int	   simple_flags;
	int		   error;

	simple_flags = (flags & PARSER_DECL_SIMPLE) ? 0 : SIMPLE_IGNORE;
	if (!simple_enter(pr->pr_si, SIMPLE_DECL, simple_flags, simple))
		return parser_good(pr);

	arena_scope(pr->pr_arena.scratch, scratch_scope);

	arena_scope(pr->pr_arena.doc, doc_scope);
	parser_arena_scope(&pr->pr_arena_scope.doc, &doc_scope, cookie);

	pr->pr_simple.decl = simple_decl_enter(lx, &scratch_scope, pr->pr_op);
	dc = doc_root(&doc_scope);
	lexer_peek_enter(lx, &s);
	error = parser_decl1(pr, dc, flags);
	lexer_peek_leave(lx, &s);
	if (error & GOOD)
		simple_decl_leave(pr->pr_simple.decl);
	simple_decl_free(pr->pr_simple.decl);
	pr->pr_simple.decl = nullptr;
	simple_leave(simple);

	return parser_good(pr);
}

/*
 * Run the forward-declaration sorting pass over a speculative parse. It only
 * applies at root level and only when PARSER_DECL_SIMPLE_FORWARD is set; the
 * pass collects consecutive struct forward declarations and sorts them when the
 * parse succeeds.
 */
static int
parser_simple_decl_forward_enter(
    struct parser *pr, unsigned int flags, struct simple_cookie *simple)
{
	struct lexer_state s;
	struct lexer	  *lx = pr->pr_lx;
	struct doc	  *dc;
	unsigned int	   simple_flags;
	int		   error;

	simple_flags = (flags & PARSER_DECL_SIMPLE_FORWARD) ? 0 : SIMPLE_IGNORE;
	if (!simple_enter(pr->pr_si, SIMPLE_DECL_FORWARD, simple_flags, simple))
		return parser_good(pr);

	arena_scope(pr->pr_arena.scratch, scratch_scope);

	arena_scope(pr->pr_arena.doc, doc_scope);
	parser_arena_scope(&pr->pr_arena_scope.doc, &doc_scope, cookie);

	pr->pr_simple.decl_forward =
	    simple_decl_forward_enter(lx, &scratch_scope, pr->pr_op);
	dc = doc_root(&doc_scope);
	lexer_peek_enter(lx, &s);
	error = parser_decl1(pr, dc, flags);
	lexer_peek_leave(lx, &s);
	if (error & GOOD)
		simple_decl_forward_leave(pr->pr_simple.decl_forward);
	simple_decl_forward_free(pr->pr_simple.decl_forward);
	pr->pr_simple.decl_forward = nullptr;
	simple_leave(simple);

	return parser_good(pr);
}
