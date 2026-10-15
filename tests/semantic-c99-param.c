/*
 * A line comment ending a parameter type must not let the following
 * parameter be joined onto the comment line.
 */

int
f(int x //
	y)
{
}
