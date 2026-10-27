#include <assert.h>
#include <ctype.h>
#include <string.h>

#include "config.h"
#include "lexer.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "libks/compiler.h"
#include "libks/list.h"
#include "libks/string.h"
#include "token.h"
#include "util.h"

/*
 * Token objects are the shared currency between the lexer, parser, documents
 * and ruler. A token has a type, a length-delimited byte string that may point
 * into the source or into arena memory, an optional source position, and two
 * dangling lists: prefixes (comments and directives before the token) and
 * suffixes (trailing comments and whitespace). Tokens are reference counted;
 * the lexer owns the primary list, while documents and rulers take additional
 * references. token_rele() releases the token's dangling tokens before the
 * token itself is poisoned, so a dangling list never outlives its owner.
 */
static const char *token_flags_str(unsigned int, struct arena_scope *);
static const char *token_type_str(int);

/*
 * Allocate a token with room for priv_size bytes of backend-private data. The
 * definition supplies the initial type, string, length and flags; the
 * reference count starts at one and the dangling lists are empty.
 */
struct token *
token_alloc(struct arena_scope *s, unsigned int priv_size, const struct token *def)
{
	struct token *tk;

	tk = arena_calloc(s, 1, sizeof(*tk) + priv_size);
	*tk = *def;
	tk->tk_refs = 1;
	tk->tk_priv_size = priv_size;
	LIST_INIT(&tk->tk_prefixes);
	LIST_INIT(&tk->tk_suffixes);
	return tk;
}

/*
 * Take an additional reference. The token must not be used after the matching
 * token_rele() has dropped the count to zero.
 */
void
token_ref(struct token *tk)
{
	tk->tk_refs++;
}

/*
 * Drop a reference. On the final release, remove the token from its dangling
 * lists (which releases those tokens in turn) and poison the storage so that a
 * stale use is caught by the arena debug support.
 */
void
token_rele(struct token *tk)
{
	struct token *fix;

	assert(tk->tk_refs > 0);
	if (--tk->tk_refs > 0)
		return;

	while ((fix = LIST_FIRST(&tk->tk_prefixes)) != nullptr)
		token_list_remove(&tk->tk_prefixes, fix);
	while ((fix = LIST_FIRST(&tk->tk_suffixes)) != nullptr)
		token_list_remove(&tk->tk_suffixes, fix);

	arena_poison(tk, sizeof(*tk) + tk->tk_priv_size);
}

/*
 * Remove all space suffixes from the given token. Returns the number of removed
 * suffixes.
 *
 * The suffixes are not unlinked: they are marked discarded, which makes the
 * document emitter ignore them while keeping optional spaces intact. Optional
 * spaces are never emitted directly and are therefore left untouched.
 */
int
token_trim(struct token *tk)
{
	struct token *suffix;
	int ntrim = 0;

	LIST_FOREACH(suffix, &tk->tk_suffixes)
	{
		/*
		 * Optional spaces are never emitted and must therefore be
		 * preserved.
		 */
		if (suffix->tk_flags & TOKEN_FLAG_OPTSPACE)
			continue;

		if (suffix->tk_type == TOKEN_SPACE) {
			suffix->tk_flags |= TOKEN_FLAG_DISCARD;
			ntrim++;
		}
	}
	return ntrim;
}

const char *
token_serialize(const struct token *tk, unsigned int flags, struct arena_scope *s)
{
	return token_serialize_with_extra_flags(tk, flags, nullptr, s);
}

const char *
token_serialize_with_extra_flags(
    const struct token *tk, unsigned int flags, const char *extra_flags, struct arena_scope *s)
{
	struct buffer *bf, *serialized_flags;
	int comma = 0;

	serialized_flags = arena_buffer_alloc(s, 128);
	if (flags & TOKEN_SERIALIZE_POSITION) {
		buffer_printf(serialized_flags, "%u:%u", tk->tk_lno, tk->tk_cno);
		comma++;
	}
	if (flags & TOKEN_SERIALIZE_FLAGS) {
		const char *serialized_token_flags;

		serialized_token_flags = token_flags_str(tk->tk_flags, s);
		if (serialized_token_flags[0] != '\0') {
			buffer_printf(
			    serialized_flags, "%s%s", comma++ ? "," : "", serialized_token_flags);
		}
	}
	if (flags & TOKEN_SERIALIZE_REFS) {
		buffer_printf(serialized_flags, "%s%d", comma++ ? "," : "", tk->tk_refs);
	}
	if (flags & TOKEN_SERIALIZE_ADDRESS) {
		buffer_printf(serialized_flags, "%s%p", comma++ ? "," : "", (const void *)tk);
	}
	if (extra_flags != nullptr) {
		buffer_printf(serialized_flags, "%s%s", comma++ ? "," : "", extra_flags);
	}

	bf = arena_buffer_alloc(s, 128);
	buffer_printf(bf, "%s", token_type_str(tk->tk_type));
	if (buffer_get_len(serialized_flags) > 0)
		buffer_printf(bf, "<%s>", buffer_str(serialized_flags));

	if (flags & TOKEN_SERIALIZE_VERBATIM) {
		buffer_printf(bf, "(");
		if (flags & TOKEN_SERIALIZE_QUOTE)
			buffer_printf(bf, "\"");
		buffer_printf(bf, "%s", KS_str_vis(tk->tk_str, tk->tk_len, s));
		if (flags & TOKEN_SERIALIZE_QUOTE)
			buffer_printf(bf, "\"");
		buffer_printf(bf, ")");
	}

	return buffer_str(bf);
}

