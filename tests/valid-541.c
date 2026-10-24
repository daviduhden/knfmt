/*
 * Macro/string juxtaposition: a call followed by a string and another
 * call must not break before the callee.
 */

const char *
f(void)
{
	return A(BBBBBBBBBBBBBBBBBBBBBBBBBBBBBB) "." A(CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC);
}

const char *
g(void)
{
	return A(BBBBBBBBBBBBBBBBBBBBBBBBBBBBBB) "x" A(CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC);
}
