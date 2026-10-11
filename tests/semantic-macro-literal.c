/*
 * Sense cpp macro alignment: a line continuation inside a string literal must
 * be preserved as is, never realigned (the padding would become part of the
 * literal).
 */

#define RESOLVE_ITEM_STR(field) \
	"a\
	bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" \
	c
