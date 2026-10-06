/*
 * c89_trigraph_ops.
 */

int
f(int a, int b)
{
	return (a ??' b) ??! ??-a;
}
