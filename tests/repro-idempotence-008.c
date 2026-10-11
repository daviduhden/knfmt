/*
 * Adjacent identifiers following a malformed for-init must stay separated:
 * crossing the broken construct must not merge them into a single identifier.
 */

void
f(void)
{
	for (
 * c99_for_decl.
 */

voiint i = 0; i < 3; i++)
		continue;
}
