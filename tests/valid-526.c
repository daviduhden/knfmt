/*
 * c89_abstract_func.
 */

int
f(void)
{
	return sizeof(int(int));
}

int
g(void)
{
	return sizeof(int(void));
}
