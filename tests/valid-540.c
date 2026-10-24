/*
 * Blank line between call arguments.
 */

void
g(int a, int b, int c)
{
}

void
f(void)
{
	g(1,

	    2,
	    3);

	h(1,

	    2);
}
