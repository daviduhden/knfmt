/*
 * Move a storage keyword that appears after other type specifiers to the front
 * of the declaration for -s, e.g. `const static int x` to `static const int x`.
 * Only static and extern are handled, and only when the token is movable. The
 * returned end token is adjusted when the moved token was the last one.
 */

#include "config.h"
#include "lexer.h"
#include "simple-storage.h"
#include "token.h"

static struct token *
find_token(int token_type, struct token *beg, struct token *end)
{
	struct token *tk = beg;

	for (;;) {
		if (tk->tk_type == token_type)
			return tk;
		if (tk == end)
			break;
		tk = token_next(tk);
	}
	return nullptr;
}

static int
move_storage_token(
    struct lexer *lx, int token_type, struct token *beg, struct token *end, struct token **out)
{
	struct token *tk = find_token(token_type, beg, end);
	if (tk == nullptr || tk == beg || !token_is_moveable(tk))
		return 0;

	*out = tk == end ? token_prev(end) : end;
	lexer_move_before(lx, beg, tk);
	return 1;
}

struct token *
simple_storage(struct lexer *lx, struct token *beg, struct token *end)
{
	struct token *out;
	if (move_storage_token(lx, TOKEN_STATIC, beg, end, &out))
		end = out;
	else if (move_storage_token(lx, TOKEN_EXTERN, beg, end, &out))
		end = out;
	return end;
}
