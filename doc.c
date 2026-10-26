#include <assert.h>
#include <err.h>
#include <float.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "diff.h"
#include "doc.h"
#include "lexer.h"
#include "libks/arena-buffer.h"
#include "libks/arena-vector.h"
#include "libks/arena.h"
#include "libks/arithmetic.h"
#include "libks/buffer.h"
#include "libks/compiler.h"
#include "libks/consistency.h"
#include "libks/list.h"
#include "libks/string.h"
#include "libks/vector.h"
#include "style.h"
#include "token.h"
#include "util.h"

/*
 * Document tree construction, inspection and rendering.
 *
 * A document describes layout, not syntax. CONCAT is an n-ary list; GROUP is a
 * region that should fit on one line if possible; LINE and SOFTLINE choose
 * between a break and a space (or nothing); INDENT/ALIGN/MINIMIZE influence
 * indentation; LITERAL and VERBATIM carry text. Rendering is a two-mode process:
 * BREAK allows line breaks, MUNGE forbids them while a group is being tested for
 * fit. Fitting is decided by walking a copy of the renderer state; the actual
 * output is produced only in BREAK mode.
 *
 * Before rendering, doc_summarize() computes a context-independent flat width
 * and purity flag for every node. Pure subtrees can be accepted or rejected as
 * a whole during fitting, which keeps fitting near-linear in flat documents.
 * Both summarization and rendering use explicit stacks so document length does
 * not consume process stack; only genuinely nested nodes recurse.
 *
 * The diff renderer (-D) is embedded in the same walk: unchanged groups are
 * muted while changed groups are emitted normally, and text outside changed
 * regions is copied from the source verbatim.
 */
#define IS_DOC_INDENT_PARENS(indent)                                           \
	((indent) > 0 && ((indent) & DOC_INDENT_PARENS))
#define IS_DOC_INDENT_FORCE(indent)                                            \
	((indent) > 0 && ((indent) & DOC_INDENT_FORCE))
#define IS_DOC_INDENT_NEWLINE(indent)                                          \
	((indent) > 0 && ((indent) & DOC_INDENT_NEWLINE))
#define IS_DOC_INDENT_WIDTH(indent)                                            \
	((indent) > 0 && ((indent) & DOC_INDENT_WIDTH))

LIST(doc_list, doc);

enum doc_diff_group {
	DOC_DIFF_GROUP_IGNORE,
	DOC_DIFF_GROUP_NOT_COVERED,
	DOC_DIFF_GROUP_COVERED,
};

struct doc_state_indent {
	unsigned int cur; /* current indent */
	unsigned int pre; /* last emitted indent */
};

struct doc_walk_state {
	struct doc_state_indent indent;
};

struct doc {
	enum doc_type dc_type;

	/* allocation trace */
	int	    dc_lno;
	const char *dc_fun;
	const char *dc_suffix;

	/* children */
	union {
		struct doc_list dc_list;
		struct doc     *dc_doc;
		struct token   *dc_tk;
	};

	/* value */
	union {
		VECTOR(struct doc_minimize) dc_minimizers;
		struct {
			const char *dc_str;
			size_t	    dc_len;
		};
		struct doc_align dc_align;
		int		 dc_int;
	};

	struct arena_scope *dc_scope;

	struct doc_walk_state dc_walk;

	/*
	 * Cached flat-width summary. Computed iteratively by
	 * doc_summarize() before layout; see DOC_SUM_*.
	 */
	unsigned int dc_sum_flat;
	unsigned int dc_sum_flags;
	/* Height of the subtree rooted at this node. */
	unsigned int dc_sum_depth;

	LIST_ENTRY(doc_list, doc);
};

struct doc_state {
	const struct style     *st_st;
	struct buffer	       *st_bf;
	struct lexer	       *st_lx;
	struct arena	       *st_scratch;
	const struct diffchunk *st_diff_chunks;

	enum doc_mode {
		BREAK,
		MUNGE,
	} st_mode;

	struct {
		const struct token *verbatim;
		const struct doc   *group;
		int		    group_has_many_chunks;
		int		    mute;
		unsigned int	    beg;
		unsigned int	    end;
		unsigned int	    muteline;
	} st_diff;

	struct doc_state_indent st_indent;

	struct {
		int idx;   /* index of best minimizer */
		int force; /* index of minimizer with force flag */
	} st_minimize;

	struct {
		unsigned int nfits;    /* # doc_fits() invocations */
		unsigned int nlines;   /* # emitted lines */
		unsigned int nexceeds; /* # characters exceeding column limit */
	} st_stats;

	unsigned int st_col;
	unsigned int st_depth;
	unsigned int st_refit;
	unsigned int st_parens;
	/* # allowed consecutive new line(s). */
	unsigned int st_maxlines;
	/* # consecutive emitted new line(s). */
	unsigned int st_nlines;
	/* Pending new line emitted on next doc_print() invocation. */
	unsigned int st_newline;
	/* Missed new line while muted. */
	unsigned int st_muteline;
	/* Optional new line(s) honored. */
	int st_optline;
	/* Muted, doc_print() does not emit anything. */
	int st_mute;
	/* Flags given to doc_exec(). */
	unsigned int st_flags;
};

struct doc_state_snapshot {
	struct doc_state sn_st;
	struct {
		char  *ptr;
		size_t len;
	} sn_bf;
};

/*
 * Tokens and line numbers extracted from a group document that covers a diff
 * chunk.
 */
struct doc_diff {
	/* Last verbatim token not covered by chunk. */
	const struct token *dd_verbatim;
	/* First token covered by chunk. */
	const struct token *dd_tk;
	/* Last seen token. */
	unsigned int dd_threshold;
	/* First token in group. */
	unsigned int dd_first;
	/* Return value for doc_diff_covers(). */
	enum doc_diff_group dd_covers;
	/* Below threshold doc_walk() return value. */
	unsigned int dd_below_threshold;
};

struct doc_fits {
	int	     fits;
	unsigned int optline;
};

struct doc_walk_queue {
	const struct doc *dc;
	int		  restore;
};

enum {
	DOC_WALK_BREAK = 0x00000001U,
	DOC_WALK_CONTINUE = 0x00000002U,
	DOC_WALK_SKIP = 0x00000004U,
	DOC_WALK_RESTORE = 0x40000000U,
};

/*
 * Cached, context-independent structural summary of a document subtree.
 *
 * A subtree is "pure" when its MUNGE-mode contribution is a fixed number of
 * columns and it has no side effects on the walking state (no hard line,
 * optional line, indentation or tab alignment). Only pure subtrees may be
 * skipped by doc_fits(); impure subtrees are traversed as before.
 */
#define DOC_SUM_VALID 0x00000001U
#define DOC_SUM_PURE 0x00000002U
#define DOC_SUM_TOOWIDE 0x00000004U
/* Saturating width sentinel; larger than any useful column limit. */
#define DOC_WIDTH_INF 0x10000000U

/*
 * Description of per document type specific semantics.
 */
static const struct doc_description {
	const char *name;

	/* Indicates which field in child union being used. */
	struct {
		uint8_t many:1;	 /* dc_list */
		uint8_t one:1;	 /* dc_doc */
		uint8_t token:1; /* dc_tk */
	} children;

	/* Indicates which field in value union being used. */
	struct {
		uint8_t minimizers:1; /* dc_minimizers */
		uint8_t integer:1;    /* dc_int */
		uint8_t string:1;     /* dc_str/dc_len */
	} value;
} doc_descriptions[] = {
    [DOC_CONCAT] =
	{
	    .name = "CONCAT",
	    .children = {.many = 1},
	},
    [DOC_GROUP] =
	{
	    .name = "GROUP",
	    .children = {.one = 1},
	},
    [DOC_INDENT] =
	{
	    .name = "INDENT",
	    .children = {.one = 1},
	},
    [DOC_NOINDENT] =
	{
	    .name = "NOINDENT",
	    .children = {.one = 1},
	},
    [DOC_ALIGN] =
	{
	    .name = "ALIGN",
	    .value = {.integer = 1},
	},
    [DOC_LITERAL] =
	{
	    .name = "LITERAL",
	    .children = {.token = 1},
	    .value = {.string = 1},
	},
    [DOC_VERBATIM] =
	{
	    .name = "VERBATIM",
	    .children = {.token = 1},
	    .value = {.string = 1},
	},
    [DOC_LINE] =
	{
	    .name = "LINE",
	},
    [DOC_SOFTLINE] =
	{
	    .name = "SOFTLINE",
	},
    [DOC_HARDLINE] =
	{
	    .name = "HARDLINE",
	},
    [DOC_OPTLINE] =
	{
	    .name = "OPTLINE",
	},
    [DOC_MUTE] =
	{
	    .name = "MUTE",
	    .value = {.integer = 1},
	},
    [DOC_UNMUTE] =
	{
	    .name = "UNMUTE",
	    .value = {.integer = 1},
	},
    [DOC_OPTIONAL] =
	{
	    .name = "OPTIONAL",
	    .children = {.one = 1},
	},
    [DOC_MINIMIZE] =
	{
	    .name = "MINIMIZE",
	    .children = {.one = 1},
	    .value = {.minimizers = 1},
	},
    [DOC_SCOPE] =
	{
	    .name = "SCOPE",
	    .children = {.one = 1},
	},
    [DOC_MAXLINES] =
	{
	    .name = "MAXLINES",
	    .children = {.one = 1},
	    .value = {.integer = 1},
	},
};

static void doc_exec1(const struct doc *, struct doc_state *);
static void doc_exec_indent(const struct doc *, struct doc_state *, int);
static void doc_exec_align(const struct doc *, struct doc_state *);
static void doc_exec_verbatim(const struct doc *, struct doc_state *);
static void doc_exec_minimize(const struct doc *, struct doc_state *);
static void doc_exec_minimize_indent(const struct doc *, struct doc_state *);
static void doc_exec_minimize_indent1(struct doc *, struct doc_state *, int);
static void doc_exec_mute(const struct doc *, struct doc_state *);
static void doc_exec_scope(const struct doc *, struct doc_state *);
static void doc_exec_maxlines(const struct doc *, struct doc_state *);
static void doc_walk(const struct doc *, struct doc_state *,
    unsigned int (*)(const struct doc *, struct doc_state *, void *), void *);
