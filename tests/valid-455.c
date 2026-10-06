/*
 * c11_alignof.
 */

int
f(void)
{
	return _Alignof(int) + alignof(double);
}
