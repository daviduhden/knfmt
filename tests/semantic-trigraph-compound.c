/*
 * A trigraph replacement is a character, not a token: ??' followed by = is
 * the ^= operator and must stay one punctuator.
 */

int
f(int a, int b)
{
	a = ? ?'= b; return a;
}