static int  doc_leads_with_break(const struct doc *);
static int  doc_fits(const struct doc *, struct doc_state *);
static unsigned int doc_fits1(const struct doc *, struct doc_state *, void *);
static unsigned int doc_print_indent(
    const struct doc *, struct doc_state *, unsigned int);
static unsigned int doc_print_indent1(
    const struct doc *, struct doc_state *, unsigned int, int);
static void	    doc_trim_spaces(const struct doc *, struct doc_state *);
static void	    doc_trim_lines(const struct doc *, struct doc_state *);
static int	    doc_is_mute(const struct doc_state *);
static int	    doc_parens_align(const struct doc_state *);
static int	    doc_has_list(const struct doc *);
static unsigned int doc_column(struct doc_state *, const char *, size_t);
static unsigned int doc_max1(const struct doc *, struct doc_state *, void *);

static void doc_state_init(
    struct doc_state *, struct doc_exec_arg *, enum doc_mode);
static void doc_state_reset_lines(struct doc_state *);
static void doc_state_snapshot(struct doc_state_snapshot *,
    const struct doc_state *, struct arena_scope *);
static void doc_state_snapshot_restore(
    const struct doc_state_snapshot *, struct doc_state *);

#define DOC_DIFF(st) (((st)->st_flags & DOC_EXEC_DIFF))

static int  doc_diff_group_enter(const struct doc *, struct doc_state *, int);
static void doc_diff_group_leave(const struct doc *, struct doc_state *, int);
static int  doc_diff_group_has_many_chunks(
    const struct doc *, struct doc_state *, unsigned int);
static void	    doc_diff_mute_enter(const struct doc *, struct doc_state *);
static void	    doc_diff_mute_leave(const struct doc *, struct doc_state *);
static void	    doc_diff_literal(const struct doc *, struct doc_state *);
static unsigned int doc_diff_verbatim(const struct doc *, struct doc_state *);
static void	    doc_diff_exit(const struct doc *, struct doc_state *);
static void	    doc_diff_emit(
    const struct doc *, struct doc_state *, unsigned int, unsigned int);
static unsigned int doc_diff_covers(
    const struct doc *, struct doc_state *, void *);
static int doc_diff_is_mute(const struct doc_state *);

#define doc_diff_leave(a, b, c) doc_diff_leave_impl((a), (b), (c), __func__)
static void doc_diff_leave_impl(
    const struct doc *, struct doc_state *, unsigned int, const char *);

/* Emit indentation after new line. */
#define DOC_PRINT_INDENT 0x00000001u
/* Internal to doc_print() used to signal that a pending new line is emitted. */
#define DOC_PRINT_NEWLINE 0x00000002u
/* Emit even if muted. */
#define DOC_PRINT_FORCE 0x00000004u

static int doc_print(
    const struct doc *, struct doc_state *, const char *, size_t, unsigned int);

static void doc_walk_state_snapshot(
    struct doc_walk_state *, const struct doc_state *);
static void doc_walk_state_restore(
    const struct doc_walk_state *, struct doc_state *);

#define doc_trace(dc, st, fmt, ...)                                            \
	do {                                                                   \
		if ((st)->st_flags & DOC_EXEC_TRACE)                           \
			doc_trace_impl((dc), (st), (fmt), __VA_ARGS__);        \
	} while (0)
static void doc_trace_impl(const struct doc *, const struct doc_state *,
    const char *, ...) __attribute__((format(printf, 3, 4)));

#define doc_trace_enter(dc, st)                                                \
	do {                                                                   \
		if ((st)->st_flags & DOC_EXEC_TRACE)                           \
			doc_trace_enter_impl((dc), (st));                      \
	} while (0)
static void doc_trace_enter_impl(const struct doc *, struct doc_state *);

#define doc_trace_leave(dc, st)                                                \
	do {                                                                   \
		if ((st)->st_flags & DOC_EXEC_TRACE)                           \
			doc_trace_leave_impl((dc), (st));                      \
	} while (0)
static void doc_trace_leave_impl(const struct doc *, struct doc_state *);

static const char *docstr(const struct doc *, struct arena_scope *);
static const char *indentstr(
    const struct doc *, const struct doc_state *, struct arena_scope *);
static const char *statestr(
    const struct doc_state *, unsigned int, struct arena_scope *);

static unsigned int count_verbatim_lines(const char *, size_t, unsigned int);

/*
 * Add two widths, saturating instead of wrapping at DOC_WIDTH_INF. Saturation is
 * what makes a too-wide subtree a stable, context-independent property.
 */
static unsigned int
doc_width_add(unsigned int a, unsigned int b)
{
	if (a >= DOC_WIDTH_INF || b >= DOC_WIDTH_INF || a > DOC_WIDTH_INF - b)
		return DOC_WIDTH_INF;
	return a + b;
}

/*
 * Compute the structural summary of a single node from its (already summed)
 * children. Must be called in post-order.
 */
static void
doc_sum_one(struct doc *dc)
{
	unsigned int flat = 0;
	int	     pure = 1;

	switch (dc->dc_type) {
	case DOC_CONCAT: {
		struct doc *child;

		LIST_FOREACH(child, &dc->dc_list)
		{
			flat = doc_width_add(flat, child->dc_sum_flat);
			if (!(child->dc_sum_flags & DOC_SUM_PURE))
				pure = 0;
		}
		break;
	}
	case DOC_GROUP:
	case DOC_NOINDENT:
	case DOC_MINIMIZE:
	case DOC_SCOPE:
	case DOC_MAXLINES:
		flat = dc->dc_doc->dc_sum_flat;
		if (!(dc->dc_doc->dc_sum_flags & DOC_SUM_PURE))
			pure = 0;
		break;
	case DOC_INDENT:
		/*
		 * Changing the indentation affects any later hard line, so the
		 * subtree is not pure even though its own width is a no-op.
		 */
		flat = dc->dc_doc->dc_sum_flat;
		pure = 0;
		break;
	case DOC_ALIGN:
		if (dc->dc_align.tabalign)
			pure = 0;
		else
			flat = doc_width_add(
			    dc->dc_align.indent, dc->dc_align.spaces);
		break;
	case DOC_LITERAL:
		/* Tab stops and new lines make the width context dependent. */
		if (dc->dc_str != nullptr &&
		    (memchr(dc->dc_str, '\n', dc->dc_len) != nullptr ||
			memchr(dc->dc_str, '\t', dc->dc_len) != nullptr))
			pure = 0;
		else
			flat = (unsigned int)dc->dc_len;
		break;
	case DOC_VERBATIM:
		if (dc->dc_str != nullptr &&
		    (memchr(dc->dc_str, '\n', dc->dc_len) != nullptr ||
			memchr(dc->dc_str, '\t', dc->dc_len) != nullptr))
			pure = 0;
		else
			flat = (unsigned int)dc->dc_len;
		break;
	case DOC_LINE:
		flat = 1;
		break;
	case DOC_SOFTLINE:
	case DOC_MUTE:
	case DOC_UNMUTE:
		flat = 0;
		break;
	case DOC_HARDLINE:
	case DOC_OPTLINE:
	case DOC_OPTIONAL:
		pure = 0;
		break;
	}

	if (flat >= DOC_WIDTH_INF)
		pure = 0;
	dc->dc_sum_flat = flat;
	dc->dc_sum_flags = DOC_SUM_VALID | (pure ? DOC_SUM_PURE : 0) |
	    (flat >= DOC_WIDTH_INF ? DOC_SUM_TOOWIDE : 0);

	{
		unsigned int		      depth = 1;
		const struct doc_description *desc =
		    &doc_descriptions[dc->dc_type];

		if (desc->children.many) {
			struct doc *child;

			LIST_FOREACH(child, &dc->dc_list)
			{
				if (child->dc_sum_depth + 1 > depth)
					depth = child->dc_sum_depth + 1;
			}
		} else if (desc->children.one) {
			if (dc->dc_doc->dc_sum_depth + 1 > depth)
				depth = dc->dc_doc->dc_sum_depth + 1;
		}
		dc->dc_sum_depth = depth;
	}
}

/*
 * Iteratively compute cached structural summaries for every node reachable
 * from (and including) root. Uses an explicit stack so that a flat document
 * of arbitrary length does not consume process stack. The tree is immutable
 * while it is being walked, so summaries stay valid for the whole layout.
 */
static void
doc_summarize(struct doc *root, struct arena *scratch)
{
	VECTOR(struct doc *) stack;
	VECTOR(struct doc *) order;
	struct doc *dc;
	size_t	    i;

	arena_scope(scratch, s);

	ARENA_VECTOR_INIT(&s, stack, 1 << 4);
	ARENA_VECTOR_INIT(&s, order, 1 << 4);

	*ARENA_VECTOR_ALLOC(stack) = root;
	while (!VECTOR_EMPTY(stack)) {
		const struct doc_description *desc;

		dc = *VECTOR_POP(stack);
		*ARENA_VECTOR_ALLOC(order) = dc;
		desc = &doc_descriptions[dc->dc_type];
		if (desc->children.many) {
			struct doc *child;

			LIST_FOREACH_REVERSE(child, &dc->dc_list)
			{
				*ARENA_VECTOR_ALLOC(stack) = child;
			}
		} else if (desc->children.one) {
			*ARENA_VECTOR_ALLOC(stack) = dc->dc_doc;
		}
	}

	/* Reverse pre-order visits every child before its parent. */
	for (i = VECTOR_LENGTH(order); i > 0; i--)
		doc_sum_one(order[i - 1]);
}

/*
 * Render the document into the caller's buffer. The tree is summarized first,
 * then rendered in BREAK mode. DOC_EXEC_TRIM removes redundant trailing blank
 * lines, and doc_diff_exit() flushes any diff bookkeeping left by -D.
 */
void
doc_exec(struct doc_exec_arg *arg)
{
	const struct doc *dc = arg->dc;
	struct doc_state  st;

	doc_summarize((struct doc *)dc, arg->scratch);
	doc_state_init(&st, arg, BREAK);
	doc_exec1(dc, &st);
	if (arg->flags & DOC_EXEC_TRIM)
		doc_trim_lines(dc, &st);
	doc_diff_exit(dc, &st);
	doc_trace(dc, &st, "%s: nfits %u", __func__, st.st_stats.nfits);
}

/*
 * Return the flat width of the document by rendering it in MUNGE mode, where
 * lines become spaces and no line breaks are emitted. The caller's buffer
 * receives the text, so pass a throwaway buffer when only the width is wanted.
 */
