/*
 * c11_generic.
 */

int
f(int x)
{
	return _Generic(x, int: 1, long: 2, default: 0);
}
