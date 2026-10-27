#include <ctype.h>
#include <string.h>

#include "arenas.h"
#include "config.h"
#include "cpp-format.h"
#include "doc.h"
#include "lexer.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "libks/compiler.h"
#include "ruler.h"
#include "style.h"
#include "token.h"
#include "trace-types.h"
#include "trace.h"
#include "util.h"

/*
 * Format multiline preprocessor directives, currently macro definitions whose
 * continuations can be aligned. The original physical spelling is analyzed to
 * infer the existing alignment style unless the configuration overrides it, and
 * the result is rebuilt as a document so the ruler can align every continuation
 * exactly. Macros whose continuations fall inside literals or split a
 * preprocessing token are left untouched because padding would change the
 * program.
 */
#define cpp_trace(op, fmt, ...) trace(TRACE_CPP, (op), (fmt), __VA_ARGS__)

enum indent_type {
	INDENT_TYPE_NONE,
	INDENT_TYPE_SPACES,
	INDENT_TYPE_TABS,
};

struct alignment {
	const char *indent;
	enum indent_type indent_type;
	enum style_keyword mode;
	unsigned int width;
	unsigned int tabs : 1, skip_first_line : 1;
};

/*
 * Trim trailing whitespace from the last line of a directive while preserving at
 * most two trailing new lines. A line consisting only of whitespace becomes at
 * most one separator new line, so the number of blank lines after a directive is
 * stable across passes. Bytes are copied explicitly because the text may contain
 * NUL; the returned length is written to lenp.
 */
static const char *
trim_line(const char *str, size_t len, struct arena_scope *s, size_t *lenp)
{
	int newlines = 0;
	char *out;
	size_t n;

	/* Honor up to two trailing new line(s). */
	if (len > 0 && str[len - 1] == '\n')
		newlines++;
	if (len > 1 && str[len - 2] == '\n')
		newlines++;

	while (len > 0 && isspace((unsigned char)str[len - 1]))
		len--;

	/*
	 * A tail made up solely of blank line(s) is the separator after the
	 * directive. The directive's own hard line already terminated the
	 * last content line, so emit at most one further new line; otherwise
	 * the number of blank lines would depend on the source layout.
	 */
	if (len == 0) {
		if (lenp != nullptr)
			*lenp = newlines > 0 ? 1 : 0;
		return newlines > 0 ? "\n" : "";
	}

	/*
	 * Copy the bytes explicitly: the line may contain NUL bytes, which a
	 * %s conversion would truncate.
	 */
	n = len + (newlines >= 2 ? 1 : 0) + (newlines >= 1 ? 1 : 0);
	out = arena_malloc(s, n);
	memcpy(out, str, len);
	if (newlines >= 2)
		out[len++] = '\n';
	if (newlines >= 1)
		out[len++] = '\n';
	if (lenp != nullptr)
		*lenp = len;
	return out;
}

/*
 * Returns non-zero if a line continuation joins two non-whitespace
 * characters, i.e. it sits inside a preprocessing token such as an identifier
 * split across physical lines. Aligning such a continuation inserts padding
 * between the halves of the token, changing the program; the macro must be
 * preserved as is instead.
 */
static int
splice_joins_token(const char *str, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len; i++) {
		unsigned char before, after;
		size_t next;

		if (str[i] != '\\')
			continue;
		if (str[i + 1] == '\n')
			next = i + 2;
		else if (i + 2 < len && str[i + 1] == '\r' && str[i + 2] == '\n')
			next = i + 3;
		else
			continue;
		if (i == 0 || next >= len)
			continue;
		before = (unsigned char)str[i - 1];
		after = (unsigned char)str[next];
		if (!isspace(before) && !isspace(after))
			return 1;
	}
	return 0;
}

/*
 * Returns non-zero if any line continuation (backslash immediately followed by
 * a newline) occurs inside a string or character literal. Aligning such a
 * continuation inserts padding into the literal, changing its value; the macro
 * must be preserved as is instead.
 */
static int
splice_inside_literal(const char *str, size_t len)
{
	enum { NORMAL, STRING, CHARACTER, BLOCK_COMMENT } state = NORMAL;
	size_t i = 0;

	while (i < len) {
		char c = str[i];

		switch (state) {
		case BLOCK_COMMENT:
			if (c == '*' && i + 1 < len && str[i + 1] == '/') {
				state = NORMAL;
				i += 2;
				continue;
			}
			i++;
			continue;
		case STRING:
		case CHARACTER:
			if (c == '\\') {
				if (i + 1 < len && str[i + 1] == '\n')
					return 1;
				if (i + 2 < len && str[i + 1] == '\r' && str[i + 2] == '\n')
					return 1;
				i += 2;
				continue;
			}
			if (c == (state == STRING ? '"' : '\'')) {
				state = NORMAL;
				i++;
				continue;
			}
			if (c == '\n') {
				/* Unterminated literal ends at the line end. */
				state = NORMAL;
				i++;
				continue;
			}
			i++;
			continue;
		case NORMAL:
			break;
		}

		if (c == '"') {
			state = STRING;
			i++;
		} else if (c == '\'') {
			state = CHARACTER;
			i++;
		} else if (c == '/' && i + 1 < len && str[i + 1] == '*') {
			state = BLOCK_COMMENT;
			i += 2;
		} else if (c == '/' && i + 1 < len && str[i + 1] == '/') {
			/* Line comment: skip to the end of the line. */
			size_t j = i;

			while (j < len && str[j] != '\n')
				j++;
			i = j;
		} else if (c == '\\' && i + 1 < len && str[i + 1] == '\n') {
			i += 2; /* splice outside a literal */
		} else {
			i++;
		}
	}
	return 0;
}

