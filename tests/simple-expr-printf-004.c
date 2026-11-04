/*
 * Preserve escaped backslashes and encoding prefixes in newline removal.
 */

void
f(void)
{
	warnx("\\n");
	warnx("\\\\n");
	warnx("x\\\n");
	warnx(u8"x\n");
	warnx(L"x\n");
	warnx(u"x\n");
	warnx(U"x\n");
	warnx("x" "y\n");
}