unsigned int
doc_width(struct doc_exec_arg *arg)
{
	struct doc_state st;

	doc_summarize((struct doc *)arg->dc, arg->scratch);
	doc_state_init(&st, arg, MUNGE);
	doc_exec1(arg->dc, &st);
	return st.st_col;
}

void
doc_remove(struct doc *dc, struct doc *parent)
{
	assert(doc_has_list(parent));
	LIST_REMOVE(&parent->dc_list, dc);
}

/*
 * Unlink the last child of parent. Documents are arena allocated, so removal
 * only detaches the node; any cleanup callback registered for it remains valid
 * until the scope ends. Returns zero when the parent has no children.
 */
int
doc_remove_tail(struct doc *parent)
{
	struct doc *dc;

	assert(doc_has_list(parent));
	dc = LIST_LAST(&parent->dc_list);
	if (dc == nullptr)
		return 0;
	LIST_REMOVE(&parent->dc_list, dc);
	return 1;
}

/*
 * Store a positive indentation amount on an INDENT node. The value is interpreted
 * when the node is executed and may contain one of the DOC_INDENT_* sentinels.
 */
void
doc_set_indent(struct doc *dc, unsigned int indent)
{
	dc->dc_int = (int)indent;
}

/*
 * Store a negative indentation amount. The renderer subtracts it from the
 * current indentation and clamps at zero.
 */
void
doc_set_dedent(struct doc *dc, unsigned int indent)
{
	dc->dc_int = -(int)indent;
}

void
doc_set_align(struct doc *dc, const struct doc_align *align)
{
	dc->dc_align = *align;
}

/*
 * Attach a child to its parent. A CONCAT parent appends to its list; any other
 * parent accepts exactly one child and must not already have one.
 */
void
doc_append(struct doc *dc, struct doc *parent)
{
	if (doc_has_list(parent)) {
		LIST_INSERT_TAIL(&parent->dc_list, dc);
	} else {
		assert(parent->dc_doc == nullptr);
		parent->dc_doc = dc;
	}
}

void
doc_move_before(struct doc *dc, struct doc *before, struct doc *parent)
{
	assert(doc_has_list(parent));
	LIST_REMOVE(&parent->dc_list, dc);
	LIST_INSERT_BEFORE(before, dc);
}

static void
doc_init(struct doc *dc)
{
	if (doc_has_list(dc))
		LIST_INIT(&dc->dc_list);
}

struct doc *
doc_root_impl(struct arena_scope *s, const char *fun, int lno)
{
	struct doc *dc;

	dc = arena_calloc(s, 1, sizeof(*dc));
	dc->dc_type = DOC_CONCAT;
	dc->dc_fun = fun;
	dc->dc_lno = lno;
	dc->dc_scope = s;
	doc_init(dc);
	return dc;
}

struct doc *
doc_alloc_impl(
    enum doc_type type, struct doc *parent, int val, const char *fun, int lno)
{
	struct doc *dc;

	dc = arena_calloc(parent->dc_scope, 1, sizeof(*dc));
	dc->dc_type = type;
	dc->dc_fun = fun;
	dc->dc_lno = lno;
	dc->dc_int = val;
	dc->dc_scope = parent->dc_scope;
	doc_init(dc);
	doc_append(dc, parent);
	return dc;
}

struct doc *
doc_indent_impl(unsigned int val, struct doc *dc, const char *fun, int lno)
{
	struct doc *indent;

	indent = doc_alloc_impl(DOC_INDENT, dc, 0, fun, lno);
	doc_set_indent(indent, val);
	return doc_alloc_impl(DOC_CONCAT, indent, 0, fun, lno);
}

struct doc *
doc_dedent_impl(unsigned int val, struct doc *dc, const char *fun, int lno)
{
	struct doc *indent;

	indent = doc_alloc_impl(DOC_INDENT, dc, 0, fun, lno);
	doc_set_dedent(indent, val);
	return doc_alloc_impl(DOC_CONCAT, indent, 0, fun, lno);
}

static void
doc_minimize_free(void *arg)
{
	struct doc *dc = arg;

	VECTOR_FREE(dc->dc_minimizers);
}

struct doc *
doc_minimize_impl(const struct doc_minimize *minimizers, size_t nminimizers,
    struct doc *parent, const char *fun, int lno)
{
	struct doc *dc;
	size_t	    i;

	dc = doc_alloc_impl(DOC_MINIMIZE, parent, 0, fun, lno);
	arena_cleanup(dc->dc_scope, doc_minimize_free, dc);
	if (VECTOR_INIT(dc->dc_minimizers))
		err(1, nullptr);
	if (VECTOR_RESERVE(dc->dc_minimizers, nminimizers))
		err(1, nullptr);
	for (i = 0; i < nminimizers; i++) {
		struct doc_minimize *dst;

		dst = VECTOR_ALLOC(dc->dc_minimizers);
		if (dst == nullptr)
			err(1, nullptr);
		*dst = minimizers[i];
	}
	return doc_alloc_impl(DOC_CONCAT, dc, 0, fun, lno);
}

struct doc *
doc_literal_impl(const char *str, struct doc *dc, const char *fun, int lno)
{
	return doc_literaln_impl(str, strlen(str), dc, fun, lno);
}

/*
 * Like doc_literal(), but the length is explicit so the text may contain NUL
 * bytes.
 */
struct doc *
doc_literaln_impl(
    const char *str, size_t len, struct doc *dc, const char *fun, int lno)
{
	struct doc *literal;

	literal = doc_alloc_impl(DOC_LITERAL, dc, 0, fun, lno);
	literal->dc_str = str;
	literal->dc_len = len;
	return literal;
}

static void
doc_token_free(void *arg)
{
	struct doc *dc = arg;

	token_rele(dc->dc_tk);
}

struct doc *
doc_token(struct token *tk, struct doc *dc, enum doc_type type, const char *fun,
    int lno)
{
	struct doc *token;

	assert(doc_descriptions[type].children.token);

	token = doc_alloc_impl(type, dc, 0, fun, lno);
	token->dc_tk = tk;
	token_ref(token->dc_tk);
	arena_cleanup(token->dc_scope, doc_token_free, token);
	token->dc_str = tk->tk_str;
	token->dc_len = tk->tk_len;
	return token;
}

/*
 * Returns the maximum value associated with the given document.
 */
int
doc_max(const struct doc *dc, struct arena *scratch)
{
	struct doc_state    st;
	struct doc_exec_arg arg = {
	    .scratch = scratch,
	};
	int max = 0;

	doc_state_init(&st, &arg, BREAK);
	doc_walk(dc, &st, doc_max1, &max);
	return max;
}

void
doc_annotate(struct doc *dc, const char *suffix)
{
	dc->dc_suffix = suffix;
}

/*
 * Explicit continuation frames for doc_exec1(). The renderer descends a
 * linear document iteratively so that a flat document of any length consumes
 * O(1) process stack; only genuinely nested nodes (indentation, scopes,
 * minimizers) recurse, bounded by nesting depth rather than document length.
 */
enum doc_exec_frame_kind {
	DOC_EXEC_NODE,	 /* process frame->dc */
	DOC_EXEC_CONCAT, /* resume iteration over a concat's children */
	DOC_EXEC_GROUP,	 /* restore group mode/diff after its child */
};

struct doc_exec_frame {
	enum doc_exec_frame_kind kind;
	const struct doc	*dc;
	/* DOC_EXEC_CONCAT */
	const struct doc *next;
	/* DOC_EXEC_GROUP */
	unsigned int oldmode;
	int	     diff;
	int	     has_mode;
};

/*
 * Iterative renderer. The explicit frame stack keeps process-stack use
 * independent of document length: CONCAT children are visited through a
 * DOC_EXEC_CONCAT frame, and a GROUP pushes a DOC_EXEC_GROUP frame so its mode
 * and diff state are restored after its child. Only nested nodes (indentation,
 * optional regions, scopes, minimizers) recurse, bounded by nesting depth.
 *
 * A GROUP in BREAK mode is refitted: doc_fits() decides whether its child can
 * be munged onto the current line, unless a preceding refit already invalidated
 * the decision. Hard lines force the next group to refit.
 */
