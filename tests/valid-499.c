/*
 * c23_attr_stmt.
 */

void
f(int x)
{
	[[likely]] if (x)
		return;
	[[unlikely]] while (x)
		break;
}
