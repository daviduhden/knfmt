#include <string.h>

#include "clang.h"
#include "config.h"
#include "doc.h"
#include "lexer.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/compiler.h"
#include "libks/list.h"
#include "options.h"
#include "parser-decl.h"
#include "parser-extern.h"
#include "parser-func.h"
#include "parser-priv.h"
#include "parser-stmt-asm.h"
#include "parser.h"
#include "token.h"
#include "trace-types.h"

static void
clang_format_verbatim(struct parser *pr, struct doc *dc, unsigned int end)
{
	struct token *off, *verbatim;
	const char   *str;
	size_t	      len;
	unsigned int  beg;

	off = pr->pr_token.clang_format_off;
	pr->pr_token.clang_format_off = nullptr;
	if (off == nullptr)
		return;
	beg = off->tk_lno + token_lines(off);
	/*
	 * A comment that does not end in a newline (e.g. a C99 comment at end
	 * of file) occupies its last line, so the region starts on the next
	 * one. Without this the comment would be emitted both as a prefix and
	 * as the start of the verbatim region, duplicating it on each pass.
	 */
	if (off->tk_len > 0 && off->tk_str[off->tk_len - 1] != '\n')
		beg++;
	token_rele(off);

	parser_trace(pr, "beg %u, end %u", beg, end);

	doc_alloc_impl(DOC_UNMUTE, dc, -1, __func__, __LINE__);
	if (pr->pr_mute > 0)
		pr->pr_mute--;
	if (!lexer_get_lines(pr->pr_lx, beg, end, &str, &len))
		return;

	verbatim = lexer_emit_synthetic(pr->pr_lx,
	    &(struct token){
		.tk_type = TOKEN_LITERAL,
		.tk_str = str,
		.tk_len = len,
	    });
	doc_token(verbatim, dc, DOC_VERBATIM, __func__, __LINE__);
	token_rele(verbatim);
}

/*
 * Emit the whole source verbatim. Used when invalid source cannot be recovered
 * by crossing a preprocessor branch without dropping content.
 */
static void
parser_verbatim_all(struct parser *pr, struct doc *dc)
{
	struct token *verbatim;
	const char   *str;
	size_t	      len;

	doc_alloc_impl(DOC_UNMUTE, dc, -1, __func__, __LINE__);
	if (!lexer_get_lines(pr->pr_lx, 1, 0, &str, &len))
		return;

	verbatim = lexer_emit_synthetic(pr->pr_lx,
	    &(struct token){
		.tk_type = TOKEN_LITERAL,
		.tk_str = str,
		.tk_len = len,
	    });
	doc_token(verbatim, dc, DOC_VERBATIM, __func__, __LINE__);
	token_rele(verbatim);
}

static void
clang_format_on(struct parser *pr, struct token *comment, struct doc *dc)
{
	/* Ignore while peeking and branching. */
	if (lexer_get_peek(pr->pr_lx) || pr->pr_token.unmute != nullptr)
		return;

	parser_trace(pr, "%s", lexer_serialize(pr->pr_lx, comment));

	clang_format_verbatim(pr, dc, comment->tk_lno);
}

static void
clang_format_off(struct parser *pr, struct token *comment, struct doc *dc)
{
	/* Ignore while peeking, branching and already off. */
	if (lexer_get_peek(pr->pr_lx) || pr->pr_token.unmute != nullptr ||
	    pr->pr_token.clang_format_off != nullptr)
		return;

	parser_trace(pr, "%s", lexer_serialize(pr->pr_lx, comment));

	doc_alloc_impl(DOC_MUTE, dc, 1, __func__, __LINE__);
	pr->pr_mute++;

	/* Must account for more than one trailing hard line(s). */
	if (pr->pr_token.clang_format_off != nullptr)
		token_rele(pr->pr_token.clang_format_off);
	token_ref(comment);
	pr->pr_token.clang_format_off = comment;
}

struct parser *
parser_alloc(const struct parser_arg *arg, struct arena_scope *s)
{
	struct parser *pr;

	pr = arena_calloc(s, 1, sizeof(*pr));
	pr->pr_op = arg->options;
	pr->pr_st = arg->style;
	pr->pr_si = arg->simple;
	pr->pr_lx = arg->lexer;
	pr->pr_clang = arg->clang;
	pr->pr_arena = *arg->arena;

	return pr;
}