static void
doc_exec1(const struct doc *root, struct doc_state *st)
{
	struct doc_exec_frame *stack;
	size_t		       n = 0, cap = 32;

	stack = malloc(cap * sizeof(*stack));
	if (stack == nullptr)
		err(1, nullptr);

#define PUSH_EXEC_FRAME(frame)                                                 \
	do {                                                                   \
		if (n == cap) {                                                \
			struct doc_exec_frame *_tmp;                           \
			cap *= 2;                                              \
			_tmp = realloc(stack, cap * sizeof(*stack));           \
			if (_tmp == nullptr)                                   \
				err(1, nullptr);                               \
			stack = _tmp;                                          \
		}                                                              \
		stack[n++] = (frame);                                          \
	} while (0)

	PUSH_EXEC_FRAME(((struct doc_exec_frame){
	    .kind = DOC_EXEC_NODE,
	    .dc = root,
	}));

	for (;;) {
		struct doc_exec_frame *fr;
		const struct doc      *dc;

		if (n == 0)
			break;
		fr = &stack[n - 1];

		if (fr->kind == DOC_EXEC_CONCAT) {
			if (fr->next == nullptr) {
				doc_trace_leave(fr->dc, st);
				n--;
				continue;
			}
			dc = fr->next;
			fr->next = LIST_NEXT(dc);
			PUSH_EXEC_FRAME(((struct doc_exec_frame){
			    .kind = DOC_EXEC_NODE,
			    .dc = dc,
			}));
			continue;
		}
		if (fr->kind == DOC_EXEC_GROUP) {
			if (fr->has_mode)
				st->st_mode = fr->oldmode;
			doc_diff_group_leave(fr->dc, st, fr->diff);
			doc_trace_leave(fr->dc, st);
			n--;
			continue;
		}

		dc = fr->dc;
		doc_trace_enter(dc, st);

		if (dc->dc_type == DOC_CONCAT) {
			fr->kind = DOC_EXEC_CONCAT;
			fr->next = LIST_FIRST(&dc->dc_list);
			continue;
		}
		if (dc->dc_type == DOC_GROUP) {
			unsigned int oldmode = 0;
			int	     diff, has_mode = 0;

			diff = doc_diff_group_enter(dc, st, 0);
			switch (st->st_mode) {
			case MUNGE:
				if (st->st_refit == 0 &&
				    !doc_leads_with_break(dc->dc_doc))
					break;
				FALLTHROUGH;
			case BREAK:
				st->st_refit = 0;
				oldmode = st->st_mode;
				has_mode = 1;
				st->st_mode = doc_fits(dc, st) ? MUNGE : BREAK;
				break;
			}
			fr->kind = DOC_EXEC_GROUP;
			fr->diff = diff;
			fr->oldmode = oldmode;
			fr->has_mode = has_mode;
			PUSH_EXEC_FRAME(((struct doc_exec_frame){
			    .kind = DOC_EXEC_NODE,
			    .dc = dc->dc_doc,
			}));
			continue;
		}

		switch (dc->dc_type) {
		case DOC_INDENT:
			doc_exec_indent(dc, st, dc->dc_int);
			break;

		case DOC_NOINDENT: {
			struct doc_state_indent oldindent;

			doc_trim_spaces(dc, st);
			oldindent = st->st_indent;
			memset(&st->st_indent, 0, sizeof(st->st_indent));
			doc_exec1(dc->dc_doc, st);
			st->st_indent = oldindent;
			break;
		}

		case DOC_ALIGN:
			doc_exec_align(dc, st);
			break;

		case DOC_LITERAL:
			doc_diff_literal(dc, st);
			doc_print(
			    dc, st, dc->dc_str, dc->dc_len, DOC_PRINT_INDENT);
			break;

		case DOC_VERBATIM:
			doc_exec_verbatim(dc, st);
			break;

		case DOC_LINE:
			switch (st->st_mode) {
			case BREAK:
				if (st->st_col > style(st->st_st, ColumnLimit))
					doc_print(
					    dc, st, " ", 1, DOC_PRINT_INDENT);
				else
					doc_print(
					    dc, st, "\n", 1, DOC_PRINT_INDENT);
				break;
			case MUNGE:
				if (doc_print(
					dc, st, " ", 1, DOC_PRINT_INDENT)) {
					doc_trace(dc, st, "%s: refit %u -> %d",
					    __func__, st->st_refit, 1);
					st->st_refit = 1;
				}
				break;
			}
			break;

		case DOC_SOFTLINE:
			switch (st->st_mode) {
			case BREAK:
				doc_print(dc, st, "\n", 1, DOC_PRINT_INDENT);
				break;
			case MUNGE:
				break;
			}
			break;

		case DOC_HARDLINE:
			doc_print(dc, st, "\n", 1, DOC_PRINT_INDENT);
			/*
			 * A hard line starts a new line; make the next group
			 * re-evaluate whether it fits instead of inheriting the
			 * surrounding mode.
			 */
			st->st_refit = 1;
			break;

		case DOC_OPTLINE:
			/*
			 * A forced optional line (dc_int) always starts a new
			 * line, e.g. after a C99 comment which otherwise would
			 * swallow the following token.
			 */
			if (st->st_optline || dc->dc_int)
				st->st_newline = 1;
			break;

		case DOC_MUTE:
		case DOC_UNMUTE:
			doc_exec_mute(dc, st);
			break;

		case DOC_OPTIONAL: {
			int oldoptline = st->st_optline;

			st->st_optline = 1;
			doc_exec1(dc->dc_doc, st);
			/* Note, could already be cleared by doc_print(). */
			if (st->st_optline)
				st->st_optline = oldoptline;
			break;
		}

		case DOC_MINIMIZE:
			doc_exec_minimize(dc, st);
			break;

		case DOC_SCOPE:
			doc_exec_scope(dc, st);
			break;

		case DOC_MAXLINES:
			doc_exec_maxlines(dc, st);
			break;

		default:
			break;
		}

		doc_trace_leave(dc, st);
		n--;
	}

#undef PUSH_EXEC_FRAME
	free(stack);
}

/*
 * Apply one INDENT node and render its child. A sentinel changes a different
 * piece of state instead of a plain amount:
 *
 * - DOC_INDENT_PARENS enters a parenthesis-alignment level, so following lines
 *   line up one column past the opening parenthesis.
 * - DOC_INDENT_FORCE emits the current indentation immediately.
 * - DOC_INDENT_NEWLINE only indents if a hard line was already emitted in this
 *   scope.
 * - DOC_INDENT_WIDTH captures the current column as the indentation.
 *
 * The previous indentation (and parenthesis level) is restored afterwards.
 */
static void
doc_exec_indent(const struct doc *dc, struct doc_state *st, int indent)
{
	unsigned int oldparens = 0;
	unsigned int oldindent = st->st_indent.cur;

	if (IS_DOC_INDENT_PARENS(indent)) {
		oldparens = st->st_parens;
		if (doc_parens_align(st))
			st->st_parens++;
	} else if (IS_DOC_INDENT_FORCE(indent)) {
		doc_print_indent(dc, st, st->st_indent.cur);
	} else if (IS_DOC_INDENT_NEWLINE(indent)) {
		if (st->st_stats.nlines > 0) {
			st->st_indent.cur +=
			    (unsigned int)(indent & ~DOC_INDENT_NEWLINE);
		}
	} else if (IS_DOC_INDENT_WIDTH(indent)) {
		if (!st->st_newline)
			st->st_indent.cur = st->st_col;
	} else {
		if (indent > 0)
			st->st_indent.cur += (unsigned int)indent;
		else if (KS_u32_sub_overflow(st->st_indent.cur,
			     (unsigned int)-indent, &st->st_indent.cur))
			st->st_indent.cur = 0;
	}
	doc_exec1(dc->dc_doc, st);
	if (IS_DOC_INDENT_PARENS(indent)) {
		st->st_parens = oldparens;
	} else if (IS_DOC_INDENT_FORCE(indent)) {
		/* nothing */
	} else {
		st->st_indent.cur = oldindent;
	}
}

/*
 * Emit the padding recorded by the ruler for an ALIGN node. With tabalign set,
 * the alignment is produced with tabs in eight-column steps followed by spaces,
 * so it matches the tab stops used for indentation. Otherwise the requested
 * amount is emitted as spaces only.
 */
static void
doc_exec_align(const struct doc *dc, struct doc_state *st)
{
	if (dc->dc_align.tabalign) {
		unsigned int indent = dc->dc_align.indent;

		while (indent > 0) {
			unsigned int n;

			n = doc_print_indent1(dc, st, 8, 1);
			indent = n < indent ? indent - n : 0;
		}
		doc_print_indent1(dc, st, dc->dc_align.spaces, 0);
	} else {
		doc_print_indent1(
		    dc, st, dc->dc_align.indent + dc->dc_align.spaces, 0);
	}
}

/*
 * Emit a verbatim document, used for comments, directives and copied diff
 * regions. Verbatim text is never indented: leading spaces are trimmed first.
 * A block ending in a newline is forced to start on a new line, and after the
 * block the indentation is restored either from the current level (when the
 * trimmed line was not empty) or from the last emitted level (when it was).
 * In -D mode the node may be the boundary of the muted unchanged region.
 */
static void
doc_exec_verbatim(const struct doc *dc, struct doc_state *st)
{
	struct token *tk = dc->dc_tk;
	unsigned int  diff, oldcol;
	int	      isblock, isnewline;

	if (doc_is_mute(st)) {
		if (!(DOC_DIFF(st) && st->st_diff.verbatim == tk))
			return;
	}

	diff = doc_diff_verbatim(dc, st);

	isblock = dc->dc_len > 1 && dc->dc_str[dc->dc_len - 1] == '\n';
	isnewline = dc->dc_len == 1 && dc->dc_str[0] == '\n';

	/* Verbatims must never be indented. */
	doc_trim_spaces(dc, st);
	oldcol = st->st_col;

	/* Verbatim blocks must always start on a new line. */
	if (isblock && st->st_col > 0)
		st->st_newline = 1;

	doc_print(dc, st, dc->dc_str, dc->dc_len, 0);

	/* Unmute in diff mode. */
	if (DOC_DIFF(st) && st->st_diff.verbatim == tk)
		st->st_diff.verbatim = nullptr;

	/* Restore indentation after emitting a verbatim block or new line. */
	if (isblock || isnewline) {
		struct doc_state_indent *it = &st->st_indent;
		unsigned int		 indent;

		if (oldcol > 0) {
			/*
			 * The line is not empty after trimming. Assume this a
			 * continuation in which the current indentation level
			 * must be used.
			 */
			indent = it->cur;
		} else {
			/*
			 * The line is empty after trimming. Assume this is not
			 * a continuation in which the previously emitted
			 * indentation must be used.
			 */
			indent = it->pre;
		}
		doc_print_indent(dc, st, indent);
	}

	doc_diff_leave(dc, st, diff);
}

static void
doc_exec_minimize(const struct doc *dc, struct doc_state *st)
{
	/* All minimizers are expected to be of the same type. */
	switch (dc->dc_minimizers[0].type) {
	case DOC_MINIMIZE_INDENT:
		doc_exec_minimize_indent(dc, st);
		break;
	}
}

/*
 * Choose the best of several candidate indentations by trial rendering. Each
 * candidate is rendered into a snapshot of the current output, and its penalty
 * is the average of the normalized number of new lines and the normalized
 * number of columns exceeding the column limit. A candidate flagged
 * DOC_MINIMIZE_FORCE wins immediately; otherwise the lowest penalty wins.
 * The winning candidate is selected for this node and recorded in
 * st_minimize.idx so nested invocations reuse the same choice.
 */