/*
 * Give a newly inserted token the position immediately following after,
 * accounting for after's suffixes. The token's own suffixes are then placed on
 * the same line after it. If after ends its line, the column of the first
 * token on the following line is used. Prefixes are deliberately not touched.
 */
void
token_position_after(struct token *after, struct token *tk)
{
	struct token *last, *suffix;
	unsigned int lno = after->tk_lno;
	unsigned int cno;

	last = LIST_EMPTY(&after->tk_suffixes) ? after : LIST_LAST(&after->tk_suffixes);

	cno = colwidth(last->tk_str, last->tk_len, last->tk_cno);
	/*
	 * If after is the last token on this line, use the column from the
	 * first token on the next line.
	 */
	if (cno == 1) {
		struct token *nx = token_next(after);
		if (nx != nullptr)
			cno = nx->tk_cno;
	}

	/* Intentionally not adjusting prefixes. */

	tk->tk_cno = cno;
	tk->tk_lno = lno;
	cno = colwidth(tk->tk_str, tk->tk_len, tk->tk_cno);

	LIST_FOREACH(suffix, &tk->tk_suffixes)
	{
		suffix->tk_cno = cno;
		suffix->tk_lno = lno;
		cno = colwidth(suffix->tk_str, suffix->tk_len, suffix->tk_cno);
	}
}

/*
 * Order two tokens by line number only. Columns are intentionally ignored; two
 * tokens on the same line compare equal, which callers use to detect "same
 * line" relationships.
 */
int
token_cmp(const struct token *a, const struct token *b)
{
	int gt, le;

	le = a->tk_lno < b->tk_lno;
	gt = a->tk_lno > b->tk_lno;
	/* Intentionally not comparing the column. */
	return (le * -1) + (gt * 1);
}

int
token_strcmp(const struct token *a, const struct token *b)
{
	return token_memcmp(a, b->tk_str, b->tk_len);
}

int
token_memcmp(const struct token *tk, const char *str, size_t len)
{
	size_t minlen;
	int cmp, gt, le;

	minlen = tk->tk_len < len ? tk->tk_len : len;
	cmp = strncmp(tk->tk_str, str, minlen);
	if (cmp != 0)
		return cmp;
	le = tk->tk_len < len;
	gt = tk->tk_len > len;
	return (le * -1) + (gt * 1);
}

/*
 * Returns non-zero if the given token preceded with preprocessor directives.
 */
int
token_has_cpp(const struct token *tk)
{
	const struct token *prefix;

	LIST_FOREACH(prefix, &tk->tk_prefixes)
	{
		if (prefix->tk_flags & TOKEN_FLAG_CPP)
			return 1;
	}
	return 0;
}

/*
 * Map a preprocessor token type to its canonical counterpart, for example a
 * specific #if variant to TOKEN_CPP_IF. token_is_decl() and the branch logic
 * compare normalized types so the exact directive spelling does not matter.
 */
int
token_type_normalize(const struct token *tk)
{
	switch (tk->tk_type) {
#define OP(type, normalized, ...)                                                                  \
	case type:                                                                                 \
		if (normalized)                                                                    \
			return normalized;                                                         \
		break;
		FOR_TOKEN_CPP(OP)
#undef OP
	}
	return tk->tk_type;
}

/*
 * Returns non-zero if the given token is preceded with whitespace.
 * Such whitespace is never emitted by the lexer we therefore have to resort to
 * inspecting the source code through the underlying lexer buffer.
 */
int
token_has_indent(const struct token *tk)
{
	return tk->tk_off > 0 && (tk->tk_str[-1] == ' ' || tk->tk_str[-1] == '\t');
}

