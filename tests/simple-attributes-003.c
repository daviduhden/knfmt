/*
 * A three-underscore attribute argument must be left as is: the "__"
 * prefix and suffix would overlap otherwise.
 */

int f(void) __attribute__((___));
int g(void) __attribute__((x));