/*
 * Classify the first byte of a continuation line as tab, space, or no
 * indentation. Only the first byte matters for choosing an alignment style.
 */
static enum indent_type
classify_indent(const char *str)
{
	if (str[0] == '\t')
		return INDENT_TYPE_TABS;
	if (str[0] == ' ')
		return INDENT_TYPE_SPACES;
	return INDENT_TYPE_NONE;
}

/*
 * Returns a pointer to the end of current line assuming it's a line
 * continuation. Trailing spaces before the backslash are included in the
 * returned end pointer, which is used as the alignment point. nx is set to the
 * first byte after the newline. Returns nullptr when the line does not end in a
 * continuation.
 */
static const char *
nextline(const char *str, size_t len, const char **nx)
{
	const char *p;

	p = memchr(str, '\n', len);
	if (p == nullptr || p == str || p[-1] != '\\')
		return nullptr;
	*nx = &p[1];
	p--; /* consume '\\' */
	while (p > str && isspace((unsigned char)p[-1]))
		p--;
	return p;
}

static int
is_not_aligned(const struct alignment *a)
{
	return strncmp(a->indent, " \\", 2) == 0;
}

static const struct alignment *
max_alignment(const struct alignment *a, unsigned int len)
{
	const struct alignment *max = nullptr;
	for (unsigned int i = 0; i < len; i++) {
		const struct alignment *candidate = &a[len - i - 1];
		if (candidate->indent_type == INDENT_TYPE_NONE)
			continue;
		if (max == nullptr || candidate->width > max->width)
			max = candidate;
	}
	return max;
}

static int
all_identical(const struct alignment *a, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len - 1; i++) {
		if (a[i].indent_type == INDENT_TYPE_NONE ||
		    a[i + 1].indent_type == INDENT_TYPE_NONE)
			continue;
		if (a[i].width != a[i + 1].width)
			return 0;
		if (a[i].indent_type != a[i + 1].indent_type)
			return 0;
	}
	return 1;
}

static int
all_not_aligned(const struct alignment *a, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (!is_not_aligned(&a[i]))
			return 0;
	}
	return 1;
}

static int
all_tabs(const struct alignment *a, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (a[i].indent_type != INDENT_TYPE_NONE && a[i].indent_type != INDENT_TYPE_TABS)
			return 0;
	}
	return 1;
}

/*
 * Infer the continuation alignment used by the source. Up to three lines are
 * inspected; all-unindented continuations mean DontAlign. If the lines after the
 * first share one indentation type and width, Right alignment is chosen with the
 * smallest observed width, preserving the existing column. Otherwise the source
 * is considered unaligned and the configured default is kept. A line exceeding
 * the column limit aborts sensing.
 */
static int
sense_alignment(const char *str, size_t len, const struct style *st, struct alignment *alignment)
{
	struct alignment lines[3] = {0};
	unsigned int maxcol = style(st, ColumnLimit);
	unsigned int nlines = 0;
	unsigned int i;

	for (i = 0; i < countof(lines); i++) {
		const char *indent, *nx;
		size_t linelen;
		unsigned int col;

		indent = nextline(str, len, &nx);
		if (indent == nullptr)
			break;

		linelen = (size_t)(nx - str);
		/* Require trailing '\\' '\n' characters. */
		if (linelen < 2)
			return 0;
		col = colwidth(str, linelen - 1, 1);
		if (col > maxcol)
			return 0;
		lines[i].indent = indent;
		lines[i].indent_type = classify_indent(indent);
		lines[i].width = col - 2;
		len -= linelen;
		str += linelen;
		nlines++;
	}

	if (all_not_aligned(lines, nlines)) {
		*alignment = (struct alignment){.mode = DontAlign};
		return 1;
	}

	/* The first line is allowed to not be aligned. */
	if (nlines >= 2 && all_identical(&lines[1], nlines - 1)) {
		unsigned int width = alignment->width;
		const struct alignment *max = max_alignment(lines, nlines);
		if (max != nullptr && max->width < width)
			width = max->width;
		*alignment = (struct alignment){
		    .mode = Right,
		    .width = width,
		    .tabs = all_tabs(lines, nlines) ? 1 : 0,
		    .skip_first_line = is_not_aligned(&lines[0]) ? 1 : 0,
		};
		return 1;
	}
	return 0;
}