int
token_has_suffix(const struct token *tk, int type)
{
	return token_list_find(&tk->tk_suffixes, type, 0) != nullptr;
}

/*
 * Returns non-zero if the given token has at least nlines number of trailing
 * hard line(s).
 */
int
token_has_line(const struct token *tk, int nlines)
{
	const struct token *suffix;
	unsigned int flags = TOKEN_FLAG_OPTSPACE;

	assert(nlines > 0 && nlines <= 2);

	if (nlines > 1)
		flags |= TOKEN_FLAG_OPTLINE;

	LIST_FOREACH(suffix, &tk->tk_suffixes)
	{
		if (suffix->tk_type == TOKEN_SPACE && (suffix->tk_flags & flags) == 0)
			return 1;
	}
	return 0;
}

/*
 * Returns non-zero if the given token has at least nlines number of trailing
 * verbatim hard line(s).
 */
int
token_has_verbatim_line(const struct token *tk, int nlines)
{
	const char *str = tk->tk_str;
	size_t len = tk->tk_len;

	for (; len > 0; len--) {
		if (str[len - 1] != '\n')
			break;
		if (--nlines == 0)
			break;
	}
	return nlines == 0;
}

/*
 * Returns non-zero if the given token has any prefix.
 */
int
token_has_prefixes(const struct token *tk)
{
	return !LIST_EMPTY(&tk->tk_prefixes);
}

/*
 * Returns non-zero if the given token has trailing tabs.
 */
int
token_has_tabs(const struct token *tk)
{
	const struct token *suffix;

	LIST_FOREACH(suffix, &tk->tk_suffixes)
	{
		if (suffix->tk_type == TOKEN_SPACE && (suffix->tk_flags & TOKEN_FLAG_OPTSPACE) &&
		    suffix->tk_str[0] == '\t')
			return 1;
	}
	return 0;
}

/*
 * Returns non-zero if the given token has trailing spaces, including tabs.
 */
int
token_has_spaces(const struct token *tk)
{
	return token_list_find(&tk->tk_suffixes, TOKEN_SPACE, TOKEN_FLAG_OPTSPACE) != nullptr;
}

int
token_has_c99_comment(const struct token *tk)
{
	return token_list_find(&tk->tk_suffixes, TOKEN_COMMENT, TOKEN_FLAG_COMMENT_C99) != nullptr;
}

/*
 * Returns non-zero if the given token represents a declaration of the given
 * type.
 */
/*
 * Returns the token preceding a trailing C23 attribute specifier, i.e. skips
 * backwards over one or more [ [ ... ] ] sequences.
 */
static const struct token *
token_skip_attributes_backward(const struct token *tk)
{
	while (tk != nullptr && tk->tk_type == TOKEN_RSQUARE) {
		const struct token *pv = tk;
		int depth = 0;

		for (; pv != nullptr; pv = token_prev(pv)) {
			if (pv->tk_type == TOKEN_RSQUARE) {
				depth++;
			} else if (pv->tk_type == TOKEN_LSQUARE) {
				if (--depth == 0)
					break;
			}
		}
		if (pv == nullptr)
			break;
		tk = token_prev(pv);
	}
	return tk;
}

/*
 * Return non-zero if tk is the last token before a left brace that starts a
 * struct, union, or enum declaration of the given type. Trailing C23 attribute
 * specifiers and the declared identifier are skipped, and a C23 enum with a
 * fixed underlying type (enum E : type-name {) is recognized.
 */
int
token_is_decl(const struct token *tk, int type)
{
	const struct token *nx;

	nx = token_next(tk);
	if (nx == nullptr || nx->tk_type != TOKEN_LBRACE)
		return 0;

	/* Skip any trailing C23 attribute specifier, e.g. struct [[a]] { ... }.
	 */
	tk = token_skip_attributes_backward(tk);
	if (tk == nullptr)
		return 0;

	/*
	 * Recognize a C23 enumeration with a fixed underlying type, i.e.
	 * enum E : type-name { ... }. Walk back over the underlying type
	 * looking for the enum keyword and a colon.
	 */
	if (type == TOKEN_ENUM) {
		const struct token *pv;
		int colon = 0;

		for (pv = tk; pv != nullptr; pv = token_prev(pv)) {
			if (pv->tk_type == TOKEN_ENUM) {
				if (colon)
					return 1;
				break;
			}
			if (pv->tk_type == TOKEN_COLON)
				colon = 1;
			else if (!colon && pv->tk_type != TOKEN_IDENT &&
			    (pv->tk_flags & TOKEN_FLAG_TYPE) == 0)
				break;
		}
	}

	if (tk->tk_type == TOKEN_IDENT) {
		tk = token_prev(tk);
		if (tk == nullptr)
			return 0;
		tk = token_skip_attributes_backward(tk);
		if (tk == nullptr)
			return 0;
	}
	return tk->tk_type == type;
}

