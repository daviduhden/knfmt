#include "config.h"
#include "libks/arena.h"
#include "libks/compiler.h"
#include "options.h"
#include "simple.h"

/*
 * State for the -s simplification passes.
 *
 * A pass is entered around a speculative parse and leaves behind the token
 * rewrite it decided on. Passes are mutually exclusive by default so their token
 * edits cannot overlap; a handful of intentionally composable combinations are
 * listed in is_pass_mutually_exclusive(). The cookie restores the previous state
 * when the pass is left, and a disabled or ignored pass simply returns zero so
 * the caller proceeds without simplifying.
 */
enum simple_state {
	SIMPLE_STATE_NOP = -1,
	SIMPLE_STATE_DISABLE = 0,
	SIMPLE_STATE_ENABLE = 1,
	SIMPLE_STATE_IGNORE = 2,
};

struct simple {
	struct {
		enum simple_state state;
		unsigned int	  flags;
	} passes[SIMPLE_LAST];
	int enable;
};

static int is_pass_mutually_exclusive(const struct simple *, enum simple_pass);

/*
 * Allocate pass state. Simplification is enabled only when -s was given; a pass
 * may still force itself on for behavior such as include sorting that is also
 * driven by the style.
 */
struct simple *
simple_alloc(struct arena_scope *eternal_scope, const struct options *op)
{
	struct simple *si;

	si = arena_calloc(eternal_scope, 1, sizeof(*si));
	si->enable = op->simple;
	return si;
}

/*
 * Try to enter a pass. Returns non-zero only when the pass may run. The cookie
 * is filled first so every caller can use the cleanup macro, even on refusal.
 * A disabled global -s, an explicit SIMPLE_IGNORE, a conflicting active pass, or
 * a re-entry into the same pass all suppress execution.
 */
int
simple_enter(struct simple *si, enum simple_pass pass, unsigned int flags,
    struct simple_cookie *cookie)
{
	*cookie = (struct simple_cookie){
	    .si = si,
	    .pass = pass,
	    .state = si->passes[pass].state,
	};

	if (!si->enable && (flags & SIMPLE_FORCE) == 0) {
		si->passes[pass].state = SIMPLE_STATE_IGNORE;
		return 0;
	}

	if (is_pass_mutually_exclusive(si, pass)) {
		unsigned int i;

		for (i = 0; i < SIMPLE_LAST; i++) {
			if (i != pass &&
			    si->passes[i].state != SIMPLE_STATE_DISABLE) {
				si->passes[pass].state = SIMPLE_STATE_DISABLE;
				return 0;
			}
		}
	}

	if ((flags & SIMPLE_IGNORE) ||
	    si->passes[pass].state != SIMPLE_STATE_DISABLE) {
		si->passes[pass].state = SIMPLE_STATE_IGNORE;
		return 0;
	}

	si->passes[pass].state = SIMPLE_STATE_ENABLE;
	si->passes[pass].flags = flags;
	return 1;
}

/*
 * Leave a pass and restore the state that was active before simple_enter(). The
 * cookie is cleared so leaving twice is harmless.
 */
void
simple_leave(struct simple_cookie *cookie)
{
	struct simple	 *si = cookie->si;
	enum simple_pass  pass = cookie->pass;
	enum simple_state state = cookie->state;

	if (si == nullptr)
		return;

	*cookie = (struct simple_cookie){0};
	if (state == SIMPLE_STATE_NOP)
		return;
	si->passes[pass].state = state;
}

/*
 * Return whether a pass is currently active. Global -s or a per-pass force flag
 * must be present, and the pass itself must have been successfully entered.
 */
int
is_simple_enabled(const struct simple *si, enum simple_pass pass)
{
	return (si->enable || (si->passes[pass].flags & SIMPLE_FORCE)) &&
	    si->passes[pass].state == SIMPLE_STATE_ENABLE;
}

/*
 * Temporarily disable all simplification, used while peeking. Returns the
 * previous setting so simple_enable() can restore it.
 */
int
simple_disable(struct simple *si)
{
	int restore = si->enable;

	si->enable = 0;
	return restore;
}

void
simple_enable(struct simple *si, int restore)
{
	si->enable = restore;
}

/*
 * All passes are mutually exclusive by default. However, some combinations are
 * explicitly allowed as they do not interfere with each other.
 */
/*
 * Return non-zero if entering pass would conflict with an already active pass.
 * All pairs are exclusive unless explicitly whitelisted; the table documents the
 * combinations whose edits are known not to interfere.
 */
static int
is_pass_mutually_exclusive(const struct simple *si, enum simple_pass pass)
{
	static const struct {
		enum simple_pass parent;
		enum simple_pass nested;
	} exceptions[] = {
	    {SIMPLE_DECL, SIMPLE_ATTRIBUTES},
	    {SIMPLE_DECL, SIMPLE_BRACES},
	    {SIMPLE_DECL, SIMPLE_DECL_PROTO},
	    {SIMPLE_DECL, SIMPLE_IMPLICIT_INT},
	    {SIMPLE_DECL, SIMPLE_STORAGE},
	    /*
	     * SIMPLE_DECL_FORWARD only operates on root level declarations
	     * as opposed to SIMPLE_DECL which only operates on nested
	     * levels. They should therefore not interfere.
	     */
	    {SIMPLE_DECL, SIMPLE_DECL_FORWARD},

	    {SIMPLE_EXPR_PARENS, SIMPLE_EXPR_SIZEOF},

	    {SIMPLE_STMT, SIMPLE_STMT_EMPTY_LOOP},

	    {SIMPLE_STMT_EMPTY_LOOP, SIMPLE_STMT},
	    {SIMPLE_STMT_EMPTY_LOOP, SIMPLE_DECL},

	    {SIMPLE_STMT_SWITCH, SIMPLE_STMT},
	};
	unsigned int i;

	for (i = 0; i < SIMPLE_LAST; i++) {
		unsigned int j;

		if (si->passes[i].state != SIMPLE_STATE_ENABLE &&
		    si->passes[i].state != SIMPLE_STATE_IGNORE)
			continue;

		for (j = 0; j < countof(exceptions); j++) {
			if (exceptions[j].parent == i &&
			    exceptions[j].nested == pass)
				return 0;
		}
	}

	return 1;
}
