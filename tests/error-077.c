/*
 * A bare _BitInt without its width is not a valid type specifier. The
 * declaration must be rejected, not silently dropped.
 */

int c;
_BitInt
