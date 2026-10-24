/*
 * c99_compound.
 */

struct s {
	int a;
};
int
f(void)
{
	return ((struct s){ .a = 1 }).a;
}
