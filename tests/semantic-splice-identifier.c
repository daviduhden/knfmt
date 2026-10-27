/*
 * A backslash-newline splice inside an identifier in a macro body keeps the
 * identifier joined; the continuation must not be aligned with padding that
 * splits it.
 */

#define CONSUMER_DESCRIPTOR                                                                        \
	(sizeof(consumer_descrip\
tor) + 1)

int
f(int consumer_descriptor)
{
	return CONSUMER_DESCRIPTOR;
}