static unsigned int
max_width(const struct style *st)
{
	unsigned int width = style(st, ColumnLimit) - style(st, IndentWidth);
	return width - (width % 8);
}

/*
 * Align line continuations.
 */
/*
 * Format a multiline directive. Returns nullptr when no realignment is needed or
 * possible, in which case the token keeps its source spelling. Otherwise the
 * function builds a document with one literal per logical line, registers each
 * line's width with the ruler, and renders the aligned result into the lexer's
 * arena. A single-line directive is only rewritten when trailing whitespace was
 * trimmed; lenp receives the returned length.
 */
const char *
cpp_format(const struct lexer *lx, struct token *tk, const struct style *st, struct arenas *arena,
    const struct options *op, size_t *lenp)
{
	struct alignment alignment = {
	    .mode = style(st, AlignEscapedNewlines),
	    .width = max_width(st),
	    .tabs = style_use_tabs(st) ? 1 : 0,
	};
	struct ruler rl;
	struct buffer *bf;
	struct doc *dc;
	const char *nx, *str;
	size_t len;
	int nlines = 0;
	int didtrim = 0;

	if (lenp != nullptr)
		*lenp = 0;

	str = tk->tk_str;
	len = tk->tk_len;
	if (nextline(str, len, &nx) == nullptr)
		return nullptr;

	/*
	 * Never realign a macro whose line continuation sits inside a string or
	 * character literal, or joins the two halves of a preprocessing token:
	 * the padding would become part of the literal or split the token.
	 */
	if (splice_inside_literal(str, len) || splice_joins_token(str, len))
		return nullptr;

	arena_scope(arena->ruler, ruler_scope);

	if (sense_alignment(str, len, st, &alignment)) {
		cpp_trace(op, "mode %s, width %u, tabs %d, skip %d",
		    style_keyword_str(alignment.mode), alignment.width, alignment.tabs ? 1 : 0,
		    alignment.skip_first_line ? 1 : 0);
	}
	switch (alignment.mode) {
	case DontAlign:
		ruler_init(&rl, 1, RULER_ALIGN_FIXED, &ruler_scope);
		break;
	case Left:
		ruler_init(
		    &rl, 0, alignment.tabs ? RULER_ALIGN_TABS : RULER_ALIGN_MIN, &ruler_scope);
		break;
	case Right:
		ruler_init(&rl, alignment.width,
		    RULER_ALIGN_MAX | (alignment.tabs ? RULER_ALIGN_TABS : 0), &ruler_scope);
		break;
	default:
		return nullptr;
	}

	arena_scope(arena->scratch, scratch_scope);

	bf = arena_buffer_alloc(lexer_get_arena_scope(lx), len);
	dc = doc_root(&scratch_scope);

	for (;;) {
		struct doc *concat;
		const char *ep, *sp;
		size_t cpplen, linelen;
		unsigned int w;

		concat = doc_alloc(DOC_CONCAT, dc);

		sp = str;
		ep = nextline(sp, len, &nx);
		if (ep == nullptr)
			break;

		cpplen = (size_t)(ep - sp);
		if (cpplen > 0) {
			const char *literal;

			literal = arena_strndup(&scratch_scope, sp, cpplen);
			doc_literaln(literal, cpplen, concat);
		}
		buffer_reset(bf);
		w = doc_width(&(struct doc_exec_arg){
		    .dc = concat,
		    .scratch = arena->scratch,
		    .bf = bf,
		    .st = st,
		});
		if (nlines == 0 && alignment.skip_first_line)
			doc_literal(" ", concat);
		else
			ruler_insert(&rl, tk, concat, 1, w, 0);

		linelen = (size_t)(nx - str);
		len -= linelen;
		str += linelen;
		nlines++;

		/* No continuation wanted if the next line is empty. */
		if (len > 0 && nx[0] != '\n')
			doc_literal("\\", concat);
		doc_alloc(DOC_HARDLINE, concat);
	}
	if (len > 0) {
		const char *literal;
		size_t litlen;

		literal = trim_line(str, len, &scratch_scope, &litlen);
		didtrim = len != litlen;
		doc_literaln(literal, litlen, dc);
	}

	/* Alignment only wanted for multiple lines. */
	if (nlines <= 1 && !didtrim)
		return nullptr;

	ruler_exec(&rl);
	buffer_reset(bf);
	doc_exec(&(struct doc_exec_arg){
	    .dc = dc,
	    .scratch = arena->scratch,
	    .bf = bf,
	    .st = st,
	});
	if (lenp != nullptr)
		*lenp = buffer_get_len(bf);
	return buffer_get_ptr(bf);
}
