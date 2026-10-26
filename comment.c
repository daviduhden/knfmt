#include <ctype.h>
#include <string.h>

#include "comment.h"
#include "config.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "style.h"
#include "token.h"
#include "util.h"

/*
 * Comment reindentation and trailing-whitespace removal. Multi-line comments are
 * rebuilt line by line: leading whitespace before a line's comment marker is
 * replaced with indentation matching the configured tab policy, and trailing
 * whitespace is removed. CRLF is preserved when the original comment used it.
 * The function returns nullptr when the comment is unchanged, so the lexer can
 * keep pointing at the original source bytes.
 */
static const char *nextline(const char *, size_t);
static const char *skipws(const char *, size_t);
static size_t	   rskipws(const char *, size_t);

static int
is_comment_trimmed(const struct token *tk, const struct buffer *bf)
{
	size_t bflen = buffer_get_len(bf);

	if (tk->tk_len != bflen)
		return 1;
	if (bflen == 0)
		return 0;
	return memcmp(tk->tk_str, buffer_get_ptr(bf), bflen) != 0;
}

/*
 * Return a reindented copy of a multi-line comment, or nullptr if no change is
 * needed or the comment is single-line/empty. The returned length is explicit so
 * embedded NUL bytes are preserved. The copy is allocated in s; intermediate
 * work uses the scratch arena. A final line without a newline (as produced by
 * merged C99 comments) is trimmed and emitted as well.
 */
const char *
comment_trim(const struct token *tk, const struct style *st,
    struct arena *scratch, struct arena_scope *s, size_t *lenp)
{
	struct buffer *bf;
	const char    *sp = tk->tk_str;
	size_t	       len = tk->tk_len;
	int	       iscrlf;

	if (lenp != nullptr)
		*lenp = 0;
	if (len == 0 || memchr(sp, '\n', len) == nullptr)
		return nullptr;

	arena_scope(scratch, scratch_scope);

	iscrlf = len >= 2 && sp[len - 2] == '\r';
	bf = arena_buffer_alloc(&scratch_scope, len + 1);
	for (;;) {
		const char *ep;
		size_t	    commlen;

		ep = skipws(sp, len);
		if (ep != nullptr && (*ep == '*' || *ep == '/')) {
			size_t wslen;

			wslen = (size_t)(ep - sp);
			strindent_buffer(
			    bf, strwidth(sp, wslen, 0), style_use_tabs(st), 0);
			len -= wslen;
			sp += wslen;
		}
		ep = nextline(sp, len);
		if (ep == nullptr) {
			/*
			 * Last line without a trailing new line, e.g. a merged
			 * block of C99 comments. Trim and emit it too.
			 */
			if (len > 0)
				buffer_puts(bf, sp, rskipws(sp, len));
			break;
		}
		commlen = (size_t)(ep - sp);
		buffer_puts(bf, sp, rskipws(sp, commlen));
		if (iscrlf)
			buffer_putc(bf, '\r');
		buffer_putc(bf, '\n');

		len -= commlen;
		sp += commlen;
	}

	if (!is_comment_trimmed(tk, bf))
		return nullptr;
	if (lenp != nullptr)
		*lenp = buffer_get_len(bf);
	return arena_strndup(s, buffer_get_ptr(bf), buffer_get_len(bf));
}

static const char *
nextline(const char *str, size_t len)
{
	const char *p;

	p = memchr(str, '\n', len);
	if (p == nullptr)
		return nullptr;
	return &p[1];
}

/*
 * Skip spaces and tabs. Returns nullptr when there is no leading whitespace or
 * when the whole input is whitespace; the latter avoids returning a pointer one
 * past the end.
 */
static const char *
skipws(const char *str, size_t len)
{
	size_t i = 0;

	for (; len > 0 && (str[i] == ' ' || str[i] == '\t'); i++, len--)
		continue;
	/*
	 * Return nullptr both when there is no leading whitespace and when the
	 * whole string is whitespace: in the latter case &str[i] points one
	 * past the end and the caller would dereference it.
	 */
	if (i == 0 || len == 0)
		return nullptr;
	return &str[i];
}

/*
 * Returns the length of the given string disregarding trailing whitespace. All
 * bytes classified by isspace(3) in the C locale are removed, including newlines.
 */
static size_t
rskipws(const char *str, size_t len)
{
	while (len > 0 && isspace((unsigned char)str[len - 1]))
		len--;
	return len;
}