int
parser_exec(
    struct parser *pr, const struct diffchunk *diff_chunks, struct buffer *bf)
{
	struct doc   *dc;
	struct clang *clang = pr->pr_clang;
	struct lexer *lx = pr->pr_lx;
	unsigned int  doc_flags = 0;
	unsigned int  ndocs = 0;
	unsigned int  brch_docs = 0;
	int	      error = 0;

	arena_scope(pr->pr_arena.doc, doc_scope);
	parser_arena_scope(&pr->pr_arena_scope.doc, &doc_scope, cookie);

	dc = doc_root(&doc_scope);

	for (;;) {
		struct doc   *concat;
		struct token *tk;

		/* Always emit EOF token as it could have prefixes. */
		if (lexer_if(lx, LEXER_EOF, &tk)) {
			if (token_has_prefixes(tk))
				parser_doc_token(pr, tk, dc);
			error = 0;
			break;
		}

		concat = doc_alloc(DOC_CONCAT, dc);
		ndocs++;

		error = parser_root(pr, concat);
		if (error & GOOD) {
			clang_stamp(clang, lx, ndocs);
			brch_docs = 0;
			pr->pr_verbatim = 0;
		} else if (error & BRCH) {
			if (!clang_branch(clang, lx, &pr->pr_token.unmute))
				break;
			brch_docs = ndocs;
			parser_reset(pr);
		} else if (error & (FAIL | NONE)) {
			int r, verbatim = 0, deferred = 0;

			r = clang_recover(clang, lx, &pr->pr_token.unmute,
			    ndocs, brch_docs, &verbatim, &deferred);
			if (deferred)
				pr->pr_lossy = 1;

			/*
			 * Either the parser declined an extern block whose
			 * closing brace is conditional, or recovery would
			 * rewind before a branch whose tokens are already
			 * removed. There is no way to rewind to a consistent
			 * position without dropping content, so preserve the
			 * whole source verbatim.
			 */
			if (pr->pr_verbatim || verbatim) {
				pr->pr_verbatim = 0;
				while (ndocs > 0) {
					doc_remove_tail(dc);
					ndocs--;
				}
				parser_verbatim_all(pr, dc);
				error = 0;
				break;
			}

			if (r == 0)
				break;
			while (r-- > 0) {
				doc_remove_tail(dc);
				ndocs--;
			}
			parser_reset(pr);
		}
	}
	if (error) {
		lexer_error_flush(lx);
		return 1;
	}

	/*
	 * A branch was crossed with no surviving stamp, so the crossed
	 * branch's tokens were removed and cannot be re-emitted. The parse
	 * nevertheless succeeded, which means the documents built since are
	 * missing that content. Emit the whole source verbatim instead. If the
	 * parse had failed, the error above would already have been reported,
	 * so genuinely unrecoverable input keeps its rejection.
	 */
	if (pr->pr_lossy) {
		pr->pr_lossy = 0;
		while (doc_remove_tail(dc))
			continue;
		parser_verbatim_all(pr, dc);
	}

	clang_format_verbatim(pr, dc, 0);

	/*
	 * A recovery may have dropped the recorded clang-format off comment
	 * while the mute it emitted is still active, which would silently
	 * suppress the rest of the file. Preserve the whole source instead.
	 */
	if (pr->pr_mute > 0) {
		pr->pr_mute = 0;
		while (ndocs > 0) {
			doc_remove_tail(dc);
			ndocs--;
		}
		parser_verbatim_all(pr, dc);
	}

	if (pr->pr_op->diffparse)
		doc_flags |= DOC_EXEC_DIFF;
	else
		doc_flags |= DOC_EXEC_TRIM;
	if (options_trace_level(pr->pr_op, TRACE_DOC) > 0)
		doc_flags |= DOC_EXEC_TRACE;
	doc_exec(&(struct doc_exec_arg){
	    .dc = dc,
	    .lx = pr->pr_op->diffparse ? pr->pr_lx : nullptr,
	    .scratch = pr->pr_arena.scratch,
	    .diff_chunks = pr->pr_op->diffparse ? diff_chunks : nullptr,
	    .bf = bf,
	    .st = pr->pr_st,
	    .flags = doc_flags,
	});