static void
doc_exec_minimize_indent(const struct doc *cdc, struct doc_state *st)
{
	/* Ugly, must be mutable for value mutation. */
	struct doc *dc = UNSAFE_CAST(struct doc *, cdc);
	VECTOR(struct doc_minimize) minimizers;
	struct doc_state_snapshot sn;
	unsigned int		  nlines = 0;
	unsigned int		  nexceeds = 0;
	int			  best = -1;
	int			  i, nminimizers;
	double			  minpenality = DBL_MAX;

	if (st->st_minimize.idx != -1) {
		doc_exec_minimize_indent1(dc, st, st->st_minimize.idx);
		return;
	}

	arena_scope(st->st_scratch, s);

	doc_state_snapshot(&sn, st, &s);
	minimizers = dc->dc_minimizers;
	nminimizers = (int)VECTOR_LENGTH(minimizers);
	for (i = 0; i < nminimizers; i++) {
		memset(&st->st_stats, 0, sizeof(st->st_stats));
		st->st_minimize.force = -1;
		st->st_flags &= ~DOC_EXEC_TRACE;

		st->st_minimize.idx = i;
		doc_exec_minimize_indent1(dc, st, i);
		st->st_minimize.idx = -1;
		if (st->st_minimize.force != -1)
			minimizers[i].flags |= DOC_MINIMIZE_FORCE;
		if (st->st_stats.nlines > nlines)
			nlines = st->st_stats.nlines;
		minimizers[i].penality.nlines = st->st_stats.nlines;
		if (st->st_stats.nexceeds > nexceeds)
			nexceeds = st->st_stats.nexceeds;
		minimizers[i].penality.nexceeds = st->st_stats.nexceeds;
		doc_state_snapshot_restore(&sn, st);
	}
	dc->dc_minimizers = minimizers;

	for (i = 0; i < nminimizers; i++) {
		struct doc_minimize *mi = &dc->dc_minimizers[i];
		double		     p = 0;

		if (nlines > 0)
			p += mi->penality.nlines / (double)nlines;
		if (nexceeds > 0)
			p += mi->penality.nexceeds / (double)nexceeds;
		p /= 2;
		mi->penality.sum = p;

		if (mi->flags & DOC_MINIMIZE_FORCE) {
			best = i;
			break;
		}
		if (p < minpenality) {
			minpenality = p;
			best = i;
		}
	}

	if (st->st_flags & DOC_EXEC_TRACE) {
		for (i = 0; i < nminimizers; i++) {
			const struct doc_minimize *mi = &dc->dc_minimizers[i];
			const char		  *suffix = "";

			if (mi->flags & DOC_MINIMIZE_FORCE)
				suffix = ", force";
			else if (i == best)
				suffix = ", best";
			doc_trace(dc, st,
			    "%s: type indent, penality %.2f, "
			    "indent %#x, nlines %u, nexceeds %u%s",
			    __func__, mi->penality.sum, mi->indent,
			    mi->penality.nlines, mi->penality.nexceeds, suffix);
		}
	}

	assert(best != -1);
	assert(st->st_minimize.idx == -1);
	st->st_minimize.idx = best;
	doc_exec_minimize_indent1(dc, st, best);
	st->st_minimize.idx = -1;
}

static void
doc_exec_minimize_indent1(struct doc *dc, struct doc_state *st, int idx)
{
	VECTOR(struct doc_minimize) minimizers = dc->dc_minimizers;

	if (minimizers[idx].flags & DOC_MINIMIZE_FORCE)
		st->st_minimize.force = idx;

	doc_exec_indent(dc, st, (int)minimizers[idx].indent);
}

/*
 * Adjust the mute nesting level. A positive value starts muting and asks the
 * diff renderer to remember whether a line was missed. Muting suppresses output
 * but hard lines still update line state. Returning to level zero ends muting;
 * DOC_MUTE then schedules one new line for the next print so the line structure
 * survives a suppressed region, while DOC_UNMUTE deliberately does not.
 */
static void
doc_exec_mute(const struct doc *dc, struct doc_state *st)
{
	/*
	 * While going mute, check if any new line is missed while being inside
	 * a diff chunk.
	 */
	if (st->st_mute == 0 && dc->dc_int > 0)
		doc_diff_mute_enter(dc, st);

	if (dc->dc_int > 0 || st->st_mute >= -dc->dc_int)
		st->st_mute += dc->dc_int;

	/*
	 * While going unmute, instruct the next doc_print() invocation to emit
	 * any missed new line.
	 */
	if (st->st_mute == 0) {
		unsigned int muteline;

		doc_diff_mute_leave(dc, st);
		muteline = st->st_muteline;
		doc_state_reset_lines(st);
		if (dc->dc_type == DOC_MUTE)
			st->st_newline = muteline;
	}
}

static void
doc_exec_scope(const struct doc *dc, struct doc_state *st)
{
	st->st_stats.nlines = 0;
	doc_exec1(dc->dc_doc, st);
}

static void
doc_exec_maxlines(const struct doc *dc, struct doc_state *st)
{
	unsigned int restore = st->st_maxlines;

	st->st_maxlines = (unsigned int)dc->dc_int;
	doc_exec1(dc->dc_doc, st);
	st->st_maxlines = restore;
}

/*
 * Traverse the document tree with an explicit work queue, invoking cb in
 * pre-order. A callback may stop the walk, skip a subtree, or request that the
 * walker state captured before descending (currently the indentation) is
 * restored after the subtree. Children are pushed in reverse so they are
 * visited in document order.
 */
static void
doc_walk(const struct doc *dc, struct doc_state *st,
    unsigned int (*cb)(const struct doc *, struct doc_state *, void *),
    void	*arg)
{
	VECTOR(struct doc_walk_queue) queue;

	arena_scope(st->st_scratch, s);

	ARENA_VECTOR_INIT(&s, queue, 1 << 4);

	/* Recursion flatten into a loop for increased performance. */
	*ARENA_VECTOR_ALLOC(queue) = (struct doc_walk_queue){.dc = dc};
	while (!VECTOR_EMPTY(queue)) {
		const struct doc_walk_queue  *tail;
		const struct doc_description *desc;
		unsigned int		      rv;

		tail = VECTOR_POP(queue);
		dc = tail->dc;
		desc = &doc_descriptions[dc->dc_type];

		if (tail->restore) {
			doc_walk_state_restore(&dc->dc_walk, st);
			continue;
		}

		rv = cb(dc, st, arg);
		if (rv & DOC_WALK_BREAK)
			break;
		if (rv & DOC_WALK_SKIP)
			continue;
		if (rv & DOC_WALK_RESTORE) {
			*ARENA_VECTOR_ALLOC(queue) = (struct doc_walk_queue){
			    .dc = dc,
			    .restore = 1,
			};
		}

		if (desc->children.many) {
			const struct doc_list *dl = &dc->dc_list;

			LIST_FOREACH_REVERSE(dc, dl)
			{
				*ARENA_VECTOR_ALLOC(queue) =
				    (struct doc_walk_queue){.dc = dc};
			}
		} else if (desc->children.one) {
			*ARENA_VECTOR_ALLOC(queue) =
			    (struct doc_walk_queue){.dc = dc->dc_doc};
		}
	}
}

static int
doc_leads_with_break(const struct doc *dc)
{
	for (;;) {
		const struct doc_description *desc =
		    &doc_descriptions[dc->dc_type];

		switch (dc->dc_type) {
		case DOC_LINE:
		case DOC_SOFTLINE:
		case DOC_OPTLINE:
			return 1;
		default:
			break;
		}
		if (desc->children.many) {
			dc = LIST_FIRST(&dc->dc_list);
			if (dc == nullptr)
				return 0;
		} else if (desc->children.one) {
			dc = dc->dc_doc;
		} else {
			return 0;
		}
	}
}

/*
 * Decide whether the group's child can be rendered on the current line in
 * MUNGE mode without exceeding the column limit. The test runs on a copy of the
 * renderer state and prints nothing. A pending new line ends the test
 * immediately; a forced optional line likewise makes the group not fit.
 */
static int
doc_fits(const struct doc *dc, struct doc_state *st)
{
	struct doc_state fst;
	struct doc_fits	 fits = {.fits = 1};
	unsigned int	 col = 0;
	unsigned int	 optline = 0;

	if (st->st_flags & DOC_EXEC_TRACE)
		st->st_stats.nfits++;

	memcpy(&fst, st, sizeof(fst));
	/* Should not perform any printing. */
	fst.st_bf = nullptr;
	fst.st_mode = MUNGE;
	doc_walk(dc, &fst, doc_fits1, &fits);
	col = fst.st_col;
	optline = fits.optline;
	doc_trace(dc, st, "%s: %u %s %u, optline %u", __func__, col,
	    fits.fits ? "<=" : ">", style(st->st_st, ColumnLimit), optline);

	return fits.fits;
}

/*
 * Walk callback for doc_fits(). It tracks the munged column and rejects as soon
 * as the column limit is exceeded. A pure subtree is added or rejected as a
 * whole through its cached summary rather than traversed. Hard lines consume a
 * pending optional line just like doc_print() does, so an optional line after a
 * hard line cannot make an overlong group appear to fit.
 */
static unsigned int
doc_fits1(const struct doc *dc, struct doc_state *st, void *arg)
{
	struct doc_fits	      *fits = arg;
	struct doc_walk_state *ws =
	    UNSAFE_CAST(struct doc_walk_state *, &dc->dc_walk);
	int restore = 0;

	if (st->st_newline) {
		fits->fits = st->st_col <= style(st->st_st, ColumnLimit);
		return DOC_WALK_BREAK;
	}

	/*
	 * A pure subtree has a context-independent flat width and no side
	 * effects on the walk state. Accept or reject it as a whole instead of
	 * descending its entire (possibly long) left spine.
	 */
	if ((dc->dc_sum_flags & (DOC_SUM_VALID | DOC_SUM_PURE)) ==
	    (DOC_SUM_VALID | DOC_SUM_PURE)) {
		unsigned int limit = style(st->st_st, ColumnLimit);

		if (dc->dc_sum_flags & DOC_SUM_TOOWIDE) {
			fits->fits = 0;
			return DOC_WALK_BREAK;
		}
		st->st_col += dc->dc_sum_flat;
		if (st->st_col > limit) {
			fits->fits = 0;
			return DOC_WALK_BREAK;
		}
		return DOC_WALK_CONTINUE | DOC_WALK_SKIP;
	}

	switch (dc->dc_type) {
	case DOC_INDENT:
		doc_walk_state_snapshot(ws, st);
		/*
		 * Only handle regular positive indentation for now as this is
		 * an estimate.
		 */
		if (!DOC_INDENT_HAS_SENTINELS(dc->dc_int) && dc->dc_int > 0)
			st->st_indent.cur += (unsigned int)dc->dc_int;
		restore = 1;
		break;

	case DOC_ALIGN: {
		unsigned int indent = dc->dc_align.indent;
		unsigned int spaces = dc->dc_align.spaces;

		if (dc->dc_align.tabalign) {
			while (indent > 0) {
				unsigned int n;

				n = doc_column(st, "\t", 1);
				indent = n < indent ? indent - n : 0;
			}
		}
		st->st_col += indent + spaces;
		break;
	}

	case DOC_LITERAL:
		doc_column(st, dc->dc_str, dc->dc_len);
		break;

	case DOC_VERBATIM:
		if (dc->dc_len > 0 && dc->dc_str[dc->dc_len - 1] != '\n')
			doc_column(st, dc->dc_str, dc->dc_len);
		break;

	case DOC_LINE:
		st->st_col++;
		break;

	case DOC_HARDLINE:
		doc_column(st, "\n", 1);
		st->st_col += st->st_indent.cur;
		/*
		 * A hard line consumes any pending optional line, matching
		 * doc_print(). Otherwise an optional line following a hard
		 * line would make an overlong group appear to fit.
		 */
		st->st_optline = 0;
		break;

	case DOC_OPTLINE:
		if (st->st_optline) {
			fits->optline = 1;
			return DOC_WALK_BREAK;
		}
		break;

	case DOC_OPTIONAL:
		st->st_optline = 1;
		break;

	case DOC_CONCAT:
	case DOC_GROUP:
	case DOC_NOINDENT:
	case DOC_SOFTLINE:
	case DOC_MUTE:
	case DOC_UNMUTE:
	case DOC_MINIMIZE:
	case DOC_SCOPE:
	case DOC_MAXLINES:
		break;
	}

	if (st->st_col > style(st->st_st, ColumnLimit)) {
		fits->fits = 0;
		return DOC_WALK_BREAK;
	}

	return DOC_WALK_CONTINUE | (restore ? DOC_WALK_RESTORE : 0);
}

