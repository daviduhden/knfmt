/*
 * A branch whose alternative fails to parse must not drop the first branch.
 * Folding the alternative here crosses back over the first branch, whose
 * tokens are already removed; the content would be lost.
 */

int	x;

#ifdef CPURESET_DELAY
int	cpureset_delay = CPURESET_DELAY;
#else
int	c <ddb/db_extern.h>
extpureset_delay = 0;
#endif

int	x;
