#include "clang.h"
#include "config.h"
#include "doc.h"
#include "lexer.h"
#include "parser-extern.h"
#include "parser-priv.h"
#include "token.h"

/*
 * Parse an extern "C" block, used by headers to give C linkage to C++
 * declarations. The closing brace is matched while peeking; if it is controlled
 * by a conditional with an alternative branch, the block is declined and
 * pr_verbatim is set so the top-level parser preserves the source instead of
 * crossing the branch and dropping content. Declarations inside the block are
 * parsed by parser_root() recursively.
 */
int
parser_extern(struct parser *pr, struct doc *dc)
{
	struct lexer_state s;
	struct lexer *lx = pr->pr_lx;
	struct token *rbrace, *tk;
	int peek = 0;

	lexer_peek_enter(lx, &s);
	if (lexer_if(lx, TOKEN_EXTERN, nullptr) && lexer_if(lx, TOKEN_STRING, nullptr) &&
	    lexer_if_pair(lx, TOKEN_LBRACE, TOKEN_RBRACE, nullptr, &rbrace)) {
		struct token *prefix;
		int conditional = 0;

		/*
		 * A closing brace that immediately follows a conditional
		 * directive with an alternative branch ("#else"/"#elif") is
		 * itself conditional. Taking it as this block's unconditional
		 * terminator leaves the parser and the branch state
		 * inconsistent: crossing the alternative removes the brace and
		 * the branch recovery re-emits the block. Leave such a
		 * construct to the top-level branch recovery. A conditional
		 * brace without an alternative (the usual C++ linkage guard) is
		 * safe.
		 */
		LIST_FOREACH(prefix, &rbrace->tk_prefixes)
		{
			struct token *alt;

			if (token_type_normalize(prefix) != TOKEN_CPP_IF)
				continue;
			alt = clang_token_branch_next(prefix);
			if (alt != nullptr && token_type_normalize(alt) == TOKEN_CPP_ELSE)
				conditional = 1;
		}
		if (!conditional) {
			peek = 1;
		} else {
			/*
			 * Signal the top-level parser so it can preserve the
			 * source verbatim rather than recover across the
			 * conditional brace.
			 */
			pr->pr_verbatim = 1;
		}
	}
	lexer_peek_leave(lx, &s);
	if (!peek)
		return parser_none(pr);

	if (lexer_expect(lx, TOKEN_EXTERN, &tk))
		parser_doc_token(pr, tk, dc);
	doc_literal(" ", dc);
	if (lexer_expect(lx, TOKEN_STRING, &tk))
		parser_doc_token(pr, tk, dc);
	doc_literal(" ", dc);
	if (lexer_expect(lx, TOKEN_LBRACE, &tk))
		parser_doc_token(pr, tk, dc);
	doc_alloc(DOC_HARDLINE, dc);
	for (;;) {
		struct token *nx;
		int error;

		if (lexer_peek(lx, &nx) && nx == rbrace)
			break;

		error = parser_root(pr, dc);
		if (error & NONE)
			break;
		if (error & HALT)
			return error;
	}
	if (lexer_expect(lx, TOKEN_RBRACE, &tk))
		parser_doc_token(pr, tk, dc);
	if (lexer_if(lx, TOKEN_SEMI, &tk))
		parser_doc_token(pr, tk, dc);
	doc_alloc(DOC_HARDLINE, dc);
	return parser_good(pr);
}