	return 0;
}

int
parser_root(struct parser *pr, struct doc *dc)
{
	int error;

	error = parser_decl(pr, dc,
	    PARSER_DECL_BREAK | PARSER_DECL_LINE | PARSER_DECL_ROOT |
		PARSER_DECL_TRIM_SEMI | PARSER_DECL_SIMPLE_FORWARD);
	if (error & NONE)
		error = parser_func_impl(pr, dc);
	if (error & NONE)
		error = parser_extern(pr, dc);
	if (error & NONE)
		error = parser_root_asm(pr, dc);
	if (error & NONE)
		return parser_fail(pr);
	return error;
}

static int
is_branch(const struct lexer *lx)
{
	struct token *back;

	return lexer_back(lx, &back) && (back->tk_flags & TOKEN_FLAG_BRANCH);
}

int
parser_fail_impl(struct parser *pr, const char *fun, int lno)
{
	struct token  fallback = {.tk_lno = 1, .tk_cno = 1};
	struct lexer *lx = pr->pr_lx;
	struct token *tk = nullptr;

	/*
	 * A pending preprocessor branch is not an error: a retry continues the
	 * parse on the other branch. Recording an error here would make callers
	 * discard the document built so far, losing syntax committed before
	 * the branch.
	 */
	if (is_branch(lx))
		return BRCH;

	if (lexer_get_error(lx))
		return FAIL;

	if (!lexer_back(lx, &tk) && !lexer_peek_first(lx, &tk))
		tk = &fallback;
	lexer_error(
	    pr->pr_lx, tk, fun, lno, "error at %s", lexer_serialize(lx, tk));

	return FAIL;
}

/*
 * Remove any preceding hard line(s) from the given token, unless the token has
 * prefixes intended to be emitted.
 */
void
parser_token_trim_before(const struct parser *UNUSED(pr), struct token *tk)
{
	struct token *pv;

	if (!token_is_moveable(tk))
		return;
	pv = token_prev(tk);
	if (pv != nullptr)
		token_trim(pv);
}

/*
 * Remove any subsequent hard line(s) from the given token.
 */
void
parser_token_trim_after(const struct parser *UNUSED(pr), struct token *tk)
{
	token_trim(tk);
}

/*
 * Returns the width of the given document.
 */
unsigned int
parser_width(struct parser *pr, const struct doc *dc)
{
	struct buffer *bf;

	arena_scope(pr->pr_arena.buffer, s);

	bf = arena_buffer_alloc(&s, 1 << 10);
	return doc_width(&(struct doc_exec_arg){
	    .dc = dc,
	    .scratch = pr->pr_arena.scratch,
	    .bf = bf,
	    .st = pr->pr_st,
	});
}

int
parser_good(const struct parser *pr)
{
	if (is_branch(pr->pr_lx))
		return BRCH;
	return lexer_get_error(pr->pr_lx) ? FAIL : GOOD;
}

int
parser_none(const struct parser *pr)
{
	if (is_branch(pr->pr_lx))
		return BRCH;
	return lexer_get_error(pr->pr_lx) ? FAIL : NONE;
}

void
parser_reset(struct parser *pr)
{
	struct token *nx;

	lexer_error_reset(pr->pr_lx);

	/* Remove last clang-format off if about to be traversed again. */
	if (pr->pr_token.clang_format_off != nullptr &&
	    lexer_peek(pr->pr_lx, &nx) &&
	    token_cmp(nx, pr->pr_token.clang_format_off) < 0) {
		token_rele(pr->pr_token.clang_format_off);
		pr->pr_token.clang_format_off = nullptr;
	}
}

int
parser_depth_enter(struct parser *pr)
{
	if (pr->pr_nest_depth >= PARSER_MAX_DEPTH)
		return 0;
	pr->pr_nest_depth++;
	return 1;
}