static unsigned int
doc_print_indent(
    const struct doc *dc, struct doc_state *st, unsigned int indent)
{
	if (st->st_parens > 0) {
		/* Align with the left parenthesis on the previous line. */
		indent = st->st_indent.pre + st->st_parens;
	} else {
		st->st_indent.pre = indent;
	}
	return doc_print_indent1(dc, st, indent, style_use_tabs(st->st_st));
}

static unsigned int
doc_print_indent1(const struct doc *UNUSED(dc), struct doc_state *st,
    unsigned int indent, int usetabs)
{
	unsigned int oldcol = st->st_col;

	st->st_col = strindent_buffer(st->st_bf, indent, usetabs, st->st_col);
	return st->st_col - oldcol;
}

/*
 * Emit text and update the renderer state. A pending new line is emitted first
 * (unless the next text is a space, which would be redundant). New lines are
 * capped by st_maxlines and trim trailing spaces; a non-new-line resets the
 * consecutive line count. Muted output is not written, but the column and line
 * state are still updated and a suppressed new line is remembered. DOC_PRINT_FORCE
 * bypasses muting and leaves the line statistics untouched, which is required
 * when copying diff regions.
 */
static int
doc_print(const struct doc *dc, struct doc_state *st, const char *str,
    size_t len, unsigned int flags)
{
	int ismute = doc_is_mute(st) && (flags & DOC_PRINT_FORCE) == 0;
	int isnewline = len == 1 && str[0] == '\n';

	if (isnewline && ismute)
		st->st_muteline = 1;

	/* Emit pending new line. */
	if (st->st_newline) {
		int isspace = len == 1 && str[0] == ' ';

		/* Redundant if a new line is about to be emitted. */
		if (isspace)
			return 0;

		/* DOC_OPTLINE has the same semantics as DOC_LINE. */
		st->st_refit = 1;
		st->st_newline = 0;
		doc_print(dc, st, "\n", 1, flags | DOC_PRINT_NEWLINE);
		if (isnewline)
			return 1;
	}

	if (isnewline) {
		if (st->st_nlines >= st->st_maxlines)
			return 1;
		st->st_nlines++;
		/*
		 * Force printing is only expected to be performed while
		 * emitting verbatim diff chunks. Such invocations must leave
		 * the statistics intact in order to not influence
		 * DOC_INDENT_NEWLINE decisions.
		 */
		if ((flags & DOC_PRINT_FORCE) == 0)
			st->st_stats.nlines++;

		/*
		 * Suppress optional line(s) while emitting a line. Mixing the
		 * two results in odd formatting.
		 */
		if ((flags & DOC_PRINT_NEWLINE) == 0 && st->st_optline) {
			doc_trace(dc, st, "%s: optline %d -> 0", __func__,
			    st->st_optline);
			st->st_optline = 0;
		}
	} else {
		st->st_nlines = 0;
	}

	if (isnewline) {
		doc_trim_spaces(dc, st);
		/* Invalidate choice of best minimizer. */
		if (st->st_minimize.force != -1)
			st->st_minimize.idx = -1;
	}
	if (!ismute)
		buffer_puts(st->st_bf, str, len);
	doc_column(st, str, len);

	if (isnewline && (flags & DOC_PRINT_INDENT))
		doc_print_indent(dc, st, st->st_indent.cur);

	return 1;
}

static void
doc_walk_state_snapshot(struct doc_walk_state *ws, const struct doc_state *st)
{
	ws->indent = st->st_indent;
}

static void
doc_walk_state_restore(const struct doc_walk_state *ws, struct doc_state *st)
{
	st->st_indent = ws->indent;
}

/*
 * Remove trailing spaces and tabs from the output and roll the column back
 * accordingly. Tabs are accounted for as advancing to the next eight-column
 * stop, matching strwidth().
 */
static void
doc_trim_spaces(const struct doc *dc, struct doc_state *st)
{
	const char  *buf = buffer_get_ptr(st->st_bf);
	size_t	     buflen = buffer_get_len(st->st_bf);
	unsigned int oldcol = st->st_col;

	while (buflen > 0) {
		unsigned int w;
		char	     ch;

		ch = buf[buflen - 1];
		if (ch != ' ' && ch != '\t')
			break;
		buflen -= buffer_pop(st->st_bf, 1);
		w = ch == '\t' ? 8 - (st->st_col % 8) : 1;
		st->st_col -= w < st->st_col ? w : st->st_col;
	}
	if (oldcol > st->st_col) {
		doc_trace(dc, st, "%s: trimmed %u character(s)", __func__,
		    oldcol - st->st_col);
	}
}

/*
 * Trim trailing consecutive new line(s) from the rendered document.
 * Necessary if a verbatim document is at the end which may include up to two
 * new lines. At least one new line is always kept, and the loop stops at the
 * first non-new-line character.
 */
static void
doc_trim_lines(const struct doc *dc, struct doc_state *st)
{
	const char *buf = buffer_get_ptr(st->st_bf);
	size_t	    buflen = buffer_get_len(st->st_bf);
	int	    ntrim = 0;

	while (
	    buflen > 1 && buf[buflen - 1] == '\n' && buf[buflen - 2] == '\n') {
		buflen -= buffer_pop(st->st_bf, 1);
		ntrim++;
	}
	if (ntrim > 0)
		doc_trace(dc, st, "%s: trimmed %d line(s)", __func__, ntrim);
}

static const struct diffchunk *
doc_diff_find_chunk(const struct doc_state *st, const struct token *tk)
{
	unsigned int i, n;

	n = count_verbatim_lines(tk->tk_str, tk->tk_len, 1);
	for (i = 0; i < n; i++) {
		const struct diffchunk *du;

		du = diff_get_chunk(st->st_diff_chunks, tk->tk_lno + i);
		if (du != nullptr)
			return du;
	}
	return nullptr;
}

/*
 * Enter diff handling for a group. Only the outermost group is considered,
 * unless nested groups are being re-evaluated after a group covered more than
 * one chunk. A group spanning a hard line is ignored so a nested single-line
 * group can be found. A group not covered by any chunk is recorded so all of
 * its children can be muted; a covered group seeds the changed line range and
 * mutes any preceding verbatim text inside the same group. Returns non-zero if
 * the caller must pair this with doc_diff_group_leave().
 */
static int
doc_diff_group_enter(const struct doc *dc, struct doc_state *st, int nested)
{
	struct doc_diff		dd;
	const struct diffchunk *du;
	unsigned int		end;

	if (!DOC_DIFF(st))
		return 0;

	/*
	 * Only relevant while entering the first group. Unless the group above
	 * us was ignored or if the group covers many diff chunks, see below.
	 */
	if (st->st_diff.group != nullptr && !nested)
		return 0;

	/*
	 * Check if the current group is covered by a diff chunk. If so, we must
	 * start emitting documents nested under the same group.
	 *
	 * A group is something intended to fit on a single line. However,
	 * there are exceptions in the sense of groups spanning multiple lines;
	 * for instance brace initializers. Such groups are ignored allowing the
	 * first nested group covering a single line to be found.
	 */
	dd = (struct doc_diff){
	    .dd_threshold = st->st_diff.beg,
	    .dd_covers = DOC_DIFF_GROUP_NOT_COVERED,
	    /*
	     * Upon entering the group for the first time, abort the walk while
	     * traversing a document with a line number less than the one from
	     * the previous diff chunk (i.e. dd_threshold) as we have already
	     * seen this document before. This could happen while traversing the
	     * same source code again after branching.
	     *
	     * Upon nested invocations, we want to move past the threshold in
	     * order to find the next diff chunk covered by this group.
	     */
	    .dd_below_threshold = nested ? DOC_WALK_CONTINUE : DOC_WALK_BREAK,
	};
	doc_walk(dc, st, doc_diff_covers, &dd);
	switch (dd.dd_covers) {
	case DOC_DIFF_GROUP_IGNORE:
		/*
		 * The group spans multiple lines. Ignore it and keep evaluating
		 * nested groups on subsequent invocations of this routine.
		 */
		return 0;
	case DOC_DIFF_GROUP_NOT_COVERED:
		/*
		 * The group is not covered by any diff chunk and all nested
		 * groups can therefore be ignored. However, if the previous
		 * group above us touched lines after the diff chunk due to
		 * reformatting make sure to reset the state.
		 */
		if (st->st_diff.end > 0)
			doc_diff_leave(dc, st, 1);
		st->st_diff.group = dc;
		return 1;
	case DOC_DIFF_GROUP_COVERED:
		/*
		 * The group is covered by a diff chunk. Make sure to leave any
		 * previous diff chunk if we're entering a new one.
		 */
		if (st->st_diff.end > 0 && dd.dd_first > st->st_diff.end)
			doc_diff_leave(dc, st, 1);
		break;
	}

	st->st_diff.group = dc;

	doc_trace(dc, st,
	    "%s: enter chunk: beg %u, end %u, first %u, "
	    "chunk %u, seen %d, nested %d",
	    __func__, st->st_diff.beg, st->st_diff.end, dd.dd_first,
	    dd.dd_tk->tk_lno, st->st_diff.end > 0, nested);

	if (st->st_diff.end > 0) {
		/*
		 * The diff chunk is spanning more than one group. Any preceding
		 * verbatim lines are already emitted at this point.
		 */
		return 1;
	}

	/*
	 * Find the corresponding diff chunk covering the token with respect to
	 * the fact that the token can span multiple lines.
	 */
	du = doc_diff_find_chunk(st, dd.dd_tk);
	assert(du != nullptr);

	/*
	 * Signal to doc_diff_leave() that this group covers more than one diff
	 * chunk. This could happen when a prefix token and the corresponding
	 * token is covered by two disjoint diff chunks, both residing in the
	 * same group.
	 */
	if (doc_diff_group_has_many_chunks(dc, st, du->du_end + 1))
		st->st_diff.group_has_many_chunks = 1;
	doc_trace(dc, st, "%s: chunk range [%u-%u], many? %d", __func__,
	    du->du_beg, du->du_end, st->st_diff.group_has_many_chunks);

	/*
	 * Take a tentative note on which line the diff chunk ends. Note that if
	 * the current group spans beyond the diff chunk, the end line will be
	 * adjusted. This can happen when reformatting causes lines to be
	 * merged.
	 */
	st->st_diff.end = du->du_end;

	/*
	 * We could still be in a muted section of the document, ignore and then
	 * restore in doc_diff_leave();
	 */
	st->st_diff.mute = st->st_mute;
	st->st_mute = 0;
	if (dd.dd_verbatim != nullptr) {
		const struct token *tk = dd.dd_verbatim;

		/*
		 * The diff chunk is preceeded with one or many verbatim tokens
		 * inside the same group, caused by verbatim tokens not being
		 * placed in seperate groups. Such verbatim tokens must
		 * not be formatted and we therefore stay mute until moving past
		 * the last verbatim token not covered by the diff chunk.
		 */
		st->st_diff.verbatim = tk;
		end = tk->tk_lno +
		    count_verbatim_lines(tk->tk_str, tk->tk_len, 0);
		doc_trace(dc, st, "%s: verbatim mute [%u-%u)", __func__,
		    st->st_diff.beg, end);
	} else {
		end = dd.dd_first;
	}

	/*
	 * Emit any preceding line(s) not covered by the diff chunk. It is of
	 * importance to begin at the line from the first token covered by this
	 * group and not the first line covered by the diff chunk; as a group
	 * represents something intended to fit on a single line but the diff
	 * chunk might only touch a subset of the group.
	 */
	doc_diff_emit(dc, st, st->st_diff.beg, end);
	return 1;
}

