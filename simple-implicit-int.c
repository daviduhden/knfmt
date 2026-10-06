#include "simple-implicit-int.h"

#include "config.h"

#include "clang.h"
#include "lexer.h"
#include "token.h"

static int
is_implicit_int(const struct token *beg, const struct token *end,
    int token_type)
{
	/* Allow implicit integer(s) to be preceded by static. */
	if (token_next(beg) == end && beg->tk_type == TOKEN_STATIC)
		beg = token_next(beg);
	return beg == end && beg->tk_type == token_type &&
	    token_is_moveable(beg);
}

/*
 * Returns non-zero if the given range consists solely of storage-class and
 * type-qualifier specifiers, meaning that the integer type is implicit as
 * standardized by C89/C90.
 */
static int
is_implicit_int_specifiers(const struct token *beg, const struct token *end)
{
	const struct token *tk;

	for (tk = beg; ; tk = token_next(tk)) {
		if ((tk->tk_flags & (TOKEN_FLAG_TYPE | TOKEN_FLAG_QUALIFIER |
		    TOKEN_FLAG_STORAGE)) == 0)
			return 0;
		/*
		 * Any type specifier other than signed and unsigned supplies
		 * an explicit type, no implicit int wanted.
		 */
		if (tk->tk_type != TOKEN_SIGNED &&
		    tk->tk_type != TOKEN_UNSIGNED &&
		    (tk->tk_flags & TOKEN_FLAG_TYPE))
			return 0;
		if (tk == end)
			break;
	}
	return 1;
}

struct token *
simple_implicit_int(struct lexer *lx, struct token *beg, struct token *end,
    int implicit_int)
{
	if (!is_implicit_int(beg, end, TOKEN_UNSIGNED) &&
	    !is_implicit_int(beg, end, TOKEN_SIGNED) &&
	    !(implicit_int && is_implicit_int_specifiers(beg, end)))
		return end;

	return lexer_insert_after(lx, end, clang_keyword_token(TOKEN_INT));
}