/*
 * Returns non-zero if the given token can be moved. Tokens carrying comments or
 * preprocessor directives as prefixes/suffixes are anchored to their source
 * position, since moving them would move or orphan that dangling text.
 */
int
token_is_moveable(const struct token *tk)
{
	const struct token *prefix;

	LIST_FOREACH(prefix, &tk->tk_prefixes)
	{
		if (prefix->tk_type == TOKEN_COMMENT || (prefix->tk_flags & TOKEN_FLAG_CPP))
			return 0;
	}

	if (token_list_find(&tk->tk_suffixes, TOKEN_COMMENT, 0) != nullptr)
		return 0;

	return 1;
}

/*
 * Returns non-zero if the given token is not part of a token_list.
 */
int
token_is_dangling(const struct token *tk)
{
	return !LIST_LINKED(tk);
}

void
token_set_str(struct token *tk, const char *str, size_t len)
{
	tk->tk_str = str;
	tk->tk_len = len;
}

/*
 * Returns non-zero if the two given tokens must be separated by whitespace in
 * order to not merge into a single, different token. This is a lexical test,
 * not a semantic one: it covers identifier/number boundaries, accidental
 * comment openers, floating constants formed across a boundary, and
 * punctuators that would combine into another operator.
 */
int
token_pair_needs_space(const struct token *a, const struct token *b)
{
	unsigned char ac, bc;

	if (a == nullptr || a->tk_len == 0 || b->tk_len == 0)
		return 0;

	ac = (unsigned char)a->tk_str[a->tk_len - 1];
	bc = (unsigned char)b->tk_str[0];

	/* Adjacent identifier or number characters always merge. */
	if ((isalnum(ac) || ac == '_') && (isalnum(bc) || bc == '_'))
		return 1;

	/* Avoid accidentally forming a comment. */
	if (ac == '/' && (bc == '/' || bc == '*'))
		return 1;

	/* A number followed by '.' forms a floating constant. */
	if (a->tk_type == TOKEN_LITERAL && bc == '.')
		return 1;

	/* '.' followed by a digit forms a floating constant. */
	if (ac == '.' && isdigit(bc))
		return 1;

	/* Adjacent punctuators that would form a different operator. */
	if ((ac == '+' && bc == '+') || (ac == '-' && (bc == '-' || bc == '>')) ||
	    (ac == '&' && bc == '&') || (ac == '|' && bc == '|') ||
	    (ac == '<' && (bc == '<' || bc == '=')) || (ac == '>' && (bc == '>' || bc == '=')) ||
	    (ac == '=' && bc == '=') || (ac == '!' && bc == '=') || (ac == '*' && bc == '=') ||
	    (ac == '/' && bc == '=') || (ac == '%' && bc == '=') || (ac == '^' && bc == '=') ||
	    (ac == '#' && bc == '#'))
		return 1;

	return 0;
}

/*
 * Count the number of complete physical lines embedded in the token text. A
 * trailing newline is not counted as an extra line. Used for multi-line
 * directives and verbatim comments.
 */
unsigned int
token_lines(const struct token *tk)
{
	const char *str = tk->tk_str;
	size_t len = tk->tk_len;
	unsigned int nlines = 0;

	while (len > 0) {
		const char *p;
		size_t linelen;

		p = memchr(str, '\n', len);
		if (p == nullptr)
			break;
		nlines++;
		linelen = (size_t)(p - str);
		len -= linelen + 1;
		str = p + 1;
	}
	return nlines;
}

void
token_list_prepend(struct token_list *tl, struct token *tk)
{
	LIST_INSERT_HEAD(tl, tk);
}

void
token_list_append(struct token_list *tl, struct token *tk)
{
	LIST_INSERT_TAIL(tl, tk);
}

void
token_list_append_after(struct token_list *tl, struct token *after, struct token *tk)
{
	LIST_INSERT_AFTER(tl, after, tk);
}

void
token_list_remove(struct token_list *tl, struct token *tk)
{
	assert(!token_is_dangling(tk));
	LIST_REMOVE(tl, tk);
	token_rele(tk);
}

/*
 * Exchange the contents of two token lists while preserving the relative order
 * within each list. Used when moving a line break from one token to another.
 */
