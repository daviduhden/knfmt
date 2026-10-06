/*
 * c23_generic_nested.
 */

int
f(int x)
{
	return _Generic(x, int: _Generic(x, int: 1, default: 0), default: 0);
}
