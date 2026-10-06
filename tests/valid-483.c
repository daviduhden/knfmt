/*
 * c23_attribute_stmt.
 */

void
f(int x)
{
	[[maybe_unused]] int y = x;

	switch (x) {
	case 1:
		[[fallthrough]];
	default:
		break;
	}
}
