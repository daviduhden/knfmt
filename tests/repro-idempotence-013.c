/*
 * A malformed declarator list must not drop punctuation. The comma before a
 * missing declarator is part of the preprocessing-token sequence.
 */

int	a,;

int	(*fp)(void),;

int	b, c,;