static void
doc_diff_group_leave(
    const struct doc *UNUSED(dc), struct doc_state *st, int enter)
{
	if (enter == 0 || !DOC_DIFF(st))
		return;
	assert(st->st_diff.group != nullptr);
	st->st_diff.group = nullptr;
	st->st_diff.group_has_many_chunks = 0;
}

static int
doc_diff_group_has_many_chunks(
    const struct doc *dc, struct doc_state *st, unsigned int threshold)
{
	struct doc_diff dd = {
	    .dd_threshold = threshold,
	    .dd_covers = DOC_DIFF_GROUP_NOT_COVERED,
	    .dd_below_threshold = DOC_WALK_CONTINUE,
	};

	doc_walk(dc, st, doc_diff_covers, &dd);
	return dd.dd_covers == DOC_DIFF_GROUP_COVERED;
}

static void
doc_diff_mute_enter(const struct doc *UNUSED(dc), struct doc_state *st)
{
	if (doc_diff_is_mute(st))
		return;
	st->st_diff.muteline = 1;
}

static void
doc_diff_mute_leave(const struct doc *dc, struct doc_state *st)
{
	if (!DOC_DIFF(st))
		return;

	if (st->st_diff.muteline && st->st_muteline) {
		doc_trace(
		    dc, st, "%s: muteline %u", __func__, st->st_diff.muteline);
		doc_state_reset_lines(st);
		doc_print(dc, st, "\n", 1, DOC_PRINT_FORCE);
	}
	st->st_diff.muteline = 0;
}

static void
doc_diff_literal(const struct doc *dc, struct doc_state *st)
{
	unsigned int lno;

	if (!DOC_DIFF(st))
		return;
	if (dc->dc_tk == nullptr || st->st_diff.end == 0)
		return;

	lno = dc->dc_tk->tk_lno;
	if (st->st_diff.group != nullptr) {
		if (lno > st->st_diff.end) {
			/*
			 * The current group spans beyond the diff chunk, adjust
			 * the end line. This can happen when reformatting
			 * causes lines to be merged.
			 */
			doc_trace(dc, st, "%s: end %u", __func__, lno);
			st->st_diff.end = lno;
		}
	} else if (lno > st->st_diff.end) {
		doc_diff_leave(dc, st, 1);
	}
}

/*
 * Track a verbatim node in -D mode. Outside a changed region this is a no-op.
 * If the node starts past the changed range the region is left. If it spans
 * beyond the range, return the number of extra lines so the caller can postpone
 * leaving the region until the node has been emitted.
 */
static unsigned int
doc_diff_verbatim(const struct doc *dc, struct doc_state *st)
{
	unsigned int lno, n;

	if (!DOC_DIFF(st))
		return 0;

	assert(dc->dc_tk != nullptr);
	lno = dc->dc_tk->tk_lno;
	if (lno == 0 || st->st_diff.end == 0)
		return 0;

	if (lno > st->st_diff.end) {
		doc_diff_leave(dc, st, 1);
		return 0;
	}

	/*
	 * A verbatim document could contain one or many hard line(s) which can
	 * takes us beyond the diff chunk. Signal that after emitting this
	 * document the diff chunk must be left.
	 */
	n = count_verbatim_lines(dc->dc_str, dc->dc_len, 0);
	if (n > 0 && lno + n > st->st_diff.end) {
		unsigned int end;

		end = (lno + n) - st->st_diff.end;
		doc_trace(dc, st, "%s: postpone leave: end %u", __func__, end);
		return end;
	}

	return 0;
}

static void
doc_diff_exit(const struct doc *dc, struct doc_state *st)
{
	if (!DOC_DIFF(st))
		return;

	/*
	 * Ensure to leave any previous diff chunk, can happen if the last group
	 * is covered by a diff chunk.
	 */
	if (st->st_diff.end > 0)
		doc_diff_leave(dc, st, 1);
	doc_diff_emit(dc, st, st->st_diff.beg, 0);
}

/*
 * Emit everything between the given lines as is.
 */
static void
doc_diff_emit(const struct doc *dc, struct doc_state *st, unsigned int beg,
    unsigned int end)
{
	const char *str;
	size_t	    len;

	doc_trace(dc, st, "%s: range [%u, %u)", __func__, beg, end);
	if (!lexer_get_lines(st->st_lx, beg, end, &str, &len))
		return;

	if (st->st_flags & DOC_EXEC_TRACE) {
		arena_scope(st->st_scratch, s);

		doc_trace(dc, st, "%s: verbatim \"%s\"", __func__,
		    KS_str_vis(str, len, &s));
	}

	doc_state_reset_lines(st);
	doc_trim_spaces(dc, st);
	doc_print(dc, st, str, len, DOC_PRINT_FORCE);
	doc_print_indent(dc, st, st->st_indent.cur);
}

/*
 * Returns non-zero if any document covers a token which is part of a diff
 * chunk. While walking, the first token covered by a chunk is remembered; a
 * hard line before one is found marks the group as spanning multiple lines.
 * Verbatim tokens not themselves covered by a chunk are tracked so the renderer
 * can keep them verbatim until the changed region starts.
 */
static unsigned int
doc_diff_covers(const struct doc *dc, struct doc_state *UNUSED(st), void *arg)
{
	struct doc_diff *dd = (struct doc_diff *)arg;

	switch (dc->dc_type) {
	case DOC_VERBATIM:
		if ((dc->dc_tk->tk_flags & TOKEN_FLAG_DIFF) == 0)
			dd->dd_verbatim = dc->dc_tk;
		FALLTHROUGH;
	case DOC_LITERAL:
		if (dc->dc_tk != nullptr) {
			unsigned int lno = dc->dc_tk->tk_lno;

			if (lno < dd->dd_threshold) {
				dd->dd_verbatim = nullptr;
				return dd->dd_below_threshold;
			}

			if (dd->dd_first == 0)
				dd->dd_first = lno;
			if (dc->dc_tk->tk_flags & TOKEN_FLAG_DIFF) {
				dd->dd_tk = dc->dc_tk;
				dd->dd_covers = DOC_DIFF_GROUP_COVERED;
				return DOC_WALK_BREAK;
			}
		}
		break;

	case DOC_HARDLINE:
		dd->dd_covers = DOC_DIFF_GROUP_IGNORE;
		return DOC_WALK_BREAK;

	default:
		break;
	}

	return DOC_WALK_CONTINUE;
}

/*
 * Returns non-zero if nothing should be emitted while being positioned on a
 * line not touched by the diff.
 */
static int
doc_diff_is_mute(const struct doc_state *st)
{
	return DOC_DIFF(st) &&
	    (st->st_diff.end == 0 || st->st_diff.verbatim != nullptr);
}

/*
 * Leave the current changed region. The next region begins after the recorded
 * end plus the caller's adjustment, and any diff-induced mute is restored. A
 * pending new line is emitted immediately because it would otherwise be lost
 * while muted. If the group covered several chunks, re-enter it to find the
 * next one.
 */
static void
doc_diff_leave_impl(const struct doc *dc, struct doc_state *st,
    unsigned int end, const char *fun)
{
	if (end == 0)
		return;

	assert(st->st_diff.end > 0);
	assert(st->st_diff.verbatim == nullptr);
	st->st_diff.beg = st->st_diff.end + end;
	st->st_diff.end = 0;
	st->st_mute = st->st_diff.mute;
	st->st_diff.mute = 0;
	doc_trace(dc, st, "doc_diff_leave: %s: leave chunk: beg %u", fun,
	    st->st_diff.beg);
	/*
	 * Emit pending new line immediately as we would otherwise lose track of
	 * it while being mute.
	 */
	if (st->st_newline)
		doc_print(dc, st, "\n", 1, DOC_PRINT_FORCE);

	if (st->st_diff.group_has_many_chunks) {
		/*
		 * The current group covers more than one diff chunk.
		 * Re-evaluate in order to enter the next one.
		 */
		doc_diff_group_enter(st->st_diff.group, st, 1);
	}
}

