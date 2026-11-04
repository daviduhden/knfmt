#include <string.h>

#include "clang.h"
#include "config.h"
#include "lexer.h"
#include "libks/arena.h"
#include "simple-expr-printf.h"
#include "token.h"

/*
 * Remove a trailing newline from the format string of printf-family calls for
 * -s. The pass knows the argument position of the format string for each
 * supported function (err(3), warn(3), and their variants), skips preceding
 * arguments, and only rewrites a string literal whose final character sequence is
 * exactly `\n"`. The literal is replaced by a fresh arena string, never modified
 * in place, because token text may point into the source buffer.
 */
enum format_argno {
	FORMAT_ARGNO_0 = 0,
	FORMAT_ARGNO_1 = 1,
	FORMAT_ARGNO_2 = 2,
};

static int
identify_function(const struct token *tk, enum format_argno *argno)
{
	enum clang_token_type type = clang_token_type(tk);

	if (type == CLANG_TOKEN_PERROR || type == CLANG_TOKEN_VWARN || type == CLANG_TOKEN_VWARNX ||
	    type == CLANG_TOKEN_WARN || type == CLANG_TOKEN_WARNX) {
		*argno = FORMAT_ARGNO_0;
		return 1;
	}

	if (type == CLANG_TOKEN_ERR || type == CLANG_TOKEN_ERRX || type == CLANG_TOKEN_VERR ||
	    type == CLANG_TOKEN_VERRX || type == CLANG_TOKEN_VWARNC || type == CLANG_TOKEN_WARNC) {
		*argno = FORMAT_ARGNO_1;
		return 1;
	}

	if (type == CLANG_TOKEN_ERRC || type == CLANG_TOKEN_VERRC) {
		*argno = FORMAT_ARGNO_2;
		return 1;
	}

	return 0;
}

static struct token *
find_format_argument_inner(struct lexer *lx, enum format_argno argno)
{
	struct token *format = nullptr;
	int i;

	if (!lexer_if(lx, TOKEN_IDENT, nullptr) || !lexer_if(lx, TOKEN_LPAREN, nullptr))
		return nullptr;

	for (i = 0; i < (int)argno; i++) {
		struct token *ignore;

		if (!lexer_pop(lx, &ignore) || !lexer_if(lx, TOKEN_COMMA, nullptr))
			return nullptr;
	}
	while (lexer_if(lx, TOKEN_STRING, &format))
		continue;
	return format;
}

static struct token *
find_format_argument(struct lexer *lx, struct token *seek, enum format_argno argno)
{
	struct lexer_state ls;
	struct token *format;

	lexer_peek_enter(lx, &ls);
	lexer_seek(lx, seek);
	format = find_format_argument_inner(lx, argno);
	lexer_peek_leave(lx, &ls);
	return format;
}

static int
string_has_line(const char *str, size_t len)
{
	const char suffix[] = "\\n\"";
	size_t suffixlen = sizeof(suffix) - 1;
	size_t nslashes = 0;

	if (len < suffixlen || memcmp(&str[len - suffixlen], suffix, suffixlen) != 0)
		return 0;
	for (size_t i = len - 2; i > 0 && str[i - 1] == '\\'; i--)
		nslashes++;
	return (nslashes % 2) != 0;
}

void
simple_expr_printf(struct lexer *lx, struct token *tk)
{
	enum format_argno argno;
	char *sanitized_format;
	struct arena_scope *s;
	struct token *format;

	if (!identify_function(tk, &argno))
		return;
	format = find_format_argument(lx, tk, argno);
	if (format == nullptr)
		return;
	if (!string_has_line(format->tk_str, format->tk_len))
		return;

	s = lexer_get_arena_scope(lx);
	/* Preserve the encoding prefix and every byte before the final escape. */
	sanitized_format = arena_strndup(s, format->tk_str, format->tk_len - 2);
	sanitized_format[format->tk_len - 3] = '"';
	token_set_str(format, sanitized_format, format->tk_len - 2);
}
