/*
 * c89_call_nested.
 */

void
f(void)
{
	g(h[i]);
	f(g(h[i]));
	foo(e * (f + 1));
}
