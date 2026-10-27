#include <stdint.h>

#include "config.h"
#include "libks/arena-buffer.h"
#include "libks/buffer.h"
#include "libks/string.h"
#include "libks/vector.h"
#include "util.h"

/*
 * Return the 1-based column reached after processing len bytes starting at the
 * given column. Tabs advance to the next multiple of eight and a new line resets
 * the column to one. The byte scan is split so that the common case (no tab and
 * no new line) is handled by the vectorized string matcher.
 */
unsigned int
colwidth(const char *str, size_t len, unsigned int cno)
{
	static const uint8_t tab_offsets[8] = {
	    [0] = 0 + 1,
	    [1] = 7 + 1,
	    [2] = 6 + 1,
	    [3] = 5 + 1,
	    [4] = 4 + 1,
	    [5] = 3 + 1,
	    [6] = 2 + 1,
	    [7] = 1 + 1,
	};
	static struct KS_str_match match;

	KS_str_match_init_once("\t\t\n\n", &match);

	while (len > 0) {
		size_t n;

		n = KS_str_match_until(str, len, &match);
		cno += n;
		str += n;
		len -= n;

		for (; len > 0; len--, str++) {
			char c = str[0];

			if (c == '\t')
				cno += tab_offsets[cno & 0x7];
			else if (c == '\n')
				cno = 1;
			else
				break;
		}
	}

	return cno;
}

/*
 * Returns the width of the last line in the given string, with respect to tabs.
 */
size_t
strwidth(const char *str, size_t len, size_t pos)
{
	static const uint8_t tab_offsets[8] = {
	    [0] = 8,
	    [1] = 7,
	    [2] = 6,
	    [3] = 5,
	    [4] = 4,
	    [5] = 3,
	    [6] = 2,
	    [7] = 1,
	};
	static struct KS_str_match match;

	KS_str_match_init_once("\t\t\n\n", &match);

	while (len > 0) {
		size_t n;

		n = KS_str_match_until(str, len, &match);
		pos += n;
		str += n;
		len -= n;

		for (; len > 0; len--, str++) {
			char c = str[0];

			if (c == '\t')
				pos += tab_offsets[pos & 0x7];
			else if (c == '\n')
				pos = 0;
			else
				break;
		}
	}

	return pos;
}

/*
 * Return the last ncomponents components of path, joined with slashes. If the
 * path has fewer components than requested, the whole path is returned
 * unchanged. The result is allocated in the given arena scope.
 */
const char *
path_slice(const char *path, unsigned int ncomponents, struct arena_scope *s)
{
	static struct KS_str_match match;
	KS_str_match_init_once("//", &match);

	VECTOR(char *) components;
	struct buffer *bf;
	unsigned int i, n;

	components = KS_str_split(path, &match, s);
	n = VECTOR_LENGTH(components);
	if (n < ncomponents) {
		/* Less components than wanted, return the whole path. */
		return path;
	}

	bf = arena_buffer_alloc(s, 1 << 8);
	for (i = n - ncomponents; i < n; i++) {
		if (buffer_get_len(bf) > 0)
			buffer_putc(bf, '/');
		buffer_printf(bf, "%s", components[i]);
	}
	return buffer_str(bf);
}

/*
 * Append indentation to the buffer, using tabs while at least eight columns
 * remain and spaces for the rest. pos is the current column and the returned
 * value is the resulting column, so callers can track tab stops exactly.
 */
size_t
strindent_buffer(struct buffer *bf, size_t indent, int usetabs, size_t pos)
{
	size_t i = 0;

	if (usetabs) {
		for (; i + 8 <= indent; i += 8) {
			buffer_putc(bf, '\t');
			pos += 8 - (pos % 8);
		}
	}
	for (; i < indent; i++) {
		buffer_putc(bf, ' ');
		pos++;
	}
	return pos;
}
