/*
 * c23_nested_generic.
 */

int
g(int x)
{
	return _Generic(x, int: _Generic(x, int: 1, default: 0), default: 0);
}