static int
doc_is_mute(const struct doc_state *st)
{
	return st->st_mute || doc_diff_is_mute(st);
}

/*
 * Returns non-zero if the current line is suitable for parenthesis alignment,
 * i.e. a line consisting of whitespace followed by one or many left
 * parenthesis. This is the desired outcome:
 *
 * 	if (a &&
 * 	    ((b &&
 * 	      c)))
 */
static int
doc_parens_align(const struct doc_state *st)
{
	const char *buf = buffer_get_ptr(st->st_bf);
	size_t	    buflen = buffer_get_len(st->st_bf);
	int	    nparens = 0;

	for (; buflen > 0 && buf[buflen - 1] == '('; buflen--)
		nparens++;
	if (nparens == 0 || buflen == 0)
		return 0;
	for (; buflen > 0; buflen--) {
		if (buf[buflen - 1] == '\n')
			break;
		if (buf[buflen - 1] != ' ' && buf[buflen - 1] != '\t')
			return 0;
	}
	return 1;
}

static int
doc_has_list(const struct doc *dc)
{
	return doc_descriptions[dc->dc_type].children.many;
}

/*
 * Set the column position, intended to be given the same string just added to
 * the document buffer.
 */
static unsigned int
doc_column(struct doc_state *st, const char *str, size_t len)
{
	unsigned int oldcol = st->st_col;
	unsigned int limit = style(st->st_st, ColumnLimit);

	st->st_col = strwidth(str, len, st->st_col);
	if (st->st_col > limit) {
		if (oldcol < limit)
			st->st_stats.nexceeds += st->st_col - limit;
		else
			st->st_stats.nexceeds += st->st_col - oldcol;
	}
	/* Cope with new line(s). */
	return st->st_col > oldcol ? st->st_col - oldcol : 0;
}

static unsigned int
doc_max1(const struct doc *dc, struct doc_state *UNUSED(st), void *arg)
{
	int *max = arg;

	switch (dc->dc_type) {
	case DOC_SOFTLINE:
		if (dc->dc_int > *max)
			*max = dc->dc_int;
		break;
	default:
		break;
	}
	return DOC_WALK_CONTINUE;
}

/*
 * Initialize renderer state. The maximum number of consecutive new lines
 * defaults to two; the diff start line is one because source lines are 1-based.
 * The consistency checks document which arguments must accompany DOC_EXEC_DIFF.
 */
static void
doc_state_init(
    struct doc_state *st, struct doc_exec_arg *arg, enum doc_mode mode)
{
	ASSERT_CONSISTENCY(arg->flags & DOC_EXEC_DIFF, arg->lx);
	ASSERT_CONSISTENCY(arg->flags & DOC_EXEC_DIFF, arg->diff_chunks);

	memset(st, 0, sizeof(*st));
	st->st_st = arg->st;
	st->st_bf = arg->bf;
	st->st_lx = arg->lx;
	st->st_scratch = arg->scratch;
	st->st_diff_chunks = arg->diff_chunks;
	st->st_maxlines = 2;
	st->st_flags = arg->flags;
	st->st_mode = mode;
	st->st_diff.beg = 1;
	st->st_minimize.idx = -1;
	st->st_minimize.force = -1;
}

/*
 * Reset state related to emitting new lines.
 */
static void
doc_state_reset_lines(struct doc_state *st)
{
	st->st_nlines = 0;
	st->st_newline = 0;
	st->st_muteline = 0;
}

/*
 * Save the complete renderer state and a copy of the output produced so far.
 * Used to try alternative layouts without disturbing the real buffer; the copy
 * is allocated in the given scratch scope.
 */
static void
doc_state_snapshot(struct doc_state_snapshot *sn, const struct doc_state *st,
    struct arena_scope *s)
{
	const char *buf = buffer_get_ptr(st->st_bf);
	size_t	    buflen = buffer_get_len(st->st_bf);

	sn->sn_st = *st;
	sn->sn_bf.ptr = arena_malloc(s, buflen);
	sn->sn_bf.len = buflen;
	memcpy(sn->sn_bf.ptr, buf, buflen);
}

static void
doc_state_snapshot_restore(
    const struct doc_state_snapshot *sn, struct doc_state *st)
{
	struct buffer *bf = st->st_bf;

	*st = sn->sn_st;
	buffer_reset(bf);
	buffer_puts(bf, sn->sn_bf.ptr, sn->sn_bf.len);
}

static void
doc_trace_impl(const struct doc *UNUSED(dc), const struct doc_state *st,
    const char *fmt, ...)
{
	va_list	     ap;
	unsigned int depth, i;

	arena_scope(st->st_scratch, s);

	fprintf(stderr, "%s", statestr(st, st->st_depth, &s));
	depth = (st->st_depth * 2) + 1;
	for (i = 0; i < depth; i++)
		fprintf(stderr, "-");

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\n");
}

static void
doc_trace_enter_impl(const struct doc *dc, struct doc_state *st)
{
	const struct doc_description *desc = &doc_descriptions[dc->dc_type];
	unsigned int		      depth = st->st_depth;
	unsigned int		      i;

	arena_scope(st->st_scratch, s);

	st->st_depth++;

	fprintf(stderr, "%s ", statestr(st, st->st_depth, &s));

	for (i = 0; i < depth; i++)
		fprintf(stderr, "  ");

	fprintf(stderr, "%s", docstr(dc, &s));
	fprintf(stderr, "(");
	if (desc->children.many)
		fprintf(stderr, "[");
	if (desc->value.integer)
		fprintf(stderr, "%d", dc->dc_int);
	if (desc->value.string) {
		fprintf(stderr, "\"%s\", %zu",
		    KS_str_vis(dc->dc_str, dc->dc_len, &s), dc->dc_len);
	}

	switch (dc->dc_type) {
	case DOC_MINIMIZE:
		if (st->st_minimize.idx == -1) {
			fprintf(stderr, "-1");
			break;
		}
		FALLTHROUGH;

	case DOC_INDENT:
	case DOC_NOINDENT: {
		fprintf(stderr, "%s", indentstr(dc, st, &s));
		break;
	}

	case DOC_LINE:
		if (st->st_mode == BREAK)
			fprintf(stderr, "\"\\n\", 1");
		else if (!st->st_newline)
			fprintf(stderr, "\" \", 1");
		else
			fprintf(stderr, "\"\", 0");
		break;

	case DOC_SOFTLINE:
		fprintf(stderr, "\"%s\", %d", st->st_mode == BREAK ? "\\n" : "",
		    st->st_mode == BREAK ? 1 : 0);
		break;

	case DOC_HARDLINE:
		fprintf(stderr, "\"\\n\", 1");
		break;

	case DOC_OPTLINE:
		fprintf(stderr, "\"%s\", %d", st->st_optline ? "\\n" : "",
		    st->st_optline ? 1 : 0);
		break;

	default:
		break;
	}
	if (!desc->children.many && !desc->children.one)
		fprintf(stderr, ")");
	fprintf(stderr, "\n");
}

static void
doc_trace_leave_impl(const struct doc *dc, struct doc_state *st)
{
	const struct doc_description *desc = &doc_descriptions[dc->dc_type];
	unsigned int		      depth = st->st_depth;
	unsigned int		      i;

	arena_scope(st->st_scratch, s);

	st->st_depth--;

	if (!desc->children.many && !desc->children.one)
		return;

	fprintf(stderr, "%s ", statestr(st, depth, &s));
	for (i = 0; i < st->st_depth; i++)
		fprintf(stderr, "  ");
	if (desc->children.many)
		fprintf(stderr, "]");
	if (desc->children.many || desc->children.one)
		fprintf(stderr, ")");
	fprintf(stderr, "\n");
}

static const char *
docstr(const struct doc *dc, struct arena_scope *s)
{
	const char *name;
	int	    suffix = dc->dc_suffix != nullptr;

	name = doc_descriptions[dc->dc_type].name;
	return arena_sprintf(s, "%s<%s:%d%s%s%s>", name, dc->dc_fun, dc->dc_lno,
	    suffix ? ", \"" : "", suffix ? dc->dc_suffix : "",
	    suffix ? "\"" : "");
}

static const char *
indentstr(
    const struct doc *dc, const struct doc_state *st, struct arena_scope *s)
{
	struct buffer *bf;
	int	       indent;

	bf = arena_buffer_alloc(s, 1 << 5);

	if (dc->dc_type == DOC_MINIMIZE)
		indent = (int)dc->dc_minimizers[st->st_minimize.idx].indent;
	else
		indent = dc->dc_int;

	if (IS_DOC_INDENT_NEWLINE(indent)) {
		buffer_printf(bf, "NEWLINE, %d, %u",
		    dc->dc_int & ~DOC_INDENT_NEWLINE, st->st_stats.nlines);
	} else if (IS_DOC_INDENT_PARENS(indent)) {
		buffer_printf(bf, "PARENS");
	} else if (IS_DOC_INDENT_FORCE(indent)) {
		buffer_printf(bf, "FORCE");
	} else if (IS_DOC_INDENT_WIDTH(indent)) {
		buffer_printf(bf, "WIDTH");
	} else {
		buffer_printf(bf, "%d", indent);
	}
	return buffer_str(bf);
}

static const char *
statestr(const struct doc_state *st, unsigned int depth, struct arena_scope *s)
{
	const char   *mute;
	unsigned char mode = 'U';

	switch (st->st_mode) {
	case BREAK:
		mode = 'B';
		break;
	case MUNGE:
		mode = 'M';
		break;
	}

	if (doc_diff_is_mute(st))
		mute = arena_strdup(s, "D");
	else
		mute = arena_sprintf(s, "%d", st->st_mute);

	return arena_sprintf(s, "[D] [%c C=%-3u D=%-3u U=%s O=%d]", mode,
	    st->st_col, depth, mute, st->st_optline);
}

static unsigned int
count_verbatim_lines(const char *str, size_t len, unsigned int fallback)
{
	unsigned int nlines = 0;

	/* Only applicable to tokens ending with a hard line. */
	if (len > 0 && str[len - 1] != '\n')
		return fallback;

	while (len > 0) {
		const char *p;
		size_t	    linelen;

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
