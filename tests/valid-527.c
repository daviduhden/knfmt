/*
 * c89_typeof_func.
 */

int
f(void)
{
	return sizeof(typeof(int(int)));
}
