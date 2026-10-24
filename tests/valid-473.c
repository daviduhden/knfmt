/*
 * c23_attr_positions.
 */

struct s {
	[[maybe_unused]] int a;
	int b [[deprecated]];
};
void
f(int x[[maybe_unused]] )
{
	(void)x;
}