void
parser_depth_leave(struct parser_depth_guard *guard)
{
	if (guard->pd_pr != nullptr)
		guard->pd_pr->pr_nest_depth--;
}

struct doc *
parser_doc_token_impl(struct parser *pr, struct token *tk, struct doc *dc,
    const char *fun, int lno)
{
	struct doc   *out;
	struct token *nx, *prefix, *suffix;
	int	      c99comment = 0;

	if (tk == pr->pr_token.unmute) {
		if (!lexer_get_peek(pr->pr_lx))
			pr->pr_token.unmute = nullptr;
		doc_alloc_impl(DOC_MUTE, dc, -1, __func__, __LINE__);
	}

	LIST_FOREACH(prefix, &tk->tk_prefixes)
	{
		if (prefix->tk_flags & TOKEN_FLAG_COMMENT_CLANG_FORMAT_ON)
			clang_format_on(pr, prefix, dc);

		doc_token(prefix, dc, DOC_VERBATIM, __func__, __LINE__);

		if (prefix->tk_flags & TOKEN_FLAG_COMMENT_CLANG_FORMAT_OFF)
			clang_format_off(pr, prefix, dc);
	}

	out = doc_token(tk, dc, DOC_LITERAL, fun, lno);

	LIST_FOREACH(suffix, &tk->tk_suffixes)
	{
		if (suffix->tk_flags & TOKEN_FLAG_DISCARD) {
			/*
			 * A discarded optional line must still be honored
			 * after a C99 comment: the comment runs to the end
			 * of the line, and without the line break the next
			 * token is swallowed by the comment.
			 */
			if (c99comment &&
			    (suffix->tk_flags & TOKEN_FLAG_OPTLINE))
				doc_alloc_impl(
				    DOC_OPTLINE, dc, 1, __func__, __LINE__);
			continue;
		}
		if (suffix->tk_flags & TOKEN_FLAG_OPTLINE) {
			/*
			 * A C99 comment runs to the end of the line, so the
			 * following line break is mandatory. A dropped
			 * optional line would let the next token be swallowed
			 * by the comment. Force the line while keeping the
			 * deferred indentation.
			 */
			doc_alloc_impl(
			    DOC_OPTLINE, dc, c99comment, __func__, __LINE__);
		} else {
			doc_token(suffix, dc, DOC_VERBATIM, __func__, __LINE__);
			if (suffix->tk_flags & TOKEN_FLAG_COMMENT_C99)
				c99comment = 1;
		}
	}

	/* Mute if we're about to branch. */
	nx = token_next(tk);
	if (nx != nullptr && (nx->tk_flags & TOKEN_FLAG_BRANCH))
		doc_alloc_impl(DOC_MUTE, dc, 1, __func__, __LINE__);

	return out;
}

int
parser_semi(struct parser *pr, struct doc *dc)
{
	struct lexer_state s;
	struct lexer	  *lx = pr->pr_lx;
	struct token	  *last = nullptr;
	struct token	  *nx, *semi;

	if (!lexer_expect(lx, TOKEN_SEMI, &semi))
		return parser_fail(pr);

	/*
	 * Discard subsequent semicolon(s) on the same line. The last one is
	 * emitted in order to honor optional lines.
	 */
	lexer_peek_enter(lx, &s);
	while (lexer_peek_if(lx, TOKEN_SEMI, &nx) && token_cmp(semi, nx) == 0 &&
	    lexer_pop(lx, &nx))
		last = nx;
	lexer_peek_leave(lx, &s);
	if (last != nullptr) {
		while (lexer_pop(lx, &semi) && semi != last)
			lexer_remove(lx, semi);
	}
	parser_doc_token(pr, semi, dc);

	return parser_good(pr);
}

void
parser_arena_scope_enter(struct parser_arena_scope_cookie *cookie,
    struct arena_scope **old_scope, struct arena_scope *new_scope)
{
	cookie->restore_scope = old_scope;
	cookie->old_scope = *old_scope;
	*old_scope = new_scope;
}

void
parser_arena_scope_leave(struct parser_arena_scope_cookie *cookie)
{
	*cookie->restore_scope = cookie->old_scope;
}