void
token_list_swap(struct token_list *src, struct token_list *dst)
{
	struct token_list tmp;
	struct token *safe, *tk;

	LIST_INIT(&tmp);

	LIST_FOREACH_SAFE(tk, src, safe)
	{
		LIST_REMOVE(src, tk);
		LIST_INSERT_TAIL(&tmp, tk);
	}

	LIST_FOREACH_SAFE(tk, dst, safe)
	{
		LIST_REMOVE(dst, tk);
		LIST_INSERT_TAIL(src, tk);
	}

	LIST_FOREACH_SAFE(tk, &tmp, safe)
	{
		LIST_REMOVE(&tmp, tk);
		LIST_INSERT_TAIL(dst, tk);
	}
}

struct token *
token_list_first(struct token_list *tl)
{
	return LIST_FIRST(tl);
}

struct token *
token_list_last(struct token_list *tl)
{
	return LIST_LAST(tl);
}

struct token *
token_find_suffix_spaces(struct token *tk)
{
	return token_list_find(&tk->tk_suffixes, TOKEN_SPACE, TOKEN_FLAG_OPTSPACE);
}

void
token_move_suffixes(struct token *src, struct token *dst)
{
	while (!LIST_EMPTY(&src->tk_suffixes)) {
		struct token *suffix;

		suffix = LIST_FIRST(&src->tk_suffixes);
		LIST_REMOVE(&src->tk_suffixes, suffix);
		LIST_INSERT_TAIL(&dst->tk_suffixes, suffix);
	}
}

void
token_move_suffixes_if(struct token *src, struct token *dst, int type)
{
	struct token *suffix, *tmp;

	LIST_FOREACH_SAFE(suffix, &src->tk_suffixes, tmp)
	{
		if (suffix->tk_type != type)
			continue;

		LIST_REMOVE(&src->tk_suffixes, suffix);
		LIST_INSERT_TAIL(&dst->tk_suffixes, suffix);
	}
}

unsigned int
token_flags_inherit(const struct token *tk)
{
	return tk->tk_flags & TOKEN_FLAG_DIFF;
}

struct token *
token_list_find(const struct token_list *tokens, int type, unsigned int flags)
{
	struct token *tk;

	if (flags > 0) {
		LIST_FOREACH(tk, tokens)
		{
			if (tk->tk_type == type && (tk->tk_flags & flags))
				return tk;
		}
	} else {
		LIST_FOREACH(tk, tokens)
		{
			if (tk->tk_type == type)
				return tk;
		}
	}

	return nullptr;
}

static const char *
token_flags_str(unsigned int token_flags, struct arena_scope *s)
{
	static const struct {
		const char *str;
		size_t len;
		unsigned int flag;
	} flags[] = {
#define F(f, s) {(s), sizeof(s) - 1, (f)}
	    F(TOKEN_FLAG_ASSIGN, "ASSIGN"),
	    F(TOKEN_FLAG_BRANCH, "BRANCH"),
	    F(TOKEN_FLAG_DISCARD, "DISCARD"),
	    F(TOKEN_FLAG_COMMENT_C99, "C99"),
	    F(TOKEN_FLAG_COMMENT_CLANG_FORMAT_ON, "CLANG_FORMAT_ON"),
	    F(TOKEN_FLAG_COMMENT_CLANG_FORMAT_OFF, "CLANG_FORMAT_OFF"),
	    F(TOKEN_FLAG_CPP, "CPP"),
	    F(TOKEN_FLAG_OPTLINE, "OPTLINE"),
	    F(TOKEN_FLAG_OPTSPACE, "OPTSPACE"),
	    F(TOKEN_FLAG_DIFF, "DIFF"),
#undef F
	};
	size_t i;
	struct buffer *bf;

	bf = arena_buffer_alloc(s, 1 << 6);

	for (i = 0; i < countof(flags); i++) {
		if ((token_flags & flags[i].flag) == 0)
			continue;

		if (buffer_get_len(bf) > 0)
			buffer_putc(bf, '|');
		buffer_puts(bf, flags[i].str, flags[i].len);
	}

	return buffer_str(bf);
}

static const char *
token_type_str(int token_type)
{
	switch (token_type) {
#define OP(type, ...)                                                                              \
	case type:                                                                                 \
		return &#type[sizeof("TOKEN_") - 1];
		FOR_TOKEN_TYPES(OP)
		FOR_TOKEN_CPP(OP)
		FOR_TOKEN_SENTINELS(OP)
#undef OP
	}
	if (token_type == LEXER_EOF)
		return "EOF";
	return nullptr;
}
