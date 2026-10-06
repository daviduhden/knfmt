/*
 * c23_generic_types.
 */

int
f(int x)
{
	return _Generic(x, int: 1, unsigned long long: 2, char *: 3, default: 0);
}
